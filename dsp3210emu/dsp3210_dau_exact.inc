/*
 * dsp3210_dau_exact.inc — the exact DAU: integer 40-bit accumulators.
 *
 * Not compiled on its own: dsp3210_emu.c #includes exactly one of
 * dsp3210_dau_double.inc / dsp3210_dau_exact.inc (this one when
 * DSP3210_DAU_EXACT is defined), so the DAU implementation composes
 * with the shared core in a single translation unit and everything
 * stays static.
 *
 * This DAU models the datapath exactly, in integers (accumulator slot
 * member `.x`): 40-bit accumulators, an exact 25x25-bit multiplier, a
 * truncating adder, automatic normalisation, and the documented
 * rounding and saturation rules.  No host floating point in the data
 * path.  See README "Which DAU?".
 */

/* ------------------------------------------------------------------ */
/* DSP32 floating point — exact integer model                          */
/*
 * 32-bit word = mantissa[31:8] | exponent[7:0]; the mantissa is a single
 * 2's-complement quantity s(!s).f…f, so M in [1,2) or [-2,-1), and
 * value = M * 2^(e-128).  e = 0 is zero (incl. "dirty zeros").
 * 40-bit accumulator = mantissa[39:16] | guard[15:8] | exponent[7:0],
 * i.e. the same mantissa with 8 extra fraction bits [IM Fig 8-8].
 *
 * dsp3210_acc.m holds the mantissa *including the implicit bit* as a
 * fixed-point integer with 31 fractional bits (24 mantissa + 8 guard - 1
 * for the implicit bit's integer position), so
 *      value = m * 2^(e-128) / 2^31.
 *
 * Internally the multiplier's product is carried at 46 fractional bits
 * (23+23, exact for a 25x25-bit signed multiply) and the adder aligns,
 * adds and truncates at that width before the result is truncated back
 * to the accumulator's 31 fractional bits [IM §8.2.3: "three integer
 * bits are retained in P", "the adder inputs and result contain eight
 * mantissa guard bits"].
 */

#define SC_ACC 31                       /* accumulator fraction bits */
#define SC_MUL 46                       /* product fraction bits */
#define ACC_ONE  (INT64_C(1) << SC_ACC) /* |m| for M = 1 */

/*
 * Left-shift a signed 64-bit value by k.  Shifting a negative signed
 * value left is undefined in C99, and this file claims to be portable
 * C99, so route it through the unsigned type: the two's-complement bit
 * pattern is what the datapath shifts.  (The arithmetic RIGHT shifts
 * elsewhere are implementation-defined rather than undefined, and are
 * deliberate — they are the adder's truncation.)
 */
static int64_t shl64(int64_t v, int k)
{
    return (int64_t)((uint64_t)v << k);
}

static dsp3210_acc acc_zero(void)
{
    dsp3210_acc r;
    r.m = 0;
    r.e = 0;
    return r;
}

/*
 * Normalise so that M is in [1,2) or [-2,-1), i.e. at fraction scale
 * `sc`: m in [2^sc, 2^(sc+1)) for positive, [-2^(sc+1), -2^sc) for
 * negative.  Note -2^sc itself (M = -1) is NOT normal — it renormalises
 * to -2^(sc+1) with e-1, which is why -1.0 encodes as -2 * 2^-1.
 * Right shifts truncate (arithmetic), which is what the hardware does.
 */
static void norm_at(int64_t *m, int *e, int sc)
{
    int64_t lo = INT64_C(1) << sc, hi = lo << 1;
    if (*m == 0) { *e = 0; return; }
    while (*m >= hi || *m < -hi) { *m >>= 1; (*e)++; }
    while ((*m > 0 && *m < lo) || (*m < 0 && *m >= -lo))
        { *m = shl64(*m, 1); (*e)--; }
}

/* The 24-bit stored mantissa field of a 32-bit word, as the full
 * 2's-complement value including the implicit bit (23 fraction bits). */
static int64_t mant24_full(uint32_t w)
{
    int32_t raw = (int32_t)(w & 0xFFFFFF00u) >> 8;   /* signed 24-bit */
    return (int64_t)raw + (raw < 0 ? -(INT64_C(1) << 23)
                                   :  (INT64_C(1) << 23));
}

