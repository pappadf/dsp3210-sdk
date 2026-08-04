/*
 * run_apps.c — assemble small DSP3210 programs and run them on the
 * emulator core, checking results the source file declares.
 *
 *   run_apps [-t] file.s [file.s ...]
 *     -t   trace every executed instruction through the disassembler
 *
 * A program declares its harness directives in ";;" comment lines
 * (";" makes them ordinary comments to the assembler):
 *
 *   ;; steps <n>                stop after n instructions (default 100000)
 *   ;; start <addr>             entry pc (default 0)
 *   ;; stop bkpt|waiti          expected stop reason (default bkpt)
 *   ;; irq-on-waiti <vec> <n>   whenever the core sits in waiti, assert
 *                               external interrupt <vec>, up to n times
 *   ;; reg r<N> == <val>        CAU register equals a 32-bit value
 *   ;; mem <addr> == <val>      32-bit word at addr equals value
 *   ;; memf <addr> ~= <f> tol <t>   word at addr, read as a DSP32
 *                               float, is within t of f
 *   ;; acc a<N> ~= <f> tol <t>  accumulator N is within t of f
 *
 * The program image is assembled at origin 0 and loaded at address 0
 * (flat external memory; processor mode, big-endian — the AV Mac
 * strapping).  Everything the emulator models is reachable: the
 * exception vector table via r22/evtp, waiti/ireturn, do-loops, the
 * DAU.  The apps deliberately avoid Apple code: they are original
 * programs with independently computed expectations.
 */

#include "dsp3210_asm.h"
#include "dsp3210_dis.h"
#include "dsp3210_emu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE  (1u << 20)
#define IMG_MAX   (1u << 20)
#define MAX_CHECKS 64

enum { CK_REG, CK_MEM, CK_MEMF, CK_ACC };

typedef struct {
    int      kind;
    uint32_t addr;      /* or register/accumulator number */
    uint32_t val;
    double   fval, tol;
    char     text[96];
} check;

typedef struct {
    uint64_t steps;
    uint32_t start;
    int      stop_waiti;      /* expected stop reason */
    int      irq_vec, irq_times;
    check    cks[MAX_CHECKS];
    int      ncks;
} spec;

static int trace;
static int failures;

/* ---- ";;" directive parsing ---------------------------------------- */

static uint32_t numval(const char *s)
{
    return (uint32_t)strtoul(s, NULL, 0);
}

static void parse_spec(const char *src, spec *sp, const char *path)
{
    const char *p = src;

    memset(sp, 0, sizeof *sp);
    sp->steps = 100000;
    sp->irq_vec = -1;

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        char line[160];
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char a[40], b[40], c[40], d[40];

        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = nl ? nl + 1 : NULL;

        const char *q = line;
        while (*q == ' ' || *q == '\t')
            q++;
        if (q[0] != ';' || q[1] != ';')
            continue;
        q += 2;

        if (sscanf(q, " steps %39s", a) == 1) {
            sp->steps = strtoull(a, NULL, 0);
        } else if (sscanf(q, " start %39s", a) == 1) {
            sp->start = numval(a);
        } else if (sscanf(q, " stop %39s", a) == 1) {
            sp->stop_waiti = !strcmp(a, "waiti");
        } else if (sscanf(q, " irq-on-waiti %39s %39s", a, b) == 2) {
            sp->irq_vec = (int)numval(a);
            sp->irq_times = (int)numval(b);
        } else if (sp->ncks < MAX_CHECKS
                   && sscanf(q, " reg r%39s == %39s", a, b) == 2) {
            check *k = &sp->cks[sp->ncks++];
            k->kind = CK_REG;
            k->addr = numval(a);
            k->val = numval(b);
            snprintf(k->text, sizeof k->text, "r%u == 0x%x",
                     k->addr, k->val);
        } else if (sp->ncks < MAX_CHECKS
                   && sscanf(q, " memf %39s ~= %39s tol %39s",
                             a, b, c) == 3) {
            check *k = &sp->cks[sp->ncks++];
            k->kind = CK_MEMF;
            k->addr = numval(a);
            k->fval = strtod(b, NULL);
            k->tol = strtod(c, NULL);
            snprintf(k->text, sizeof k->text, "memf 0x%x ~= %s",
                     k->addr, b);
        } else if (sp->ncks < MAX_CHECKS
                   && sscanf(q, " mem %39s == %39s", a, b) == 2) {
            check *k = &sp->cks[sp->ncks++];
            k->kind = CK_MEM;
            k->addr = numval(a);
            k->val = numval(b);
            snprintf(k->text, sizeof k->text, "mem 0x%x == 0x%x",
                     k->addr, k->val);
        } else if (sp->ncks < MAX_CHECKS
                   && sscanf(q, " acc a%39s ~= %39s tol %39s",
                             a, b, c) == 3) {
            check *k = &sp->cks[sp->ncks++];
            k->kind = CK_ACC;
            k->addr = numval(a);
            k->fval = strtod(b, NULL);
            k->tol = strtod(c, NULL);
            snprintf(k->text, sizeof k->text, "a%u ~= %s", k->addr, b);
        } else if (sscanf(q, " %39s", d) == 1) {
            fprintf(stderr, "%s: unrecognised directive: ;;%s\n",
                    path, q);
            failures++;
        }
    }
}

