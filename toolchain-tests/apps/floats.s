; floats.s — DAU dot product and conversions.
;
; Exercises: DA multiply/accumulate (formats 1 and 3) inside a do-loop,
; .float data in the DSP32 format, Z-field stores, the int32/float32
; special functions, accumulator reads.
;
; Every operand is exactly representable in 24 bits, so the dot product
; is exact:
;   1.5*2 - 2.25*4 + 3*(-1) + 0.5*8 + 10*0.25 - 0.125*16 + 8*0.5 + 2*3.5
;   = 3 - 9 - 3 + 4 + 2.5 - 2 + 4 + 7 = 6.5
;
; int32() honours the dauc float->int rounding mode, which resets to
; round-to-nearest-ties-up [IM Table 8-3]: int32(6.5) = 7.
;
;; memf 0x700 ~= 6.5 tol 1e-6
;; mem 0x704 == 0x7
;; memf 0x714 ~= 42.0 tol 1e-6
;; acc a0 ~= 6.5 tol 1e-6
;; acc a2 ~= 42.0 tol 1e-6

start:  r1 = (ushort24) va
        r2 = (ushort24) vb
        r3 = 0x700
        a0 = *r1++ * *r2++      ; first product
        do 0, 6                 ; one instruction, seven times
        a0 = a0 + *r1++ * *r2++
        *r3++ = a0 = a0         ; DSP32 float at 0x700
        *r3++ = a1 = int32(a0)  ; truncated int at 0x704

        r4 = 0x2a               ; 42
        r5 = 0x710
        *r5 = r4                ; (register-indirect: flat memory, not
        r6 = 0x714              ;  the on-chip window a *L store hits)
        *r6 = a2 = float32(*r5) ; 42 -> 42.0 at 0x714
        bkpt

va:     .float 1.5, -2.25, 3.0, 0.5, 10.0, -0.125, 8.0, 2.0
vb:     .float 2.0, 4.0, -1.0, 8.0, 0.25, 16.0, 0.5, 3.5
