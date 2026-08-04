; irq.s — external interrupts, the vector table, waiti and ireturn.
;
; Exercises: evtp (r22) dispatch through a vector table built with .org,
; emr unmasking, waiti with its latent instruction, handler state
; isolation (ps is shadowed at interrupt level), ireturn's instruction-
; shadow replay (the interrupt hits with "r2 = *COUNTER" prefetched;
; ireturn must execute it, or r2 reads stale data and the loop never
; exits).
;
; The harness asserts EXT0 (vector 8) each time the core reaches waiti,
; three times; the handler counts them, the main loop exits at 3.
;
;; irq-on-waiti 8 3
;; stop bkpt
;; mem 0x50030200 == 0x3
;; reg r2 == 0x3

        .equ COUNTER, 0x200

start:  r22 = (ushort24) vtab   ; r22 is evtp
        r1 = 0x100              ; emr bit 8 = EXT0
        emr = (short) r1
loop:   waiti
        nop                     ; waiti's latent instruction
        r2 = *COUNTER
        nop                     ; load-use spacing
        r2 - 0x3
        if (lt) goto loop
        nop
        bkpt

        .org 0x100              ; 16 vectors x 8 bytes
vtab:
        .org 0x140              ; vector 8: EXT0
        goto handler
        nop

        .org 0x180
handler:
        r3 = *COUNTER
        nop
        r3 = r3 + 0x1
        *COUNTER = r3
        ireturn
