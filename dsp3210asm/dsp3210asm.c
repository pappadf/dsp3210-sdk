/*
 * dsp3210asm.c — command-line driver for the DSP3210 reference assembler.
 *
 *   dsp3210asm [options] file.s
 *     -a addr   address of the first byte (default 0; labels, pc-relative
 *               branches and .org are all relative to this)
 *     -o file   output file (default: input basename + ".bin")
 *     -e        little-endian output words (default big-endian, as on
 *               the AV Macs; the image must be a whole number of words)
 *     -q        quiet: no summary line
 *
 * Errors print as "file:line: message" and exit nonzero.
 */

#include "dsp3210_asm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_IMAGE (16u << 20)

static void usage(void)
{
    fprintf(stderr,
        "usage: dsp3210asm [-a addr] [-o out.bin] [-e] [-q] file.s\n");
    exit(2);
}

static unsigned long arg_num(const char *s)
{
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0') {
        fprintf(stderr, "dsp3210asm: bad number '%s'\n", s);
        exit(2);
    }
    return v;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *outpath = NULL;
    uint32_t org = 0;
    int little = 0, quiet = 0;
    char outbuf[1024];
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-' || !arg[1]) {
            if (path) {
                fprintf(stderr, "dsp3210asm: unexpected argument '%s'\n", arg);
                usage();
            }
            path = arg;
            continue;
        }
        if (!strcmp(arg, "-a") && ++i < argc)
            org = (uint32_t)arg_num(argv[i]);
        else if (!strcmp(arg, "-o") && ++i < argc) outpath = argv[i];
        else if (!strcmp(arg, "-e")) little = 1;
        else if (!strcmp(arg, "-q")) quiet = 1;
        else usage();
    }
    if (!path)
        usage();

    /* read the source */
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    if (fseek(fp, 0, SEEK_END) != 0) { perror(path); return 1; }
    long fsz = ftell(fp);
    if (fsz < 0) { perror(path); return 1; }
    if (fseek(fp, 0, SEEK_SET) != 0) { perror(path); return 1; }
    char *src = malloc((size_t)fsz + 1);
    if (!src) { fprintf(stderr, "out of memory\n"); return 1; }
    if (fread(src, 1, (size_t)fsz, fp) != (size_t)fsz) {
        fprintf(stderr, "%s: read failed\n", path);
        return 1;
    }
    src[fsz] = '\0';
    fclose(fp);

    /* assemble */
    uint8_t *img = calloc(1, MAX_IMAGE);
    if (!img) { fprintf(stderr, "out of memory\n"); return 1; }
    uint32_t len = 0;
    dsp3210_asm_err err;
    if (dsp3210_assemble(src, org, img, MAX_IMAGE, &len, &err)) {
        fprintf(stderr, "%s:%d: %s\n", path, err.line, err.msg);
        return 1;
    }

    if (little) {
        uint32_t k;
        if (len & 3) {
            fprintf(stderr, "%s: image length 0x%x is not word-aligned; "
                    "cannot byte-swap for -e\n", path, len);
            return 1;
        }
        for (k = 0; k + 3 < len; k += 4) {
            uint8_t t;
            t = img[k];     img[k]     = img[k + 3]; img[k + 3] = t;
            t = img[k + 1]; img[k + 1] = img[k + 2]; img[k + 2] = t;
        }
    }

    /* pick the output name */
    if (!outpath) {
        const char *base = strrchr(path, '/');
        const char *dot;
        base = base ? base + 1 : path;
        dot = strrchr(base, '.');
        snprintf(outbuf, sizeof outbuf, "%.*s.bin",
                 (int)(dot && dot != base ? (size_t)(dot - base)
                                          : strlen(base)), base);
        outpath = outbuf;
    }

    FILE *out = fopen(outpath, "wb");
    if (!out) { perror(outpath); return 1; }
    if (fwrite(img, 1, len, out) != len) {
        fprintf(stderr, "%s: write failed\n", outpath);
        return 1;
    }
    fclose(out);

    if (!quiet)
        printf("%s: 0x%x bytes at 0x%x -> %s\n", path, len, org, outpath);
    free(src);
    free(img);
    return 0;
}
