# The AT&T DSP3210 - CPU programmer's reference

A condensed, searchable reference for the DSP3210's **CPU core**: the
register set, the memory model, the DSP32 floating-point format, the
execution and exception rules, and the complete instruction set with
encodings.  It exists because the only circulating copies of AT&T's
*DSP3210 Information Manual* are page scans.

**About this document.**  It is written from scratch; no AT&T text or
figures are reproduced.  The technical facts come from the AT&T
*DSP3210 Information Manual* (cited as `[IM §x]` so you can follow up
in a scan if you have one) and from behaviour verified on real DSP3210
machine code; every encoding and semantic statement here is
implemented by this SDK's assembler, disassembler and emulator and is
exercised by their test suites, including an exhaustive
encode/decode agreement sweep.  Where the manual is ambiguous or
self-contradictory, that is said explicitly.  Bits are numbered 31..0,
bit 31 the MSB; `$` prefixes hex.

Out of scope: the on-chip peripherals (SIO, DMA, timer, BIO pins) and
everything electrical.  Peripheral control registers live in the
memory map, not in the register set, and are board-specific in use.

---

## 1. Programming model

The DSP3210 executes one 32-bit instruction per word, on 4-byte
boundaries.  Internally it has two execution units, and the
instruction set splits the same way:

- **CAU** - the control arithmetic unit: 32-bit integer ALU, address
  generation, branches, moves.  ("CA instructions".)
- **DAU** - the data arithmetic unit: 32-bit floating-point
  multiply-accumulate with 40-bit accumulators, one multiply + add per
  instruction, up to three memory operands with pointer
  post-modification.  ("DA instructions".)

### 1.1 Register set

| Register | Size | Role |
|---|---|---|
| `r0` | 32 | hardwired zero: reads 0, writes discard |
| `r1`-`r14` | 32 | general; **the only registers usable as DA memory pointers** |
| `r15`-`r19` | 32 | general; **the only registers usable as DA post-increment registers** |
| `r20` | 32 | general; the **error trace register** - error/reset dispatch is a `call` that stores the aborted pc here |
| `r21` (`sp`) | 32 | general; the stack pointer by convention - `sp = sp++` / `sp = sp--` move it by **4** |
| `r22` (`evtp`) | 32 | general; the **exception vector table pointer** - exceptions dispatch to `evtp + 8·vector` |
| `pc` | 32 | program counter.  As a source operand it reads **the address of the current instruction + 8** (§4.1) |
| `pcsh` | 32 | pc shadow: the interrupt resume address.  Readable *and writable* as a normal register operand |
| `a0`-`a3` | 40 | DAU accumulators: 24-bit mantissa + 8 guard bits + 8-bit exponent (§3.2) |

CAU registers are addressed by a 5-bit code that is deliberately
discontinuous - see §6.3.

### 1.2 IO registers

Reachable only by the register↔IO-register and memory↔IO-register move
formats (§7.4); each has a 5-bit code.  All other codes are reserved.
(`ioc`, `dmac`, `tcon`, `bioc` and the peripherals are **not** here -
they are memory-mapped.)

| Code | Register | Size | Role |
|---|---|---|---|
| 0 | `ps` | 16 | processor status: the flags (§1.3).  Only bits 3-0 (the CAU flags) are writable |
| 8 | `emr` | 16 | error/interrupt mask: **bit *n* enables vector *n*** (§5.2) |
| 10 | `spc` | - | write-only pseudo-register: a store to `spc` is the `waiti`/`bkpt`/`sftrst` operation, selected by the move size (§7.5) |
| 12 | `pcw` | 16 | processor control word (§1.4) |
| 14 | `dauc` | 8 | DAU control (§1.5) |
| 15 | `ctr` | 6 | clip-test register: DAU sign history (§1.6) |

### 1.3 Flags - the `ps` register

| Bit | Flag | Set by |
|---|---|---|
| 0 | `n` | CAU negative |
| 1 | `z` | CAU zero |
| 2 | `v` | CAU overflow |
| 3 | `c` | CAU carry / borrow |
| 4 | `N` | DAU negative |
| 5 | `Z` | DAU zero |
| 6 | `U` | DAU underflow |
| 7 | `V` | DAU overflow |
| 8 | `IBF` | SIO input buffer full |
| 9 | `OBE` | SIO output buffer empty |
| 10 | `SY` | SIO sync pin state |
| 11 | `FB` | frame boundary |
| 12 | `IR0` | external interrupt 0 pin state |
| 13 | `IR1` | external interrupt 1 pin state |

