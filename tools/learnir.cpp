/*
 * learnir: learn one IR signal from another remote with the Harmony 900's
 * IR receiver (libconcord learn_from_remote over the remote's TCP port
 * 3074, normal or safe mode). Retries in 5 s windows until a signal
 * arrives, so the key on the other remote can simply be pressed now and
 * then. Writes carrier, raw mark/space durations (us) and Logitech's
 * posting encoding to <out>, and decodes NEC frames.
 *
 *   learnir <out.txt> [attempts, default 12]
 *
 * Build:
 *   g++ -O2 -o local/bin/learnir tools/learnir.cpp -Ilocal/include \
 *       -Llocal/lib -Wl,-rpath,$PWD/local/lib -lconcord
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <libconcord.h>

static void no_progress(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                        void *, const uint32_t *)
{
}

static bool near(uint32_t v, uint32_t want)
{
    return v > want * 7 / 10 && v < want * 13 / 10;
}

/* NEC: 9000 mark, 4500 space, 32 bits of 560 mark + 560/1690 space, LSB first */
static void decode_nec(const uint32_t *s, uint32_t n)
{
    if (n < 2 + 64 || !near(s[0], 9000) || !near(s[1], 4500))
        return;
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t mark = s[2 + 2 * i], space = s[3 + 2 * i];
        if (!near(mark, 560))
            return;
        if (near(space, 1690))
            v |= 1u << i;
        else if (!near(space, 560))
            return;
    }
    unsigned a = v & 0xff, na = (v >> 8) & 0xff, c = (v >> 16) & 0xff, nc = v >> 24;
    printf("NEC: address 0x%02X%s, command 0x%02X%s\n", a,
           (a ^ na) == 0xff ? "" : (" (extended, high byte 0x" + std::to_string(na) + ")").c_str(),
           c, (c ^ nc) == 0xff ? "" : " (command check failed)");
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <out.txt> [attempts]\n", argv[0]);
        return 2;
    }
    int attempts = argc == 3 ? atoi(argv[2]) : 12;
    int err;
    if ((err = init_concord()) || (err = get_identity(no_progress, NULL))) {
        fprintf(stderr, "FAILED: connect/identity (error %d)\n", err);
        return 1;
    }
    printf("remote: arch %d, firmware %d.%d type %d\n", get_arch(),
           get_fw_ver_maj(), get_fw_ver_min(), get_fw_type());
    uint32_t carrier = 0, *sig = NULL, len = 0;
    for (int i = 1; i <= attempts; i++) {
        printf("attempt %d/%d: press the key on the other remote...\n", i, attempts);
        fflush(stdout);
        err = learn_from_remote(&carrier, &sig, &len, no_progress, NULL);
        if (!err && len >= 4)
            break;
        if (sig) {
            delete_ir_signal(sig);
            sig = NULL;
        }
    }
    if (err || !sig) {
        fprintf(stderr, "FAILED: no IR signal (error %d)\n", err);
        deinit_concord();
        return 1;
    }
    printf("carrier %u Hz, %u durations\n", carrier, len);
    decode_nec(sig, len);
    char *post = NULL;
    encode_for_posting(carrier, sig, len, &post);
    FILE *f = fopen(argv[1], "w");
    if (f) {
        fprintf(f, "carrier %u\nlength %u\nsignal", carrier, len);
        for (uint32_t i = 0; i < len; i++)
            fprintf(f, " %u", sig[i]);
        fprintf(f, "\nposting %s\n", post ? post : "");
        fclose(f);
        printf("wrote %s\n", argv[1]);
    }
    printf("first durations:");
    for (uint32_t i = 0; i < len && i < 16; i++)
        printf(" %u", sig[i]);
    printf("\n");
    if (post)
        delete_encoded_signal(post);
    delete_ir_signal(sig);
    deinit_concord();
    return 0;
}
