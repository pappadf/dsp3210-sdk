; fib.s — iterative Fibonacci with a loop-counter branch.
;
; Exercises: the format-3a loop branch (decrement-and-test), delayed
; branches, register moves, post-incremented stores.  Runs the
; recurrence 19 times: r2 ends at fib(20) = 6765 = 0x1a6d, and the
; sequence 1, 2, 3, 5, … 6765 is stored at 0x420.
;
;; mem 0x50030500 == 0x1a6d
;; mem 0x420 == 0x1
;; mem 0x424 == 0x2
;; mem 0x428 == 0x3
;; mem 0x468 == 0x1a6d
;; reg r2 == 0x1a6d

        .equ RESULT, 0x500

start:  r1 = 0x0                ; f(0)
        r2 = 0x1                ; f(1)
        r5 = 0x420              ; sequence buffer
        r10 = 0x11              ; 17 -> body runs 17+2 = 19 times
loop:   r3 = r1 + r2
        *r5++ = r3
        r1 = r2
        r2 = r3
        if (r10-- >= 0) goto loop
        nop                     ; delay slot
        *RESULT = r2
        bkpt
