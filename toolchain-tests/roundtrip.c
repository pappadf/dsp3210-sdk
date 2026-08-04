/*
 * roundtrip.c — cross-validation of the DSP3210 assembler and
 * disassembler, three ways:
 *
 *   1. CORPUS: ~150 statements, one per syntax form, written in the
 *      assembler's canonical spelling.  Each must survive
 *      text -> word -> text unchanged, and re-assembling the
 *      disassembly must reproduce the word bit for bit.
 *
 *   2. SWEEP/FUZZ: millions of instruction words (a per-opcode
 *      structured sweep plus a deterministic xorshift stream).  For
 *      every word the disassembler accepts cleanly (status OK and no
 *      '?' placeholder), the assembler must accept the text, and:
 *        - the re-assembled word w2 must disassemble to the *same
 *          text* (the architecture has aliases — e.g. two "no Z write"
 *          spellings, 6d subtract vs format-5 add — so w2 may differ
 *          from w, but never in meaning or spelling);
 *        - w2 must be a fixed point: assembling its disassembly must
 *          give w2 again (the assembler is a canonicaliser);
 *        - branch class and resolved targets must be preserved.
 *
 *   3. PROGRAMS: each app given on the command line is assembled, its
 *      image is disassembled into a complete source (data words that do
 *      not decode cleanly become ".word 0x…"), that source is
 *      re-assembled, and the two images' disassemblies must match line
 *      for line; words the assembler itself emitted must match bit for
 *      bit.
 *
 * Usage: roundtrip [apps.s ...]
 */

#include "dsp3210_asm.h"
#include "dsp3210_dis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static unsigned long checks;

#define ADDR 0x1000u   /* fixed instruction address for parts 1 and 2 */

/* ------------------------------------------------------------------ */
/* part 1: canonical corpus                                            */

