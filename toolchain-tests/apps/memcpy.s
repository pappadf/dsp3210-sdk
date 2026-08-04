; memcpy.s — byte-wise copy, then byte-wise reversal.
;
; Exercises: (byte) loads and stores with post-increment (the pointer
; moves by the operand size, i.e. 1), the 3a loop branch, a decrementing
; pointer walk inside a do-loop.  The 16-byte message "DSP3210 ROCKS!!\0"
; is copied to DST and reversed into RDST.
;
;; mem 0x600 == 0x44535033
;; mem 0x604 == 0x32313020
;; mem 0x608 == 0x524f434b
;; mem 0x60c == 0x53212100
;; mem 0x620 == 0x212153
;; mem 0x624 == 0x4b434f52
;; mem 0x628 == 0x20303132
;; mem 0x62c == 0x33505344

        .equ DST,  0x600
        .equ RDST, 0x620

start:  r1 = (ushort24) src
        r2 = (ushort24) DST
        r9 = 0xe                ; 14 -> body runs 14+2 = 16 times
copy:   r3 = (byte) *r1++
        nop
        *r2++ = (byte) r3
        if (r9-- >= 0) goto copy
        nop

        r1 = (ushort24) src
        r1 = r1 + 0xf           ; last byte
        r4 = (ushort24) RDST
        do 3, 15                ; four instructions, sixteen times
        r3 = (byte) *r1
        r1 = r1 - 0x1
        nop
        *r4++ = (byte) r3
        bkpt

src:    .word 0x44535033, 0x32313020, 0x524f434b, 0x53212100