dsp3210_acc dsp3210_acc_unpack(uint32_t w)
{
    dsp3210_acc r;
    if ((w & 0xFFu) == 0)
        return acc_zero();                   /* incl. dirty zeros */
    r.m = shl64(mant24_full(w), 8);          /* 23 -> 31 fraction bits */
    r.e = (int16_t)(w & 0xFFu);
    return r;
}

uint32_t dsp3210_acc_pack(dsp3210_acc a)
{
    int64_t m24;
    uint32_t raw;
    if (a.e == 0 || a.m == 0)
        return 0;
    m24 = a.m >> 8;                          /* truncate the guard bits */
    raw = (uint32_t)(m24 - (m24 < 0 ? -(INT64_C(1) << 23)
                                    :  (INT64_C(1) << 23)));
    return ((raw & 0xFFFFFFu) << 8) | (uint32_t)(a.e & 0xFF);
}

/* The stored mantissa+guard field (accumulator bits 39-8): the value
 * with the implicit bit removed. */
static void dsp3210_acc_raw_of(dsp3210_acc a, int64_t *mant_guard)
{
    *mant_guard = (a.e == 0) ? 0
                : (a.m - (a.m < 0 ? -(INT64_C(1) << SC_ACC)
                                  :  (INT64_C(1) << SC_ACC))) & 0xFFFFFFFFu;
}

double dsp3210_acc_double(dsp3210_acc a)
{
    if (a.e == 0)
        return 0.0;
    return ldexp((double)a.m, (int)a.e - 128 - SC_ACC);
}

dsp3210_acc dsp3210_acc_from_double(double v)
{
    dsp3210_acc r;
    int exp;
    double f;
    if (v == 0.0 || !isfinite(v))
        return acc_zero();
    /* A host double stands in for a 32-bit DSP32 datum, so round to the
     * 24-bit mantissa and leave the guard bits zero — exactly what
     * loading a memory operand into an accumulator gives. */
    f = frexp(v, &exp);                      /* v = f * 2^exp, |f|∈[.5,1) */
    {   int e = exp - 1 + 128;
        /* Ties go to the greater value (toward +inf), matching the DSP's
         * own `round` instruction and the sibling core's helper, so both
         * cores encode host test data bit-identically. */
        int64_t m = (int64_t)floor(f * ldexp(1.0, 24) + 0.5);  /* scale 23 */
        norm_at(&m, &e, 23);
        if (e > 255) {
            r.m = (v < 0) ? -(INT64_C(1) << 32) : (INT64_C(1) << 32) - 256;
            r.e = 255;
            return r;
        }
        if (e < 1)
            return acc_zero();
        r.m = shl64(m, 8);                   /* scale 23 -> 31, guard = 0 */
        r.e = (int16_t)e;
    }
    return r;
}

/* legacy helpers, kept so rigs and tests can build against either core */
double dsp3210_dsp32_to_double(uint32_t w)
{
    return dsp3210_acc_double(dsp3210_acc_unpack(w));
}

uint32_t dsp3210_double_to_dsp32(double v)
{
    if (v != 0.0 && isfinite(v)) {
        int exp;
        frexp(v, &exp);
        if (exp - 1 + 128 > 255)             /* saturate [IM §8.2.4.1] */
            return v > 0 ? 0x7FFFFFFFu : 0x800000FFu;
    }
    return dsp3210_acc_pack(dsp3210_acc_from_double(v));
}

/* ---- the multiplier: exact 25x25 -> 50-bit signed product ---- */
/* Inputs are 32-bit words (an accumulator feeding the multiplier has
 * already had its guard bits truncated by acc_pack) [IM §8.2.3]. */
static void dau_mul(uint32_t xw, uint32_t yw, int64_t *pm, int *pe)
{
    unsigned ex = xw & 0xFFu, ey = yw & 0xFFu;
    if (ex == 0 || ey == 0) { *pm = 0; *pe = 0; return; }
    *pm = mant24_full(xw) * mant24_full(yw);     /* scale 46 */
    *pe = (int)ex + (int)ey - 128;
}

