/*
 * dsp3210_asm.h — reference assembler for the AT&T DSP3210
 *
 * Sources of truth:
 *   - AT&T "DSP3210 Information Manual", chapter 4 (instruction set) and
 *     chapter 10 (instruction and register encodings), Tables 10-1..10-3.
 *   - Hardware-verified encodings recovered from real DSP3210 systems
 *     (Macintosh Quadra 840AV / Centris 660AV).
 *   - The companion reference disassembler (../dsp3210dis): the accepted
 *     statement syntax is exactly the disassembler's output syntax, so
 *     that assemble(disassemble(w)) is well defined, plus labels and the
 *     data directives below.
 *
 * Every instruction assembles to exactly one 32-bit word.  Where the
 * architecture provides more than one encoding for the same assembler
 * text (e.g. "goto 0x40" as format 1b or 8a, "r3 = r7" as an ALU move or
 * a 3-operand add), the assembler picks one canonical encoding; the
 * choices are documented in the README.  The output is a fixed point of
 * assemble∘disassemble: re-assembling the disassembly of assembler
 * output reproduces it bit for bit.
 *
 * Statements, one per line (";" or "//" starts a comment):
 *
 *   label:                        define a label (may share a line)
 *   <instruction>                 disassembler syntax, see README
 *   .org  <expr>                  move the location counter (>= origin)
 *   .word <expr> [, <expr> ...]   32-bit data words
 *   .float <f> [, <f> ...]        DSP32-format 32-bit floats
 *   .space <expr>                 zero-filled bytes
 *   .align <expr>                 zero-fill to a power-of-two boundary
 *   .equ  <name>, <expr>          define a constant
 *
 * Expressions are a number, a symbol, or a +/- chain of those.  Numbers
 * are 0x…, $…, or decimal.  A symbol used as a branch/call target
 * assembles pc-relative; numeric targets are absolute.
 *
 * Portable C99: no I/O, no allocation, no global state.
 */

#ifndef DSP3210_ASM_H
#define DSP3210_ASM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSP3210_ASM_OK  = 0,
    DSP3210_ASM_ERR = -1
};

typedef struct {
    int  line;       /* 1-based source line of the error (0 if n/a) */
    char msg[160];   /* human-readable description */
} dsp3210_asm_err;

/*
 * Assemble a complete source text.
 *   src  — nul-terminated assembler source
 *   org  — address of the first byte (the location counter's start)
 *   out  — receives the image, big-endian words, from address org;
 *          gaps created by .org/.space/.align are zero-filled
 *   cap  — capacity of out in bytes
 *   len  — receives the image length (highest byte written + 1 - org)
 *   err  — optional; filled in on failure
 * Returns DSP3210_ASM_OK or DSP3210_ASM_ERR.
 */
int dsp3210_assemble(const char *src, uint32_t org,
                     uint8_t *out, size_t cap, uint32_t *len,
                     dsp3210_asm_err *err);

/*
 * Assemble a single statement (one instruction, or ".word <n>") with no
 * label context.  addr is the address the instruction will execute at
 * (used for pc-relative operands, mirroring dsp3210_disassemble).
 */
int dsp3210_encode(const char *stmt, uint32_t addr, uint32_t *word,
                   dsp3210_asm_err *err);

/*
 * Host double -> DSP32 32-bit float (IM §3.4.2).  Bit-identical to the
 * emulator's dsp3210_double_to_dsp32; duplicated so the assembler stands
 * alone.  Used by the .float directive.
 */
uint32_t dsp3210_asm_dsp32(double v);

#ifdef __cplusplus
}
#endif

#endif /* DSP3210_ASM_H */
