/*
 * dsp3210emu.c — command-line driver for the DSP3210 reference emulator.
 *
 *   dsp3210emu [options] file.bin
 *     -a addr    load address of the binary (default 0)
 *     -s addr    start pc (default: the load address)
 *     -o offset  byte offset into the file (default 0)
 *     -l bytes   how many bytes of the file to load (default: to end)
 *     -m size    flat memory size in bytes (default 16 MB)
 *     -e         little-endian memory (default big-endian, as on the AV
 *                Macs; this clears pcw[8])
 *     -c         computer mode (pcw[10]=1; on-chip window at $0000xxxx).
 *                Default is processor mode, as strapped on the AV Macs.
 *     -n count   stop after this many instructions (default 100000000)
 *     -t         trace: disassemble each instruction as it executes
 *     -d addr,len  hex-dump a memory range after the run (repeatable)
 *     -q         quiet: no register dump at exit
 */

#include "dsp3210_emu.h"
#include "dsp3210_dis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
        "usage: dsp3210emu [-a addr] [-s addr] [-o offset] [-l bytes]\n"
        "                  [-m size] [-e] [-c] [-n count] [-t]\n"
        "                  [-d addr,len] [-q] file.bin\n");
    exit(2);
}

static unsigned long num_ul(const char *p)
{
    char *end;
    unsigned long v = strtoul(p, &end, 0);
    if (end == p || *end != '\0') {
        fprintf(stderr, "dsp3210emu: bad number '%s'\n", p);
        exit(2);
    }
    return v;
}

static uint32_t num(const char *p)
{
    return (uint32_t)num_ul(p);
}

static void dump_regs(dsp3210_emu *s)
{
    static const char *const lvl[] = { "base", "interrupt", "error",
                                       "double-error" };
    int i;
    for (i = 0; i < 23; i++)
        printf("r%-2d %08x%s", i, s->r[i], (i % 6 == 5) ? "\n" : "  ");
    printf("\n");
    printf("pc  %08x  pcsh %08x  ps %04x  emr %04x  pcw %04x  "
           "dauc %02x  ctr %02x\n",
           s->pc, s->pcsh, s->ps, s->emr, s->pcw, s->dauc, s->ctr);
    for (i = 0; i < 4; i++) {
        /* pack the 40-bit fields to a 32-bit word (guard truncated) so
         * the same line prints faithfully on either DAU */
        int64_t mg = 0;
        int e = 0;
        dsp3210_acc_raw(s, i, &mg, &e);
        printf("a%d  %.9g (0x%08x)%s", i, dsp3210_acc_get(s, i),
               (uint32_t)((((uint64_t)mg >> 8) & 0xFFFFFFu) << 8)
                   | ((uint32_t)e & 0xFFu),
               i == 3 ? "\n" : "  ");
    }
    printf("level %s  insns %llu\n", lvl[s->level & 3],
           (unsigned long long)s->icount);
}

static void hexdump(dsp3210_emu *s, uint32_t addr, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint8_t b = 0;
        uint32_t v;
        if (dsp3210_peek(s, addr + i, 1, &v) == 0)
            b = (uint8_t)v;
        if (i % 16 == 0)
            printf("%08x:", addr + i);
        printf(" %02x", b);
        if (i % 16 == 15 || i == len - 1)
            printf("\n");
    }
}