/* ---- the adder: S + P, exact align/add, truncate to 24+8 bits ---- */
static dsp3210_acc dau_add(dsp3210_acc s, int64_t pm, int pe)
{
    int64_t sm, rm;
    int se, re;

    sm = (s.e == 0) ? 0 : shl64(s.m, SC_MUL - SC_ACC);  /* 31 -> 46 bits */
    se = s.e;

    if (sm == 0 && pm == 0)
        return acc_zero();
    if (sm == 0)        { rm = pm; re = pe; }
    else if (pm == 0)   { rm = sm; re = se; }
    else if (se >= pe) {
        int d = se - pe;
        rm = sm + (d > 62 ? 0 : (pm >> d));
        re = se;
    } else {
        int d = pe - se;
        rm = pm + (d > 62 ? 0 : (sm >> d));
        re = pe;
    }
    norm_at(&rm, &re, SC_MUL);
    if (rm == 0)
        return acc_zero();
    {   dsp3210_acc r;
        r.m = rm >> (SC_MUL - SC_ACC);       /* truncate to 24+8 bits */
        r.e = (int16_t)re;
        {   int e = re;
            norm_at(&r.m, &e, SC_ACC);       /* no-op in practice */
            r.e = (int16_t)e;
        }
        return r;
    }
}

static dsp3210_acc acc_negate(dsp3210_acc a)
{
    int e = a.e;
    if (a.e == 0)
        return a;
    a.m = -a.m;
    norm_at(&a.m, &e, SC_ACC);               /* -(-2) = 2 needs e+1 */
    a.e = (int16_t)e;
    return a;
}

/* round(): 40-bit -> 32-bit, round to nearest, ties to the greater
 * value, guard bits cleared [IM ROUND page] */
static dsp3210_acc acc_round(dsp3210_acc a)
{
    int e = a.e;
    if (a.e == 0)
        return a;
    a.m = shl64((a.m + 128) >> 8, 8);        /* +2^7 then truncate */
    norm_at(&a.m, &e, SC_ACC);
    a.e = (int16_t)e;
    return a;
}

/* exact integer -> float */
static dsp3210_acc acc_from_i64(int64_t v)
{
    dsp3210_acc r;
    int e = 128 + SC_ACC;
    if (v == 0)
        return acc_zero();
    r.m = v;
    norm_at(&r.m, &e, SC_ACC);
    r.e = (int16_t)e;
    return r;
}

/*
 * exact float -> integer, with the dauc[5:4] rounding modes and
 * saturation [IM §8.2.5, INT16/INT32 pages].  `bits` is 8, 16 or 32.
 */
static int64_t acc_to_int(dsp3210_acc a, unsigned dauc, int bits)
{
    int mode = (dauc >> 4) & 3;
    int shift;
    int64_t i, frac_mask, lo, hi;

    if (bits == 8) { lo = 0; hi = 255; }
    else if (bits == 16) { lo = -32768; hi = 32767; }
    else { lo = INT32_MIN; hi = INT32_MAX; }

    if (a.e == 0)
        return 0;
    shift = (int)a.e - 128 - SC_ACC;         /* value = m * 2^shift */
    if (shift >= 0) {                        /* integral; may overflow */
        if (shift > 40)
            return a.m < 0 ? lo : hi;
        i = shl64(a.m, shift > 24 ? 24 : shift);
        if (shift > 24)
            return a.m < 0 ? lo : hi;
    } else {
        int sh = -shift;
        if (sh > 63) {
            /* |value| < 2^-32: the whole mantissa shifts out.  The
             * rounding mode still decides the result — floor gives -1
             * for any negative, nearest and truncate-towards-zero give
             * 0 [IM §8.2.5] — so do NOT short-circuit past it. */
            i = (mode == 1 && a.m < 0) ? -1 : 0;
        } else {
            /* sh can be 63 here, so build the mask in unsigned
             * arithmetic: 1 << 63 is undefined for a signed int64. */
            frac_mask = (int64_t)((UINT64_C(1) << sh) - 1u);
            i = a.m >> sh;                   /* floor */
            if (!(mode & 1)) {               /* x0: round to nearest,
                                                ties up = floor(v + 1/2) */
                if ((a.m >> (sh - 1)) & 1)
                    i += 1;
            } else if (mode == 3) {          /* truncate towards zero */
                if (a.m < 0 && (a.m & frac_mask))
                    i += 1;
            }
            /* mode 1 (truncate towards -inf) is plain floor */
        }
    }
    if (i < lo) return lo;
    if (i > hi) return hi;
    return i;
}

