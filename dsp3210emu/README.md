# dsp3210emu - reference emulator core for the AT&T DSP3210

A small, dependency-free interpreter for the AT&T DSP3210 (the DSP in the
Macintosh Quadra 840AV / Centris 660AV), written as a **reference model**
for use in tests and as ground truth for a real emulator: every semantic
decision is traceable to the AT&T *DSP3210 Information Manual* (chapter 4
instruction pages, chapter 7 processor states, chapter 8 DAU, chapter 10
encodings), cross-checked against hardware-verified behaviour recovered
from real DSP3210 systems and against the companion reference
disassembler `dsp3210dis`.

Performance is a non-goal.  The interpreter is the simplest possible
shape - one switch on the 6-bit top-level opcode - and the companded
encoder literally searches all 256 codes.

## One core, two DAUs

The decode, the CAU (integer/branch/move) instructions, the exception
model and the memory map live once, in `dsp3210_emu.c`.  The DAU (the
floating-point datapath) has **two drop-in implementations**, textually
included by the core so each builds as a single translation unit; the
Makefile compiles the core twice:

| Build | DAU | When to use it |
|---|---|---|
| `libdsp3210emu.a`, `dsp3210emu`, (default) | **approximate** (`dsp3210_dau_double.inc`): host doubles stand in for the 40-bit accumulators.  At least as precise as hardware - a double's 53-bit mantissa strictly contains mantissa+guard - but guard-bit rounding is not bit-exact. | reading the code, general-purpose emulation; bit-identical to the exact DAU on typical audio-rate code |
| `libdsp3210emu-exact.a`, `dsp3210emu-exact` (`-DDSP3210_DAU_EXACT`) | **exact** (`dsp3210_dau_exact.inc`): the datapath in integers - 40-bit accumulators, exact 25×25-bit multiplier, truncating adder, documented rounding/saturation.  No host floating point in the data path. | when guard bits, exact rounding or saturation corner cases matter |

Both cores share this header and the same ABI (the accumulator slot is a
union; use the `dsp3210_acc_*` accessors), pass the same 31-test suite,
and run every program in `../toolchain-tests` identically.  Where they
were ever observed to differ (a 157-probe DAU comparison during
development: 10 corner-case probes), the exact DAU matched the
documented hardware behaviour in all of them.

## Contents

| File | What it is |
|---|---|
| `dsp3210_emu.h` / `dsp3210_emu.c` | the shared core - portable C99, no I/O, no global state |
| `dsp3210_dau_double.inc` | the approximate DAU (included by the core; not compiled alone) |
| `dsp3210_dau_exact.inc` | the exact DAU (included by the core; not compiled alone) |
| `dsp3210emu.c` | command-line driver (links `../dsp3210dis` for tracing) |
| `test_dsp3210_emu.c` | 31 unit tests: hand-encoded programs covering every instruction family, the exception model, and the DSP32 float codec - run against **both** DAUs |
| `Makefile` | `make` builds both libraries, both CLIs and both test binaries; `make test` runs the suite on both |

## What is (and isn't) implemented - version 1

**Complete decode.** Every one of the 64 top-level opcodes is executed or
raises the architected illegal-opcode error (the seven patterns of IM
§7.5.3.2, vector 2).  There is no "unimplemented instruction" path.

**CA (integer/branch/move) instructions are implemented to the letter:**

- delayed branches, including two back-to-back branches (I1, I2, A, B
  order), `call` storing insn+8, and `pc` reading insn+8 as an operand;
