# Contributing

Thanks for your interest - DSP3210 knowledge is scarce and contributions
that make these tools more faithful to the silicon are very welcome.

## Building and testing

```sh
make            # builds everything (no dependencies beyond a C99 compiler)
make test       # runs every suite; all of it must stay green
```

`make test` runs the per-tool unit suites, the ~12-million-word
assembler⇄disassembler roundtrip sweep, and the test programs on both
DAU implementations.  CI runs the same on gcc and clang (Linux) and
clang (macOS), plus one job under AddressSanitizer and
UndefinedBehaviorSanitizer - a reference model is only ground truth if
it is free of undefined behaviour, and the numeric results can be right
while the C is not.  A change is only done when `make test` passes from
a clean tree, sanitizers included:

```sh
make CC="gcc -fsanitize=address,undefined -fno-sanitize-recover=undefined" test
```

(Pass sanitizer and hardening flags through `CC` rather than `CFLAGS`
so that they reach the link steps too; `CFLAGS` is yours to set and is
appended to the mandatory flags, so `make CFLAGS=-O0` also works.)

## What contributions are most valuable

1. **Measurements from real DSP3210 silicon.**  Two DAU datapath details
   rest on documented reasoning rather than measurement (adder
   truncation, and the product's width entering the adder - see
   `dsp3210emu/README.md`).  If you own a Quadra 840AV / Centris 660AV
   or an AT&T VCOS board, a handful of test vectors would settle them.
2. **Peripheral models** (SIO, DMA, timer, BIO) behind the existing
   memory-hook interface.
3. **More test programs** - drop a `.s` file with `;;` expectation
   directives into `toolchain-tests/apps/`; no C changes needed.  Keep
   expectations hand-computed from the Information Manual, not captured
   from the emulator.
4. **Documentation** - corrections or additions to
   `docs/dsp3210-cpu-reference.md`, with `[IM §]` citations (and never
   copied manual text - see below).
5. Ports, packaging, bindings.

## Ground rules

- **Cite the manual.**  Every encoding or semantic decision traces to
  the AT&T *DSP3210 Information Manual* (chapter 4 instruction pages,
  chapters 7/8, chapter 10 tables) or to behaviour verified on real
  systems.  Comments in the sources use `[IM §x.y]` for the former.
- **No vendor code.**  Nothing from Apple, Commodore or AT&T binaries
  may be committed - no ROM dumps, no extracted modules, no multi-
  instruction listings.  (Citing individual instruction words as test
  vectors is fine.)
- **Keep the algebra.**  The assembler's accepted syntax is exactly the
  disassembler's output syntax.  A change to either side must keep
  `toolchain-tests/roundtrip` green - text→word→text identity and the
  canonical-form fixed point over the whole clean opcode space.
- **Style:** portable C99, no dependencies, no allocation in the
  libraries, 4-space indent, ~76-column comments - match the
  surrounding code.

## Licence

By contributing you agree that your contributions are licensed under
the MIT licence (see `LICENSE`).
