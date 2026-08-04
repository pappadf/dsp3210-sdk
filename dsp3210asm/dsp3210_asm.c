/*
 * dsp3210_asm.c — reference assembler for the AT&T DSP3210
 *
 * See dsp3210_asm.h for provenance and the accepted syntax.  Bit-field
 * references are to the DSP3210 Information Manual, Tables 10-1
 * (instruction encodings), 10-2 (CA field encodings) and 10-3 (DA field
 * encodings), cross-checked against hardware-verified encodings from
 * real DSP3210 systems and against the companion disassembler.
 */

#include "dsp3210_asm.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* limits                                                              */

#define MAX_TOKS   64
#define MAX_SYMS   1024
#define NAME_LEN   48

/* ------------------------------------------------------------------ */
/* tokens                                                              */

enum { TK_END, TK_ID, TK_NUM, TK_FNUM, TK_PUNCT };

typedef struct {
    int      t;
    char     s[NAME_LEN];  /* identifier / punctuator / number spelling */
    int64_t  v;            /* TK_NUM value */
    double   f;            /* TK_FNUM value */
} tok;

typedef struct {
    char     name[NAME_LEN];
    uint32_t val;
} sym;

typedef struct {
    /* output */
    uint8_t *out;
    size_t   cap;
    uint32_t org, loc, hi;
    int      pass;              /* 1 or 2; 0 = single-statement mode */

    /* symbols */
    sym      syms[MAX_SYMS];
    int      nsyms;

    /* current statement */
    tok      tks[MAX_TOKS];
    int      ntk, pos;

    /* error reporting */
    int      line;
    dsp3210_asm_err *err;
    jmp_buf  jb;
} A;

static void fail(A *a, const char *fmt, ...)
{
    if (a->err) {
        va_list ap;
        a->err->line = a->line;
        va_start(ap, fmt);
        vsnprintf(a->err->msg, sizeof a->err->msg, fmt, ap);
        va_end(ap);
    }
    longjmp(a->jb, 1);
}

/* ------------------------------------------------------------------ */
/* name tables (IM Table 10-2)                                         */

/* 5-bit CAU register field: r0=0, r1-14=1-14, pc=15, r15-19=17-21,
 * -n/+n=22/23, r20-22=24-26, pcsh=30.  sp/evtp are aliases. */
static int reg5_code(const char *s)
{
    if (s[0] == 'r') {
        char *e;
        long n = strtol(s + 1, &e, 10);
        if (*e || e == s + 1 || n < 0 || n > 22)
            return -1;
        if (n <= 14) return (int)n;
        if (n <= 19) return (int)n + 2;
        return (int)n + 4;
    }
    if (!strcmp(s, "pc"))   return 15;
    if (!strcmp(s, "pcsh")) return 30;
    if (!strcmp(s, "sp"))   return 25;   /* r21 */
    if (!strcmp(s, "evtp")) return 26;   /* r22 */
    return -1;
}

static const char *const cond_names[64] = {
    "false","true","pl","mi","ne","eq","vc","vs",
    "cc","cs","ge","lt","gt","le","hi","ls",
    "auc","aus","age","alt","ane","aeq","avc","avs",
    "agt","ale",0,0,0,0,0,0,
    "ibe","ibf","obf","obe",0,0,0,0,
    "syc","sys","fbc","fbs","ir0c","ir0s","ir1c","ir1s",
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static int cond_code(const char *s)
{
    int i;
    for (i = 0; i < 64; i++)
        if (cond_names[i] && !strcmp(s, cond_names[i]))
            return i;
    return -1;
}

static int ior_code(const char *s)
{
    if (!strcmp(s, "ps"))   return 0;
    if (!strcmp(s, "emr"))  return 8;
    if (!strcmp(s, "spc"))  return 10;
    if (!strcmp(s, "pcw"))  return 12;
    if (!strcmp(s, "dauc")) return 14;
    if (!strcmp(s, "ctr"))  return 15;
    return -1;
}

/* move-size keyword -> 3-bit W field ((long), the default, is never
 * written; 101/110 are reserved) */
static int wsize_code(const char *s)
{
    if (!strcmp(s, "byte"))   return 0;
    if (!strcmp(s, "char"))   return 1;
    if (!strcmp(s, "ushort")) return 2;
    if (!strcmp(s, "short"))  return 3;
    if (!strcmp(s, "hbyte"))  return 4;
    if (!strcmp(s, "long"))   return 7;
    return -1;
}

static int gfunc_code(const char *s)
{
    static const char *const g[16] = {
        "ic", "oc", "float16", "int16", "round", "ifalt", "ifaeq",
        "ifagt", "float32", "int32", 0, 0, "ieee", "dsp", "seed", 0
    };
    int i;
    for (i = 0; i < 16; i++)
        if (g[i] && !strcmp(s, g[i]))
            return i;
    return -1;
}

/* ALU F field (IM Table 10-2 "CA - F Field") */
enum {
    F_ADD = 0, F_SHL = 1, F_RSUB = 2, F_CRADD = 3, F_SUB = 4,
    F_ANDC = 6, F_CMP = 7, F_XOR = 8, F_ROR = 9, F_OR = 10,
    F_ROL = 11, F_SHR = 12, F_ASR = 13, F_AND = 14, F_BTST = 15
};

#define RC_R0    0u
#define RC_PC   15u
#define RC_MINUS 22u
#define RC_PLUS  23u

/* ------------------------------------------------------------------ */
/* tokenizer                                                           */

static int punct_len(const char *p)
{
    static const char *const multi[] = {
        "<<|", "<<<", ">>>", "$>>", "++", "--", "<<", ">>", ">=", "&~", 0
    };
    int i;
    for (i = 0; multi[i]; i++)
        if (!strncmp(p, multi[i], strlen(multi[i])))
            return (int)strlen(multi[i]);
    if (strchr("=+-*(),&|^#:", *p))
        return 1;
    return 0;
}

static int is_id_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c == '_' || c == '.';
}

static int is_id_char(char c)
{
    return is_id_start(c) || (c >= '0' && c <= '9');
}

