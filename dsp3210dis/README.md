# dsp3210dis - reference disassembler for the AT&T DSP3210

A small, dependency-free disassembler for the AT&T DSP3210, written as a
**reference implementation**: every encoding decision is traceable to the
AT&T *DSP3210 Information Manual* (chapter 4 instruction pages and
chapter 10 encoding tables), cross-checked against hardware-verified
encodings recovered from real DSP3210 systems.

## Contents

| File | What it is |
|---|---|
| `dsp3210_dis.h` / `dsp3210_dis.c` | the library - portable C99, no I/O, no allocation, no global state |
| `dsp3210dis.c` | command-line tool built on the library |
| `test_dsp3210.c` | 100 unit vectors (manual examples, hardware-verified words, illegal opcodes) |
| `Makefile` | `make` builds `libdsp3210dis.a` + `dsp3210dis`; `make test` runs the unit tests |

## Library API

```c
#include "dsp3210_dis.h"

dsp3210_insn ins;
dsp3210_disassemble(dsp3210_read_be32(bytes), address, &ins);
/* ins.text      - assembler text, e.g. "*r1++ = a0 = a1 - *r2-- * *r3++r17"
   ins.status    - DSP3210_OK / DSP3210_ILLEGAL / DSP3210_RESERVED
   ins.klass     - DSP3210_CLASS_CA (integer/branch/move) or _DA (float)
   ins.is_branch - instruction has a delay slot
   ins.has_target/ins.target - resolved absolute branch target */
```

The `address` argument only matters for pc-relative operands: the pc
value an instruction observes is the address of the instruction *after*
the delay slot, i.e. `insn_addr + 8` (IM CALL page; confirmed on real
DSP3210 boot code).

## CLI

```
dsp3210dis [-a addr] [-o offset] [-n bytes] [-e] [-q] file
  -a addr    load address of the first word (default 0)
  -o offset  byte offset into the file (default 0)
  -n bytes   how many bytes to disassemble (default: to end of file)
  -e         little-endian words (default big-endian, as on the AV Macs)
  -q         omit the address/word columns
```

Example - a program assembled by the companion `dsp3210asm`
(`../toolchain-tests/apps/fib.s`):

```
$ ../dsp3210asm/dsp3210asm -q -o fib.bin ../toolchain-tests/apps/fib.s
$ ./dsp3210dis fib.bin
00000000: 94200000  r1 = 0x0
00000004: 94400001  r2 = 0x1
00000008: 94a00420  r5 = 0x420
0000000c: 95400011  r10 = 0x11
00000010: 98030822  r3 = r1 + r2
00000014: 9de32817  *r5++ = r3
00000018: 98011020  r1 = r2
0000001c: 98021820  r2 = r3
00000020: 0d4fffe8  if (r10-- >= 0) goto pc-0x18 ; -> 0x00000010
00000024: 80000000  nop
00000028: 1de20500  *0x500 = r2
0000002c: 9d60040a  bkpt
```

## Syntax conventions

Output follows the AT&T assembler syntax used throughout the Information
Manual: `(short)` is printed, `(long)` (the default) is not; `goto`
destinations render as `{0xN, rB, rB±0xN, pc±0xN}`; `nop`, `ireturn`,
`waiti`, `bkpt` and `sftrst` are recognised special encodings of
`if (false) goto`, `if (true) goto pcsh`, and the three `spc = r0`
stores.  `return (rM)` is encoding-identical to `goto rM` and prints as
the latter.  Resolved branch targets are appended by the CLI as
`; -> 0xADDR`.

Decode oddities are kept visible rather than smoothed over: reserved
register codes print as `?rNN`, reserved DA operand fields as `?`, the
seven illegal opcodes (IM §7.5.3.2: `000000 000001 000010 001111 010110
010111 100010`) and reserved encodings become `.word 0x… ;
illegal/reserved` with `ins.status` set accordingly.

## Encoding notes established while validating (worth keeping)

- **The 5-bit CAU register field is discontinuous** (IM Table 10-2):
  `0`=r0, `1-14`=r1-r14, `15`=pc, `17-21`=r15-r19, `22/23`=the `-n`/`+n`
  pseudo-operands, `24-26`=r20-r22, `30`=pcsh; 16, 27-29, 31 reserved.
- **A DA Z field with p=1111 is a store through the Y operand.**  The
  manual calls p=1111 "not allowed" and it was long misread as a second
  spelling of "no write" (0x7F), but shipped code depends on the store:
  the result goes through Y's own effective address and the Z field's
  I bits post-modify Y's pointer register - rendered as, e.g.,
  `*r2++ = a2 = *r2 + a0`.  With an accumulator Y there is nothing to
  store through and the field decodes as no write (which is where the
  misreading came from).  The true no-write marker is the manual's
  `0000111` (0x07).
- Two of the manual's worked examples contain typos (the OR example
  encodes r15 with the pc code; the SHIFT LEFT example shows the `>>`
  F-code).  The chapter 10 tables - corroborated by the manual's own
  CALL/IRETURN/DO examples and by real machine code - are authoritative.
- `sp = sp++` / `sp = sp--` (which move sp by 4, not 1) share the F=add
  encoding with `rD = rS ± 1`; they are distinguished by rD = rS1 = r21.

## Leniency policy - what does *not* raise a status

By design the disassembler is permissive about fields the manual reserves
but that carry no decode ambiguity, because its job is reading real ROM
images rather than validating an assembler's output.  Specifically these
render a marker but still return `DSP3210_OK`:

- reserved CAU register codes (16, 27-29, 31) print `?rNN`, reserved IO
  codes print `?iorN`, reserved `W` sizes print `(?size)`;
- must-be-zero fields are not checked at all (DA bit 25, format-5 bits
  20-14, the 7b/7c/7d zero fields, 3b/3c bits 22-18) - so the
  `waiti`/`bkpt`/`sftrst` special forms are recognised even when those
  bits are dirty;
- a DA Z field with `p = 0000` and `i` in 000-110 decodes as "no write",
  where Table 10-3 allows only `i = 111` there.

Only the seven illegal opcodes and genuinely ambiguous DA/ALU encodings
set `DSP3210_ILLEGAL` / `DSP3210_RESERVED`.  If you need strict encoding
validation, treat the `?` markers in `ins.text` as errors - or better,
round-trip the text through `../dsp3210asm`.

Note also that `ins.is_branch` marks every control transfer, **including
`ireturn`** - which is the one branch with no delay slot.  Use the
`ins.no_delay_slot` flag when driving delay-slot logic.

## Validation

`make test` runs 100 vectors: the manual's worked encoding examples and
words verified against real DSP3210 machine code.  Beyond the unit
vectors, this decoder was developed against every body of real DSP3210
code available to the authors - Apple's Mac AV ROM DSP kernel, AT&T's
mask boot ROM, and shipped `dspf` task modules - all of which read
exactly as their documentation describes (none of that vendor code is
included in this repository).  `../toolchain-tests` additionally proves
the decoder agrees with the companion assembler over ~12 million
instruction words.
