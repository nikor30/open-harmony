/*
 * zregion: read one region of a Harmony 900 (usbnet / "Z" protocol) to a
 * file. Read-only.
 *
 *   zregion <region> <out.bin>
 *
 * Regions (libconcord remote.h): 0 bootloader, 1 safe mode, 2 application,
 * 3 embedding config, 4 user config, 5 PIC, 7 Z-Wave, 8 blaster config,
 * 9 state variables, 10 logging.
 *
 * Needs libconcord built with patches/libconcord-dump-size-env.patch
 * (lc_z_read_region, which refuses regions larger than the buffer).
 *
 * Build:
 *   g++ -O2 -o local/bin/zregion tools/zregion.cpp -Ilocal/include \
 *       -Llocal/lib -Wl,-rpath,$PWD/local/lib -lconcord
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <libconcord.h>

extern "C" int lc_z_read_region(uint8_t region, uint8_t *buf, uint32_t buf_len,
                                uint32_t *len);

static const uint32_t MAX_REGION = 4 * 1024 * 1024; /* the 900 has 2 MiB flash */

/* get_identity calls its callback unconditionally */
static void no_progress(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                        void *, const uint32_t *)
{
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <region 0-15> <out.bin>\n", argv[0]);
        return 2;
    }
    int region = atoi(argv[1]);
    if (region < 0 || region > 15) {
        fprintf(stderr, "region must be 0-15\n");
        return 2;
    }

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

    std::vector<uint8_t> buf(MAX_REGION);
    uint32_t len = 0;
    err = lc_z_read_region(region, buf.data(), buf.size(), &len);
    printf("region %d: announced %u bytes, %s (error %d)\n", region, len,
           err ? "FAILED" : "read", err);
    if (!err) {
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(buf.data(), 1, len, f) != len || fclose(f)) {
            perror(argv[2]);
            err = 1;
        } else {
            printf("wrote %s\n", argv[2]);
        }
    }
    deinit_concord();
    return err ? 1 : 0;
}
