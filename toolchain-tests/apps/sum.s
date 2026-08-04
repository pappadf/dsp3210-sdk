; sum.s — sum ten 32-bit words with a hardware do-loop.
;
; Exercises: (ushort24) address loads, do-loops (3b), post-incremented
; register-indirect loads (7c), register adds (6b), direct stores (7a).
; Expected result: 0x101+0x202+0x303+0x404+0x505+1+2+3+4+0x99 = 0xfb2.
;
;; mem 0x50030400 == 0xfb2
;; reg r2 == 0xfb2
;; reg r3 == 0x99

        .equ RESULT, 0x400

start:  r1 = (ushort24) data
        r2 = 0x0
        do 2, 9                 ; three instructions, ten times
        r3 = *r1++
        nop                     ; load-use spacing [IM §4.4.1.3]
        r2 = r2 + r3
        *RESULT = r2
        bkpt

data:   .word 0x101, 0x202, 0x303, 0x404, 0x505
        .word 1, 2, 3, 4, 0x99
