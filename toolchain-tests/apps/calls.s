; calls.s — nested subroutine calls with a software stack.
;
; Exercises: call/return with different link registers, delay slots on
; call and goto, saving a link register on the r21 (sp) stack with
; post-incremented stores and sp = sp-- (which moves by 4), compares
; driving conditional loops.  The CAU has no multiplier, so factorial
; is computed by a nested repeated-addition multiply — which is the
; point: lots of control flow.
;
;   fact(5)  = 120    = 0x78
;   fact(7)  = 5040   = 0x13b0
;
;; mem 0x50030600 == 0x78
;; mem 0x50030604 == 0x13b0
;; reg r21 == 0x700

start:  r21 = 0x700             ; stack base
        r1 = 0x5
        call fact (r18)
        nop
        *0x600 = r2
        r1 = 0x7
        call fact (r18)
        nop
        *0x604 = r2
        bkpt

; r2 = r1!   (r1 >= 2; clobbers r3-r6)
fact:   *r21++ = r18            ; push the link: fact calls mul
        r2 = 0x1
        r4 = r1
floop:  r3 = r2
        r5 = r4
        call mul (r17)
        nop
        r4 = r4 - 1
        r4 - 0x1
        if (gt) goto floop
        nop
        sp = sp--               ; pop the link
        r18 = *r21
        nop                     ; load-use spacing
        goto r18
        nop

; r2 = r3 * r5  by repeated addition (r5 >= 2; clobbers r6)
mul:    r2 = 0x0
        r6 = r5 - 0x2           ; body runs (r5-2)+2 = r5 times
mloop:  r2 = r2 + r3
        if (r6-- >= 0) goto mloop
        nop
        goto r17
        nop
