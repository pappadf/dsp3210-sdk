# dsp3210-sdk - an open toolchain for the AT&T DSP3210

[![ci](https://github.com/pappadf/dsp3210-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/pappadf/dsp3210-sdk/actions/workflows/ci.yml)

Assembler, disassembler and emulator for the **AT&T DSP3210**, the
32-bit floating-point DSP that shipped in the Macintosh Quadra 840AV /
Centris 660AV and reached prototype stage in Commodore's unreleased
Amiga 3000+ and AA3000 designs (1991).  If you are doing DSP3210 retrocomputing - writing new DSP
code for an AV Mac, building an emulator of a machine that contains one,
or exploring what the Amiga's DSP sidecar would have been - this is a
complete, self-contained, tested toolchain in portable C99.

Everything in this repository is original code, written from the AT&T
*DSP3210 Information Manual* (chapters 4, 7, 8 and 10).  **No Apple,
Commodore or AT&T code is included.**  (The tools were additionally
validated, in a separate research effort, by running real vendor
binaries - Apple's DSP kernel, AT&T's on-chip mask boot ROM, a shipped
audio module - but none of that material ships here; see "Provenance"
below.)

## A note on AI

**AI tools were used in the making of this project** - writing code,
drafting documentation, and reviewing changes.  That is worth stating
plainly up front, because this is offered as a *reference*
implementation and you should know how it was produced before trusting
it as ground truth.

## The pieces

| Directory | What it is |
|---|---|
| [`dsp3210dis/`](dsp3210dis/) | reference **disassembler** - a pure function of (word, address); no I/O, no allocation. 100 unit vectors. |
| [`dsp3210asm/`](dsp3210asm/) | reference **assembler** - two-pass, labels, data directives; its input syntax is exactly the disassembler's output syntax, so the pair round-trips. 138 unit vectors. |
| [`dsp3210emu/`](dsp3210emu/) | reference **emulator core** - complete instruction-set decode, exact CA (integer/branch/move) semantics, the full exception model.  One shared core with **two drop-in DAU implementations** selected at build time: approximate (host doubles - simple, fast) and exact (integer 40-bit accumulators, exact multiplier/adder/rounding).  Builds `libdsp3210emu.a` and `libdsp3210emu-exact.a` from the same sources. |
| [`toolchain-tests/`](toolchain-tests/) | the three tools tested **in combination**: a ~12-million-word disassemble→assemble→disassemble sweep, plus seven DSP programs assembled and executed on **both** DAUs against hand-computed results. |
| [`docs/`](docs/) | **[the DSP3210 CPU programmer's reference](docs/dsp3210-cpu-reference.md)** - registers, memory model, DSP32 float format, execution/exception rules and the complete instruction set with encodings, written from scratch (the AT&T manual only circulates as unsearchable page scans). |

Which DAU?  Start with the approximate one (the default: simpler,
faster, and bit-identical to the exact DAU on typical audio-rate code);
link `libdsp3210emu-exact.a` when accumulator guard bits, exact rounding
or saturation corner cases matter.  Same header, same ABI, same tests.

## Quick start

```sh
make            # builds everything
make test       # runs every test suite (a few minutes; ~12 M checks)
```

Write a program and run it:

```sh
$ cat > hello.s <<'EOF'
start:  r1 = 0x0                ; f(0)
        r2 = 0x1                ; f(1)
        r10 = 0x11              ; 19 iterations
loop:   r3 = r1 + r2
        r1 = r2
        r2 = r3
        if (r10-- >= 0) goto loop
        nop                     ; delay slot
        *0x100 = r2             ; on-chip window (0x50030100)
        bkpt
EOF
$ dsp3210asm/dsp3210asm hello.s
$ dsp3210dis/dsp3210dis hello.bin          # read it back
$ dsp3210emu/dsp3210emu -t -d 0x50030100,4 hello.bin   # trace + run
```

Or embed the libraries - each is a pair of C99 files with no
dependencies:

```c
#include "dsp3210_asm.h"   /* dsp3210_assemble(), dsp3210_encode()   */
#include "dsp3210_dis.h"   /* dsp3210_disassemble()                  */
#include "dsp3210_emu.h"   /* dsp3210_init/reset/load/step(), hooks  */
```

The emulator models the CPU core (both memory maps, the exception
system, waiti/ireturn, do-loops, the DSP32 float format); peripherals
(SIO, DMA, timer, BIO) are not modelled - their MMIO words behave as
RAM, and read/write hooks let an embedder supply the rest of a machine.

## The assembly language and the architecture

The instruction set, registers, float format and execution rules are
condensed in **[docs/dsp3210-cpu-reference.md](docs/dsp3210-cpu-reference.md)** -
a searchable programmer's reference written from scratch, with `[IM §]`
citations back into the AT&T manual for anyone holding a scan.

The assembler accepts exactly the AT&T syntax of the Information
Manual, as printed by the disassembler - plus labels and
`.org/.word/.float/.space/.align/.equ`.  See
[`dsp3210asm/README.md`](dsp3210asm/README.md) for the grammar and the
canonical-encoding choices, and the programs in
[`toolchain-tests/apps/`](toolchain-tests/apps/) for working examples of
loops, subroutine calls, DAU multiply-accumulate and interrupt handling.

## How it is tested

- each tool has its own unit suite (`make test` in each directory);
- `toolchain-tests/roundtrip` proves assembler and disassembler agree
  over the entire cleanly-decodable opcode space (~12.5 M words:
  per-opcode exhaustive halfword sweeps plus a deterministic random
  stream) - every decode re-assembles to the identical text, and the
  assembler's output is a fixed point;
- `toolchain-tests/run_apps` assembles seven original DSP programs and
  executes them on **both** cores, checking registers, memory, DSP32
  floats and interrupt counts declared in the sources;
- CI runs all of it on gcc and clang (Linux) and clang (macOS), and once
  more under AddressSanitizer and UndefinedBehaviorSanitizer - a
  reference model is only ground truth if it is also free of undefined
  behaviour.

## Provenance

These tools were developed as reference implementations inside a
hardware-research effort on the Macintosh Quadra 840AV/660AV, where they
were validated against real vendor code: Apple's Real Time Manager DSP
kernel boots on the emulator, AT&T's mask boot ROM runs all three reset
paths, and a shipped sample-rate-converter module produces
signal-correct output.  That vendor material cannot be redistributed and
is **not** part of this repository; what ships here is only the original
toolchain and its self-contained test system.  A handful of single
32-bit instruction words observed on real systems remain as test-vector
citations (e.g. `9CFA2817` = `r22 = *r5++`).

## Status / roadmap

- CPU core and toolchain: complete and cross-validated (see above).
- Not yet modelled: on-chip peripherals (SIO, DMA, timer, BIO), pipeline
  latencies, bus arbitration.  Two DAU datapath details rest on
  documented reasoning rather than silicon measurements (adder
  truncation, product width into the adder) - settling them needs real
  hardware; see [`dsp3210emu/README.md`](dsp3210emu/README.md).
- Contributions welcome, especially measurements from real DSP3210
  silicon (Mac AV owners: the hardware is on your logic board) - see
  [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE).  All code in this repository is original work by the
project authors.

## Trademarks

All trademarks referenced in this project - including AT&T, DSP3210,
Apple, Macintosh, Quadra, Centris, Commodore and Amiga - are the
property of their respective owners and are used for identification
purposes only.  This project does not claim any endorsement by or
affiliation with the trademark holders.
