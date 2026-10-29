/*
 * flashapp: check or write the Harmony One application image at 0x020000.
 *
 *   flashapp check app.bin   read-only: validate the image, compare it with
 *                            the remote's flash sector 0x020000
 *   flashapp write app.bin   safe mode only: validate, save the current
 *                            sector, erase, write, verify
 *
 * app.bin is the image as it sits at 0x020000 (header first), at most
 * 64 KiB. The header must be valid, or the base firmware would refuse to
 * start it: "HG" at +8, length at +4 = file size - 8, and the XOR checksum
 * (seeded 21 43, byte pairs +4..end) at +0.
 *
 * Needs libconcord built with patches/libconcord-dump-size-env.patch
 * (lc_raw_*, limited to flash 0x020000-0x03FFFF).
 *
 * Build:
 *   g++ -O2 -o local/bin/flashapp tools/flashapp.cpp -Ilocal/include \
 *       -Llocal/lib -Wl,-rpath,$PWD/local/lib -lconcord
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <libconcord.h>

extern "C" {
int lc_raw_read(uint32_t addr, uint32_t len, uint8_t *out);
int lc_raw_erase_sector(uint32_t addr);
int lc_raw_write(uint32_t addr, uint32_t len, const uint8_t *data);
}

static const uint32_t SECTOR = 0x020000;
static const uint32_t SIZE = 0x10000;

/* get_identity calls its callback unconditionally */
static void no_progress(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                        void *, const uint32_t *)
{
}

static int fail(const char *what, int err)
{
    fprintf(stderr, "FAILED: %s (error %d)\n", what, err);
    deinit_concord();
    return 1;
}

static bool load(const char *path, std::vector<uint8_t> &img)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return false;
    }
    img.resize(SIZE + 1);
    img.resize(fread(img.data(), 1, img.size(), f));
    fclose(f);
    return true;
}

/* Returns NULL if the header is valid, else what is wrong. */
static const char *check_header(const std::vector<uint8_t> &img)
{
    if (img.size() < 0x30 || img.size() > SIZE) {
        return "size must be 0x30..0x10000 bytes";
    }
    if (img.size() % 2) {
        return "size must be even";
    }
    if (img[8] != 'H' || img[9] != 'G') {
        return "no \"HG\" magic at +8";
    }
    uint32_t len = img[4] | img[5] << 8;
    if (len != img.size() - 8) {
        return "length at +4 does not match the file size";
    }
    uint8_t a = 0x21, b = 0x43;
    for (size_t i = 4; i < img.size(); i += 2) {
        a ^= img[i];
        b ^= img[i + 1];
    }
    if (img[0] != a || img[1] != b) {
        return "checksum at +0 is wrong";
    }
    return NULL;
}

/* Compares the sector with the image followed by erased flash. */
static long first_diff(const std::vector<uint8_t> &sector,
                       const std::vector<uint8_t> &img)
{
    for (uint32_t i = 0; i < SIZE; i++) {
        uint8_t want = i < img.size() ? img[i] : 0xFF;
        if (sector[i] != want) {
            return i;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "check") && strcmp(argv[1], "write"))) {
        fprintf(stderr, "usage: %s check|write app.bin\n", argv[0]);
        return 2;
    }
    bool write = strcmp(argv[1], "write") == 0;

    std::vector<uint8_t> img;
    if (!load(argv[2], img)) {
        return 1;
    }
    const char *bad = check_header(img);
    printf("image %s: %zu bytes, version %d.%d, header %s\n", argv[2],
           img.size(), img.size() > 7 ? img[7] >> 4 : 0,
           img.size() > 7 ? img[7] & 0xF : 0, bad ? bad : "OK");
    if (bad) {
        return 1;
    }

    int err;
    if ((err = init_concord())) {
        return fail("init_concord (remote connected?)", err);
    }
    if ((err = get_identity(no_progress, NULL))) {
        return fail("get_identity", err);
    }
    printf("remote: arch %d, firmware %d.%d type %d, flash %02X:%02X\n",
           get_arch(), get_fw_ver_maj(), get_fw_ver_min(), get_fw_type(),
           get_flash_mfg(), get_flash_id());
    if (get_arch() != 12 || get_flash_mfg() != 0x1F || get_flash_id() != 0xC8) {
        return fail("this is not the Harmony One with an AT49BV322A", 0);
    }

    std::vector<uint8_t> sector(SIZE);
    if ((err = lc_raw_read(SECTOR, SIZE, sector.data()))) {
        return fail("read sector 0x020000", err);
    }
    long diff = first_diff(sector, img);

    if (!write) {
        if (diff < 0) {
            printf("remote sector 0x020000 matches the image\n");
        } else {
            printf("remote sector 0x020000 differs, first at 0x%06lX\n",
                   SECTOR + diff);
        }
        deinit_concord();
        return 0;
    }

    if (get_fw_type() != 4) {
        return fail("writing needs safe mode (firmware type 4); "
                    "hold Off while inserting the battery", 0);
    }

    char saved[64];
    snprintf(saved, sizeof(saved), "sector-0x020000-%ld.bin", (long)time(NULL));
    FILE *f = fopen(saved, "wb");
    if (!f || fwrite(sector.data(), 1, SIZE, f) != SIZE || fclose(f)) {
        return fail("saving the current sector", 0);
    }
    printf("1/4 saved current sector to %s\n", saved);

    printf("2/4 erase sector 0x020000\n");
    if ((err = lc_raw_erase_sector(SECTOR))) {
        return fail("erase", err);
    }
    printf("3/4 write %zu bytes\n", img.size());
    if ((err = lc_raw_write(SECTOR, img.size(), img.data()))) {
        return fail("write", err);
    }
    printf("4/4 read back and verify\n");
    if ((err = lc_raw_read(SECTOR, SIZE, sector.data()))) {
        return fail("read back", err);
    }
    if ((diff = first_diff(sector, img)) >= 0) {
        fprintf(stderr, "mismatch at 0x%06lX: read 0x%02X\n", SECTOR + diff,
                sector[diff]);
        return fail("verify", 0);
    }
    printf("OK: image written and verified. Reinsert the battery without "
           "holding a key to start it.\n");
    deinit_concord();
    return 0;
}