int main(int argc, char **argv)
{
    uint32_t load_addr = 0, start = 0, file_off = 0, file_len = 0;
    uint32_t mem_size = 16u << 20;
    uint64_t max_insns = 100000000ull;
    int little = 0, computer = 0, trace = 0, quiet = 0;
    int have_start = 0, have_len = 0;
    struct { uint32_t addr, len; } dumps[16];
    int ndumps = 0;
    const char *path = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-' || !arg[1]) {
            if (path) {
                fprintf(stderr, "dsp3210emu: unexpected argument '%s'\n", arg);
                usage();
            }
            path = arg;
            continue;
        }
        if (!strcmp(arg, "-a") && ++i < argc) load_addr = num(argv[i]);
        else if (!strcmp(arg, "-s") && ++i < argc) {
            start = num(argv[i]); have_start = 1;
        } else if (!strcmp(arg, "-o") && ++i < argc) file_off = num(argv[i]);
        else if (!strcmp(arg, "-l") && ++i < argc) {
            file_len = num(argv[i]); have_len = 1;
        } else if (!strcmp(arg, "-m") && ++i < argc) mem_size = num(argv[i]);
        else if (!strcmp(arg, "-n") && ++i < argc) {
            char *end;
            max_insns = strtoull(argv[i], &end, 0);
            if (end == argv[i] || *end != '\0') {
                fprintf(stderr, "dsp3210emu: bad number '%s'\n", argv[i]);
                exit(2);
            }
        } else if (!strcmp(arg, "-e")) little = 1;
        else if (!strcmp(arg, "-c")) computer = 1;
        else if (!strcmp(arg, "-t")) trace = 1;
        else if (!strcmp(arg, "-q")) quiet = 1;
        else if (!strcmp(arg, "-d") && ++i < argc) {
            char *comma;
            if (ndumps >= (int)(sizeof dumps / sizeof dumps[0])) {
                fprintf(stderr, "dsp3210emu: at most %d -d ranges\n",
                        (int)(sizeof dumps / sizeof dumps[0]));
                return 1;
            }
            dumps[ndumps].addr = (uint32_t)strtoul(argv[i], &comma, 0);
            if (comma == argv[i] || (*comma != '\0' && *comma != ',')) {
                fprintf(stderr, "dsp3210emu: bad -d range '%s'\n", argv[i]);
                exit(2);
            }
            dumps[ndumps].len = (*comma == ',') ? num(comma + 1) : 64;
            ndumps++;
        } else {
            usage();
        }
    }
    if (!path)
        usage();
    if (mem_size == 0) {
        fprintf(stderr, "dsp3210emu: memory size must be nonzero\n");
        return 1;
    }

    /* read the binary */
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    if (fseek(fp, 0, SEEK_END) != 0) { perror(path); return 1; }
    long fsz = ftell(fp);
    if (fsz < 0) { perror(path); return 1; }
    /* Clamp before subtracting: an -o past the end would otherwise wrap
     * file_len around to nearly 4 GB. */
    if (file_off > (uint32_t)fsz) {
        fprintf(stderr, "%s: offset 0x%x is past the end of the file "
                "(0x%lx bytes)\n", path, file_off, (unsigned long)fsz);
        return 1;
    }
    if (!have_len || file_len > (uint32_t)fsz - file_off)
        file_len = (uint32_t)fsz - file_off;
    uint8_t *img = malloc(file_len ? file_len : 1);
    if (!img) { fprintf(stderr, "out of memory\n"); return 1; }
    if (fseek(fp, (long)file_off, SEEK_SET) != 0) { perror(path); return 1; }
    if (fread(img, 1, file_len, fp) != file_len) {
        fprintf(stderr, "%s: short read\n", path);
        return 1;
    }
    fclose(fp);

    /* set up the core */
    static dsp3210_emu s;
    uint8_t *mem = calloc(1, mem_size);
    if (!mem) { fprintf(stderr, "out of memory\n"); return 1; }
    dsp3210_init(&s, mem, mem_size);
    dsp3210_reset(&s, computer ? 1u : 0u);  /* strap bit 0 → pcw[10] */
    if (little)
        s.pcw &= (uint16_t)~(1u << 8);

    if (dsp3210_load(&s, load_addr, img, file_len)) {
        fprintf(stderr, "load address 0x%x+0x%x is outside memory\n",
                load_addr, file_len);
        return 1;
    }
    free(img);

    if (!have_start)
        start = load_addr;
    s.pc = start;
    s.npc = start + 4;

    /* run */
    int st = DSP3210_STEP_OK;
    while (s.icount < max_insns) {
        if (trace) {
            uint32_t w;
            if (dsp3210_peek(&s, s.pc, 4, &w) == 0) {
                dsp3210_insn ins;
                dsp3210_disassemble(w, s.pc, &ins);
                printf("%08x: %08x  %s\n", s.pc, w, ins.text);
            }
        }
        st = dsp3210_step(&s);
        if (st != DSP3210_STEP_OK)
            break;
    }

    if (!quiet) {
        printf("stopped: %s after %llu instructions (pc=%08x",
               st == DSP3210_STEP_OK ? "instruction limit"
                                     : dsp3210_step_name(st),
               (unsigned long long)s.icount, s.pc);
        if (s.last_vector >= 0)
            printf(", last exception vector %d", s.last_vector);
        printf(")\n");
        dump_regs(&s);
    }
    for (i = 0; i < ndumps; i++)
        hexdump(&s, dumps[i].addr, dumps[i].len);

    return st == DSP3210_STEP_DERROR ? 3 : 0;
}