static const char *const corpus[] = {
    /* fixed words */
    "nop", "ireturn", "waiti", "bkpt", "sftrst",
    /* goto (0b/1b, 8a) */
    "goto r1", "goto r18+0x40", "goto r5-0x40", "goto pc",
    "goto pc+0x10", "goto pc-0x24", "goto 0x1234", "goto 0xffffff80",
    "goto 0x123456", "goto pc+0x20000", "goto r2+0x10000",
    "goto pcsh+0x4", "goto -n+0x8",
    "if (false) goto r3+0x4", "if (pl) goto r1+0x800",
    "if (eq) goto pc+0x10", "if (ne) goto 0x40", "if (mi) goto r2",
    "if (vc) goto pc-0x8", "if (vs) goto r1", "if (cc) goto r1",
    "if (cs) goto r1", "if (ge) goto r1", "if (lt) goto r1",
    "if (gt) goto r1", "if (le) goto r1", "if (hi) goto r1",
    "if (ls) goto r1", "if (auc) goto r1", "if (aus) goto r1",
    "if (age) goto r1", "if (alt) goto r1", "if (ane) goto r1",
    "if (aeq) goto r1", "if (avc) goto r1", "if (avs) goto r1",
    "if (agt) goto r1", "if (ale) goto r1", "if (ibe) goto r1",
    "if (ibf) goto r1", "if (obf) goto r1", "if (obe) goto r1",
    "if (syc) goto r1", "if (sys) goto r1", "if (fbc) goto r1",
    "if (fbs) goto r1", "if (ir0c) goto r1", "if (ir0s) goto r1",
    "if (ir1c) goto r1", "if (ir1s) goto r1",
    /* loop branch (3a) */
    "if (r1-- >= 0) goto r2-0xab", "if (r19-- >= 0) goto pc-0x14",
    "if (r10-- >= 0) goto 0x100", "if (r21-- >= 0) goto pc",
    /* call (4a, 8c) */
    "call pc+0x111 (r20)", "call 0x40 (r1)", "call r6+0xc (r18)",
    "call 0x123456 (r20)", "call pc (r17)", "call r6 (r15)",
    /* do (3b/3c) */
    "do 4, r20", "do 2, 100", "do 0, 0", "do 127, 2047",
    "dolock 1, 10", "dolock 2, r5", "doblock r3", "doblock 100",
    "doblock 3, 100",
    /* shift-or (4b) */
    "r22 = r0 <<| 0x5003", "r2 = r2 <<| 0x5003", "pc = r1 <<| 0xffff",
    /* format 5 */
    "r11 = (short) r1 + 0x20", "r11 = r1 + 0x20", "r11 = r1 - 0x20",
    "r2 = (short) 0x800", "r2 = 0x800", "r2 = -0x800",
    "r5 = pc - 0x24", "r1 = r22 + 0x4",
    "r3 = r3 + -0x5", "r3 = r3 - -0x5", "r3 = r3 - 0x0",
    /* ALU register forms (6a/6b) */
    "r2 = r14 # r4", "if (cc) r1 = (short) r1 & r2",
    "r5 = (short) r4 &~ r3", "if (ir0c) r2 = r3 ^ r4",
    "r4 = r2 $>> r13", "r15 = r16 | r17", "r18 = r19 & r20",
    "if (mi) r10 = -r5", "r4 = r9 - r4", "r3 = r7", "r3 = r7 * 2",
    "sp = sp++", "sp = sp--", "if (eq) sp = (short) sp++",
    "r4 = r9 - 1", "r4 = r9 + 1", "if (vs) r5 = (short) r5 + 1",
    "if (ane) r3 = (short) r2 << 1", "r3 = r2 >> 1", "r3 = r2 $>> 1",
    "r3 = r2 << r4", "r3 = r2 >> r4",
    "if (cs) r5 = r6 <<<1", "r5 = r6 >>>1",
    "r5 & r13", "(short) r5 & r13", "r1 - r2", "if (hi) r1 - r2",
    "r2 = pc", "r1 = +n", "r1 - -n",
    /* ALU immediate forms (6c/6d) */
    "r1 - 0xf", "r6 & 0x80", "(short) r6 & 0x80",
    "r15 = r15 | 0x55", "r2 = r2 >> 30", "r2 = r2 << 5",
    "r2 = r2 $>> 31", "r2 = 0x5 - r2", "r3 = (short) r3 ^ -0x1",
    "r3 = r3 & 0x7fff", "r3 = r3 &~ 0x80", "r3 = r3 # 0x1234",
    "r3 = r3 << 1",
    /* moves (7a-7d) */
    "r10 = (ushort) *0xaa", "r1 = *0xaa", "r1 = (byte) *0x8000",
    "*0x1234 = (short) r5", "*0xfffc = r22",
    "r4 = *r2++", "r4 = *r2--", "r4 = *r2", "r4 = *r2++r16",
    "r4 = (char) *r2++", "r4 = (hbyte) *r2", "r22 = *r5++",
    "*r3++ = (byte) r4", "*r0 = r0", "*r1++ = (hbyte) r3",
    "*r2++r19 = r14", "pcsh = *r0", "*r6-- = pc",
    "emr = (short) *r2", "*r1 = (short) ps", "pcw = *r5++",
    "*r2++ = dauc", "ctr = (byte) *r3",
    "pcw = (short) r1", "r7 = dauc", "r1 = (byte) dauc",
    "emr = (short) r0", "ps = r4", "r9 = spc", "spc = (char) r1",
    /* 24-bit load (8b) */
    "r2 = (ushort24) 0x80000", "r15 = (ushort24) 0x1234",
    "pcsh = (ushort24) 0xffffff",
    /* DA multiply/accumulate, all formats and signs */
    "a0 = *r1", "a3 = -*r2++", "a1 = a2", "a2 = -a2",
    "a1 = *r5-- - *r7++", "a1 = *r5 + *r7", "a0 = a1 + a2",
    "a1 = -*r5 + *r7", "a0 = *r1 + 0.0 * *r2", "a0 = *r1 - 0.0 * a2",
    "a2 = *r14++ - a0 * *r10--", "a1 = *r3 + a3 * a0",
    "a0 = a0 + a0 * a0", "a0 = -a1 + a2 * a3",
    "a3 = *r12++ * *r11--", "a0 = -*r1++ * a2", "a2 = a3 * a1",
    "a2 = a1 + *r2 * *r3", "a1 = -a2 - *r4++r18 * a0",
    "a1 = 1.0 - *r2 * *r3", "a1 = -1.0 + a0 * a1",
    "a1 = -0.0 - *r2 * *r3",
    "a1 = (*r13 = a3) - *r1--", "a0 = (*r3++ = *r1++) + a0",
    "a0 = -(*r2++r16 = *r1) * *r8--",
    "a0 = -a1 + (*r2-- = *r3++r17) * *r1++",
    "a2 = 1.0 + (*r6 = a1) * a0", "a3 = -0.0 + (*r7++ = *r8) * *r9",
    "*r1 = a2 = *r2--", "*r1++ = a0 = a1 - *r2-- * *r3++r17",
    "*r2++r16 = a2 = *r14++ - a0 * *r10--",
    "*r2++r17 = a3 = *r12++ * *r11--", "*r4 = a1 = a1 + a2",
    "*r5++r15 = a0 = -a3", "a2 = *r2 + a0",
    /* DA special functions */
    "a0 = ic(*r2++)", "a1 = oc(a1)", "a3 = float16(*r2)",
    "a3 = float32(*r2)", "a0 = int16(a0)", "a2 = int32(*r1++)",
    "a3 = round(*r1--)", "a2 = ifalt(*r7--)", "a0 = ifaeq(*r7)",
    "a1 = ifagt(*r9++r18)", "a1 = ieee(*r9++)", "a1 = dsp(*r9++)",
    "a3 = seed(*r9++)", "*r1++ = a0 = ic(*r2++)",
    "*r12++r16 = a3 = round(*r1--)", "*r13 = a1 = ifagt(*r9++r18)",
};

