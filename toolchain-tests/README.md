# toolchain-tests - assembler ⇄ disassembler ⇄ emulator, in combination

The SDK's three tools are tested **against each other**: the assembler
(`../dsp3210asm`), the disassembler (`../dsp3210dis`) and the emulator
core with both of its DAU implementations (`../dsp3210emu`, approximate
and exact).  Everything here is original code - the test programs were
written for this suite, with expectations computed by hand from the
AT&T Information Manual.

```
$ make test
corpus: 216 statements round-tripped
fuzz: 12327944 clean decodes re-assembled (2908005 aliases canonicalised)
programs: 7 image(s) round-tripped
12328167 checks, 0 failures
ok   apps/bitops.s (32 insns, 10 checks)
…                                     (run_apps: the approximate DAU)
7 programs, 0 failures
…                                          (run_apps_exact: the exact DAU)
7 programs, 0 failures
```

## roundtrip - assembler ⇄ disassembler

Three layers, weakest to strongest:

1. **Corpus.** ~216 statements, at least one per syntax form.  Each
   must survive text → word → text unchanged, and re-assembling the
   disassembly must reproduce the word bit for bit.

2. **Sweep + fuzz.**  For all 64 opcodes, every value of the low
   halfword and of the mid halfword (with the other bits both clean
   and dirty), plus four million xorshift words - ~12.3 M words that
   disassemble cleanly (status OK, no `?` placeholder).  For each:
   the assembler must accept the text; the re-assembled word must
   disassemble to the *identical text* with identical branch class and
   resolved target; and it must be a fixed point of
   assemble∘disassemble.  Words may legitimately change - the
   architecture has ~2.9 M aliases in this set (two "no Z write"
   spellings, subtract-vs-add-of-negative, format 5 vs 6d, 8a vs 1b…) -
   but meaning and spelling may not.

3. **Programs.**  Every app below is assembled, its image disassembled
   into a complete source (words that don't decode cleanly become
   `.word 0x…`), that source re-assembled, and the images compared.

Layer 2 is what makes the pair trustworthy: it proves the two
independently written codecs agree on the *entire* cleanly decodable
opcode space, not just on examples.  (Building the assembler this way
found no disassembler bugs - the two agreed everywhere once the
assembler learned the encoding aliases above.)

## run_apps - assembler → emulator

`run_apps` assembles each program with the library, loads it at 0,
runs it (processor mode, big-endian - the AV Mac strapping), and
checks the results the source declares in `;;` directives:

```
;; steps <n>                 ;; reg r<N> == <val>
;; start <addr>              ;; mem <addr> == <val>
;; stop bkpt|waiti           ;; memf <addr> ~= <f> tol <t>   (DSP32)
;; irq-on-waiti <vec> <n>    ;; acc a<N> ~= <f> tol <t>
```

`make test` runs every app on **both DAUs**; `run_apps -t app.s`
traces each instruction through the disassembler.  New tests are new
`.s` files - no C changes needed.

| App | What it proves |
|---|---|
| `sum.s` | do-loops (3b), post-incremented loads, register adds, direct stores |
| `fib.s` | the 3a loop branch, delayed branches, register moves - fib(20) = 0x1a6d |
| `memcpy.s` | `(byte)` moves post-increment by 1, both loop kinds; copies and reverses a 16-byte string |
| `bitops.s` | the whole ALU: shifts, rotates, `&~`/`^`/`|`/`#`, compare + conditional increment, reverse subtract, `sp = sp±±` moving by 4 |
| `calls.s` | nested call/return on distinct link registers, delay slots, a software stack on r21 - factorial by repeated-addition multiply |
| `floats.s` | DA MACs in a do-loop, `.float` DSP32 data, Z-field stores, `int32`/`float32` - an exact dot product (6.5) |
| `irq.s` | evtp vector dispatch, emr, waiti's latent instruction, ireturn's instruction-shadow replay (the loop only exits if the replay happens) |

## Findings

No emulator bugs were found by this suite - both DAUs agree with each
other and with the manual on everything the apps exercise.  Two
behaviours that *look* like bugs and are not, recorded here so the next
reader doesn't re-investigate:

- **Format-7a direct addresses land in the on-chip window.**
  `*0x400 = r2` in processor mode writes 0x50030400, not flat 0x400
  [IM §6.3] - the emulator is right, and the apps' checks use the
  0x5003xxxx addresses.
- **`int32()`/`int16()` round, they don't truncate.**  dauc resets to
  round-to-nearest-ties-up [IM Table 8-3], so `int32(6.5)` is 7.

One assembler-level pitfall the hard way: an **immediate compare
sign-extends its 16-bit operand** (`r7 - 0xd007` compares against
0xffffd007), so `bitops.s` compares against a `(ushort24)`-loaded
register instead.

## Build

```
make            # roundtrip, run_apps (approximate DAU), run_apps_exact
make test       # everything shown above
```
