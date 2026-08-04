# dsp3210asm - reference assembler for the AT&T DSP3210

A two-pass assembler whose statement syntax is **exactly the companion
disassembler's output syntax** (`../dsp3210dis`), plus labels and data
directives.  Every encoding decision is traceable to the AT&T *DSP3210
Information Manual* (chapter 4 instruction pages, chapter 10 encoding
tables) and to hardware-verified encodings recovered from real DSP3210
systems.

Defining the syntax as the disassembler's output makes the pair
testable as an algebra:

- **`disassemble(assemble(text)) == text`** for every accepted
  statement, and
- **`assemble(disassemble(word))` decodes to the same text as `word`**
  for every word the disassembler decodes cleanly - verified over
  twelve million words by `../toolchain-tests/roundtrip`.

## Contents

| File | What it is |
|---|---|
| `dsp3210_asm.h` / `dsp3210_asm.c` | the library - portable C99, no I/O, no allocation, no global state |
| `dsp3210asm.c` | command-line tool built on the library |
| `test_dsp3210_asm.c` | 138 unit vectors (manual examples, ROM-verified words, error cases, full programs) |
| `Makefile` | `make` builds `libdsp3210asm.a` + `dsp3210asm`; `make test` runs the unit tests |

## Library API

```c
#include "dsp3210_asm.h"

/* whole programs: labels, directives, two passes */
uint8_t img[65536]; uint32_t len; dsp3210_asm_err err;
if (dsp3210_assemble(src, /*org*/0, img, sizeof img, &len, &err))
    fprintf(stderr, "line %d: %s\n", err.line, err.msg);

/* single statements (the disassembler's inverse) */
uint32_t word;
dsp3210_encode("r4 = *r2++", /*addr*/0x1000, &word, &err);
```

The image is emitted as big-endian words (the AV Mac byte order) from
`org` upward; `.org`/`.space`/`.align` gaps are zero-filled.  `addr`
matters only for pc-relative operands, mirroring `dsp3210_disassemble`.

## CLI

```
dsp3210asm [-a addr] [-o out.bin] [-e] [-q] file.s
  -a addr   address of the first byte (default 0)
  -o file   output (default: input basename + .bin)
  -e        little-endian words (default big-endian)
  -q        quiet
```

## Syntax

One statement per line; `;` or `//` starts a comment; numbers are
`0x…`, `$…` or decimal.  Instructions are written exactly as
`dsp3210dis` prints them - `r4 = *r2++`, `if (eq) goto pc+0x10`,
`*r1++ = a0 = a1 - *r2-- * *r3++r17`, `do 2, 100`, `waiti` - see the
disassembler README for the conventions.  On top of that:

```
label:                       ; define a label (may share the line)
        .org   0x100         ; move the location counter (forward gaps
                             ;   are zero-filled; backward is allowed)
        .word  1, sym+4      ; 32-bit data words
        .float 1.5, -0.125   ; DSP32-format floats [IM §3.4.2]
        .space 16            ; zero bytes
        .align 8             ; zero-fill to a power-of-two boundary
        .equ   NAME, 0x400   ; constant
```

- A **label or symbol used as a branch/call target assembles
  pc-relative** (position-independent); a bare number is absolute.
  `r2 = (ushort24) label` takes a label's absolute address.
- Symbols may be used wherever a number may; `+`/`-` chains are
  accepted in directives and targets.  Symbols that would shadow the
  machine vocabulary (`r5`, `pc`, `goto`, `eq`, `dauc`, …) are
  rejected.
- `sp`/`evtp` are accepted as aliases for `r21`/`r22` on input.

## Canonical encodings

The architecture gives several instructions more than one encoding, and
several encodings the same assembler spelling.  The assembler always
picks one canonical word (so its output is a fixed point of
assemble∘disassemble); the notable choices:

| Spelling | Encoding chosen | The alternative |
|---|---|---|
| `goto 0x40` / `call 0x40 (rM)` | format 0b/1b / 4a when the value fits 16 bits signed | format 8a/8c (used beyond 16 bits) |
| `goto label` | pc-relative, never absolute | - |
| `rD = rS` | 6a/6b ALU add with rS2 = r0 (what Apple's assembler emits, e.g. ROM `98180020`) | format 5 with N = 0 |
| `rD = rS + 0x5` | format 5 three-op add | 6c/6d immediate add (rD = rS only) |
| `rD = rD + -0x5`, `± 0x0`, `r0 = r0 + N` | 6c/6d immediate ALU (these are its printed spellings) | format 5 |
| `rD = rS + 1` (bare `1`) | increment, rS2 = `+n` - the spelling distinguishes it from `+ 0x1` | format 5 with N = 1 |
| `r21 = r21 ± 1` | F=subtract with `∓n` (the add form prints as `sp = sp±±`) | - |
| `A - +n/-n`, `r0 - B` | reverse subtract (prints operands swapped) | F=subtract, which prints `A ± 1` / `-B` |
| `rD = rD << 5` | 6c/6d immediate shift | reg form with rS2 = `+n` when count = 1 and rD ≠ rS |
| `rD = rS >>>1` | register rotate with rS2 = 0 | immediate rotate |
| DA `aN = Y + X` | format 1 with M = 1.0 | format 1 M = 110 (tap) without a Z write |
| DA `aN = aM ± Y * X` | format 3 | format 2 without a Z write |
| DA `aN = Y ± aM * X`, all-accumulator operands | format 1 | format 3 |
| DA "no Z write" | `1111111` (0x7F - Apple's assembler convention) | the manual's `0000111` |

`do K, N` writes the raw field values, matching the disassembler (the
hardware executes K+1 instructions N+1 times).

## Validation

`make test` runs 138 vectors: every worked example in the manual's
chapter 4.6 that the disassembler's test suite decodes, re-encoded from
text and compared against the known word; words verified against real
DSP3210 machine code (`9CFA2817`, `93405003`, the `spc`
pseudo-instructions, …); rejection tests; and whole programs through
labels and every directive.

The heavier evidence lives in **`../toolchain-tests/`**: a 12-million-
word disassemble→assemble→disassemble sweep (exhaustive low/mid
halfwords for all 64 opcodes plus a deterministic random stream), and
seven programs assembled here and executed on both emulator cores.