Bits 15-14 read 0.  Writing `ps` affects only `n z v c`.  Per-
instruction flag effects are summarised in §9.  Note one behaviour the
manual states only inside a pipeline restriction and shipped code
relies on: **moves that load a register set `n`/`z`** from the loaded
(extended) value [IM §4.4.1.3].

### 1.4 `pcw` - processor control word

The CPU-relevant bits (the rest configure bus wait states and are
board-specific):

| Bit | Meaning |
|---|---|
| 8 | memory byte order: 1 = big-endian (how the AV Macs run) |
| 10 | memory map: 0 = **processor mode** (on-chip window at `$5003xxxx`), 1 = **computer mode** (window at `$0000xxxx`) |
| 13 | lock: writing 1 makes `pcw` read-only until reset or an error exception |

At reset, bits 13-10 latch the BIO7..BIO4 pin straps (that is how a
board selects boot behaviour and initial map).

### 1.5 `dauc` - DAU control

| Bits | Meaning |
|---|---|
| 1-0 | companded-data law for `ic`/`oc`: µ-law or A-law, input and output selects.  *The manual contradicts itself* between its Table 8-3 and the OC instruction page on the output-select polarity; real code (verified against a shipped audio module) follows **Table 8-3**: bit 1 = 1 selects A-law output |
| 5-4 | float→integer rounding for `int16`/`int32`: `x0` round to nearest, ties up; `01` truncate toward -∞; `11` truncate toward 0 [IM Table 8-3].  **Reset state is round-to-nearest** - `int32(6.5)` is 7, not 6 |
| 6 | conditional-store behaviour of `ifalt`/`ifaeq`/`ifagt`: with bit 6 = 1, a suppressed store still performs the Z pointer post-modification |
| 7 | must be 0 |

### 1.6 `ctr` - clip-test register

A 6-bit shift register: every DA arithmetic result shifts its `N`
(negative) flag in at the bottom [IM Table 8-4].  Reading `ctr` after a
block of six operations gives the sign history - the fast way to test
"did anything clip" in a filter kernel.

---

## 2. Memory model

- 32-bit byte addresses; instructions are 32-bit words on 4-byte
  boundaries.
- **On-chip 64 KB window** (boot ROM, MMIO, RAM banks): one physical
  array, decoded at `$0000xxxx` in computer mode and `$5003xxxx` in
  processor mode (`pcw[10]`, §1.4).  The window moves *live* - the
  mask boot ROM's own startup sequence executes across the switch.
- External memory occupies the rest of the map (in computer mode,
  external RAM starts above the window).
- Byte order follows `pcw[8]`; both orders are architectural.
- Alignment: 32-bit accesses must be 4-byte aligned, 16-bit accesses
  2-byte aligned; violations raise the address error (vector 4).  If
  that error is *masked*, the access completes with the low address
  bits ignored - shipped code exploits this to reach a byte-wide MMIO
  register with a long store.
- **Format 7a direct addressing** (`*L`, a 16-bit address in the
  instruction) reaches the **on-chip window**, not flat address `L`
  (in processor mode, `*$0400` is `$50030400`).

---

## 3. The DSP32 floating-point format

Not IEEE 754.  An emulator or converter that reaches for `float` gets
wrong answers.

### 3.1 The 32-bit memory format

```
bit 31        30 ........ 8   7 ...... 0
    s         f (fraction)    e (exponent)
    └── mantissa, 24 bits ┘
```

- The **exponent is the low byte**; the mantissa the high 24 bits -
  the opposite layout to IEEE 754.
- **No separate sign bit**: the 24 mantissa bits are one
  2's-complement quantity of the form `s (!s) . f…f` - bit 31 is the
  sign and the *implicit second bit* is its complement (not a constant
  1).  So the represented mantissa M lies in [1, 2) for positive
  values and [-2, -1) for negative ones.