/* ---- IEEE 754 single <-> DSP32, exact, no host float involved ---- */
static uint32_t acc_to_ieee(dsp3210_acc a)
{
    int64_t m = a.m;
    int e = (int)a.e - 128;                  /* value = M * 2^e */
    uint32_t sign = 0, frac;
    if (a.e == 0 || m == 0)
        return 0;                            /* DSP32 zero -> +0.0 */
    if (m < 0) {
        sign = 0x80000000u;
        m = -m;                              /* |M| in (1, 2] */
        if (m == (INT64_C(1) << 32)) { m >>= 1; e += 1; }  /* |M| == 2 */
    }
    /* now m in [2^31, 2^32): 1.f with 31 fraction bits; IEEE keeps 23 */
    frac = (uint32_t)((m - (INT64_C(1) << 31)) >> 8);   /* truncate */
    e += 127;                                /* IEEE bias: E = dspexp + 127 */
    if (e >= 255) return sign | 0x7F800000u; /* out of IEEE range */
    if (e <= 0)   return sign;               /* flush denormals to zero */
    return sign | ((uint32_t)e << 23) | (frac & 0x7FFFFFu);
}

static dsp3210_acc acc_from_ieee(uint32_t w)
{
    unsigned sgn = w >> 31, exp = (w >> 23) & 0xFFu;
    uint32_t frac = w & 0x7FFFFFu;
    dsp3210_acc r;
    int e;
    if (exp == 0)
        return acc_zero();                   /* zero and denormals */
    r.m = (INT64_C(1) << 31) | ((int64_t)frac << 8);
    e = (int)exp - 127 + 128;      /* dspexp = E - 127 */
    if (sgn) {
        r.m = -r.m;
        norm_at(&r.m, &e, SC_ACC);
    }
    if (e > 255) { r.m = sgn ? -(INT64_C(1) << 32) : (INT64_C(1) << 32) - 1;
                   e = 255; }
    if (e < 1)
        return acc_zero();
    r.e = (int16_t)e;
    return r;
}

/* ------------------------------------------------------------------ */
/* DAU flags + clip-test history                                       */

/*
 * Flags and the clip-test history from an exact result.  Overflow and
 * underflow are judged against the 40-bit range: |value| <= (2-2^-31) *
 * 2^127 and >= 1 * 2^-127, i.e. the exponent leaving [1,255] after
 * normalisation [IM §8.2.1, DOC §1.5.6].
 */
static dsp3210_acc da_flags(dsp3210_emu *s, dsp3210_acc res, int affect_vu)
{
    unsigned N, Z, V = 0, U = 0;
    if (affect_vu && res.e != 0) {
        if (res.e > 255) {
            V = 1;
            res.m = res.m < 0 ? -(INT64_C(1) << 32) : (INT64_C(1) << 32) - 1;
            res.e = 255;                     /* saturate */
        } else if (res.e < 1) {
            U = 1;
            res = acc_zero();                /* flush to zero */
        }
    }
    N = res.e != 0 && res.m < 0;
    Z = res.e == 0;
    s->ps = (uint16_t)((s->ps & ~0xF0u)
          | (N ? DSP3210_PS_N : 0) | (Z ? DSP3210_PS_Z : 0)
          | (U ? DSP3210_PS_U : 0) | (V ? DSP3210_PS_V : 0));
    s->ctr = (uint8_t)(((s->ctr << 1) | N) & 0x3F);  /* [IM Table 8-4] */
    if (V || U)
        take_error(s, DSP3210_VEC_UV);       /* masked unless emr[5] */
    return res;
}

/* ------------------------------------------------------------------ */
/* DA operands (7-bit X/Y/Z fields) [IM Table 10-3; DOC §1.5.3]        */

typedef struct {
    int      from_acc;   /* 1: accumulator (or reserved field) */
    unsigned acc;        /* accumulator number when from_acc */
    uint32_t raw;        /* raw bits read when memory */
    dsp3210_acc val;     /* the accumulator itself when from_acc */
} da_opnd;

static void da_postmod(dsp3210_emu *s, unsigned p, unsigned i, int size)
{
    switch (i & 7) {
    case 0: break;
    case 6: s->r[p] -= (uint32_t)size; break;
    case 7: s->r[p] += (uint32_t)size; break;
    default: s->r[p] += s->r[14 + (i & 7)]; break;   /* r15-r19, unscaled */
    }
}

