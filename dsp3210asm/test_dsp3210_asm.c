/*
 * test_dsp3210_asm.c — unit tests for the DSP3210 reference assembler.
 *
 * Three kinds of vectors:
 *   1. EXPECT_WORD(text, word): the text must assemble to exactly that
 *      word, and the word must disassemble back to exactly that text.
 *      Used wherever the assembler's canonical encoding coincides with
 *      an independently known word (manual examples, ROM-verified words
 *      recovered from real DSP3210 systems, Apple's assembler output).
 *   2. EXPECT_RT(text): the text assembles (to some encoding) and the
 *      disassembly of the result is the identical text.  Used where the
 *      architecture has several encodings for one spelling and the
 *      canonical choice differs from a historical vector.
 *   3. EXPECT_ERR(text): the statement must be rejected.
 *
 * The full-program tests at the bottom exercise labels, .org/.word/
 * .float/.space/.align/.equ and the two-pass machinery.
 */

#include "dsp3210_asm.h"
#include "dsp3210_dis.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

#define ADDR 0x1000u

static void expect_word(const char *text, uint32_t want)
{
    dsp3210_asm_err err;
    dsp3210_insn ins;
    uint32_t w = 0;

    checks++;
    if (dsp3210_encode(text, ADDR, &w, &err)) {
        printf("FAIL \"%s\": %s\n", text, err.msg);
        failures++;
        return;
    }
    if (w != want) {
        printf("FAIL \"%s\": got %08x, want %08x\n", text,
               (unsigned)w, (unsigned)want);
        failures++;
        return;
    }
    dsp3210_disassemble(w, ADDR, &ins);
    if (strcmp(ins.text, text) != 0) {
        printf("FAIL %08x: disassembles to \"%s\", not \"%s\"\n",
               (unsigned)w, ins.text, text);
        failures++;
    }
}

static void expect_rt(const char *text)
{
    dsp3210_asm_err err;
    dsp3210_insn ins;
    uint32_t w = 0;

    checks++;
    if (dsp3210_encode(text, ADDR, &w, &err)) {
        printf("FAIL \"%s\": %s\n", text, err.msg);
        failures++;
        return;
    }
    dsp3210_disassemble(w, ADDR, &ins);
    if (ins.status != DSP3210_OK || strcmp(ins.text, text) != 0) {
        printf("FAIL \"%s\": assembles to %08x = \"%s\" (%s)\n", text,
               (unsigned)w, ins.text, dsp3210_status_name(ins.status));
        failures++;
    }
}

static void expect_err(const char *text)
{
    dsp3210_asm_err err;
    uint32_t w = 0;

    checks++;
    if (dsp3210_encode(text, ADDR, &w, &err) == DSP3210_ASM_OK) {
        printf("FAIL \"%s\": expected an error, got %08x\n", text,
               (unsigned)w);
        failures++;
    }
}

static void expect_image(const char *what, const char *src, uint32_t org,
                         const uint32_t *words, unsigned nwords)
{
    static uint8_t img[65536];
    dsp3210_asm_err err;
    uint32_t len = 0;
    unsigned i;

    checks++;
    if (dsp3210_assemble(src, org, img, sizeof img, &len, &err)) {
        printf("FAIL %s: line %d: %s\n", what, err.line, err.msg);
        failures++;
        return;
    }
    if (len != nwords * 4) {
        printf("FAIL %s: length 0x%x, want 0x%x\n", what,
               (unsigned)len, nwords * 4);
        failures++;
        return;
    }
    for (i = 0; i < nwords; i++) {
        uint32_t got = dsp3210_read_be32(img + 4 * i);
        if (got != words[i]) {
            printf("FAIL %s: word %u is %08x, want %08x\n", what, i,
                   (unsigned)got, (unsigned)words[i]);
            failures++;
            return;
        }
    }
}