static void tokenize(A *a, const char *p, const char *end)
{
    a->ntk = 0;
    a->pos = 0;
    while (p < end) {
        tok *t;
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }
        if (*p == ';' || (*p == '/' && p + 1 < end && p[1] == '/'))
            break;                                    /* comment */
        if (a->ntk >= MAX_TOKS - 1)
            fail(a, "statement too long");
        t = &a->tks[a->ntk];
        memset(t, 0, sizeof *t);

        if ((*p >= '0' && *p <= '9')
            || (*p == '$' && p + 1 < end
                && strchr("0123456789abcdefABCDEF", p[1]))) {
            /* number: 0x…, $…, decimal, or a decimal float */
            char buf[NAME_LEN];
            size_t n = 0;
            int isf = 0, hex = 0;
            if (*p == '$') {
                hex = 1;
                p++;
                buf[n++] = '0'; buf[n++] = 'x';
            } else if (p[0] == '0' && p + 1 < end
                       && (p[1] == 'x' || p[1] == 'X')) {
                hex = 1;
                buf[n++] = *p++; buf[n++] = *p++;
            }
            while (p < end && n < sizeof buf - 1) {
                char c = *p;
                if (hex ? !!strchr("0123456789abcdefABCDEF", c)
                        : (c >= '0' && c <= '9')) {
                    buf[n++] = c; p++;
                } else if (!hex && (c == '.' || c == 'e' || c == 'E')) {
                    isf = 1;
                    buf[n++] = c; p++;
                    if ((c == 'e' || c == 'E') && p < end
                        && (*p == '+' || *p == '-') && n < sizeof buf - 1)
                        buf[n++] = *p++;
                } else {
                    break;
                }
            }
            buf[n] = '\0';
            if (n >= sizeof t->s)
                fail(a, "number too long");
            strcpy(t->s, buf);
            if (isf) {
                t->t = TK_FNUM;
                t->f = strtod(buf, NULL);
            } else {
                t->t = TK_NUM;
                t->v = (int64_t)strtoull(buf, NULL, 0);
            }
        } else if (is_id_start(*p)) {
            size_t n = 0;
            while (p < end && is_id_char(*p)) {
                if (n < sizeof t->s - 1)
                    t->s[n++] = *p;
                p++;
            }
            t->s[n] = '\0';
            t->t = TK_ID;
        } else {
            int n = punct_len(p);
            if (!n)
                fail(a, "unexpected character '%c'", *p);
            memcpy(t->s, p, (size_t)n);
            t->s[n] = '\0';
            t->t = TK_PUNCT;
            p += n;
        }
        a->ntk++;
    }
    a->tks[a->ntk].t = TK_END;
    a->tks[a->ntk].s[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* token cursor helpers                                                */

static tok *cur(A *a)          { return &a->tks[a->pos]; }
static tok *peek(A *a, int n)  { return &a->tks[a->pos + n <= a->ntk
                                               ? a->pos + n : a->ntk]; }

static int at_end(A *a)        { return cur(a)->t == TK_END; }

static int is_p(A *a, const char *p)
{
    return cur(a)->t == TK_PUNCT && !strcmp(cur(a)->s, p);
}

static int is_id(A *a, const char *s)
{
    return cur(a)->t == TK_ID && !strcmp(cur(a)->s, s);
}

static int eat_p(A *a, const char *p)
{
    if (is_p(a, p)) { a->pos++; return 1; }
    return 0;
}

static void need_p(A *a, const char *p)
{
    if (!eat_p(a, p))
        fail(a, "expected '%s', got '%s'", p, cur(a)->s);
}

static void need_end(A *a)
{
    if (!at_end(a))
        fail(a, "trailing junk: '%s'", cur(a)->s);
}

/* ------------------------------------------------------------------ */
/* symbols and expressions                                             */

static sym *sym_find(A *a, const char *name)
{
    int i;
    for (i = 0; i < a->nsyms; i++)
        if (!strcmp(a->syms[i].name, name))
            return &a->syms[i];
    return NULL;
}

static void sym_define(A *a, const char *name, uint32_t val)
{
    if (strlen(name) >= NAME_LEN)
        fail(a, "symbol name too long: %s", name);
    if (reg5_code(name) >= 0 || ior_code(name) >= 0 || cond_code(name) >= 0
        || wsize_code(name) >= 0 || gfunc_code(name) >= 0
        || !strcmp(name, "n") || !strcmp(name, "goto")
        || !strcmp(name, "call") || !strcmp(name, "if")
        || !strcmp(name, "do") || !strcmp(name, "dolock")
        || !strcmp(name, "doblock") || !strcmp(name, "nop")
        || !strcmp(name, "ireturn") || !strcmp(name, "waiti")
        || !strcmp(name, "bkpt") || !strcmp(name, "sftrst")
        || !strcmp(name, "ushort24")
        || (name[0] == 'a' && name[1] >= '0' && name[1] <= '3'
            && !name[2]))
        fail(a, "'%s' is a reserved name", name);
    if (a->pass == 2) {
        return;                       /* defined during pass 1 */
    }
    if (sym_find(a, name))
        fail(a, "symbol '%s' redefined", name);
    if (a->nsyms >= MAX_SYMS)
        fail(a, "too many symbols");
    strcpy(a->syms[a->nsyms].name, name);
    a->syms[a->nsyms].val = val;
    a->nsyms++;
}

static uint32_t sym_value(A *a, const char *name)
{
    sym *s = sym_find(a, name);
    if (!s) {
        if (a->pass == 1)
            return 0;                 /* forward reference, sized only */
        fail(a, "undefined symbol '%s'", name);
    }
    return s->val;
}

/* one expression primary: number or symbol */
static int64_t expr_primary(A *a, int need_defined)
{
    tok *t = cur(a);
    if (t->t == TK_NUM) {
        a->pos++;
        return t->v;
    }
    if (t->t == TK_ID && reg5_code(t->s) < 0 && ior_code(t->s) < 0) {
        int64_t v;
        if (need_defined && !sym_find(a, t->s))
            fail(a, "'%s' must be defined before use here", t->s);
        v = (int64_t)sym_value(a, t->s);
        a->pos++;
        return v;
    }
    fail(a, "expected a number or symbol, got '%s'", t->s);
    return 0;
}

/* +/- chain of primaries.  need_defined: symbols must already exist
 * (directives that affect layout are evaluated during pass 1). */
static int64_t expr_eval(A *a, int need_defined)
{
    int64_t v;
    int neg = 0;
    if (eat_p(a, "-")) neg = 1;
    else eat_p(a, "+");
    v = expr_primary(a, need_defined);
    if (neg) v = -v;
    for (;;) {
        if (eat_p(a, "+"))      v += expr_primary(a, need_defined);
        else if (eat_p(a, "-")) v -= expr_primary(a, need_defined);
        else break;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* CA operand parsing                                                  */

/*
 * A 5-bit CA register field, including the +n/-n pseudo-operands (which
 * arrive as two tokens) and pc/pcsh/sp/evtp.  Returns the code or -1
 * without consuming anything.
 */
static int try_reg5(A *a)
{
    tok *t = cur(a);
    if (t->t == TK_PUNCT && (!strcmp(t->s, "+") || !strcmp(t->s, "-"))
        && peek(a, 1)->t == TK_ID && !strcmp(peek(a, 1)->s, "n")) {
        int code = (t->s[0] == '+') ? (int)RC_PLUS : (int)RC_MINUS;
        a->pos += 2;
        return code;
    }
    if (t->t == TK_ID) {
        int code = reg5_code(t->s);
        if (code >= 0) {
            a->pos++;
            return code;
        }
    }
    return -1;
}

static unsigned need_reg5(A *a)
{
    int code = try_reg5(a);
    if (code < 0)
        fail(a, "expected a register, got '%s'", cur(a)->s);
    return (unsigned)code;
}

enum { OP_REG, OP_IMM, OP_NEGREG };

typedef struct {
    int      k;       /* OP_* */
    unsigned reg;     /* OP_REG / OP_NEGREG: 5-bit code */
    int64_t  v;       /* OP_IMM */
    int      bare1;   /* OP_IMM spelled as the bare decimal "1" */
    int      neg;     /* OP_IMM spelled with an explicit '-' */
} copnd;

/* register (incl. pseudo-operands), immediate, or -register */
static copnd ca_operand(A *a)
{
    copnd o;
    int r;
    memset(&o, 0, sizeof o);

    r = try_reg5(a);                       /* handles +n/-n too */
    if (r >= 0) {
        o.k = OP_REG;
        o.reg = (unsigned)r;
        return o;
    }
    if (is_p(a, "--") && peek(a, 1)->t == TK_ID
        && !strcmp(peek(a, 1)->s, "n")) {
        a->pos += 2;                       /* "--n": negated -n pseudo */
        o.k = OP_NEGREG;
        o.reg = RC_MINUS;
        return o;
    }
    if (is_p(a, "-")) {
        a->pos++;
        r = try_reg5(a);
        if (r >= 0) {
            o.k = OP_NEGREG;
            o.reg = (unsigned)r;
            return o;
        }
        o.k = OP_IMM;
        o.v = -expr_primary(a, 0);
        o.neg = 1;
        return o;
    }
    if (is_p(a, "+")) {
        a->pos++;
        o.k = OP_IMM;
        o.v = expr_primary(a, 0);
        return o;
    }
    if (cur(a)->t == TK_NUM) {
        o.k = OP_IMM;
        o.v = cur(a)->v;
        o.bare1 = !strcmp(cur(a)->s, "1");
        a->pos++;
        return o;
    }
    if (cur(a)->t == TK_ID) {              /* symbol */
        o.k = OP_IMM;
        o.v = (int64_t)sym_value(a, cur(a)->s);
        a->pos++;
        return o;
    }
    fail(a, "expected an operand, got '%s'", cur(a)->s);
    return o;
}

/* range helpers */
static int fits_s16(int64_t v)
{
    /* accept both -0x8000..0x7FFF and the 32-bit sign-extended spelling
     * the disassembler prints (0xFFFF8000..0xFFFFFFFF) */
    if (v >= -0x8000 && v <= 0x7FFF)
        return 1;
    return v >= 0 && (uint64_t)v <= 0xFFFFFFFFu
        && ((uint64_t)v & 0xFFFF8000u) == 0xFFFF8000u;
}

static uint32_t u16of(A *a, int64_t v, const char *what)
{
    if (!fits_s16(v) && (v < 0 || v > 0xFFFF))
        fail(a, "%s out of range: %lld", what, (long long)v);
    return (uint32_t)v & 0xFFFFu;
}

static uint32_t u24of(A *a, int64_t v, const char *what)
{
    if (v < 0 || v > 0xFFFFFF)
        fail(a, "%s out of 24-bit range: %lld", what, (long long)v);
    return (uint32_t)v;
}

/* ------------------------------------------------------------------ */
/* register-indirect memory operand (formats 7c/7d), parsed generically
 * so it can be re-encoded either as CA (rP/rI codes) or as a DA Z field */

typedef struct {
    unsigned rp;     /* 5-bit CA code of the base register */
    int      mod;    /* 0 none, 1 ++, 2 --, 3 ++reg */
    unsigned ri;     /* 5-bit CA code of the modifier register (mod 3) */
} camem;

static camem ca_mem(A *a)                  /* cursor is at '*' */
{
    camem m;
    memset(&m, 0, sizeof m);
    need_p(a, "*");
    m.rp = need_reg5(a);
    if (eat_p(a, "++")) {
        int r = try_reg5(a);
        if (r >= 0) {
            m.mod = 3;
            m.ri = (unsigned)r;
        } else {
            m.mod = 1;
        }
    } else if (eat_p(a, "--")) {
        m.mod = 2;
    }
    return m;
}

static unsigned camem_ri(const camem *m)
{
    switch (m->mod) {
    case 1:  return RC_PLUS;
    case 2:  return RC_MINUS;
    case 3:  return m->ri;
    default: return RC_R0;
    }
}

/* CA reg-number (not code) of a 5-bit code, or -1 for pc/pseudos */
static int code_regnum(unsigned code)
{
    if (code <= 14) return (int)code;
    if (code >= 17 && code <= 21) return (int)code - 2;
    if (code >= 24 && code <= 26) return (int)code - 4;
    return -1;
}

/* after a '*', is the memory operand a 16-bit direct address (format
 * 7a) rather than register-indirect?  A number or a non-register symbol
 * means direct. */
static int mem_is_direct(A *a)
{
    tok *t = peek(a, 1);
    if (t->t == TK_NUM)
        return 1;
    return t->t == TK_ID && reg5_code(t->s) < 0 && ior_code(t->s) < 0;
}

/* re-encode a parsed memory operand as a 7-bit DA field */
static unsigned camem_to_da(A *a, const camem *m)
{
    int p = code_regnum(m->rp);
    unsigned i;
    if (p < 1 || p > 14)
        fail(a, "DA memory pointer must be r1-r14");
    switch (m->mod) {
    case 0: i = 0; break;
    case 1: i = 7; break;
    case 2: i = 6; break;
    default: {
        int r = code_regnum(m->ri);
        if (r < 15 || r > 19)
            fail(a, "DA post-modify register must be r15-r19");
        i = (unsigned)r - 14;
        break;
    }
    }
    return ((unsigned)p << 3) | i;
}

/* ------------------------------------------------------------------ */
/* branch targets                                                      */

enum { TGT_ABS, TGT_PCREL, TGT_REG };

typedef struct {
    int      kind;
    unsigned rb;      /* TGT_REG */
    int64_t  n;
} btgt;

/*
 * {N, rB, rB±N, pc±N, label±N}.  A symbol resolves pc-relative (so label
 * code is position-independent); a bare number is absolute.
 */
static btgt parse_target(A *a, uint32_t addr)
{
    btgt t;
    int r;
    memset(&t, 0, sizeof t);

    r = try_reg5(a);
    if (r >= 0) {
        if (r == (int)RC_PC)
            t.kind = TGT_PCREL;
        else {
            t.kind = TGT_REG;
            t.rb = (unsigned)r;
        }
        if (is_p(a, "+") || is_p(a, "-")) {
            int neg = is_p(a, "-");
            a->pos++;
            t.n = expr_primary(a, 0);
            if (neg)
                t.n = -t.n;
        }
        return t;
    }
    if (cur(a)->t == TK_ID) {              /* label: pc-relative */
        int64_t v = (int64_t)sym_value(a, cur(a)->s);
        a->pos++;
        while (is_p(a, "+") || is_p(a, "-")) {
            int neg = is_p(a, "-");
            a->pos++;
            v += neg ? -expr_primary(a, 0) : expr_primary(a, 0);
        }
        t.kind = TGT_PCREL;
        t.n = v - ((int64_t)addr + 8);
        return t;
    }
    t.kind = TGT_ABS;
    t.n = expr_eval(a, 0);
    return t;
}

/* formats 0b/1b (16-bit N) with 8a fallback for unconditional gotos */
static uint32_t enc_goto(A *a, unsigned c, const btgt *t)
{
    unsigned rb;
    switch (t->kind) {
    case TGT_ABS:   rb = RC_R0; break;
    case TGT_PCREL: rb = RC_PC; break;
    default:        rb = t->rb; break;
    }
    if (fits_s16(t->n))
        return 0x80000000u | (c << 21) | (rb << 16)
             | ((uint32_t)t->n & 0xFFFFu);
    if (c == 1 && t->n >= 0 && t->n <= 0xFFFFFF)   /* format 8a */
        return (5u << 29) | (((uint32_t)t->n >> 16) << 21) | (rb << 16)
             | ((uint32_t)t->n & 0xFFFFu);
    fail(a, "branch displacement %lld out of range%s", (long long)t->n,
         c == 1 ? "" : " (conditional branches are limited to 16 bits)");
    return 0;
}

/* format 4a (16-bit N) with 8c fallback for absolute calls */
static uint32_t enc_call(A *a, unsigned rm, const btgt *t)
{
    unsigned rb;
    switch (t->kind) {
    case TGT_ABS:   rb = RC_R0; break;
    case TGT_PCREL: rb = RC_PC; break;
    default:        rb = t->rb; break;
    }
    if (fits_s16(t->n))
        return (0x04u << 26) | (rm << 21) | (rb << 16)
             | ((uint32_t)t->n & 0xFFFFu);
    if (t->kind == TGT_ABS && t->n >= 0 && t->n <= 0xFFFFFF) /* 8c */
        return (7u << 29) | (((uint32_t)t->n >> 16) << 21) | (rm << 16)
             | ((uint32_t)t->n & 0xFFFFu);
    fail(a, "call displacement %lld out of range", (long long)t->n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* CA ALU / move / fmt-5 encoders                                      */

static uint32_t alu_r(unsigned e, unsigned f, unsigned rd, unsigned rs1,
                      unsigned c, unsigned rs2)
{
    return (e << 31) | (0x0Cu << 25) | (f << 21) | (rd << 16)
         | (rs1 << 11) | (c << 5) | rs2;
}

static uint32_t alu_i(unsigned e, unsigned f, unsigned rd, uint32_t n16)
{
    return (e << 31) | (0x0Du << 25) | (f << 21) | (rd << 16)
         | (n16 & 0xFFFFu);
}

static uint32_t enc_fmt5(A *a, unsigned e, unsigned rd, unsigned rs3,
                         int64_t n)
{
    if (!fits_s16(n))
        fail(a, "immediate %lld out of 16-bit range", (long long)n);
    return ((e ? 0x25u : 0x05u) << 26) | (rd << 21) | (rs3 << 16)
         | ((uint32_t)n & 0xFFFFu);
}

/* format 7c (register <-> memory) / 7d (io register <-> memory) */
static uint32_t enc_mv_ind(unsigned io, unsigned t, unsigned w, unsigned rh,
                           const camem *m)
{
    return (0x27u << 26) | (io << 25) | (t << 24) | (w << 21) | (rh << 16)
         | (m->rp << 11) | camem_ri(m);
}

/* format 7a (register <-> 16-bit direct address) */
static uint32_t enc_mv_dir(unsigned t, unsigned w, unsigned rh, uint32_t l)
{
    return (0x07u << 26) | (t << 24) | (w << 21) | (rh << 16)
         | (l & 0xFFFFu);
}

/* format 7b (register <-> io register) */
static uint32_t enc_mv_ior(unsigned t, unsigned w, unsigned rh, unsigned ior)
{
    return (0x27u << 26) | (t << 24) | (w << 21) | (rh << 16) | (1u << 10)
         | ior;
}

/* ------------------------------------------------------------------ */
/* DA (floating point) statements                                      */

/* one 7-bit X/Y operand: aN, or *rP with post-modification */
static unsigned da_operand(A *a)
{
    tok *t = cur(a);
    if (t->t == TK_ID && t->s[0] == 'a' && t->s[1] >= '0' && t->s[1] <= '3'
        && !t->s[2]) {
        a->pos++;
        return (unsigned)(t->s[1] - '0');       /* p=0000, i=aN */
    }
    if (is_p(a, "*")) {
        camem m = ca_mem(a);
        return camem_to_da(a, &m);
    }
    fail(a, "expected a DA operand (aN or *rP), got '%s'", t->s);
    return 0;
}

static int da_is_acc(unsigned field)   { return (field >> 3) == 0; }

#define DA_NOWRITE 0x7Fu   /* Apple's assembler convention; the manual's
                              examples use 0x07 — both mean "no Z write" */

/* a parsed additive term of a DA expression */
enum { D_OP, D_PROD, D_TAP, D_TAPPROD, D_ZERO, D_ONE, D_ZEROPROD };

typedef struct {
    int      kind;
    unsigned y, x, z;
} dterm;

static dterm da_term(A *a)
{
    dterm t;
    memset(&t, 0, sizeof t);

    if (cur(a)->t == TK_FNUM) {
        double f = cur(a)->f;
        a->pos++;
        if (f == 0.0) {
            if (eat_p(a, "*")) {
                t.kind = D_ZEROPROD;
                t.x = da_operand(a);
            } else {
                t.kind = D_ZERO;
            }
        } else if (f == 1.0) {
            t.kind = D_ONE;
        } else {
            fail(a, "only 0.0 and 1.0 appear literally in DA expressions");
        }
        return t;
    }
    if (eat_p(a, "(")) {                       /* tap: (Z = Y) */
        camem m = ca_mem(a);
        t.z = camem_to_da(a, &m);
        need_p(a, "=");
        t.y = da_operand(a);
        need_p(a, ")");
        if (eat_p(a, "*")) {
            t.kind = D_TAPPROD;
            t.x = da_operand(a);
        } else {
            t.kind = D_TAP;
        }
        return t;
    }
    t.y = da_operand(a);
    if (eat_p(a, "*")) {
        t.kind = D_PROD;
        t.x = da_operand(a);
    } else {
        t.kind = D_OP;
    }
    return t;
}

static uint32_t da_word(unsigned fmt, unsigned m, unsigned fs, unsigned ss,
                        unsigned n, unsigned x, unsigned y, unsigned z)
{
    return (fmt << 29) | (m << 26) | (fs << 24) | (ss << 23) | (n << 21)
         | (x << 14) | (y << 7) | z;
}

/*
 * aN already consumed along with its '='.  zpre carries an optional
 * "Z = aN = …" store parsed by the caller (have_z says whether).
 *
 * Canonical encodings for text the hardware can produce two ways:
 *   aN = Y + X            -> format 1 with M = 1.0 (never the M=110 tap
 *                            form without a Z write)
 *   aN = aM ± Y * X       -> format 3 (never format 2 without a Z write)
 *   aN = Y + aM * X       -> format 1 (both format 1 and 3 print this
 *                            when every operand is an accumulator)
 *   no Z write            -> Z = 1111111 (Apple's assembler convention)
 */
static uint32_t enc_da(A *a, unsigned n, unsigned zpre, int have_z)
{
    unsigned fmt, m, fs = 0, ss = 0, x = 0, y, z;
    int neg1;
    dterm t1, t2;
    int two = 0;

    /* special functions: aN = g(Y) */
    if (cur(a)->t == TK_ID && gfunc_code(cur(a)->s) >= 0
        && peek(a, 1)->t == TK_PUNCT && !strcmp(peek(a, 1)->s, "(")) {
        unsigned g = (unsigned)gfunc_code(cur(a)->s);
        a->pos++;
        need_p(a, "(");
        y = da_operand(a);
        need_p(a, ")");
        need_end(a);
        return (0x0Fu << 27) | (g << 23) | (n << 21) | (y << 7)
             | (have_z ? zpre : DA_NOWRITE);
    }

    neg1 = eat_p(a, "-");
    t1 = da_term(a);
    if (eat_p(a, "+")) {
        two = 1; ss = 0;
        t2 = da_term(a);
    } else if (eat_p(a, "-")) {
        two = 1; ss = 1;
        t2 = da_term(a);
    } else {
        memset(&t2, 0, sizeof t2);
    }
    need_end(a);

    if (!two) {
        switch (t1.kind) {
        case D_OP:                       /* aN = [-]Y */
            fmt = 1; m = 4; fs = (unsigned)neg1; ss = 0;
            y = t1.y; x = 0;
            z = have_z ? zpre : DA_NOWRITE;
            break;
        case D_PROD:                     /* aN = [-]Y * X */
            fmt = 3; m = 4; fs = 0; ss = (unsigned)neg1;
            y = t1.y; x = t1.x;
            z = have_z ? zpre : DA_NOWRITE;
            break;
        case D_TAPPROD:                  /* aN = [-](Z = Y) * X */
            if (have_z)
                fail(a, "tap forms carry their store inside the ()");
            fmt = 2; m = 4; fs = 0; ss = (unsigned)neg1;
            y = t1.y; x = t1.x; z = t1.z;
            break;
        default:
            fail(a, "not an encodable DA expression");
            return 0;
        }
        return da_word(fmt, m, fs, ss, n, x, y, z);
    }

    /* two terms: t1 is the adder input, t2 the product (or plain X) */
    fs = (unsigned)neg1;
    switch (t1.kind) {
    case D_ZERO:
    case D_ONE: {
        m = (t1.kind == D_ONE) ? 5u : 4u;
        if (t2.kind == D_PROD) {
            fmt = 3; y = t2.y; x = t2.x;
            z = have_z ? zpre : DA_NOWRITE;
        } else if (t2.kind == D_TAPPROD) {
            if (have_z)
                fail(a, "tap forms carry their store inside the ()");
            fmt = 2; y = t2.y; x = t2.x; z = t2.z;
        } else {
            fail(a, "0.0/1.0 must be added to a product");
            return 0;
        }
        break;
    }
    case D_TAP:                          /* aN = [-](Z = Y) ± X */
        if (t2.kind != D_OP)
            fail(a, "a tap term may only be added to a plain operand");
        if (have_z)
            fail(a, "tap forms carry their store inside the ()");
        fmt = 1; m = 6;
        y = t1.y; x = t2.y; z = t1.z;
        break;
    case D_OP:
        if (t2.kind == D_OP) {           /* aN = [-]Y ± X  (M = 1.0) */
            fmt = 1; m = 5;
            y = t1.y; x = t2.y;
            z = have_z ? zpre : DA_NOWRITE;
        } else if (t2.kind == D_ZEROPROD) {   /* aN = [-]Y ± 0.0 * X */
            fmt = 1; m = 4;
            y = t1.y; x = t2.x;
            z = have_z ? zpre : DA_NOWRITE;
        } else if (t2.kind == D_PROD) {
            if (da_is_acc(t2.y)) {       /* aN = [-]Y ± aM * X */
                fmt = 1; m = t2.y & 7u;
                y = t1.y; x = t2.x;
            } else if (da_is_acc(t1.y)) {/* aN = [-]aM ± Y * X */
                fmt = 3; m = t1.y & 7u;
                y = t2.y; x = t2.x;
            } else {
                fail(a, "one of the adder input and the first product "
                        "factor must be an accumulator");
                return 0;
            }
            z = have_z ? zpre : DA_NOWRITE;
        } else if (t2.kind == D_TAPPROD) {
            if (!da_is_acc(t1.y))
                fail(a, "the adder input of a tap product must be an "
                        "accumulator, 0.0 or 1.0");
            if (have_z)
                fail(a, "tap forms carry their store inside the ()");
            fmt = 2; m = t1.y & 7u;
            y = t2.y; x = t2.x; z = t2.z;
        } else {
            fail(a, "not an encodable DA expression");
            return 0;
        }
        break;
    default:
        fail(a, "not an encodable DA expression");
        return 0;
    }
    return da_word(fmt, m, fs, ss, n, x, y, z);
}

/* "aN" at cursor?  (used to route statements) */
static int at_acc(A *a)
{
    tok *t = cur(a);
    return t->t == TK_ID && t->s[0] == 'a' && t->s[1] >= '0'
        && t->s[1] <= '3' && !t->s[2];
}

/* ------------------------------------------------------------------ */
/* CA assignments and compares                                         */

/* optional "(keyword)" after '='/statement start; -1 if none */
static int try_paren_kw(A *a, int *is_u24)
{
    *is_u24 = 0;
    if (is_p(a, "(") && peek(a, 1)->t == TK_ID
        && peek(a, 2)->t == TK_PUNCT && !strcmp(peek(a, 2)->s, ")")) {
        const char *name = peek(a, 1)->s;
        int w = wsize_code(name);
        if (w >= 0) {
            a->pos += 3;
            return w;
        }
        if (!strcmp(name, "ushort24")) {
            a->pos += 3;
            *is_u24 = 1;
            return -1;
        }
    }
    return -1;
}

static void no_cond(A *a, unsigned c, const char *what)
{
    if (c != 1)
        fail(a, "%s cannot take a condition", what);
}

static unsigned e_of_kw(A *a, int kw)
{
    if (kw < 0)  return 1;                     /* (long) */
    if (kw == 3) return 0;                     /* (short) */
    fail(a, "only (short) applies to arithmetic");
    return 0;
}

/* immediate ALU ops write "rD = rD op N": the source must repeat rD */
static void need_same(A *a, unsigned rd, const copnd *o, const char *op)
{
    if (o->k != OP_REG || o->reg != rd)
        fail(a, "immediate '%s' needs the destination as its source "
                "(rD = rD %s N)", op, op);
}

/*
 * Everything of the shape "LHS = RHS" with a register LHS, plus the
 * no-store compare/bit-test statements.  cond comes from an "if (…)"
 * prefix (1 = unconditional); pre_kw from a leading "(short)".
 */
static uint32_t enc_reg_stmt(A *a, unsigned c, int pre_kw, uint32_t addr)
{
    copnd lhs, o1, o2;
    unsigned rd, e;
    int kw, is_u24;
    (void)addr;

    lhs = ca_operand(a);
    if (lhs.k != OP_REG)
        fail(a, "expected a register statement");
    rd = lhs.reg;

    if (!eat_p(a, "=")) {
        /* compare / bit test: [ (short) ] rS1 {-,&} {rS2, N} */
        unsigned f;
        e = (pre_kw == 3) ? 0u : 1u;
        if (pre_kw >= 0 && pre_kw != 3)
            fail(a, "only (short) applies to compares");
        if (eat_p(a, "-"))      f = F_CMP;
        else if (eat_p(a, "&")) f = F_BTST;
        else {
            fail(a, "expected '=', '-' or '&' after %s", cur(a)->s);
            return 0;
        }
        /* "r1 - -n" style pseudo-operands parse via ca_operand */
        o2 = ca_operand(a);
        need_end(a);
        if (o2.k == OP_REG)
            return alu_r(e, f, 0, rd, c, o2.reg);
        if (o2.k == OP_IMM) {
            no_cond(a, c, "an immediate compare");
            return alu_i(e, f, rd, u16of(a, o2.v, "compare immediate"));
        }
        fail(a, "bad compare operand");
        return 0;
    }

    if (pre_kw >= 0)
        fail(a, "size keyword belongs after '='");

    kw = try_paren_kw(a, &is_u24);
    if (is_u24) {                              /* 8b: rD = (ushort24) M */
        uint32_t m = u24of(a, expr_eval(a, 0), "(ushort24) value");
        need_end(a);
        no_cond(a, c, "a 24-bit load");
        return (6u << 29) | ((m >> 16) << 21) | (rd << 16) | (m & 0xFFFFu);
    }

    /* moves with a memory or io-register RHS */
    if (is_p(a, "*")) {
        unsigned w = (kw < 0) ? 7u : (unsigned)kw;
        no_cond(a, c, "a move");
        if (mem_is_direct(a)) {                /* 7a: rD = (w) *L */
            uint32_t l;
            a->pos++;                          /* '*' */
            l = u16of(a, expr_eval(a, 0), "direct address");
            need_end(a);
            return enc_mv_dir(0, w, rd, l);
        }
        {
            camem m = ca_mem(a);
            need_end(a);
            return enc_mv_ind(0, 0, w, rd, &m);
        }
    }
    if (cur(a)->t == TK_ID && ior_code(cur(a)->s) >= 0) {
        unsigned w = (kw < 0) ? 7u : (unsigned)kw;
        unsigned ior = (unsigned)ior_code(cur(a)->s);
        a->pos++;
        need_end(a);
        no_cond(a, c, "a move");
        return enc_mv_ior(0, w, rd, ior);      /* 7b load */
    }
    if (at_acc(a))
        fail(a, "accumulators only appear in DA statements (aN = …)");

    /* arithmetic RHS */
    e = e_of_kw(a, kw);
    o1 = ca_operand(a);

    if (o1.k == OP_NEGREG) {                   /* rD = -rS */
        need_end(a);
        return alu_r(e, F_SUB, rd, RC_R0, c, o1.reg);
    }

    if (at_end(a)) {
        if (o1.k == OP_REG)                    /* move: canonical ALU add */
            return alu_r(e, F_ADD, rd, o1.reg, c, RC_R0);
        no_cond(a, c, "an immediate load");    /* SET: format 5, rS3=r0 */
        return enc_fmt5(a, e, rd, RC_R0, o1.v);
    }

    if (o1.k == OP_REG && (is_p(a, "++") || is_p(a, "--"))) {
        unsigned rs2 = is_p(a, "++") ? RC_PLUS : RC_MINUS;
        a->pos++;                              /* sp = sp++ and friends */
        need_end(a);
        return alu_r(e, F_ADD, rd, o1.reg, c, rs2);
    }

    if (o1.k == OP_IMM) {                      /* rD = N - rD (RSUB) */
        if (!eat_p(a, "-"))
            fail(a, "an immediate here can only start 'rD = N - rD'");
        o2 = ca_operand(a);
        need_end(a);
        need_same(a, rd, &o2, "-");
        no_cond(a, c, "an immediate reverse subtract");
        return alu_i(e, F_RSUB, rd, u16of(a, o1.v, "immediate"));
    }

    /* o1 is a register; a binary operator follows */
    {
        const char *op = cur(a)->s;
        unsigned rs1 = o1.reg;

        if (eat_p(a, "+")) {
            o2 = ca_operand(a);
            need_end(a);
            if (o2.k == OP_REG)
                return alu_r(e, F_ADD, rd, rs1, c, o2.reg);
            if (o2.k == OP_IMM && o2.bare1) {  /* rD = rS + 1 (INCR) */
                if (rd == 25 && rs1 == 25)     /* F_ADD would print the
                                                  "sp = sp++" special */
                    return alu_r(e, F_SUB, rd, rs1, c, RC_MINUS);
                return alu_r(e, F_ADD, rd, rs1, c, RC_PLUS);
            }
            if (o2.k == OP_IMM) {
                /* the 6d add's own spellings — "rD = rD + -0x5",
                 * "+ 0x0", and a source of r0 (which format 5 prints
                 * as a plain load) — stay in the immediate ALU form;
                 * everything else is the format-5 three-op add */
                if ((o2.neg || o2.v == 0 || rs1 == RC_R0)
                    && rs1 == rd && c == 1)
                    return alu_i(e, F_ADD, rd,
                                 u16of(a, o2.v, "immediate"));
                no_cond(a, c, "a three-operand add");
                return enc_fmt5(a, e, rd, rs1, o2.v);
            }
        } else if (eat_p(a, "-")) {
            o2 = ca_operand(a);
            need_end(a);
            if (o2.k == OP_REG
                && (o2.reg == RC_PLUS || o2.reg == RC_MINUS
                    || rs1 == RC_R0))
                /* "A - +n/-n" and "r0 - B": F_SUB would print those as
                 * "A ± 1" and "-B"; the reverse subtract prints its
                 * operands swapped, preserving the spelling */
                return alu_r(e, F_RSUB, rd, o2.reg, c, rs1);
            if (o2.k == OP_REG)
                return alu_r(e, F_SUB, rd, rs1, c, o2.reg);
            if (o2.k == OP_IMM && o2.bare1) {  /* rD = rS - 1 (DECR) */
                if (rd == 25 && rs1 == 25)     /* avoid "sp = sp--" */
                    return alu_r(e, F_SUB, rd, rs1, c, RC_PLUS);
                return alu_r(e, F_ADD, rd, rs1, c, RC_MINUS);
            }
            if (o2.k == OP_IMM) {
                if ((o2.neg || o2.v == 0 || rs1 == RC_R0)
                    && rs1 == rd && c == 1)
                    return alu_i(e, F_SUB, rd,
                                 u16of(a, o2.v, "immediate"));
                no_cond(a, c, "a three-operand add");
                return enc_fmt5(a, e, rd, rs1, -o2.v);
            }
        } else if (eat_p(a, "*")) {            /* rD = rS * 2 */
            o2 = ca_operand(a);
            need_end(a);
            if (o2.k != OP_IMM || o2.v != 2)
                fail(a, "the CAU can only multiply by 2 (rD = rS * 2)");
            return alu_r(e, F_ADD, rd, rs1, c, rs1);
        } else if (is_p(a, "<<") || is_p(a, ">>") || is_p(a, "$>>")) {
            unsigned f = is_p(a, "<<") ? F_SHL
                       : is_p(a, ">>") ? F_SHR : F_ASR;
            a->pos++;
            o2 = ca_operand(a);
            need_end(a);
            if (o2.k == OP_REG)
                return alu_r(e, f, rd, rs1, c, o2.reg);
            if (o2.k == OP_IMM) {
                if (rs1 == rd && c == 1) {     /* rD = rD << N */
                    if (o2.v < 0 || o2.v > 31)
                        fail(a, "shift count must be 0-31");
                    return alu_i(e, f, rd, (uint32_t)o2.v);
                }
                if (o2.v == 1)                 /* rD = rS << 1 */
                    return alu_r(e, f, rd, rs1, c, RC_PLUS);
                fail(a, c == 1
                     ? "immediate shifts need the destination as source"
                     : "a conditional shift must be by a register or 1");
            }
        } else if (is_p(a, "<<<") || is_p(a, ">>>")) {
            unsigned f = is_p(a, "<<<") ? F_ROL : F_ROR;
            a->pos++;
            if (cur(a)->t != TK_NUM || cur(a)->v != 1)
                fail(a, "rotates are always by 1 (<<<1 / >>>1)");
            a->pos++;
            need_end(a);
            return alu_r(e, f, rd, rs1, c, RC_R0);
        } else if (eat_p(a, "<<|")) {          /* 4b shift-or */
            int64_t v;
            no_cond(a, c, "shift-or");
            if (e == 0)
                fail(a, "shift-or has no (short) form");
            v = expr_eval(a, 0);
            need_end(a);
            return (0x24u << 26) | (rd << 21) | (rs1 << 16)
                 | u16of(a, v, "shift-or immediate");
        } else if (is_p(a, "#") || is_p(a, "&~") || is_p(a, "^")
                   || is_p(a, "|") || is_p(a, "&")) {
            unsigned f = is_p(a, "#") ? F_CRADD
                       : is_p(a, "&~") ? F_ANDC
                       : is_p(a, "^") ? F_XOR
                       : is_p(a, "|") ? F_OR : F_AND;
            a->pos++;
            o2 = ca_operand(a);
            need_end(a);
            if (o2.k == OP_REG)
                return alu_r(e, f, rd, rs1, c, o2.reg);
            if (o2.k == OP_IMM) {
                need_same(a, rd, &o1, op);
                no_cond(a, c, "an immediate logical op");
                return alu_i(e, f, rd, u16of(a, o2.v, "immediate"));
            }
        }
        fail(a, "cannot encode this operand combination");
        return 0;
    }
}

/* statements starting with '*': stores and Z-prefixed DA */
static uint32_t enc_store_stmt(A *a, unsigned c)
{
    no_cond(a, c, "a store");

    if (mem_is_direct(a)) {                    /* 7a: *L = (w) rH */
        uint32_t l;
        unsigned w, rh;
        int kw, is_u24;
        a->pos++;                              /* '*' */
        l = u16of(a, expr_eval(a, 0), "direct address");
        need_p(a, "=");
        kw = try_paren_kw(a, &is_u24);
        if (is_u24)
            fail(a, "(ushort24) is a load form");
        w = (kw < 0) ? 7u : (unsigned)kw;
        rh = need_reg5(a);
        need_end(a);
        return enc_mv_dir(1, w, rh, l);
    }

    {
        camem m = ca_mem(a);
        int kw, is_u24;
        unsigned w;
        need_p(a, "=");

        if (at_acc(a) && peek(a, 1)->t == TK_PUNCT
            && !strcmp(peek(a, 1)->s, "=")) {  /* Z = aN = …  (DA) */
            unsigned n = (unsigned)(cur(a)->s[1] - '0');
            unsigned z = camem_to_da(a, &m);
            a->pos += 2;
            return enc_da(a, n, z, 1);
        }

        kw = try_paren_kw(a, &is_u24);
        if (is_u24)
            fail(a, "(ushort24) is a load form");
        w = (kw < 0) ? 7u : (unsigned)kw;

        if (cur(a)->t == TK_ID && ior_code(cur(a)->s) >= 0) {
            unsigned ior = (unsigned)ior_code(cur(a)->s);   /* 7d store */
            a->pos++;
            need_end(a);
            return enc_mv_ind(1, 1, w, ior, &m);
        }
        {
            unsigned rh = need_reg5(a);        /* 7c store */
            need_end(a);
            return enc_mv_ind(0, 1, w, rh, &m);
        }
    }
}

/* statements with an io-register LHS (7b store / 7d load) */
static uint32_t enc_ior_stmt(A *a, unsigned c)
{
    unsigned ior = (unsigned)ior_code(cur(a)->s);
    int kw, is_u24;
    unsigned w;

    no_cond(a, c, "a move");
    a->pos++;
    need_p(a, "=");
    kw = try_paren_kw(a, &is_u24);
    if (is_u24)
        fail(a, "(ushort24) loads a CAU register");
    w = (kw < 0) ? 7u : (unsigned)kw;

    if (is_p(a, "*")) {                        /* 7d: iorH = (w) *rP */
        camem m = ca_mem(a);
        need_end(a);
        return enc_mv_ind(1, 0, w, ior, &m);
    }
    {
        unsigned rh = need_reg5(a);            /* 7b: iorH = (w) rH */
        need_end(a);
        return enc_mv_ior(1, w, rh, ior);
    }
}

/* ------------------------------------------------------------------ */
/* whole-instruction dispatch                                          */

static uint32_t encode_instr(A *a, uint32_t addr)
{
    unsigned c = 1;                            /* condition: true */

    /* fixed words */
    if (is_id(a, "nop"))     { a->pos++; need_end(a); return 0x80000000u; }
    if (is_id(a, "ireturn")) { a->pos++; need_end(a); return 0x803E0000u; }
    if (is_id(a, "waiti"))   { a->pos++; need_end(a); return 0x9DE0040Au; }
    if (is_id(a, "bkpt"))    { a->pos++; need_end(a); return 0x9D60040Au; }
    if (is_id(a, "sftrst"))  { a->pos++; need_end(a); return 0x9D00040Au; }

    if (is_id(a, "goto")) {
        btgt t;
        a->pos++;
        t = parse_target(a, addr);
        need_end(a);
        return enc_goto(a, 1, &t);
    }
    if (is_id(a, "call")) {
        btgt t;
        unsigned rm;
        a->pos++;
        t = parse_target(a, addr);
        need_p(a, "(");
        rm = need_reg5(a);
        need_p(a, ")");
        need_end(a);
        return enc_call(a, rm, &t);
    }
    if (is_id(a, "do") || is_id(a, "dolock") || is_id(a, "doblock")) {
        unsigned b = is_id(a, "dolock") ? 1u : 0u;
        unsigned m = is_id(a, "doblock") ? 1u : 0u;
        int64_t k = 0;
        uint32_t w = (0x23u << 26) | (b << 24) | (m << 23);
        a->pos++;
        if (!m || (cur(a)->t == TK_NUM
                   && peek(a, 1)->t == TK_PUNCT
                   && !strcmp(peek(a, 1)->s, ","))) {
            k = expr_eval(a, 1);
            need_p(a, ",");
        }
        if (k < 0 || k > 127)
            fail(a, "do instruction count must be 0-127");
        w |= (uint32_t)k << 11;
        {
            int r = try_reg5(a);
            if (r >= 0) {
                w |= (1u << 25) | (uint32_t)r;
            } else {
                int64_t l = expr_eval(a, 0);
                if (l < 0 || l > 2047)
                    fail(a, "do iteration count must be 0-2047");
                w |= (uint32_t)l;
            }
        }
        need_end(a);
        return w;
    }

    if (is_id(a, "if")) {
        a->pos++;
        need_p(a, "(");
        if (cur(a)->t == TK_ID && cond_code(cur(a)->s) >= 0) {
            c = (unsigned)cond_code(cur(a)->s);
            a->pos++;
            need_p(a, ")");
            if (is_id(a, "goto")) {
                btgt t;
                a->pos++;
                t = parse_target(a, addr);
                need_end(a);
                return enc_goto(a, c, &t);
            }
            /* falls through to the conditional ALU statement below */
        } else {
            /* 3a: if (rM-- >= 0) goto {N, rB±N, pc±N, label} */
            unsigned rm = need_reg5(a);
            btgt t;
            unsigned rb;
            need_p(a, "--");
            need_p(a, ">=");
            if (cur(a)->t != TK_NUM || cur(a)->v != 0)
                fail(a, "loop branches test '>= 0'");
            a->pos++;
            need_p(a, ")");
            if (!is_id(a, "goto"))
                fail(a, "expected 'goto'");
            a->pos++;
            t = parse_target(a, addr);
            need_end(a);
            rb = (t.kind == TGT_ABS) ? RC_R0
               : (t.kind == TGT_PCREL) ? RC_PC : t.rb;
            if (!fits_s16(t.n))
                fail(a, "loop branch displacement out of range");
            return (0x03u << 26) | (rm << 21) | (rb << 16)
                 | ((uint32_t)t.n & 0xFFFFu);
        }
    }

    /* DA without a Z store: aN = … */
    if (at_acc(a)) {
        unsigned n = (unsigned)(cur(a)->s[1] - '0');
        no_cond(a, c, "a DA instruction");
        a->pos++;
        need_p(a, "=");
        return enc_da(a, n, 0, 0);
    }

    /* memory-LHS stores (and Z-prefixed DA) */
    if (is_p(a, "*"))
        return enc_store_stmt(a, c);

    /* io-register LHS */
    if (cur(a)->t == TK_ID && ior_code(cur(a)->s) >= 0)
        return enc_ior_stmt(a, c);

    /* "(short) rS1 - rS2" compare, or register statement */
    {
        int is_u24;
        int pre_kw = try_paren_kw(a, &is_u24);
        if (is_u24)
            fail(a, "(ushort24) belongs after 'rD ='");
        return enc_reg_stmt(a, c, pre_kw, addr);
    }
}

/* ------------------------------------------------------------------ */
/* output                                                              */

static void put_byte(A *a, uint8_t b)
{
    if (a->pass == 2) {
        if ((uint64_t)a->loc - a->org >= a->cap)
            fail(a, "output buffer too small (at address 0x%x)", a->loc);
        a->out[a->loc - a->org] = b;
    }
    a->loc++;
    if (a->loc > a->hi)
        a->hi = a->loc;
}

static void emit32(A *a, uint32_t w)
{
    put_byte(a, (uint8_t)(w >> 24));
    put_byte(a, (uint8_t)(w >> 16));
    put_byte(a, (uint8_t)(w >> 8));
    put_byte(a, (uint8_t)w);
}

/* ------------------------------------------------------------------ */
/* DSP32 float encoding (bit-identical to the emulator's converter)    */

uint32_t dsp3210_asm_dsp32(double v)
{
    int exp;
    double m;
    long f;
    unsigned e;
    uint32_t mant24;

    if (v == 0.0 || !isfinite(v))
        return v > 0 ? 0x7FFFFFFFu : (isnan(v) || v == 0.0) ? 0
                                                            : 0x800000FFu;
    m = frexp(v, &exp);
    m *= 2.0;
    exp -= 1;                                /* |m| ∈ [1,2) */
    if (m >= 0) {
        f = (long)floor((m - 1.0) * 0x1p23 + 0.5);
        if (f == 1L << 23) { f = 0; exp++; }
        mant24 = (uint32_t)f;
    } else {
        if (m == -1.0) { m = -2.0; exp--; }
        f = (long)floor((m + 2.0) * 0x1p23 + 0.5);
        if (f == 1L << 23) { f = 0; exp--; }
        mant24 = 0x800000u | (uint32_t)f;
    }
    if (exp + 128 > 255)
        return v > 0 ? 0x7FFFFFFFu : 0x800000FFu;
    if (exp + 128 < 1)
        return 0;
    e = (unsigned)(exp + 128);
    return (mant24 << 8) | e;
}

/* ------------------------------------------------------------------ */
/* directives and lines                                                */

static void do_directive(A *a)
{
    const char *d = cur(a)->s;
    a->pos++;

    if (!strcmp(d, ".org")) {
        int64_t v = expr_eval(a, 1);
        need_end(a);
        if (v < a->org || v > 0xFFFFFFFFll)
            fail(a, ".org 0x%llx is before the origin 0x%x",
                 (long long)v, a->org);
        if ((uint32_t)v > a->loc) {
            while (a->loc < (uint32_t)v)
                put_byte(a, 0);
        } else {
            a->loc = (uint32_t)v;
        }
        return;
    }
    if (!strcmp(d, ".word")) {
        do {
            int64_t v = expr_eval(a, 0);
            if (v < -0x80000000ll || v > 0xFFFFFFFFll)
                fail(a, ".word value out of 32-bit range");
            emit32(a, (uint32_t)v);
        } while (eat_p(a, ","));
        need_end(a);
        return;
    }
    if (!strcmp(d, ".float")) {
        do {
            double f;
            int neg = eat_p(a, "-");
            if (cur(a)->t == TK_FNUM)      f = cur(a)->f;
            else if (cur(a)->t == TK_NUM)  f = (double)cur(a)->v;
            else {
                fail(a, ".float needs a numeric literal");
                return;
            }
            a->pos++;
            emit32(a, dsp3210_asm_dsp32(neg ? -f : f));
        } while (eat_p(a, ","));
        need_end(a);
        return;
    }
    if (!strcmp(d, ".space")) {
        int64_t v = expr_eval(a, 1);
        need_end(a);
        if (v < 0 || v > 0x1000000)
            fail(a, ".space size out of range");
        while (v-- > 0)
            put_byte(a, 0);
        return;
    }
    if (!strcmp(d, ".align")) {
        int64_t v = expr_eval(a, 1);
        need_end(a);
        if (v <= 0 || (v & (v - 1)))
            fail(a, ".align needs a power of two");
        while (a->loc & (uint32_t)(v - 1))
            put_byte(a, 0);
        return;
    }
    if (!strcmp(d, ".equ")) {
        char name[NAME_LEN];
        int64_t v;
        if (cur(a)->t != TK_ID)
            fail(a, ".equ needs a name");
        if (strlen(cur(a)->s) >= NAME_LEN)
            fail(a, "symbol name too long");
        strcpy(name, cur(a)->s);
        a->pos++;
        need_p(a, ",");
        v = expr_eval(a, 1);
        need_end(a);
        if (v < -0x80000000ll || v > 0xFFFFFFFFll)
            fail(a, ".equ value out of 32-bit range");
        sym_define(a, name, (uint32_t)v);
        return;
    }
    fail(a, "unknown directive '%s'", d);
}

static void process_line(A *a)
{
    /* labels: IDENT ':' (several may share a line) */
    while (cur(a)->t == TK_ID && cur(a)->s[0] != '.'
           && peek(a, 1)->t == TK_PUNCT && !strcmp(peek(a, 1)->s, ":")) {
        sym_define(a, cur(a)->s, a->loc);
        a->pos += 2;
    }
    if (at_end(a))
        return;
    if (cur(a)->t == TK_ID && cur(a)->s[0] == '.') {
        do_directive(a);
        return;
    }
    if (a->loc & 3)
        fail(a, "instruction at unaligned address 0x%x", a->loc);
    if (a->pass == 1) {
        a->loc += 4;                   /* every instruction is one word */
        if (a->loc > a->hi)
            a->hi = a->loc;
    } else {
        emit32(a, encode_instr(a, a->loc));
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */

/* The context is ~56 KB (mostly the symbol table), so it is not
 * memset wholesale: every array is guarded by its counter. */
static void ctx_init(A *a, uint8_t *out, size_t cap, dsp3210_asm_err *err)
{
    a->out = out;
    a->cap = cap;
    a->err = err;
    a->org = a->loc = a->hi = 0;
    a->pass = 0;
    a->nsyms = 0;
    a->ntk = a->pos = 0;
    a->line = 0;
    if (err) {
        err->line = 0;
        err->msg[0] = '\0';
    }
}

int dsp3210_assemble(const char *src, uint32_t org,
                     uint8_t *out, size_t cap, uint32_t *len,
                     dsp3210_asm_err *err)
{
    A ctx;
    A *a = &ctx;

    ctx_init(a, out, cap, err);
    if (setjmp(a->jb))
        return DSP3210_ASM_ERR;

    for (a->pass = 1; a->pass <= 2; a->pass++) {
        const char *p = src;
        a->org = org;
        a->loc = org;
        a->hi = org;
        a->line = 0;
        while (*p) {
            const char *nl = strchr(p, '\n');
            const char *end = nl ? nl : p + strlen(p);
            a->line++;
            tokenize(a, p, end);
            process_line(a);
            p = nl ? nl + 1 : end;
        }
    }
    if (len)
        *len = a->hi - org;
    return DSP3210_ASM_OK;
}

int dsp3210_encode(const char *stmt, uint32_t addr, uint32_t *word,
                   dsp3210_asm_err *err)
{
    A ctx;
    A *a = &ctx;

    ctx_init(a, NULL, 0, err);
    a->pass = 2;
    a->line = 1;
    a->org = addr;
    a->loc = addr;
    if (setjmp(a->jb))
        return DSP3210_ASM_ERR;

    tokenize(a, stmt, stmt + strlen(stmt));
    if (at_end(a))
        fail(a, "empty statement");
    if (is_id(a, ".word")) {
        int64_t v;
        a->pos++;
        v = expr_eval(a, 0);
        need_end(a);
        if (v < -0x80000000ll || v > 0xFFFFFFFFll)
            fail(a, ".word value out of 32-bit range");
        *word = (uint32_t)v;
        return DSP3210_ASM_OK;
    }
    *word = encode_instr(a, addr);
    return DSP3210_ASM_OK;
}