/* Read an X/Y operand of the given size.  Returns -1 on a memory fault
 * (exception already raised).  Memory words are NOT interpreted here —
 * the caller converts .raw as float/int/byte as the instruction needs;
 * .val is filled with the accumulator value for register-direct fields. */
static int da_read(dsp3210_emu *s, unsigned f, int size, da_opnd *o)
{
    unsigned p = (f >> 3) & 15, i = f & 7;
    memset(o, 0, sizeof *o);
    if (p == 0 || p == 15) {           /* register direct (or reserved) */
        o->from_acc = 1;
        o->acc = i & 3;
        o->val = s->a[i & 3].x;
        o->raw = dsp3210_acc_pack(s->a[i & 3].x);  /* guard bits truncated,
                                        as when an accumulator feeds the
                                        multiplier [IM §8.2.3] */
        return 0;
    }
    if (mem_read(s, s->r[p], size, &o->raw))
        return -1;
    da_postmod(s, p, i, size);
    return 0;
}

/* "No write" Z spellings: manual uses p=0000,i=111; Apple's assembler
 * uses p=1111,i=111 — both accepted (README of dsp3210dis). */
static int da_z_is_write(unsigned z)
{
    unsigned p = (z >> 3) & 15, i = z & 7;
    if (i == 7 && (p == 0 || p == 15))
        return 0;
    return p != 0;
}

static int da_write(dsp3210_emu *s, unsigned f, int size, uint32_t val)
{
    unsigned p = (f >> 3) & 15, i = f & 7;
    if (p == 0 || p == 15)
        return 0;                      /* no write */
    if (mem_write(s, s->r[p], size, val))
        return -1;
    da_postmod(s, p, i, size);
    return 0;
}

/* Perform a Z field's address post-modification WITHOUT storing.  The
 * ifalt/ifaeq/ifagt pages are explicit: with dauc[6] = 1 the memory
 * write is conditional "but the address postmodifications are always
 * performed". */
static void da_postmod_only(dsp3210_emu *s, unsigned f, int size)
{
    unsigned p = (f >> 3) & 15, i = f & 7;
    if (p == 0 || p == 15)
        return;
    da_postmod(s, p, i, size);
}

/* An operand feeding the ADDER keeps full 40-bit precision when it comes
 * from an accumulator; a memory operand has zero guard bits. */
static dsp3210_acc da_adder(const da_opnd *o)
{
    return o->from_acc ? o->val : dsp3210_acc_unpack(o->raw);
}

/* An operand feeding the MULTIPLIER is always 32-bit: an accumulator's
 * guard bits are truncated first [IM §8.2.3]. */
static uint32_t da_mult(const da_opnd *o)
{
    return o->raw;
}

/* ------------------------------------------------------------------ */
/* companded conversions (ic/oc) [IM §8.2.4.2]
 *
 * µ-law byte (bit 0 first): ~m0 ~m1 ~m2 ~m3 ~n0 ~n1 ~n2 ~s
 *   Y = (-1)^s * ((16.5+M)*2^N - 16.5)
 * A-law byte:               ~m0 m1 ~m2 m3 ~n0 n1 ~n2 ~s   (XOR 0xD5)
 *   Y = (-1)^s * (16.5+M)*2^N   (N>=1);   (-1)^s * (0.5+M)*2   (N=0)
 */

/* Exact integer decode.  The formulas produce half-integers for mu-law
 * ((16.5+M)*2^N - 16.5), so these return TWICE the value. */
static int64_t mulaw_decode_2x(unsigned b)
{
    unsigned u = ~b & 0xFFu;
    unsigned M = u & 15, N = (u >> 4) & 7, S = (u >> 7) & 1;
    int64_t y = (int64_t)(33 + 2 * M) * ((int64_t)1 << N) - 33;
    return S ? -y : y;
}

static int64_t alaw_decode_2x(unsigned b)
{
    unsigned u = (b ^ 0xD5u) & 0xFFu;
    unsigned M = u & 15, N = (u >> 4) & 7, S = (u >> 7) & 1;
    int64_t y = N ? (int64_t)(33 + 2 * M) * ((int64_t)1 << N)
                  : (int64_t)(1 + 2 * M) * 2;
    return S ? -y : y;
}

/* Nearest-code encode, compared on the exact doubled integers so no host
 * float rounding enters.  256 candidates; performance is a non-goal. */
