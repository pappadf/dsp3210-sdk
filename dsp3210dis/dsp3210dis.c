/*
 * dsp3210dis — command-line front end for the DSP3210 reference
 * disassembler (dsp3210_dis.c).
 *
 * usage: dsp3210dis [options] file
 *   -a addr    load address of the first disassembled word (default 0);
 *              used for pc-relative branch targets
 *   -o offset  byte offset into the file to start at (default 0)
 *   -n bytes   number of bytes to disassemble (default: to end of file)
 *   -e         words are little-endian (default big-endian, as in the
 *              Macintosh Quadra 840AV/660AV images)
 *   -q         quiet: suppress the address/word columns
 * Numbers accept 0x prefixes.  Use "-" as the file to read stdin.
 */

#include "dsp3210_dis.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fputs("usage: dsp3210dis [-a addr] [-o offset] [-n bytes] [-e] [-q] file\n"
          "  -a addr    load address of first word (default 0)\n"
          "  -o offset  byte offset into file (default 0)\n"
          "  -n bytes   number of bytes to disassemble (default: rest)\n"
          "  -e         little-endian words (default big-endian)\n"
          "  -q         omit address/word columns\n", stderr);
    exit(2);
}

static unsigned long arg_num(const char *s)
{
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0') {
        fprintf(stderr, "dsp3210dis: bad number '%s'\n", s);
        exit(2);
    }
    return v;
}

int main(int argc, char **argv)
{
    unsigned long addr = 0, offset = 0, length = 0;
    int have_length = 0, little = 0, quiet = 0;
    const char *path = NULL;
    FILE *fp;
    unsigned char *data;
    size_t size, cap, i;
    int arg;

    for (arg = 1; arg < argc; arg++) {
        const char *a = argv[arg];
        if (a[0] == '-' && a[1] != '\0') {
            if (!strcmp(a, "-a") && arg + 1 < argc)
                addr = arg_num(argv[++arg]);
            else if (!strcmp(a, "-o") && arg + 1 < argc)
                offset = arg_num(argv[++arg]);
            else if (!strcmp(a, "-n") && arg + 1 < argc) {
                length = arg_num(argv[++arg]);
                have_length = 1;
            } else if (!strcmp(a, "-e"))
                little = 1;
            else if (!strcmp(a, "-q"))
                quiet = 1;
            else
                usage();
        } else if (!path) {
            path = a;
        } else {
            usage();
        }
    }
    if (!path)
        usage();

    if (!strcmp(path, "-")) {
        fp = stdin;
    } else {
        fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "dsp3210dis: %s: %s\n", path, strerror(errno));
            return 1;
        }
    }

    /* slurp the file */
    cap = 1 << 16;
    size = 0;
    data = (unsigned char *)malloc(cap);
    if (!data) {
        fputs("dsp3210dis: out of memory\n", stderr);
        return 1;
    }
    for (;;) {
        size_t n;
        if (size == cap) {
            cap *= 2;
            data = (unsigned char *)realloc(data, cap);
            if (!data) {
                fputs("dsp3210dis: out of memory\n", stderr);
                return 1;
            }
        }
        n = fread(data + size, 1, cap - size, fp);
        size += n;
        if (n == 0) {
            if (ferror(fp)) {
                fprintf(stderr, "dsp3210dis: %s: read error\n", path);
                return 1;
            }
            break;
        }
    }
    if (fp != stdin)
        fclose(fp);

    if (offset > size)
        offset = size;
    if (!have_length || length > size - offset)
        length = size - offset;
    length &= ~3ul;    /* whole words only */

    for (i = 0; i < length; i += 4) {
        uint32_t w = little ? dsp3210_read_le32(data + offset + i)
                            : dsp3210_read_be32(data + offset + i);
        dsp3210_insn ins;

        dsp3210_disassemble(w, (uint32_t)(addr + i), &ins);
        if (quiet)
            printf("%s", ins.text);
        else
            printf("%08lx: %08lx  %s", (unsigned long)(addr + i),
                   (unsigned long)w, ins.text);
        if (ins.has_target)
            printf(" ; -> 0x%08lx", (unsigned long)ins.target);
        putchar('\n');
    }

    free(data);
    return 0;
}
