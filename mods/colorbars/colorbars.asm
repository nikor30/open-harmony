; colorbars: show the stock firmware's unused RGB test pattern at boot.
;
; Appended after the end of the 3.4 application image. main (0x28A0E) calls
; this instead of 0x2112C (see build.py). It runs the original call, keeps
; its result in W, draws the colour bars with the stock routine 0x229D0
; (176x220, red/green/blue) and waits about 2 s so they can be seen.
;
; Position independent apart from the absolute CALL targets, so it is
; assembled at 0 and placed by build.py.

        #include <p18f87j50.inc>
        radix hex

ORIG_FILL   equ 0x2112C     ; original call target at 0x28A42
COLORBARS   equ 0x229D0     ; stock test pattern, otherwise unused
RESULT      equ 0x84        ; bank 0xD: main stores the call result here
CNT_LO      equ 0x00        ; bank 0xD scratch, also used by the stock fills
CNT_HI      equ 0x01
DELAY       equ .90         ; outer loops of ~22 ms at 12 MIPS

        org 0
        call    ORIG_FILL, 0
        movlb   0x0D
        movwf   RESULT, BANKED      ; keep the original result

        call    COLORBARS, 0

        movlb   0x0D
        movlw   DELAY
outer:  clrf    CNT_HI, BANKED
middle: clrf    CNT_LO, BANKED
inner:  clrwdt
        decfsz  CNT_LO, F, BANKED
        bra     inner
        decfsz  CNT_HI, F, BANKED
        bra     middle
        decfsz  WREG, F, ACCESS
        bra     outer

        movlb   0x0D
        movf    RESULT, W, BANKED   ; return the original result
        return  0

        end