static int64_t acc_to_int(dsp3210_acc a, unsigned dauc, int bits);

static unsigned companded_encode_acc(dsp3210_acc v, int alaw)
{
    dsp3210_acc t = v;
    int64_t v2;
    unsigned b, best = 0;
    int64_t bestd = INT64_MAX;
    if (t.e) t.e = (int16_t)(t.e + 1);       /* * 2 exactly */
    v2 = acc_to_int(t, 0 /* round to nearest */, 32);
    for (b = 0; b < 256; b++) {
        int64_t d = (alaw ? alaw_decode_2x(b) : mulaw_decode_2x(b)) - v2;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = b; }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* DA special functions (format 5) [IM Table 4-4]                      */

enum {
    G_IC = 0, G_OC = 1, G_FLOAT16 = 2, G_INT16 = 3, G_ROUND = 4,
    G_IFALT = 5, G_IFAEQ = 6, G_IFAGT = 7, G_FLOAT32 = 8, G_INT32 = 9,
    G_IEEE = 12, G_DSP = 13, G_SEED = 14
};


static int exec_da_special(dsp3210_emu *s, uint32_t w)
{
    unsigned g = bits(w, 26, 23);
    unsigned n = bits(w, 22, 21);
    unsigned y = bits(w, 13, 7);
    unsigned z = bits(w, 6, 0);
    int zw = da_z_is_write(z);
    int ysz = (g == G_IC) ? 1 : (g == G_FLOAT16) ? 2 : 4;
    int zsz = (g == G_OC) ? 1 : (g == G_INT16) ? 2 : 4;
    da_opnd Y;
    uint32_t zbits = 0;
    dsp3210_acc res = s->a[n].x;

    if (da_read(s, y, ysz, &Y))
        return DSP3210_STEP_OK;

    /* The special-function result reaches memory from the DAU's own S
     * register, which is why the manual requires the Z write to be
     * issued in the same instruction.  For the integer/companded/IEEE
     * conversions the accumulator's own bits are then "unpredictable";
     * this model leaves the numeric value there (documented deviation),
     * while the Z bits are exact. */
    switch (g) {
    case G_IC: {                               /* byte -> float, NZ00 */
        unsigned code = Y.from_acc
                      ? (unsigned)((dsp3210_acc_pack(Y.val) >> 24) & 0xFFu)
                      : (Y.raw & 0xFFu);
        unsigned conv = s->dauc & 15;
        if (conv & 4) {                        /* x1xx unsigned linear in */
            res = acc_from_i64((int64_t)code);
        } else {                               /* mu-law / A-law in: the
                                        decode is a half-integer, so build
                                        it from 2*value and halve exactly */
            int64_t twice = (conv & 1) ? alaw_decode_2x(code)
                                       : mulaw_decode_2x(code);
            res = acc_from_i64(twice);
            if (res.e) res.e = (int16_t)(res.e - 1);
        }
        res = da_flags(s, res, 0);
        zbits = dsp3210_acc_pack(res);
        break;
    }
    case G_OC: {                               /* float -> byte, no flags */
        unsigned conv = s->dauc & 15, code;
        if (conv & 8) {                        /* 1xxx unsigned linear out */
            int64_t t = acc_to_int(da_adder(&Y), s->dauc, 8);
            code = (unsigned)t;
        } else {                               /* mu-law / A-law out */
            code = companded_encode_acc(da_adder(&Y), (conv & 2) != 0);
        }
        res = acc_from_i64((int64_t)code);
        zbits = code;
        break;
    }
    case G_FLOAT16: {                          /* int16 -> float, NZ00 */
        int64_t iv = Y.from_acc
                   ? (int64_t)(int16_t)(dsp3210_acc_pack(Y.val) >> 16)
                   : (int64_t)(int16_t)(Y.raw & 0xFFFFu);
        res = da_flags(s, acc_from_i64(iv), 0);
        zbits = dsp3210_acc_pack(res);
        break;
    }
    case G_FLOAT32: {                          /* int32 -> float, NZ00 */
        /* An accumulator's integer lane is bits 39-8 — mantissa AND
         * guard bits, symmetric with where INT32 deposits its result —
         * not the packed mantissa||exponent word. */
        int64_t iv;
        if (Y.from_acc) {
            int64_t mg;
            dsp3210_acc_raw_of(Y.val, &mg);
            iv = (int64_t)(int32_t)(uint32_t)mg;
        } else {
            iv = (int64_t)(int32_t)Y.raw;
        }
        res = da_flags(s, acc_from_i64(iv), 0);
        zbits = dsp3210_acc_pack(res);
        break;
    }
    case G_INT16: {                            /* float -> int16, no flags */
        int64_t iv = acc_to_int(da_adder(&Y), s->dauc, 16);
        res = acc_from_i64(iv);
        zbits = (uint32_t)iv & 0xFFFFu;
        break;
    }
    case G_INT32: {                            /* float -> int32, no flags */
        int64_t iv = acc_to_int(da_adder(&Y), s->dauc, 32);
        res = acc_from_i64(iv);
        zbits = (uint32_t)iv;
        break;
    }
    case G_ROUND: {                            /* float40 -> float32, NZVU */
        res = da_flags(s, acc_round(da_adder(&Y)), 1);
        zbits = dsp3210_acc_pack(res);
        break;
    }
    case G_IFALT:
    case G_IFAEQ:
    case G_IFAGT: {                            /* conditional load */
        unsigned AN = !!(s->ps & DSP3210_PS_N), AZ = !!(s->ps & DSP3210_PS_Z);
        int cond = (g == G_IFALT) ? AN
                 : (g == G_IFAEQ) ? AZ
                                  : !(AN | AZ);
        if (cond)
            res = da_adder(&Y);
        zbits = dsp3210_acc_pack(res);
        if (zw && (s->dauc & 0x40u) && !cond) {
            /* dauc[6] = 1: the store is conditional, but the pointer
             * post-modification still happens [IM IFALT/IFAEQ/IFAGT] */
            da_postmod_only(s, z, zsz);
            zw = 0;
        }
        break;
    }
    case G_IEEE: {                             /* DSP32 -> IEEE, no flags */
        dsp3210_acc yv = da_adder(&Y);
        zbits = acc_to_ieee(yv);
        res = yv;
        break;
    }
    case G_DSP: {                              /* IEEE -> DSP32, NZVU/NaN */
        uint32_t iw = Y.from_acc ? dsp3210_acc_pack(Y.val) : Y.raw;
        unsigned sgn = iw >> 31, exp = (iw >> 23) & 0xFFu;
        uint32_t mant = iw & 0x7FFFFFu;
        if (exp == 255) {                      /* +-inf / NaN [DSP page] */
            zbits = sgn ? 0x800000FFu : 0x7FFFFFFFu;
            res = dsp3210_acc_unpack(zbits);
            s->ps = (uint16_t)((s->ps & ~0xF0u)
                  | (sgn ? DSP3210_PS_N : 0)
                  | ((!sgn || mant) ? DSP3210_PS_V : 0));
            s->ctr = (uint8_t)(((s->ctr << 1) | (sgn ? 1 : 0)) & 0x3F);
            if (mant)
                take_error(s, DSP3210_VEC_NAN);
        } else if (exp == 0) {                 /* zero / denormal -> +0 */
            res = acc_zero();
            s->ps = (uint16_t)((s->ps & ~0xF0u) | DSP3210_PS_Z
                  | (mant ? DSP3210_PS_U : 0));
            s->ctr = (uint8_t)((s->ctr << 1) & 0x3F);
            zbits = 0;
        } else {
            res = da_flags(s, acc_from_ieee(iw), 1);
            zbits = dsp3210_acc_pack(res);
        }
        break;
    }
    case G_SEED: {                             /* reciprocal seed, NZU V=0 */
        uint32_t yb = Y.from_acc ? dsp3210_acc_pack(Y.val) : Y.raw;
        uint32_t sb = yb ^ 0x7FFFFFFFu;        /* invert all but the sign */
        /* SEED page: "DAU FLAGS AFFECTED: NZU, V=0" — inverting an
         * exponent of $FF yields 0, i.e. an underflow to zero */
        res = da_flags(s, dsp3210_acc_unpack(sb), 0);
        if ((yb & 0xFFu) == 0xFFu && (sb & 0xFFu) == 0)
            s->ps |= DSP3210_PS_U;
        zbits = sb;
        break;
    }
    default:                                   /* reserved G codes */
        return DSP3210_STEP_OK;
    }

    s->a[n].x = res;
    if (zw)
        da_write(s, z, zsz, zbits);
    return DSP3210_STEP_OK;
}

/* ------------------------------------------------------------------ */
/* DA multiply/accumulate (formats 1-4) [IM Table 4-2]                 */

static int exec_da(dsp3210_emu *s, uint32_t w)
{
    unsigned fmt = bits(w, 31, 29);
    unsigned m   = bits(w, 28, 26);
    unsigned n   = bits(w, 22, 21);
    unsigned xf  = bits(w, 20, 14);
    unsigned yf  = bits(w, 13, 7);
    unsigned zf  = bits(w, 6, 0);
    unsigned fs  = bits(w, 24, 24);    /* sign before the adder operand */
    unsigned ss  = bits(w, 23, 23);    /* sign of the product term */
    int zw = da_z_is_write(zf);
    da_opnd X, Y;
    dsp3210_acc adder, res;
    int64_t pm;
    int pe, tap;

    static const uint32_t ONE = 0x00000080u;   /* 1.0 as a DSP32 word */

    if (fmt == 3 && m >= 6)
        return exec_da_special(s, w);

    /* Operand fetch order is X then Y: the X and Y registers are loaded
     * in machine states 1 and 2 of the same instruction cycle
     * [IM §8.2.6, Figure 8-12].  It only matters when one pointer
     * register is post-modified in both fields (which the assembler
     * rejects), but there is no reason not to match the hardware. */
    if (da_read(s, xf, 4, &X))
        return DSP3210_STEP_OK;
    if (da_read(s, yf, 4, &Y))
        return DSP3210_STEP_OK;

    if (fmt == 1 && m == 6) {
        /* FADD-TAP: aN = [-](Z=Y) {+,-} X */
        adder = da_adder(&Y);
        dau_mul(ONE, da_mult(&X), &pm, &pe);
        tap = 1;
    } else if (fmt == 1) {
        /* FMULT-ADD-STORE family: aN = [-]Y {+,-} {aM,0,1} * X.
         * The adder input Y keeps its guard bits when it is an
         * accumulator; the multiplier inputs never do. */
        adder = da_adder(&Y);
        if (m == 4)
            { pm = 0; pe = 0; }                    /* 0.0 * X */
        else
            dau_mul(m == 5 ? ONE : dsp3210_acc_pack(s->a[m].x),
                    da_mult(&X), &pm, &pe);
        tap = 0;
    } else {
        /* fmt 2 (tap) / fmt 3 (store): aN = [-]{aM,0,1} {+,-} Y*X */
        if (m == 4)
            adder = acc_zero();
        else if (m == 5)
            adder = dsp3210_acc_unpack(ONE);
        else
            adder = s->a[m].x;
        dau_mul(da_mult(&Y), da_mult(&X), &pm, &pe);
        tap = (fmt == 2);
    }

    if (fs)
        adder = acc_negate(adder);
    if (ss)
        pm = -pm;                                  /* exact at scale 46 */

    res = da_flags(s, dau_add(adder, pm, pe), 1);
    s->a[n].x = res;

    if (zw) {
        uint32_t zbits = tap ? da_mult(&Y)        /* the tap passes Y on */
                             : dsp3210_acc_pack(res);
        da_write(s, zf, 4, zbits);
    }
    return DSP3210_STEP_OK;
}

double dsp3210_acc_get(const dsp3210_emu *s, int n)
{
    return dsp3210_acc_double(s->a[n & 3].x);
}

void dsp3210_acc_set(dsp3210_emu *s, int n, double v)
{
    s->a[n & 3].x = dsp3210_acc_from_double(v);
}

void dsp3210_acc_raw(const dsp3210_emu *s, int n, int64_t *mant_guard,
                     int *exp)
{
    dsp3210_acc a = s->a[n & 3].x;
    if (mant_guard) {
        /* the stored bits 39-8: the implicit bit is not stored, so the
         * field is the value minus (+/-)1 at the mantissa's scale */
        int64_t m = a.m;
        *mant_guard = (a.e == 0) ? 0
                    : (m - (m < 0 ? -(INT64_C(1) << SC_ACC)
                                  :  (INT64_C(1) << SC_ACC))) & 0xFFFFFFFFu;
    }
    if (exp)
        *exp = a.e;
}

