; The EGA has 256 KB: 64 KB per plane, and the BIOS says so.
;
; It had 128 KB, 32 KB per plane, so offsets from 8000h wrapped onto the screen at 0000h. Software
; that assumes a 256 KB card without asking -- Monkey Island's EGA version keeps its cursor buffer
; there, Commander Keen its tiles -- drew into the top of the picture. The upper half is in PSRAM
; now (see src/ega.h).
;
; In mode 0Dh, plane 0 gets 11h at A000:0000 and 22h at A000:8000; both must read back as written.
; INT 10h AH=12h BL=10h must give BL=3, 256 KB.
;
; pctool: video=ega
; pctool: egarom=../data/fs/ega.bin
; pctool: expect=8:PASS: egamem
; pctool: time=8

%include "lib.inc"

test_main:
  mov   si, msg_start
  call  print

  mov   ah, 0x12                ; INT 10h AH=12h BL=10h: EGA information, BL = memory
  mov   bl, 0x10
  int   0x10
  mov   [memsize], bl

  mov   ax, 0x000d              ; 320x200 in 16 colours
  int   0x10

  mov   dx, 0x3c4               ; Sequencer map mask: plane 0 only
  mov   ax, 0x0102
  out   dx, ax
  mov   dx, 0x3ce               ; Graphics Controller read map select: plane 0
  mov   ax, 0x0004
  out   dx, ax
  mov   ax, 0xa000
  mov   es, ax
  mov   byte [es:0x0000], 0x11
  mov   byte [es:0x8000], 0x22
  mov   al, [es:0x0000]
  mov   ah, [es:0x8000]
  mov   [planes], ax

  mov   ax, 0x0003              ; back to text for the result
  int   0x10
  mov   si, msg_start
  call  print
  call  newline

  cmp   byte [memsize], 3
  jne   .fail
  cmp   word [planes], 0x2211
  jne   .fail
  mov   si, msg_pass
  call  print
  jmp   done

.fail:
  mov   si, msg_fail
  call  print
  mov   al, [memsize]
  xor   ah, ah
  call  print_hex
  mov   al, ' '
  mov   ah, 0x0e
  xor   bx, bx
  int   0x10
  mov   ax, [planes]
  call  print_hex
  jmp   done

msg_start db 'egamem: 256 KB of EGA memory ', 0
msg_pass  db 'PASS: egamem', 13, 10, 0
msg_fail  db 'FAIL: egamem, BL and the planes at 8000h/0000h: ', 0

memsize   db 0
planes    dw 0

TEST_END