- `ireturn` executing *no* delay slot;
- `do`/`dolock`/`doblock` loops (K+1 instructions, L+1 or (rM&0x7FF)+1
  times; `dolock` masks interrupts for the loop's duration);
- the discontinuous register-code space, hard-wired r0, writable `pcsh`,
  the `+n`/`-n` pseudo-operands, and `sp = sp++`/`sp--` moving by 4;
- 16-bit ("short") flag rules per IM Table 4-1, including carry-out-of-
  bit-15, sign extension of short results (zero extension for the short
  logical shifts), the borrow-style carry on subtract, carry-reverse add,
  and rotates through carry;
- all move sizes (byte/char/ushort/short/hbyte/long) with their exact
  register-bit selections, memory-direct `*L` against the on-chip window
  ($0000xxxx or $5003xxxx per `pcw[10]`), post-modification by ±size or
  an increment register, and the IO-register namespace (`ps`, `emr`,
  `spc`, `pcw` with its bit-13 write lock, `dauc`, `ctr`);
- `waiti` (ignored if an interrupt is pending; the latent instruction
  runs when one arrives), `bkpt` (halts the emulator), `sftrst` (drops
  to base level).

**DA (floating-point) instructions.**  All decode, addressing side
effects (pointer post-modification by the correct per-instruction
operand size, X/Y reads, Z stores including the tap forms) and
flag/`ctr` updates are performed exactly; the arithmetic is one of the
two DAUs above.  All 13 special functions are implemented in both
(`ic`, `oc`, `float16/32`, `int16/32`, `round`, `ifalt/ifaeq/ifagt`,
`ieee`, `dsp`, `seed`), including the manual's µ-law/A-law formulas,
the three rounding modes, saturation, and the IEEE ∞/NaN/denormal
special cases with the NaN error (vector 6).

**Exception model** per IM §7.5: the 16-entry vector table at `evtp`,
`goto evtp+vn*8` interrupt dispatch with hardware context save
(a0-a3, `dauc`, `ps`, `ctr`, do-loop state) and `pcsh`, `call evtp+vn*8
(r20)` error dispatch with the abort model (do-loops killed, `pcw`
unlocked), `emr` masking with the bit-number = vector-number rule,
maskable-vs-non-maskable nesting rules, and the double-error halt.
Address errors (misaligned word/half-word) raise vector 4; bus errors
(unmapped memory) vector 1.

**Memory model:** a flat RAM buffer at address 0 (size of your choosing)
plus the 64 KB on-chip window, which is **one** physical array decoded at
`$50030000` in processor mode and at `$0000xxxx` in computer mode (the
boot ROM's SAR path runs across that switch).
Endianness follows `pcw[8]` (big-endian by default, as on the AV Macs).
Optional read/write hooks let an embedder replace the whole map.

**Not modelled (deliberately, v1):** pipeline latencies and the
assembler-enforced restrictions of IM §4.4 (everything behaves as if all
latencies were zero); the SIO, DMA controller, timer and BIO peripherals
(their MMIO words are plain RAM); bus arbitration.  Interrupts are
recognised only at instruction boundaries and never in the shadow of a
*taken* delayed branch.  (The IM makes all branch instructions, and
loads, non-interruptible for one cycle; in a zero-latency model the
architectural result is the same either way, so only the taken-branch
window is modelled.)  The `irsh` instruction-shadow replay **is**
modelled explicitly - `ireturn` re-executes the word prefetched before
the interrupt and resumes at `pcsh`, which the boot ROM's SAR path
depends on.

### Known deviations (documented, all in corner cases)

Specific to the **approximate DAU** (the exact DAU gets these right):

- **Overflow saturation constants are the 32-bit ones.**  The V/U
  *thresholds* are the 40-bit range, as the IM requires, but a saturated
  result clamps to `$7FFFFFFF` / its negation rather than to the 40-bit
  all-ones mantissa+guard - so `minneg*max` lands one ULP from the
  documented `$800000FF`.
- **`ic(aN)` reads the accumulator's most significant byte** (IM IC page)
  by packing the modelled double first, so a value whose packed exponent
  differs from hardware's parked bits reads differently.  The exact DAU
  reads the same lane from the real register.

Common to both DAUs:

- `oc`, `int16`, `int32` and `ieee`, whose hardware result is a raw bit
  pattern parked in accumulator bit positions ("the lower portion
  contains unpredictable data"), leave the *numeric* result in `aN`
  instead.  Their Z memory writes - which the manual requires to be
  issued in the same instruction anyway - carry the correct bits.
- Special-function operands taken *from an accumulator* use the numeric
  value (so `float16(aN)` after `aN = int16(...)` composes as expected);
  `dsp(aN)` is undefined by the manual (Y may not be an accumulator).
- Reserved encodings (ALU F=0101, W sizes 101/110, reserved register/IO/
  condition/G codes) execute as benign no-ops or zero-reads; only the
  seven architected illegal opcodes raise an error.  Reserved W sizes
  transfer 32 bits.
- On error entry `r20` = aborted-instruction address + 8 (the value `pc`
  reads as an operand, per "the pc represents the address of the
  prefetched instruction").
- A move that targets the `pc` register code (in no replacement table)
  acts as an immediate branch.
- `bkpt`'s hardware behaviour is undocumented; the emulator halts.
- Companded (`oc`) encoding picks the nearest code by exhaustive search
  over the manual's decode formulas; hardware tie-breaking is unknown.
  The µ/A-law *output* select follows IM Table 8-3 (`dauc` bit 1 = 1 →
  A-law out); note the OC instruction page states the opposite polarity.
- Within one DA instruction operands are processed Y, then X, then Z; if
  one pointer register appears in several fields with post-modification
  (an assembler-rejected construct), that order decides the result.

## The exact DAU model (`dsp3210_dau_exact.inc`)

```
32-bit datum       mantissa[31:8]  | exponent[7:0]
40-bit accumulator mantissa[39:16] | guard[15:8] | exponent[7:0]
```

The mantissa field is a single 2's-complement quantity `s (!s) . f…f`,
so the represented mantissa M lies in [1,2) or [-2,-1) and the implicit
leading bit is `!s` - not a constant 1 as in IEEE 754.  The core keeps
that value *including* the implicit bit as a fixed-point integer
(`dsp3210_acc` in the header), so `1.0` is `{m = 2^31, e = 128}` and
`-1.0` normalises to `{m = -2^32, e = 127}` - i.e. -2 × 2⁻¹, which is
why -1.0 encodes as `$8000007F`.

Operations, all in integer arithmetic ([IM §8.2.3], [IM Figure 8-9]):

| Stage | Behaviour |
|---|---|
| multiplier | exact 25×25-bit signed product at 46 fraction bits.  Accumulator inputs have their **guard bits truncated first**.  The product keeps three integer bits, so it may be denormalised (-2 × -2 = 4) |
| adder | inputs are the product and S (a 40-bit accumulator *with* its guard bits, or a 32-bit memory datum with zero guard); exponents are aligned by arithmetic right shift (**truncation**, not rounding), then added |
| normalise | automatic, so an accumulated result is always normal; then truncated from 46 back to 31 fraction bits |
| range | overflow when the exponent leaves 255, underflow below 1, judged on the **40-bit** value; overflow saturates the mantissa+guard field, underflow flushes to zero, both setting V/U and raising vector 5 |
| `round` | the only rounding step: 40 → 32 bits, round to nearest with ties to the greater value, guard bits cleared |
| `float16`/`float32` | exact - 32 mantissa+guard bits hold any 32-bit integer without loss |
| `int16`/`int32` | the three `dauc[5:4]` modes (nearest-ties-up, truncate to -∞, truncate to 0) with saturation, computed on the integer mantissa |
| `ieee`/`dsp` | exact bit-level conversions, no host float involved, including the ±∞ / NaN / denormal special cases |
| `ic`/`oc` | µ-law and A-law via the manual's formulas evaluated on **doubled** integers (the µ-law decode is a half-integer); encoding picks the nearest code by exhaustive comparison |
| `seed` | `Y ^ 0x7FFFFFFF` - invert all but the sign, as documented |

### Two places the exact model rests on reasoning, not citation

**Adder truncation.**  The manual specifies no rounding in the adder and
provides `round` explicitly for the 40 → 32 step, and an arithmetic
right shift is what the datapath does - so this DAU truncates.  Nothing
in the IM states it outright.

**The product's width entering the adder.**  This DAU carries the
product at its full 46 fraction bits through exponent alignment and
truncates only the final accumulator write.  IM §8.2.3 says "the adder
inputs (P and S) and result contain eight mantissa guard bits in
addition to the standard 24 fractional bits", which read literally would
truncate P to 32 fraction bits *before* the add.  The two differ when
the sum normalises left (cancellation) or when sub-2⁻³² product bits
would survive into the guard field.  Settling which reading matches
silicon needs real hardware or a vendor test vector.

Smaller seams of the same kind: an operand shifted out by more than 62
places contributes 0 rather than the -1 ulp that the truncating-adder
model would give for a negative (a host-`int64` width, not a datapath
width); companded encoding quantises 2·Y to an integer before the
nearest-code search, so boundary ties can differ from a true
nearest-to-Y comparison; and `ieee()` flushes below-range values to a
signed zero and never emits denormals, which hardware may or may not do.

## Library API

```c
#include "dsp3210_emu.h"

static uint8_t ram[1 << 20];
dsp3210_emu s;
dsp3210_init(&s, ram, sizeof ram);   /* includes reset; pc = 0 */
dsp3210_load(&s, 0, code, code_len); /* bytes, through the address map */
s.pc = entry; s.npc = entry + 4;

while (dsp3210_step(&s) == DSP3210_STEP_OK)
    ;
/* s.r[], s.ps, s.level, s.icount ... are yours to inspect;
   accumulators via dsp3210_acc_get/_set/_raw (see the header) */
```

Link `libdsp3210emu.a` or `libdsp3210emu-exact.a` - same header, same
ABI; the accumulator slots are a union and portable code reads them
through `dsp3210_acc_get`/`_set`/`_raw`.  `dsp3210_step` returns
`DSP3210_STEP_OK`, `_WAITI` (wait-for-interrupt, nothing pending),
`_BKPT`, or `_DERROR` (double error - dead until reset).
`dsp3210_request_interrupt(&s, DSP3210_VEC_EXT0)` asserts an interrupt;
it is taken when unmasked in `emr` and the core is at base level.
`dsp3210_peek`/`dsp3210_poke` access memory without raising exceptions.
The DSP32↔double float codecs are exported for building test vectors
and are bit-identical in both cores (ties resolve toward +infinity, the
direction the DSP's own `round` uses); the exact core adds
`dsp3210_acc_double`/`_from_double`/`_pack`/`_unpack` for building and
inspecting accumulators directly.

## CLI

```
dsp3210emu [options] file.bin
  -a addr      load address of the binary (default 0)
  -s addr      start pc (default: the load address)
  -o offset    byte offset into the file
  -l bytes     how many bytes of the file to load
  -m size      flat memory size (default 16 MB)
  -e           little-endian memory (default big-endian; clears pcw[8])
  -c           computer mode (pcw[10]=1); default is processor mode
  -n count     stop after this many instructions (default 1e8)
  -t           trace each instruction through the reference disassembler
  -d addr,len  hex-dump a range after the run (repeatable)
  -q           quiet (no register dump)
```

(`dsp3210emu-exact` is the same driver linked against the exact DAU.)

Example - a Fibonacci program assembled by the companion `dsp3210asm`
(the sequence loop stores fib(20) = 0x1a6d; a format-7a direct store
lands in the on-chip window, hence the `0x5003xxxx` dump address):

```
$ ../dsp3210asm/dsp3210asm -q -o fib.bin ../toolchain-tests/apps/fib.s
$ ./dsp3210emu -d 0x50030500,4 fib.bin
stopped: bkpt after 120 instructions (pc=00000030)
r0  00000000  r1  00001055  r2  00001a6d  r3  00001a6d  ...
pc  00000030  pcsh 00000000  ps 0000  emr 0000  pcw 038f  ...
level base  insns 120
50030500: 00 00 1a 6d
```

## Validation

`make test` runs the 31-test suite twice - once per DAU (the test
source is shared and uses only the uniform accessors).  Notable
oracles:

- a real DSP3210 boot stub's `r5 = pc - 0x24` (raw word `0x94AFFFDC`,
  which real code relies on producing 0 at address 0x1C) pins the
  delayed-branch pc semantics against hardware-facing code;
- the three `spc` pseudo-instruction encodings are checked byte-for-byte
  against hardware-verified values (`$9DE0040A`/`$9D60040A`/
  `$9D00040A`);
- encoder outputs are spot-checked against machine words the validated
  disassembler decodes (`93405003` = `r22 = r0 <<| 0x5003`);
- the two-back-to-back-branches order, delay-slot execution, `call`'s
  insn+8, do-loop iteration counts, 16-bit flag rules, the interrupt
  round trip with shadow restore, `waiti` wake-up ordering, and the
  DSP32 float codec's format identities (`1.0` = `0x00000080`,
  `-4.0` = `0x80000081`, dirty zeros, saturation).

During its original development this core additionally booted Apple's
Mac AV DSP kernel and AT&T's mask boot ROM, and ran a shipped `dspf`
audio module to a signal-level ground truth - none of that vendor code
is included in this repository.  `../toolchain-tests` carries the
self-contained integration evidence.