/* ---- one program ---------------------------------------------------- */

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    char *buf;
    if (!fp) { perror(path); return NULL; }
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fprintf(stderr, "%s: read failed\n", path);
        fclose(fp);
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(fp);
    return buf;
}

static int run_one(const char *path)
{
    char *src = read_file(path);
    static uint8_t img[IMG_MAX];
    static dsp3210_emu s;
    static uint8_t mem[MEM_SIZE];
    dsp3210_asm_err err;
    uint32_t len = 0;
    spec sp;
    int bad = 0, i;
    int st = DSP3210_STEP_OK;
    uint64_t guard;

    if (!src)
        return 1;
    parse_spec(src, &sp, path);

    if (dsp3210_assemble(src, 0, img, sizeof img, &len, &err)) {
        printf("FAIL %s: line %d: %s\n", path, err.line, err.msg);
        free(src);
        return 1;
    }
    free(src);

    memset(mem, 0, sizeof mem);
    dsp3210_init(&s, mem, sizeof mem);
    dsp3210_reset(&s, 0);                     /* processor mode, BE */
    if (dsp3210_load(&s, 0, img, len)) {
        printf("FAIL %s: image does not fit\n", path);
        return 1;
    }
    s.pc = sp.start;
    s.npc = sp.start + 4;

    for (guard = 0; s.icount < sp.steps && guard < 4 * sp.steps + 64;
         guard++) {
        if (trace) {
            uint32_t w;
            if (dsp3210_peek(&s, s.pc, 4, &w) == 0) {
                dsp3210_insn ins;
                dsp3210_disassemble(w, s.pc, &ins);
                printf("%08x: %08x  %s\n", s.pc, (unsigned)w, ins.text);
            }
        }
        st = dsp3210_step(&s);
        if (st == DSP3210_STEP_OK)
            continue;
        if (st == DSP3210_STEP_WAITI && sp.irq_times > 0) {
            dsp3210_request_interrupt(&s, sp.irq_vec);
            sp.irq_times--;
            continue;
        }
        break;
    }

    /* stop reason */
    {
        int want = sp.stop_waiti ? DSP3210_STEP_WAITI : DSP3210_STEP_BKPT;
        if (st != want) {
            printf("FAIL %s: stopped on %s after %llu insns "
                   "(pc=%08x), expected %s\n", path,
                   dsp3210_step_name(st),
                   (unsigned long long)s.icount, s.pc,
                   dsp3210_step_name(want));
            bad++;
        }
    }

    for (i = 0; i < sp.ncks; i++) {
        check *k = &sp.cks[i];
        switch (k->kind) {
        case CK_REG:
            if (k->addr > 22 || s.r[k->addr] != k->val) {
                printf("FAIL %s: %s, got 0x%x\n", path, k->text,
                       k->addr <= 22 ? s.r[k->addr] : 0);
                bad++;
            }
            break;
        case CK_MEM: {
            uint32_t v = 0;
            if (dsp3210_peek(&s, k->addr, 4, &v) || v != k->val) {
                printf("FAIL %s: %s, got 0x%x\n", path, k->text, v);
                bad++;
            }
            break;
        }
        case CK_MEMF: {
            uint32_t v = 0;
            double f;
            if (dsp3210_peek(&s, k->addr, 4, &v)) {
                printf("FAIL %s: %s, unreadable\n", path, k->text);
                bad++;
                break;
            }
            f = dsp3210_dsp32_to_double(v);
            if (fabs(f - k->fval) > k->tol) {
                printf("FAIL %s: %s, got %.9g (0x%08x)\n", path,
                       k->text, f, v);
                bad++;
            }
            break;
        }
        case CK_ACC: {
            double f = dsp3210_acc_get(&s, (int)(k->addr & 3));
            if (fabs(f - k->fval) > k->tol) {
                printf("FAIL %s: %s, got %.9g\n", path, k->text, f);
                bad++;
            }
            break;
        }
        }
    }

    if (!bad)
        printf("ok   %s (%llu insns, %d checks)\n", path,
               (unsigned long long)s.icount, sp.ncks + 1);
    return bad;
}

int main(int argc, char **argv)
{
    int i, nrun = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) {
            trace = 1;
            continue;
        }
        failures += run_one(argv[i]);
        nrun++;
    }
    if (!nrun) {
        fprintf(stderr, "usage: run_apps [-t] file.s ...\n");
        return 2;
    }
    printf("%d program%s, %d failure%s\n", nrun, nrun == 1 ? "" : "s",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