static void test_corpus(void)
{
    size_t i;
    for (i = 0; i < sizeof corpus / sizeof corpus[0]; i++) {
        const char *text = corpus[i];
        dsp3210_asm_err err;
        dsp3210_insn ins;
        uint32_t w = 0, w2 = 0;

        checks++;
        if (dsp3210_encode(text, ADDR, &w, &err)) {
            printf("FAIL corpus \"%s\": %s\n", text, err.msg);
            failures++;
            continue;
        }
        dsp3210_disassemble(w, ADDR, &ins);
        if (ins.status != DSP3210_OK || strcmp(ins.text, text) != 0) {
            printf("FAIL corpus \"%s\": %08x reads back \"%s\" (%s)\n",
                   text, (unsigned)w, ins.text,
                   dsp3210_status_name(ins.status));
            failures++;
            continue;
        }
        if (dsp3210_encode(ins.text, ADDR, &w2, &err) || w2 != w) {
            printf("FAIL corpus \"%s\": %08x -> \"%s\" -> %08x\n",
                   text, (unsigned)w, ins.text, (unsigned)w2);
            failures++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* part 2: sweep + fuzz                                                */

static unsigned long fuzz_clean, fuzz_alias;

static void fuzz_word(uint32_t w)
{
    dsp3210_insn i1, i2, i3;
    dsp3210_asm_err err;
    uint32_t w2 = 0, w3 = 0;

    dsp3210_disassemble(w, ADDR, &i1);
    if (i1.status != DSP3210_OK || strchr(i1.text, '?'))
        return;                       /* not claimed round-trippable */
    checks++;
    fuzz_clean++;

    if (dsp3210_encode(i1.text, ADDR, &w2, &err)) {
        printf("FAIL fuzz %08x \"%s\": %s\n", (unsigned)w, i1.text,
               err.msg);
        failures++;
        return;
    }
    dsp3210_disassemble(w2, ADDR, &i2);
    if (i2.status != DSP3210_OK || strcmp(i2.text, i1.text) != 0) {
        printf("FAIL fuzz %08x \"%s\" -> %08x \"%s\"\n", (unsigned)w,
               i1.text, (unsigned)w2, i2.text);
        failures++;
        return;
    }
    /* class and resolved targets must survive */
    if (i1.klass != i2.klass || i1.is_branch != i2.is_branch
        || i1.no_delay_slot != i2.no_delay_slot
        || i1.has_target != i2.has_target
        || (i1.has_target && i1.target != i2.target)) {
        printf("FAIL fuzz %08x -> %08x \"%s\": semantics changed\n",
               (unsigned)w, (unsigned)w2, i1.text);
        failures++;
        return;
    }
    if (w2 != w) {
        /* alias collapsed to canonical form: must be a fixed point */
        fuzz_alias++;
        if (dsp3210_encode(i2.text, ADDR, &w3, &err) || w3 != w2) {
            dsp3210_disassemble(w3, ADDR, &i3);
            printf("FAIL fuzz %08x: canonical %08x not a fixed point "
                   "(-> %08x)\n", (unsigned)w, (unsigned)w2,
                   (unsigned)w3);
            failures++;
        }
    }
}

static uint32_t xs_state = 0x3210DA1Du;

static uint32_t xorshift32(void)
{
    uint32_t x = xs_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xs_state = x;
    return x;
}

static void test_fuzz(void)
{
    unsigned op;
    unsigned long n;

    /* structured sweep: every opcode with an exhaustive low halfword
     * (catches every operand-field decode in the bottom 16 bits) */
    for (op = 0; op < 64; op++) {
        uint32_t lo;
        for (lo = 0; lo <= 0xFFFF; lo++) {
            fuzz_word((op << 26) | lo);
            fuzz_word((op << 26) | 0x02970000u | lo);  /* dirty mid bits */
        }
    }
    /* structured sweep: every opcode with an exhaustive mid halfword */
    for (op = 0; op < 64; op++) {
        uint32_t mid;
        for (mid = 0; mid <= 0xFFFF; mid++)
            fuzz_word((op << 26) | (mid << 10) | 0x217u);
    }
    /* random words */
    for (n = 0; n < 4000000ul; n++)
        fuzz_word(xorshift32());
}

/* ------------------------------------------------------------------ */
/* part 3: program-level roundtrip                                     */

#define IMG_MAX (1u << 20)

/* dump one word as roundtrip-safe source: instruction text when the
 * decode is clean, ".word 0x…" otherwise */
static void dump_line(char *buf, size_t cap, uint32_t w, uint32_t addr)
{
    dsp3210_insn ins;
    dsp3210_disassemble(w, addr, &ins);
    if (ins.status == DSP3210_OK && !strchr(ins.text, '?'))
        snprintf(buf, cap, "%s", ins.text);
    else
        snprintf(buf, cap, ".word 0x%08x", (unsigned)w);
}

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

static void test_program(const char *path)
{
    char *src = read_file(path);
    static uint8_t img1[IMG_MAX], img2[IMG_MAX];
    char *dump;
    dsp3210_asm_err err;
    uint32_t len1 = 0, len2 = 0, addr;
    size_t pos;

    checks++;
    if (!src) { failures++; return; }
    if (dsp3210_assemble(src, 0, img1, sizeof img1, &len1, &err)) {
        printf("FAIL %s: line %d: %s\n", path, err.line, err.msg);
        failures++;
        free(src);
        return;
    }
    free(src);
    len1 = (len1 + 3u) & ~3u;

    /* disassemble the whole image into a source text */
    dump = malloc((size_t)(len1 / 4 + 2) * 160);
    if (!dump) { printf("FAIL %s: oom\n", path); failures++; return; }
    pos = 0;
    for (addr = 0; addr < len1; addr += 4) {
        char line[144];
        dump_line(line, sizeof line, dsp3210_read_be32(img1 + addr), addr);
        pos += (size_t)sprintf(dump + pos, "%s\n", line);
    }

    /* re-assemble the dump and compare */
    if (dsp3210_assemble(dump, 0, img2, sizeof img2, &len2, &err)) {
        printf("FAIL %s: reassembly: line %d: %s\n", path, err.line,
               err.msg);
        failures++;
        free(dump);
        return;
    }
    if (len2 != len1) {
        printf("FAIL %s: reassembly length 0x%x != 0x%x\n", path,
               (unsigned)len2, (unsigned)len1);
        failures++;
        free(dump);
        return;
    }
    for (addr = 0; addr < len1; addr += 4) {
        uint32_t w1 = dsp3210_read_be32(img1 + addr);
        uint32_t w2 = dsp3210_read_be32(img2 + addr);
        char l1[144], l2[144];
        dump_line(l1, sizeof l1, w1, addr);
        dump_line(l2, sizeof l2, w2, addr);
        if (strcmp(l1, l2) != 0) {
            printf("FAIL %s @0x%x: \"%s\" reassembled as \"%s\"\n",
                   path, addr, l1, l2);
            failures++;
            break;
        }
        /* canonical words (all the assembler ever emits for
         * instructions) must survive bit for bit */
        if (w1 != w2) {
            dsp3210_asm_err e2;
            uint32_t wc = 0;
            if (dsp3210_encode(l1, addr, &wc, &e2) == DSP3210_ASM_OK
                && wc == w1) {
                printf("FAIL %s @0x%x: canonical %08x reassembled as "
                       "%08x\n", path, addr, (unsigned)w1, (unsigned)w2);
                failures++;
                break;
            }
        }
    }
    free(dump);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int i;

    test_corpus();
    printf("corpus: %lu statements round-tripped\n", checks);

    test_fuzz();
    printf("fuzz: %lu clean decodes re-assembled (%lu aliases "
           "canonicalised)\n", fuzz_clean, fuzz_alias);

    for (i = 1; i < argc; i++)
        test_program(argv[i]);
    if (argc > 1)
        printf("programs: %d image(s) round-tripped\n", argc - 1);

    printf("%lu checks, %d failure%s\n", checks, failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
