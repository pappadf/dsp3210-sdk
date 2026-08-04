; bitops.s — the CAU's whole logical/shift repertoire, with results
; computed by hand.
;
; Exercises: (ushort24), immediate or/and/and-not/xor-free ops, register
; and immediate shifts, arithmetic shift, rotates, compare + conditional
; increment, reverse subtract, the sp = sp++/-- stack bumps (by 4).
;
; Results are stored with format-7a direct addressing, which reaches the
; on-chip 64 KB window — at 0x5003xxxx in processor mode [IM §6.3] —
; hence the check addresses.
;
;; mem 0x50030640 == 0xdeadbe
;; mem 0x50030644 == 0xd347
;; mem 0x50030648 == 0xd007
;; mem 0x5003064c == 0xd008
;; mem 0x50030650 == 0x64
;; mem 0x50030654 == 0x6804
;; mem 0x50030658 == 0xd000
;; reg r11 == 0xd008
;; reg r21 == 0x104

start:  r1 = (ushort24) 0xdead00
        r1 = r1 | 0xbe          ; 0xdeadbe
        r2 = r1
        r2 = r2 << 8            ; 0xdeadbe00
        r3 = r2
        r3 = r3 >> 16           ; 0x0000dead
        r4 = r3
        r4 = r4 $>> 4           ; 0x00000dea
        r5 = r3 ^ r4            ; 0xdead ^ 0xdea = 0xd347
        r6 = r5
        r6 = r6 & 0xff0         ; 0x0340
        r7 = r5 - r6            ; 0xd007
        r13 = (ushort24) 0xd007 ; (an immediate compare would sign-extend)
        r7 - r13                ; compare: equal
        if (eq) r8 = r7 + 1     ; taken -> 0xd008
        r9 = 0x0
        r9 = 0x64 - r9          ; reverse subtract -> 0x64
        r10 = r8 >>>1           ; rotate right -> 0x6804
        r11 = r10 <<<1          ; rotate left  -> 0xd008
        r12 = r11 &~ r4         ; 0xd008 &~ 0xdea = 0xd000
        r21 = 0x100
        sp = sp++               ; +4
        sp = sp++               ; +4
        sp = sp--               ; -4 -> 0x104
        *0x640 = r1
        *0x644 = r5
        *0x648 = r7
        *0x64c = r8
        *0x650 = r9
        *0x654 = r10
        *0x658 = r12
        bkpt