int main(void)
{
    /* ---- fixed words and spc pseudo-instructions (hardware-verified words) ---- */
    expect_word("nop",     0x80000000u);
    expect_word("ireturn", 0x803E0000u);
    expect_word("waiti",   0x9DE0040Au);
    expect_word("bkpt",    0x9D60040Au);
    expect_word("sftrst",  0x9D00040Au);

    /* ---- goto / conditional goto (0b/1b, 8a) ---- */
    expect_word("goto r1",              0x80210000u);
    expect_word("goto r18",             0x80340000u);
    expect_word("if (pl) goto r1+0x800",0x80410800u);
    expect_word("if (eq) goto pc+0x10", 0x80AF0010u);
    expect_word("goto pc",              0x802F0000u);
    expect_word("goto pc-0x24",         0x802FFFDCu);
    expect_word("goto 0x1234",          0x80201234u);   /* 1b, not 8a */
    expect_word("goto 0xfffffffc",      0x8020FFFCu);
    expect_word("goto r2+0x10000",      0xA0220000u);   /* 8a fallback */
    expect_word("goto 0x123456",        0xA2403456u);
    expect_word("goto pc+0x20000",      0xA04F0000u);
    expect_word("if (false) goto r3+0x4",0x80030004u);
    expect_err ("if (eq) goto 0x123456");   /* conditional: 16-bit only */
    expect_err ("goto 0x2000000");

    /* ---- loop branch (3a) — IM GOTO-LOOP example ---- */
    expect_word("if (r1-- >= 0) goto r2-0xab", 0x0C22FF55u);
    expect_word("if (r10-- >= 0) goto pc-0x14", 0x0D4FFFECu);

    /* ---- call (4a, 8c) — IM CALL example ---- */
    expect_word("call pc+0x111 (r20)", 0x130F0111u);
    expect_word("call 0x40 (r1)",      0x10200040u);
    expect_word("call r6+0xc (r18)",   0x1286000Cu);    /* ROM boot */
    expect_word("call 0x123456 (r20)", 0xE2583456u);    /* 8c */

    /* ---- do / dolock / doblock (3b/3c) — IM DO example ---- */
    expect_word("do 4, r20",   0x8E002018u);
    expect_word("do 2, 100",   0x8C001064u);
    expect_word("dolock 1, 10",0x8D00080Au);
    expect_word("doblock r3",  0x8E800003u);
    expect_rt  ("doblock 100");
    expect_rt  ("doblock 3, 100");
    expect_err ("do 128, 5");
    expect_err ("do 1, 2048");

    /* ---- shift-or (4b) — hardware-verified words ---- */
    expect_word("r22 = r0 <<| 0x5003", 0x93405003u);
    expect_word("r2 = r2 <<| 0x5003",  0x90425003u);
    expect_word("r1 = r1 <<| 0xabcd",  0x9021ABCDu);

    /* ---- 3-operand add / set (5a/5b) — IM ADD/SET examples ---- */
    expect_word("r11 = (short) r1 + 0x20", 0x15610020u);
    expect_word("r11 = r1 + 0x20",         0x95610020u);
    expect_word("r2 = (short) 0x800",      0x14400800u);
    expect_word("r5 = pc - 0x24",          0x94AFFFDCu);  /* ROM boot */
    expect_rt  ("r11 = r1");         /* canonical: ALU add, not 5b N=0 */
    expect_rt  ("r3 = 0x0");
    expect_rt  ("r3 = -0x8000");
    expect_err ("r3 = 0x8000");      /* needs (ushort24) or <<| */

    /* ---- ALU register forms (6a/6b) — IM chapter 4.6 examples ---- */
    expect_word("r2 = r14 # r4",                0x98627024u);
    expect_word("if (cc) r1 = (short) r1 & r2", 0x19C10902u);
    expect_word("r5 = (short) r4 &~ r3",        0x18C52023u);
    expect_word("r5 & r13",                     0x99E0282Du);
    expect_word("if (ir0c) r2 = r3 ^ r4",       0x99021D84u);
    expect_word("r4 = r2 $>> r13",              0x99A4102Du);
    expect_word("if (mi) r10 = -r5",            0x988A0065u);
    expect_word("r4 = r9 - r4",  (1u<<31)|(0x0Cu<<25)|(4u<<21)|(4u<<16)
                                 |(9u<<11)|(1u<<5)|4u);
    expect_word("r3 = r7",       (1u<<31)|(0x0Cu<<25)|(0u<<21)|(3u<<16)
                                 |(7u<<11)|(1u<<5)|0u);
    expect_word("r3 = r7 * 2",   (1u<<31)|(0x0Cu<<25)|(0u<<21)|(3u<<16)
                                 |(7u<<11)|(1u<<5)|7u);
    expect_word("sp = sp++",     (1u<<31)|(0x0Cu<<25)|(0u<<21)|(25u<<16)
                                 |(25u<<11)|(1u<<5)|23u);
    expect_word("sp = sp--",     (1u<<31)|(0x0Cu<<25)|(0u<<21)|(25u<<16)
                                 |(25u<<11)|(1u<<5)|22u);
    expect_word("r4 = r9 - 1",   (1u<<31)|(0x0Cu<<25)|(0u<<21)|(4u<<16)
                                 |(9u<<11)|(1u<<5)|22u);   /* DECR */
    expect_word("if (vs) r5 = (short) r5 + 1",
                                 (0x0Cu<<25)|(0u<<21)|(5u<<16)
                                 |(5u<<11)|(7u<<5)|23u);   /* INCR */
    expect_word("if (ane) r3 = (short) r2 << 1",
                                 (0x0Cu<<25)|(1u<<21)|(3u<<16)
                                 |(2u<<11)|(20u<<5)|23u);
    expect_rt  ("if (cs) r5 = r6 <<<1");
    expect_rt  ("if (cs) r5 = r6 >>>1");
    expect_rt  ("r2 = pc");
    expect_rt  ("r1 = +n");
    expect_rt  ("r1 - -n");

    /* ---- ALU immediate forms (6c/6d) ---- */
    expect_word("r1 - 0xf",             0x9AE1000Fu);
    expect_word("r15 = r15 | 0x55",     0x9B510055u);
    expect_word("r2 = r2 >> 30",        0x9B82001Eu);
    /* canonical "rD = rS - N" is format 5 with a negated N; the 6d
     * subtract encoding decodes to the same text (fuzz-covered) */
    expect_word("r2 = r2 - 0x5",  0x9442FFFBu);
    expect_word("r2 = 0x5 - r2",  (1u<<31)|(0x0Du<<25)|(2u<<21)|(2u<<16)|5u);
    expect_word("r3 = (short) r3 ^ -0x1",
                                  (0x0Du<<25)|(8u<<21)|(3u<<16)|0xFFFFu);
    expect_word("r6 & 0x80",      (1u<<31)|(0x0Du<<25)|(15u<<21)|(6u<<16)
                                  |0x80u);
    expect_rt  ("r3 = r3 << 1");     /* canonical: immediate form */
    expect_err ("r3 = r2 & 0x5");    /* immediate logicals need rD = rD */
    expect_err ("if (eq) r3 = r3 & 0x5");   /* no condition field */
    expect_err ("r3 = r3 << 32");

    /* ---- moves (7a-7d) — IM examples + hardware-verified words ---- */
    expect_word("r10 = (ushort) *0xaa",  0x1C4A00AAu);
    expect_word("r1 = *0xaa",            0x1CE100AAu);
    expect_word("*0x1234 = (short) r5",  0x1D651234u);
    expect_word("r4 = *r2++",            0x9CE41017u);
    expect_word("r4 = *r2++r16",         0x9CE41012u);
    expect_word("*r3++ = (byte) r4",     0x9D041817u);
    expect_word("r22 = *r5++",           0x9CFA2817u);
    expect_word("*r0 = r0",              0x9DE00000u);
    expect_word("*r1++ = (hbyte) r3",    0x9D830817u);
    expect_word("emr = (short) *r2",     0x9E681000u);
    expect_word("*r1 = (short) ps",      0x9F600800u);
    expect_word("pcw = (short) r1",      0x9D61040Cu);
    expect_word("r7 = dauc",             0x9CE7040Eu);
    expect_word("r1 = (byte) dauc",      0x9C01040Eu);
    expect_word("emr = (short) r0",      0x9D600408u);
    expect_word("pcsh = *r0",            0x9CFE0000u);  /* boot ROM SAR */
    expect_rt  ("r3 = (char) *r7--");
    expect_rt  ("*r2++r19 = r14");
    expect_rt  ("pcw = *r5++");
    expect_err ("ps = pcw");             /* no ior-to-ior move */
    expect_err ("r1 = (long) r2 + r3");  /* (long) never printed */

    /* ---- 24-bit immediate load (8b) — IM SET24 example ---- */
    expect_word("r2 = (ushort24) 0x80000", 0xC1020000u);
    expect_word("r15 = (ushort24) 0x1234", 0xC0111234u);
    expect_err ("r2 = (ushort24) 0x1000000");

    /* ---- DA multiply/accumulate — IM chapter 4.6 examples.
     * Words with a real Z store match the manual bit for bit; "no
     * write" is the manual's 0x07.  A Z operand through the same
     * pointer as a memory Y encodes as p=1111 — the store through Y's
     * effective address (hardware-verified; the old reading of 0x7F as
     * a second no-write spelling was wrong). ---- */
    expect_word("*r1 = a2 = *r2--",
                (1u<<29)|(4u<<26)|(2u<<21)|(0x16u<<7)|0x08u);
    expect_word("*r1++ = a0 = a1 - *r2-- * *r3++r17",
                (3u<<29)|(1u<<26)|(1u<<23)|(0x1Bu<<14)|(0x16u<<7)|0x0Fu);
    expect_word("a0 = -a1 + (*r2-- = *r3++r17) * *r1++",
                (2u<<29)|(1u<<26)|(1u<<24)|(0x0Fu<<14)|(0x1Bu<<7)|0x16u);
    expect_word("*r2++r16 = a2 = *r14++ - a0 * *r10--",
                (1u<<29)|(1u<<23)|(2u<<21)|(0x56u<<14)|(0x77u<<7)|0x12u);
    expect_word("*r2++r17 = a3 = *r12++ * *r11--",
                (3u<<29)|(4u<<26)|(3u<<21)|(0x5Eu<<14)|(0x67u<<7)|0x13u);
    expect_word("a0 = -(*r2++r16 = *r1) * *r8--",
                (2u<<29)|(4u<<26)|(1u<<23)|(0x46u<<14)|(0x08u<<7)|0x12u);
    /* Z through Y's own pointer -> p=1111 (shipped assembler output) */
    expect_word("*r2++ = a2 = *r2 + a0",      0x3440087Fu);
    expect_word("*r1 = a0 = float32(*r1)",    0x7C000478u);
    expect_word("*r2++ = a0 = *r2 * a1",      0x7000487Fu);
    expect_word("*r8++ = a0 = *r8 + a1 * *r10++", 0x2415E07Fu);
    expect_word("a0 = (*r2++r16 = *r2++r15) + a0", 0x380008FAu);
    expect_word("a3 = a1 + (*r4++r15 = *r4++r15) * a0", 0x446010F9u);
    /* plain no-write forms now canonicalise to the manual's 0x07 */
    expect_word("a2 = *r2 + a0",              0x34400807u);
    expect_word("a0 = (*r3++ = *r1++) + a0",  0x3800079Fu);
    expect_rt  ("a1 = *r5-- - *r7++");
    expect_rt  ("a1 = (*r13 = a3) - *r1--");
    expect_rt  ("a0 = a0 + a0 * a0");
    expect_rt  ("a3 = -*r2++");
    expect_rt  ("a1 = -0.0 - *r2 * *r3");
    expect_rt  ("a1 = 1.0 - *r2 * *r3");
    expect_rt  ("a0 = *r1 + 0.0 * *r2");
    expect_rt  ("a0 = -*r1++ * a2");
    expect_err ("a0 = *r1 + *r2 * *r3");     /* two memory reads + mem Y */
    expect_err ("a0 = *r15++");              /* DA pointers are r1-r14 */
    expect_err ("a0 = *r1++r3");             /* post-modify is r15-r19 */
    expect_err ("*r1 = a0 = (*r2 = *r3) + a1");  /* two Z stores */
    expect_err ("if (eq) a0 = *r1");

    /* ---- DA special functions — IM chapter 4.6 examples ---- */
    expect_word("*r1++ = a0 = ic(*r2++)",
                (0x0Fu<<27)|(0u<<23)|(0u<<21)|(0x17u<<7)|0x0Fu);
    expect_word("*r1++r15 = a3 = float16(*r2)",
                (0x0Fu<<27)|(2u<<23)|(3u<<21)|(0x10u<<7)|0x09u);
    expect_word("*r5++r17 = a3 = float32(*r2)",
                (0x0Fu<<27)|(8u<<23)|(3u<<21)|(0x10u<<7)|0x2Bu);
    expect_word("*r3 = a0 = int16(a0)",
                (0x0Fu<<27)|(3u<<23)|(0u<<21)|(0x00u<<7)|0x18u);
    expect_word("*r12++ = a2 = int32(*r1++)",
                (0x0Fu<<27)|(9u<<23)|(2u<<21)|(0x0Fu<<7)|0x67u);
    expect_word("*r12++r16 = a3 = round(*r1--)",
                (0x0Fu<<27)|(4u<<23)|(3u<<21)|(0x0Eu<<7)|0x62u);
    expect_word("*r11-- = a2 = ifalt(*r7--)",
                (0x0Fu<<27)|(5u<<23)|(2u<<21)|(0x3Eu<<7)|0x5Eu);
    expect_word("*r13 = a1 = ifagt(*r9++r18)",
                (0x0Fu<<27)|(7u<<23)|(1u<<21)|(0x4Cu<<7)|0x68u);
    expect_word("*r4-- = a1 = ieee(*r9++)",
                (0x0Fu<<27)|(12u<<23)|(1u<<21)|(0x4Fu<<7)|0x26u);
    expect_word("*r1++ = a1 = dsp(*r9++)",
                (0x0Fu<<27)|(13u<<23)|(1u<<21)|(0x4Fu<<7)|0x0Fu);
    expect_rt  ("a1 = oc(a1)");
    expect_rt  ("a0 = ifaeq(*r7)");
    expect_rt  ("a3 = seed(*r9++)");

    /* ---- full programs: labels, directives, two passes ---- */
    {
        static const uint32_t want[] = {
            0x94AF0008u,             /* r5 = pc + 0x8                */
            0x80AFFFFCu,             /* if (eq) goto next (pc-0x4)   */
            0x80000000u,             /* next: nop                    */
            0x0000002Au,             /* data: .word 40+2             */
            0x40000080u,             /* .float 1.5                   */
            0x80000080u,             /* .float -2.0                  */
        };
        expect_image("labels+data",
            "start:  r5 = pc + 0x8\n"
            "        if (eq) goto next   ; comment\n"
            "next:\n"
            "        nop\n"
            "data:   .word 40+2\n"
            "        .float 1.5, -2.0\n",
            0x100, want, 6);
    }
    {
        static const uint32_t want[] = {
            0x802F0004u,             /* goto main (pc+0x4)           */
            0x80000000u,             /* nop                          */
            0x00000000u,             /* .space 4                     */
            0x9CE11011u,             /* main: r1 = *r2++r15          */
            0x14400400u,             /* r2 = (short) COUNTER         */
        };
        expect_image("org+equ",
            "        .equ COUNTER, 0x400\n"
            "        goto main\n"
            "        nop\n"
            "        .space 4\n"
            "        .align 4\n"
            "main:   r1 = *r2++r15\n"
            "        r2 = (short) COUNTER\n",
            0x200, want, 5);
    }
    {
        /* .org gap is zero-filled; label after the gap resolves */
        static const uint32_t want[] = {
            0x802F0008u,             /* goto vec (pc+0x8 = 0x10)     */
            0x80000000u,
            0x00000000u,
            0x00000000u,
            0x9D60040Au,             /* vec: bkpt                    */
        };
        expect_image("org gap",
            "goto vec\n"
            "nop\n"
            ".org 0x10\n"
            "vec: bkpt\n",
            0, want, 5);
    }
    expect_err(".word 0x1ffffffff");

    /* symbols cannot shadow the machine vocabulary, and "name = value"
     * is not a definition (.equ is) */
    {
        static uint8_t img[256];
        dsp3210_asm_err err;
        uint32_t len;
        checks++;
        if (dsp3210_assemble("r5: nop\n", 0, img, sizeof img, &len,
                             &err) == DSP3210_ASM_OK
            || dsp3210_assemble("next = 0\n", 0, img, sizeof img, &len,
                                &err) == DSP3210_ASM_OK
            || dsp3210_assemble("x: nop\nx: nop\n", 0, img, sizeof img,
                                &len, &err) == DSP3210_ASM_OK) {
            printf("FAIL: bad symbol definitions accepted\n");
            failures++;
        }
    }

    printf("%d checks, %d failure%s\n", checks, failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