- Value = `M × 2^(e - 128)`, `e` unsigned, bias 128.
- **`e` = 0 is zero**, regardless of the mantissa bits ("dirty
  zeros" are treated as zero and flushed to a true zero on any write).
  There is exactly one zero; no negative zero.
- **No NaNs, no infinities, no denormals.**  IEEE ±∞ converts (via
  `dsp`) to the saturation values `$7FFFFFFF` / `$800000FF`; IEEE
  denormals convert to zero; an IEEE NaN raises the NaN error
  (vector 6).
- Range: about ±[2⁻¹²⁷, 2·2¹²⁷] - over 1500 dB - with a full 24 bits
  of precision at every magnitude.  Examples: `1.0` = `$00000080`,
  `-1.0` = `$8000007F` (i.e. -2 × 2⁻¹), `1.5` = `$40000080`.

### 3.2 The 40-bit accumulators

```
bit 39 ............. 16   15 ..... 8   7 ..... 0
     mantissa (24 bits)   guard (8)    exponent
```

Same scheme with 8 extra fraction ("guard") bits.  Guard-bit rules
[IM §8.2.3]:

- an accumulator feeding the **multiplier** has its guard bits
  truncated first;
- an accumulator feeding the **adder** keeps them;
- `round` is the only rounding operation: 40 → 32 bits, round to
  nearest with **ties toward the greater value**, guard bits cleared;
- overflow/underflow (vector 5, `V`/`U`) are judged against the
  **40-bit** range; overflow saturates, underflow flushes to zero.

Two datapath details the manual never states outright (this SDK's
exact DAU documents its reasoned choices): whether the adder's
alignment shift truncates or rounds (truncation is modelled), and
whether the product is narrowed to 32 fraction bits before the add
(full width is modelled).  Settling them needs real silicon.

---

## 4. Execution rules

### 4.1 Delayed branches

Every control transfer - `goto`, conditional `goto`, `call`,
`return`, the loop branch - executes **the instruction after it** (the
"latent" instruction, the delay slot) before the branch takes effect.
Two back-to-back branches are legal and both are honoured: for
`I1: goto A` / `I2: goto B` the execution order is I1, I2, A, B
[IM §4.4.2.3].

`pc` read as an operand yields **the address of the instruction after
the delay slot**, i.e. the branch instruction's address + 8.  `call`
stores that same value in its link register - so a subroutine returns
past the caller's delay slot with a plain `goto rM`.

**`ireturn` is the one exception**: it has *no* delay slot.  The
memory word after `ireturn` is never executed; in its place the CPU
replays, from the instruction shadow register, the instruction it had
already fetched when the interrupt hit, then resumes at `pcsh` (§5.4).

### 4.2 Hardware do-loops

`do K, N` executes the next **K+1** instructions **N+1** times with no
branch overhead (K ≤ 127 as encoded; N ≤ 2047, or from a register,
`(rM & $7FF) + 1` iterations).  `dolock` is the same with interrupts
masked for the loop's duration; `doblock` repeats a **single**
instruction.  The body must not contain control transfers, and the
loop machinery is part of the state interrupts save/restore.  (In
assembler source the counts are the raw field values: `do 2, 9` runs a
three-instruction body ten times.)

### 4.3 Pipeline restrictions (real hardware)

On silicon these are enforced by AT&T's assembler; they matter when
writing code intended for hardware (this SDK's emulator is
deliberately zero-latency and forgiving):

- a CAU/IO register loaded from memory, and **the flags that load
  set**, cannot be referenced by the very next instruction
  [IM §4.4.1.3];
- a DA memory write is not readable for 3 instructions; an accumulator
  used as a *multiplier* input must have been written ≥3 instructions
  earlier; DAU condition codes need ≥4;
- a Z-field pointer that was post-modified may only be reused as a Z
  pointer in the immediately following instruction;
- a CAU/IO store may not directly follow a DA instruction with a
  Y-field memory read.

Exactly three things are non-interruptible for one cycle [IM §7.5.1]:
register loads (from memory or IO register), all branch instructions,
and the `sp = sp++`/`sp--` bumps.

---

## 5. Exceptions and interrupts

### 5.1 Processing levels

Four states [IM §7.5]: **base**, **interrupt**, **error**, **double
error**.  Interrupts use a *continuation* model (state saved, resumed
by `ireturn`); errors an *abort* model (the offending instruction is
aborted, recovery is by software); a double error halts the chip until
hardware reset.

### 5.2 Vectors and masking

The vector table is 16 entries × **8 bytes** (two instructions each),
based at `evtp` (= `r22`, cleared by reset).  For every maskable
source, **the `emr` bit number equals the vector number**.

| Vector | Source | Class |
|---|---|---|
| 0 | reset | non-maskable |
| 1 | bus error | non-maskable |
| 2 | illegal opcode | non-maskable |
| 3 | reserved | non-maskable |
| 4 | address error (misalignment) | error, `emr[4]` |
| 5 | DAU overflow/underflow | error, `emr[5]` |
| 6 | NaN on `dsp()` conversion | error, `emr[6]` |
| 7 | reserved | - |
| 8 | EXT0 (IR0 pin) - **highest interrupt priority** | interrupt, `emr[8]` |
| 9 | timer | interrupt, `emr[9]` |
| 10 | reserved | - |
| 11 | SIO input buffer full | interrupt, `emr[11]` |
| 12 | SIO output buffer empty | interrupt, `emr[12]` |
| 13 | SIO DMA input frame | interrupt, `emr[13]` |
| 14 | SIO DMA output frame | interrupt, `emr[14]` |
| 15 | EXT1 (IR1 pin) - lowest priority | interrupt, `emr[15]` |

The interrupt facility is **single-level**: fixed priority, no
nesting.  Interrupt entry disables interrupts; only `ireturn`
re-enables them.

### 5.3 Dispatch

Dispatch is a real branch executed by the sequencer:

- interrupt: `goto evtp + 8·vn` + `nop`;
- error and reset: **`call evtp + 8·vn (r20)`** + `nop` - which is why
  `r20` holds the aborted pc after an error (specifically the aborted
  instruction's address + 8, the same value `pc` reads as an operand).

Since each table slot is two instructions and dispatch is a delayed
branch, a slot holds either `goto handler` + latent instruction, or -
the three-cycle "quick interrupt" - one useful instruction +
`ireturn`.

### 5.4 Interrupt entry and exit

Entry saves, into hardware shadow registers: the invisible do-loop
state, `a0`-`a3` (with guard bits), `dauc`, `ps` and `ctr`; puts the
resume address in `pcsh`; latches the already-fetched next instruction
into the instruction shadow register `irsh`; and disables interrupts.
`r1`-`r22`, `emr` and `pcw` are **not** saved.  (The manual is
ambiguous about `ps`/`ctr` - its Figure 4-1 gives them shadows, its
§7.5.1 narrative omits them; shadowing them is the reading consistent
with shipped code.)

`ireturn` (`$803E0000`) restores the shadows, re-enables interrupts,
executes the `irsh` instruction in place of a delay slot (§4.1) and
resumes at `pcsh`.  Because `pcsh` is a writable register, a handler
can redirect the return - the mask boot ROM's startup does exactly
that.

### 5.5 Error entry, `sftrst`, double error

Error entry aborts the current instruction, kills active do-loops,
disables interrupts and DMA, and clears the `pcw` lock.  Recovery is
by software: fix things up, then `sftrst` to drop back to base level
and branch.  Nesting rules [IM §7.5.2]: a maskable error at error
level is ignored; a non-maskable error while handling a non-maskable
error is a **double error** - the chip halts (asserting both IACK
pins) until RESTN.

### 5.6 `waiti`

`waiti` idles the CPU until an unmasked interrupt arrives.  It is
ignored if an interrupt is already pending, may only be executed at
base level, and **its latent instruction executes when the interrupt
is recognised** - so a `nop` conventionally follows it.

---

## 6. Instruction encoding

### 6.1 Top-level opcode map

Every instruction is one 32-bit word; the top 6 bits (with the top 3
for the wide-immediate formats) select the format:

| Bits 31-26 | Format | Instruction |
|---|---|---|
| `000000`, `000001`, `000010` | - | **illegal opcode** |
| `000011` | 3a | `if (rM-- >= 0) goto` - loop branch |
| `000100` | 4a | `call {N, rB±N, pc±N} (rM)` |
| `000101` | 5a | `rD = (short) rS3 + N` |
| `000110` | 6a/6c | short ALU (bit 25: 0 = register, 1 = immediate) |
| `000111` | 7a | memory-direct move `rH = *L` / `*L = rH` (bit 25 must be 0) |
| `001xxx` | DA 1/4 | multiply/accumulate, adder input = Y (M = `111` illegal) |
| `010xxx` | DA 2 | multiply/accumulate, tap: Y is also stored via Z (M = `11x` illegal) |
| `011xxx` | DA 3/5 | multiply/accumulate (M ≤ `101`) / special function (`01111…`) |
| `100000` | 0b/1b | `if (COND) goto {N, rB±N, pc±N}` - also `nop`, `ireturn` |
| `100010` | - | **illegal opcode** |
| `100011` | 3b/3c | `do` / `dolock` / `doblock` (bit 25: iteration count immediate/register) |
| `100100` | 4b | `rD = rS <<| N` - shift-or |
| `100101` | 5b | `rD = rS3 + N` (32-bit) |
| `100110` | 6b/6d | 32-bit ALU (bit 25 as above) |
| `100111` | 7b/7c/7d | register↔IO-reg, register↔memory, IO-reg↔memory moves |
| `101xxx` | 8a | `goto {M, rB+M}` - 24-bit immediate |
| `110xxx` | 8b | `rD = (ushort24) M` |
| `111xxx` | 8c | `call M (rM)` |

The **seven illegal opcodes** are exactly `000000 000001 000010 001111
010110 010111 100010` [IM §7.5.3.2]; they raise vector 2.  (`001111`,
`010110`, `010111` are the DA formats' disallowed M values.)

### 6.2 CA field layouts

```
0b/1b  cond. goto   31-27=10000   C:26-21   rB:20-16   N:15-0
3a     loop branch  31-26=000011  rM:25-21  rB:20-16   N:15-0
3b     do (imm)     31-26=100011  25=0  B:24  M:23  K:17-11  L:10-0
3c     do (reg)     31-26=100011  25=1  B:24  M:23  K:17-11  rM:4-0
4a     call         31-26=000100  rM:25-21  rB:20-16   N:15-0
4b     shift-or     31-26=100100  rD:25-21  rS:20-16   N:15-0
5a/5b  3-op add     31-26=x00101  rD:25-21  rS3:20-16  N:15-0        (bit 31: 0=short 1=long)
6a/6b  ALU reg      30-25=001100  F:24-21  rD:20-16  rS1:15-11  C:10-5  rS2:4-0   (bit 31 as above)
6c/6d  ALU imm      30-25=001101  F:24-21  rD:20-16  N:15-0
7a     mem direct   31-25=0001110  T:24  W:23-21  rH:20-16  L:15-0
7c     reg indirect 31-25=1001110  T:24  W:23-21  rH:20-16  rP:15-11  bit10=0  rI:4-0
7b     reg <-> ior  31-25=1001110  T:24  W:23-21  rH:20-16  0:15-11   bit10=1  ior:4-0
7d     mem <-> ior  31-25=1001111  T:24  W:23-21  ior:20-16 rP:15-11  bit10=0  rI:4-0
8a/8b/8c            31-29=101/110/111   M-hi:28-21   rB|rD|rM:20-16   M-lo:15-0
```

`T` = transfer direction (0: into the register, 1: out of it).  `N` is
sign-extended 16 bits.  The 24-bit `M` of formats 8x is `M-hi:M-lo`,
zero-extended.  In formats 0b/1b/3a/4a, `rB` = `r0` makes `N` an
absolute address and `rB` = `pc` makes it pc-relative (target =
instruction address + 8 + N).

### 6.3 The CAU register-code space (5 bits - deliberately discontinuous)

| Code | Operand | Code | Operand |
|---|---|---|---|
| `00000` | `r0` (zero) | `10000` | reserved |
| `00001`-`01110` | `r1`-`r14` | `10001`-`10101` | `r15`-`r19` |
| `01111` | `pc` | `10110` / `10111` | `-n` / `+n` pseudo-operands |
| | | `11000`-`11010` | `r20`, `r21` (sp), `r22` (evtp) |
| | | `11011`-`11101`, `11111` | reserved |
| | | `11110` | `pcsh` |

The `-n`/`+n` codes are not registers: in an ALU source they mean
∓1/±1 (the increment/decrement forms; ±4 in the `sp = sp±±` special
case), and in the `rI` post-modify position they mean "the operand
size" (so `*rP++` post-increments by 1, 2 or 4 as the move size
dictates).  `rI` = `r0` means no post-modification; any other `rI`
adds that register's value (unscaled).  **There is no displacement
field in register-indirect addressing.**

### 6.4 Condition codes (6 bits)

| Code | Cond | Meaning | Code | Cond | Meaning |
|---|---|---|---|---|---|
| 0 | `false` | never (a `nop`) | 16-17 | `auc`/`aus` | DAU U clear/set |
| 1 | `true` | always | 18-19 | `age`/`alt` | DAU N clear/set |
| 2-3 | `pl`/`mi` | n clear/set | 20-21 | `ane`/`aeq` | DAU Z clear/set |
| 4-5 | `ne`/`eq` | z clear/set | 22-23 | `avc`/`avs` | DAU V clear/set |
| 6-7 | `vc`/`vs` | v clear/set | 24-25 | `agt`/`ale` | !(N∨Z) / N∨Z |
| 8-9 | `cc`/`cs` | c clear/set | 32-35 | `ibe ibf obf obe` | SIO buffer states |
| 10-11 | `ge`/`lt` | signed ≥ / < | 40-43 | `syc sys fbc fbs` | sync / frame bits |
| 12-13 | `gt`/`le` | signed > / ≤ | 44-47 | `ir0c ir0s ir1c ir1s` | IR pin states |
| 14-15 | `hi`/`ls` | unsigned > / ≤ | | | others reserved |

### 6.5 Move sizes (the 3-bit `W` field)

| W | Keyword | Transfer | Register effect on load |
|---|---|---|---|
| `000` | `(byte)` | 8 bits | zero-extended |
| `001` | `(char)` | 8 bits | sign-extended |
| `010` | `(ushort)` | 16 bits | zero-extended |
| `011` | `(short)` | 16 bits | sign-extended |
| `100` | `(hbyte)` | 8 bits | into bits 15-8 ("high byte") |
| `111` | `(long)` - the default, never written | 32 bits | - |

`101`/`110` are reserved.  Stores write the corresponding lane of the
register (`(hbyte)` stores bits 15-8).  Post-modification by `±n`
moves the pointer by the transfer size.

### 6.6 ALU functions (the 4-bit `F` field)

| F | Syntax | Operation |
|---|---|---|
| 0 | `+` | add (also register move `rD = rS`, doubling `rD = rS * 2`, `±1` with the pseudo-operands, `sp = sp±±`) |
| 1 | `<<` | logical shift left by rS2/N (5 LSBs) |
| 2 | (imm) `rD = N - rD` | reverse subtract |
| 3 | `#` | carry-reverse add: bit-reversed operands added, result bit-reversed back - carry propagates MSB→LSB (for FFT-style bit-reversed indexing) |
| 4 | `-` | subtract (also negate `rD = -rS`) |
| 5 | - | reserved |
| 6 | `&~` | and-complement (bit clear) |
| 7 | `-` (no store) | compare |
| 8 | `^` | exclusive or |
| 9 | `>>>1` | rotate right by 1 **through carry** |
| 10 | `\|` | or |
| 11 | `<<<1` | rotate left by 1 **through carry** (≡ add with carry-in) |
| 12 | `>>` | logical shift right |
| 13 | `$>>` | arithmetic shift right |
| 14 | `&` | and |
| 15 | `&` (no store) | bit test |

Register forms (6a/6b) take a condition `C`; the whole instruction is
suppressed when it is false.  Immediate forms (6c/6d) have no
condition field and reuse `rD` as the source: `rD = rD op N`.  In
`(short)` mode operations are 16-bit and results are sign-extended
into the register (the short logical shifts zero-extend before
shifting).

---

## 7. CA instruction reference

Notation: `{target}` = `{N, rB, rB±N, pc±N}` as in §6.2; sizes/flags
per §6.5/§9.

### 7.1 Control transfer (all with one delay slot, §4.1)

| Syntax | Format | Semantics |
|---|---|---|
| `goto {target}` | 1b (C=true) / 8a | unconditional branch; 8a extends the reach to a 24-bit `M` (absolute or `rB+M`) |
| `if (cond) goto {target}` | 0b/1b | branch if `cond` (§6.4) |
| `nop` | 0b | `if (false) goto r0+0` = `$80000000` |
| `if (rM-- >= 0) goto {target}` | 3a | loop branch: branch if `rM` ≥ 0 (**tested before** the decrement), then decrement `rM` always.  A loop entered at its body runs `count+2` times for an initial `rM = count` |
| `call {target} (rM)` | 4a / 8c | branch and link: `rM` = call address + 8 (past the delay slot); 8c gives a 24-bit absolute target |
| `return (rM)` | = `goto rM` | encoding-identical to `goto rM` |
| `ireturn` | 0b special (`$803E0000`) | return from interrupt: restores shadows, replays `irsh`, resumes at `pcsh`; **no delay slot** (§5.4) |

### 7.2 Do loops (§4.2)

| Syntax | Format | Semantics |
|---|---|---|
| `do K, N` / `do K, rM` | 3b/3c | next K+1 instructions, N+1 (or `(rM&$7FF)+1`) times |
| `dolock K, N` / `dolock K, rM` | 3b/3c (B=1) | ditto, interrupts masked for the duration |
| `doblock N` / `doblock rM` | 3b/3c (M=1) | one instruction, N+1 times |

### 7.3 Arithmetic and logic

| Syntax | Format | Notes |
|---|---|---|
| `rD = [(short)] rS3 + N` | 5a/5b | three-operand add with a 16-bit signed immediate; with `rS3 = r0` this is the **load-immediate** `rD = N` |
| `rD = (ushort24) M` | 8b | load a 24-bit zero-extended immediate; **no flags** |
| `rD = rS <<| N` | 4b | shift-or: `rD = rS \| (N << 16)` - pairs with 8b/5x to build any 32-bit constant in two instructions; always 32-bit |
| `[if (cond)] rD = [(short)] rS1 op rS2` | 6a/6b | ALU register forms, `op` from §6.6 |
| `rD = [(short)] rD op N` | 6c/6d | ALU immediate forms (16-bit signed N; shift counts use the 5 LSBs) |
| `[if (cond)] [(short)] rS1 - rS2`, `rD - N` | 6x F=7 | compare: subtract, set flags, no store |
| `[if (cond)] [(short)] rS1 & rS2`, `rD & N` | 6x F=15 | bit test: and, set n/z, no store |
| `[if (cond)] rD = [(short)] rS ± 1` | 6a/6b F=0/4 | increment/decrement via the `+n`/`-n` pseudo-operands |
| `[if (cond)] sp = [(short)] sp++` / `sp--` | 6a/6b F=0 | move `r21` by **4** (same encoding family as `±1`, distinguished by rD = rS1 = r21) |

### 7.4 Moves

| Syntax | Format | Semantics |
|---|---|---|
| `rH = (w) *L` / `*L = (w) rH` | 7a | direct address: 16-bit `L` into the **on-chip window** (§2) |
| `rH = (w) *rP[±±/++rI]` / `*rP[…] = (w) rH` | 7c | register-indirect with post-modification (§6.3) |
| `rH = (w) ior` / `ior = (w) rH` | 7b | CAU register ↔ IO register (§1.2) |
| `ior = (w) *rP[…]` / `*rP[…] = (w) ior` | 7d | IO register ↔ memory |

Loads (into CAU or IO registers) set `n`/`z` from the extended value.
Memory is not modified by loads; stores set no flags.

### 7.5 The `spc` pseudo-instructions

All three are format-7b stores of `r0` to `spc` (code 10),
distinguished **only by the move size**:

| Syntax | Word | Semantics |
|---|---|---|
| `waiti` | `$9DE0040A` (`(long)`) | wait for interrupt (§5.6) |
| `bkpt` | `$9D60040A` (`(short)`) | breakpoint; hardware behaviour undocumented (this SDK's emulator halts) |
| `sftrst` | `$9D00040A` (`(byte)`) | software reset: drop the processing level to base - its sole function; no registers change |

---

## 8. DA instruction reference

### 8.1 The multiply/accumulate forms

Every DA arithmetic instruction computes at most one product and one
sum and writes an accumulator, optionally teeing the result (or the Y
operand) to memory:

```
aN = [-] adder ± product
```

with an optional `Z =` store.  The encodable shapes (F is the sign
before the adder input, S the sign of the product; `M` selects the
adder/multiplier constant: `000`-`011` = a0-a3, `100` = 0.0, `101` =
1.0):

| Shape (assembler syntax) | Format | M |
|---|---|---|
| `[Z =] aN = [-]Y + aM * X` | 1 | aM |
| `[Z =] aN = [-]Y ± X` | 1 | 1.0 |
| `[Z =] aN = [-]Y` | 1 | 0.0 |
| `aN = [-](Z = Y) ± X` | 1 (M=`110`) | - ("tap" of the adder input) |
| `aN = [-]aM ± (Z = Y) * X` | 2 | aM |
| `aN = [-](Z = Y) * X` | 2 | 0.0 |
| `[Z =] aN = [-]aM ± Y * X` | 3 | aM |
| `[Z =] aN = [-]Y * X` | 3 | 0.0 |

Field layout (formats 1, 2, 3):

```
31-29 = format   28-26 = M   25 = 0   24 = F   23 = S
22-21 = N (destination aN)   20-14 = X   13-7 = Y   6-0 = Z
```

### 8.2 X/Y/Z operand fields (7 bits: `p`:6-3, `i`:2-0)

- `p` = `0001`-`1110`: memory via pointer **r1-r14**, with
  post-modification `i`: `000` none, `001`-`101` add **r15-r19**,
  `110`/`111` = `--`/`++` by the operand size.
- `p` = `0000`: register-direct - `i` = `000`-`011` selects a0-a3
  (X and Y only); `i` = `111` means **no Z write** (Z only).
- `p` = `1111` is not allowed.

Two spellings of "no Z write" exist in the wild: the manual's examples
use `0000111` and shipped assembler output uses `1111111`; both are
unambiguous (p=`1111` cannot address memory) and both must be
accepted by tools.

Within one instruction the operand order is Y, then X, then Z.  All
three memory references transfer 32-bit DSP32 words; the arithmetic
is §3's semantics with 40-bit accumulation.

### 8.3 Special functions (format 5)

```
31-27 = 01111   G:26-23   N:22-21   20-14 = 0   Y:13-7   Z:6-0
[Z =] aN = g(Y)
```

| G | Function | Semantics | Flags |
|---|---|---|---|
| 0 | `ic` | companded (µ/A-law) byte → float, law per `dauc` | N Z, V=U=0 |
| 1 | `oc` | float → companded byte (nearest code) | none |
| 2 | `float16` | 16-bit signed int → float | N Z, V=U=0 |
| 8 | `float32` | 32-bit signed int → float | N Z, V=U=0 |
| 3 | `int16` | float → 16-bit signed int, rounding per `dauc[5:4]`, saturating | none |
| 9 | `int32` | float → 32-bit signed int, ditto | none |
| 4 | `round` | 40-bit accumulator → 32-bit float, ties to the greater value | N Z V U |
| 5 | `ifalt` | conditional load: `aN = Y` if DAU N set, else `aN` unchanged; the Z write stores `aN`'s resulting value (with `dauc[6]` = 1 a suppressed store still post-modifies the Z pointer) | none |
| 6 | `ifaeq` | ditto, if DAU Z set | none |
| 7 | `ifagt` | ditto, if neither N nor Z | none |
| 12 | `ieee` | DSP32 → IEEE 754 single | none |
| 13 | `dsp` | IEEE 754 single → DSP32; ±∞ saturates, denormals flush to 0, NaN raises vector 6 | N Z V U |
| 14 | `seed` | reciprocal seed: invert all word bits except the sign - the Newton-Raphson division starter | N Z U, V=0 |

G = 10, 11, 15 are reserved.  The int/byte-pattern results (`oc`,
`int16`, `int32`, `ieee`) are meaningful in the **Z memory write**,
which the manual requires to be issued in the same instruction; what
remains in the accumulator's low bits is architecturally
unpredictable.

---

## 9. Flag effects summary

| Instructions | n | z | v | c |
|---|---|---|---|---|
| add / subtract / compare / negate / reverse-subtract / `±1` / format-5 add / `<<<1` | ✓ | ✓ | ✓ | ✓ (carry or borrow) |
| `#` carry-reverse add | ✓ | ✓ | 0 | ✓ (reversed carry-out) |
| and / or / xor / and-complement / bit test / shifts | ✓ | ✓ | 0 | 0 |
| `>>>1` | ✓ | ✓ | 0 | ✓ (bit shifted out) |
| shift-or (4b) | ✓ | ✓ | 0 | 0 |
| register/IO loads (7a-7d) | ✓ | ✓ | 0 | 0 |
| `(ushort24)` load (8b), stores, branches, `call`, `do` | - | - | - | - |

DA arithmetic and `round`/`dsp` set **N Z** always and **V U** on
range violations (§3.2), shift `N` into `ctr`, and raise vector 5 /
vector 6 when unmasked.  In `(short)` mode the CAU rules apply at 16
bits (carry out of bit 15, overflow into bit 15) and results are
sign-extended into the register.

---

## 10. Assembler syntax

The syntax used throughout this document is the AT&T assembler syntax
of the Information Manual, exactly as accepted by this SDK's
[`dsp3210asm`](../dsp3210asm/README.md) and printed by
[`dsp3210dis`](../dsp3210dis/README.md) - see those READMEs for the
directive set (`.org/.word/.float/.space/.align/.equ`), labels, and
the canonical choices where the hardware offers two encodings for one
spelling.  Working example programs live in
[`toolchain-tests/apps/`](../toolchain-tests/apps/).

---

*All trademarks referenced in this document are the property of their
respective owners and are used for identification purposes only; no
endorsement by or affiliation with the trademark holders is claimed
(see the project [README](../README.md)).*
