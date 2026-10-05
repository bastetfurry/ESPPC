; POSTBOX.COM -- receives files from the host over the emulator's postbox and writes them to disk.
;
; The postbox is INT 0xFF with AX=0x55EE; see the note above class Machine in src/machine.h.
;
; This same binary is used two ways. Normally it is run from the DOS prompt. For bootstrapping it
; is injected straight into memory with the debugger's "l" and started with "j", and then used to
; write itself to disk -- which is how it gets there in the first place, with no DEBUG.COM and no
; COPY CON encoding needed.
;
; The injected case is why the program sets up its own DS and stack rather than trusting the ones
; DOS would have given a real .COM, and why it never exits by itself unless told to: it is entered
; while COMMAND.COM is parked inside INT 21h AH=0Ah, so there is no sane return path and AH=4Ch
; would terminate COMMAND.COM rather than us. Only DOS calls above function 0Ch are made, which is
; the same rule a TSR follows on the DOS idle path, so the file writes are safe from there.
;
; Host protocol, after the host has opened a session and this end has answered "R":
;   'F' <namelen:1> <name> <length:4 LE> <data>  -> "K", or "E" + DOS error low byte
;   'X'  exit through INT 21h AH=4Ch          (safe only when started from the prompt)
;   'P'  park forever                          (what the bootstrap uses; the host then reboots)
;   'Q'  ping, answered with "Q"

        bits 16
        org  0x100

BUFSIZE equ 512

start:
        mov  ax, cs
        mov  ds, ax
        mov  es, ax
        cli
        mov  ss, ax
        mov  sp, 0xfffe
        sti

        ; Is anyone listening? AX comes back as the signature on every postbox call, which is how
        ; a guest tells a real postbox from an unhandled interrupt.
        mov  ax, 0x55ee
        xor  bx, bx
        int  0xff
        cmp  ax, 0x55ee
        jne  no_postbox

        ; Wait for the host to open its session before saying anything: opening clears both rings,
        ; so a hello sent first would simply be discarded.
wait_host:
        mov  ax, 0x55ee
        xor  bx, bx
        int  0xff
        or   cx, cx
        jz   wait_host

        mov  al, 'R'
        call send

main_loop:
        call recv
        cmp  al, 'F'
        je   do_file
        cmp  al, 'X'
        je   do_exit
        cmp  al, 'P'
        je   do_park
        cmp  al, 'Q'
        jne  main_loop
        mov  al, 'Q'
        call send
        jmp  main_loop

do_park:
        jmp  do_park

do_exit:
        mov  ax, 0x4c00
        int  0x21

; ---------------------------------------------------------------------------------------------
do_file:
        call recv                       ; name length
        mov  cl, al
        xor  ch, ch
        mov  di, fname
        jcxz name_done
name_loop:
        call recv
        stosb
        loop name_loop
name_done:
        mov  byte [di], 0               ; ASCIIZ for INT 21h

        call recv                       ; length, four bytes little endian
        mov  [flen], al
        call recv
        mov  [flen+1], al
        call recv
        mov  [flen+2], al
        call recv
        mov  [flen+3], al

        mov  ah, 0x3c                   ; create, truncating anything already there
        xor  cx, cx
        mov  dx, fname
        int  0x21
        jc   file_error
        mov  [fhandle], ax

copy_loop:
        mov  ax, [flen]
        or   ax, [flen+2]
        jz   copy_done

        mov  di, buffer                 ; fill up to BUFSIZE, or until the file runs out
        xor  si, si
fill_loop:
        cmp  si, BUFSIZE
        jae  fill_done
        mov  ax, [flen]
        or   ax, [flen+2]
        jz   fill_done
        call recv
        stosb
        inc  si
        sub  word [flen], 1
        sbb  word [flen+2], 0
        jmp  fill_loop
fill_done:

        mov  ah, 0x40
        mov  bx, [fhandle]
        mov  cx, si
        mov  dx, buffer
        int  0x21
        jc   close_then_error
        cmp  ax, cx                     ; a short write means the disk is full
        jne  close_then_error
        jmp  copy_loop

copy_done:
        mov  ah, 0x3e
        mov  bx, [fhandle]
        int  0x21
        jc   file_error

        mov  ah, 0x0d                   ; flush DOS's buffers, so a reboot straight after is safe
        int  0x21

        mov  al, 'K'
        call send
        jmp  main_loop

close_then_error:
        push ax
        mov  ah, 0x3e
        mov  bx, [fhandle]
        int  0x21
        pop  ax
file_error:
        mov  cl, al                     ; DOS error code, low byte
        mov  al, 'E'
        call send
        mov  al, cl
        call send
        jmp  main_loop

; ---------------------------------------------------------------------------------------------
; Send AL. Carry back from the postbox means the outbound ring is full, so keep offering it.
send:
        push ax
        push bx
        push cx
        mov  cl, al
send_retry:
        mov  ax, 0x55ee
        mov  bx, 1
        int  0xff
        jc   send_retry
        pop  cx
        pop  bx
        pop  ax
        ret

; Receive into AL, waiting for one to turn up. Carry means nothing is waiting yet.
recv:
        push bx
        push cx
        push dx
recv_retry:
        mov  ax, 0x55ee
        mov  bx, 2
        int  0xff
        jc   recv_retry
        pop  dx
        pop  cx
        pop  bx
        ret

no_postbox:
        mov  ah, 0x09
        mov  dx, msg_nopb
        int  0x21
        mov  ax, 0x4c01
        int  0x21

msg_nopb db 'No postbox: this needs the ESPPC host channel.', 13, 10, '$'

fhandle  dw 0
flen     dd 0
fname    times 80 db 0
buffer   times BUFSIZE db 0
