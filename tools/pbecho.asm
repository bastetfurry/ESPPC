; Postbox echo stub -- a test for the INT 0xFF channel, not a program to keep.
;
; Deliberately touches no memory and uses no stack: the emulator consumes INT 0xFF without
; pushing a frame, so this can be dropped anywhere with the debugger's "l" and started with "j"
; even though it has no PSP and inherits whatever segments the interrupted program had.
;
; Reads bytes from the host and sends each one straight back. ESC (0x1b) parks it.

        bits 16
        org 0

poll:
        mov  ax, 0x55ee
        mov  bx, 2              ; ReceiveByte
        int  0xff
        jc   poll               ; carry = nothing waiting

        mov  cl, al             ; AL holds the byte; keep it, AX is about to be reloaded
        cmp  cl, 0x1b
        je   parked

send:
        mov  ax, 0x55ee
        mov  bx, 1              ; SendByte, CL = the byte
        int  0xff
        jc   send               ; carry = outbound buffer full, try again
        jmp  poll

parked:
        jmp  parked
