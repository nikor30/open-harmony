/*
 * flashtest: check the Harmony One's flash erase/write path on a block
 * nothing uses.
 *
 * Needs libconcord built with patches/libconcord-dump-size-env.patch, which
 * adds lc_raw_read/lc_raw_erase_sector/lc_raw_write. Those refuse every
 * address outside flash 0x030000-0x03FFFF.
 *
 * Steps: check identity (Harmony One, arch 12, AT49BV322A, safe mode),
 * check the block is empty, erase, write a pattern, verify, erase, verify.
 *
 * Build:
 *   g++ -O2 -o flashtest tools/flashtest.cpp -Ilocal/include \
 *       -Llocal/lib -Wl,-rpath,$PWD/local/lib -lconcord
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <libconcord.h>

extern "C" {
int lc_raw_read(uint32_t addr, uint32_t len, uint8_t *out);
int lc_raw_erase_sector(uint32_t addr);
int lc_raw_write(uint32_t addr, uint32_t len, const uint8_t *data);
}

static const uint32_t BLOCK = 0x030000;
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

static bool all_ff(const std::vector<uint8_t> &buf, uint32_t *bad)
{
    for (uint32_t i = 0; i < buf.size(); i++) {
        if (buf[i] != 0xFF) {
            *bad = i;
            return false;
        }
    }
    return true;
}

static int read_block(std::vector<uint8_t> &buf)
{
    return lc_raw_read(BLOCK, SIZE, buf.data());
}

int main(int argc, char **argv)
{
    bool allow_normal = argc > 1 && strcmp(argv[1], "--allow-normal-mode") == 0;
    int err;

    if ((err = init_concord())) {
        return fail("init_concord (remote connected?)", err);
    }
    if ((err = get_identity(no_progress, NULL))) {
        return fail("get_identity", err);
    }
    printf("arch %d, firmware %d.%d type %d, flash %02X:%02X %s\n",
           get_arch(), get_fw_ver_maj(), get_fw_ver_min(), get_fw_type(),
           get_flash_mfg(), get_flash_id(), get_flash_part_num());

    if (get_arch() != 12 || get_flash_mfg() != 0x1F || get_flash_id() != 0xC8) {
        return fail("this is not the Harmony One with an AT49BV322A", 0);
    }
    if (get_fw_type() != 4 && !allow_normal) {
        return fail("remote is not in safe mode (firmware type 4); "
                    "hold Off while inserting the battery", 0);
    }

    std::vector<uint8_t> buf(SIZE), pattern(SIZE);
    for (uint32_t i = 0; i < SIZE; i++) {
        pattern[i] = (uint8_t)((i * 7) ^ (i >> 8) ^ 0x5A);
    }
    uint32_t bad;

    printf("1/6 read 0x%06X-0x%06X, expect empty\n", BLOCK, BLOCK + SIZE - 1);
    if ((err = read_block(buf))) {
        return fail("read", err);
    }
    if (!all_ff(buf, &bad)) {
        fprintf(stderr, "block is not empty at 0x%06X (0x%02X); not touching it\n",
                BLOCK + bad, buf[bad]);
        return fail("pre-check", 0);
    }

    printf("2/6 erase sector 0x%06X\n", BLOCK);
    if ((err = lc_raw_erase_sector(BLOCK))) {
        return fail("erase", err);
    }

    printf("3/6 write 64 KiB test pattern\n");
    if ((err = lc_raw_write(BLOCK, SIZE, pattern.data()))) {
        return fail("write", err);
    }

    printf("4/6 read back and compare\n");
    if ((err = read_block(buf))) {
        return fail("read back", err);
    }
    for (uint32_t i = 0; i < SIZE; i++) {
        if (buf[i] != pattern[i]) {
            fprintf(stderr, "mismatch at 0x%06X: wrote 0x%02X, read 0x%02X\n",
                    BLOCK + i, pattern[i], buf[i]);
            return fail("verify pattern", 0);
        }
    }

    printf("5/6 erase sector 0x%06X again\n", BLOCK);
    if ((err = lc_raw_erase_sector(BLOCK))) {
        return fail("second erase", err);
    }

    printf("6/6 read back, expect empty\n");
    if ((err = read_block(buf))) {
        return fail("final read", err);
    }
    if (!all_ff(buf, &bad)) {
        fprintf(stderr, "not empty at 0x%06X (0x%02X)\n", BLOCK + bad, buf[bad]);
        return fail("verify erase", 0);
    }

    printf("OK: erase, write and read work on 0x%06X-0x%06X\n",
           BLOCK, BLOCK + SIZE - 1);
    deinit_concord();
    return 0;
}
