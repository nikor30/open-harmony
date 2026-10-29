/*
 * zwrite: write region 12 (platform, a ZIP) of a Harmony 900 in safe mode.
 *
 *   zwrite <platform.zip>
 *
 * Before writing: the file must start like a ZIP and pass `unzip -tqq`
 * (the remote's own first check), and the remote must be a Harmony 900
 * (arch 15) in safe mode (firmware type 4). The current region 12 is saved
 * to region12-before-<time>.zip; after writing, the region is read back to
 * region12-after-<time>.zip for comparison.
 *
 * Needs libconcord built with patches/libconcord-dump-size-env.patch
 * (lc_z_write_region, which only accepts region 12, and lc_z_read_region).
 *
 * Build:
 *   g++ -O2 -o local/bin/zwrite tools/zwrite.cpp -Ilocal/include \
 *       -Llocal/lib -Wl,-rpath,$PWD/local/lib -lconcord
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <spawn.h>
#include <sys/wait.h>
#include <vector>

#include <libconcord.h>

extern "C" {
int lc_z_read_region(uint8_t region, uint8_t *buf, uint32_t buf_len,
                     uint32_t *len);
int lc_z_write_region(uint8_t region, const uint8_t *data, uint32_t len);
}

extern char **environ;

static const uint8_t REGION = 12;
static const uint32_t MAX_SIZE = 32 * 1024 * 1024;

/* get_identity calls its callback unconditionally */
static void no_progress(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                        void *, const uint32_t *)
{
}

static bool unzip_test(const char *path)
{
    char *argv[] = {(char *)"unzip", (char *)"-tqq", (char *)path, NULL};
    pid_t pid;
    int status;
    if (posix_spawnp(&pid, "unzip", NULL, NULL, argv, environ) != 0 ||
        waitpid(pid, &status, 0) < 0) {
        perror("unzip");
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool save(const char *path, const uint8_t *data, uint32_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, len, f) != len || fclose(f)) {
        perror(path);
        return false;
    }
    return true;
}

static int read_region(const char *path)
{
    std::vector<uint8_t> buf(MAX_SIZE);
    uint32_t len = 0;
    int err = lc_z_read_region(REGION, buf.data(), buf.size(), &len);
    if (err) {
        fprintf(stderr, "reading region %d failed (error %d)\n", REGION, err);
        return 1;
    }
    if (!save(path, buf.data(), len)) {
        return 1;
    }
    printf("saved region %d (%u bytes) to %s\n", REGION, len, path);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <platform.zip>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    std::vector<uint8_t> data(MAX_SIZE + 1);
    size_t len = fread(data.data(), 1, data.size(), f);
    fclose(f);
    if (len == 0 || len > MAX_SIZE || memcmp(data.data(), "PK\x03\x04", 4)) {
        fprintf(stderr, "%s: not a ZIP of at most 32 MiB\n", argv[1]);
        return 1;
    }
    if (!unzip_test(argv[1])) {
        fprintf(stderr, "%s: unzip -t failed, not writing\n", argv[1]);
        return 1;
    }
    printf("%s: %zu bytes, unzip -t OK\n", argv[1], len);

    int err;
    if ((err = init_concord()) || (err = get_identity(no_progress, NULL))) {
        fprintf(stderr, "FAILED: connect/identity (error %d)\n", err);
        return 1;
    }
    printf("remote: arch %d, firmware %d.%d type %d\n", get_arch(),
           get_fw_ver_maj(), get_fw_ver_min(), get_fw_type());
    if (get_arch() != 15) {
        fprintf(stderr, "FAILED: not a Harmony 900 (arch 15)\n");
        deinit_concord();
        return 1;
    }
    if (get_fw_type() != 4) {
        fprintf(stderr, "FAILED: not in safe mode (firmware type 4); hold Off "
                        "while inserting the battery\n");
        deinit_concord();
        return 1;
    }

    char before[64], after[64];
    long now = (long)time(NULL);
    snprintf(before, sizeof(before), "region12-before-%ld.zip", now);
    snprintf(after, sizeof(after), "region12-after-%ld.zip", now);

    printf("1/3 save current region %d\n", REGION);
    if (read_region(before)) {
        deinit_concord();
        return 1;
    }
    printf("2/3 write %zu bytes to region %d\n", len, REGION);
    if ((err = lc_z_write_region(REGION, data.data(), len))) {
        fprintf(stderr, "FAILED: write (error %d); %s holds the previous "
                        "contents\n", err, before);
        deinit_concord();
        return 1;
    }
    printf("3/3 read back\n");
    err = read_region(after);
    deinit_concord();
    if (!err) {
        printf("OK: written; compare %s with %s\n", argv[1], after);
    }
    return err;
}
