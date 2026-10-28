; The CGA's picture, checked by frame checksums in four phases a second apart:
;
;   1. 320x200 with the start address near the end of the 8 KB banks: the picture wraps round
;      within each bank, with the bank's 192 unused bytes in between (ec6453a)
;   2. 80x25 text with the start address near the end of the 16 KB, and a red border (9f2e8a5)
;   3. 80x100 text: the 6845 reprogrammed as the tweaked games do, a grid of the 16 x 16 colour
;      pairs in the half block character (b74178d)
;   4. 40x25 text with a green border
;
; The checksums were taken from frames that were looked at; a change in how any of these is drawn
; changes them.
;
; pctool: framesum=0.9:139bab5f57b35b9d
; pctool: framesum=1.9:25682408dd46a64d
; pctool: framesum=2.9:d639949f6964528d
; pctool: framesum=3.9:1e0bedae3b56f37f
; pctool: time=4

%include "lib.inc"

test_main:
  ; 1. 320x200, the start address 3960 words (7920 bytes) into each bank
  mov   ax, 0x0004
  int   0x10
  mov   ax, 0xb800
  mov   es, ax
  xor   di, di
.bank_bytes:                    ; rows of 80 bytes in four colours, the unused bytes striped
  mov   ax, di
  and   ax, 0x1fff
  cmp   ax, 8000
  jae   .gap
  mov   bl, 80
  div   bl                      ; AL: the row
  and   al, 3
  mov   bl, 0x55
  mul   bl
  test  di, 0x2000
  jz    .store
  not   al                      ; the odd lines' bank in the other colours
  jmp   .store
.gap:
  mov   al, 0xe4
.store:
  stosb
  cmp   di, 0x4000
  jb    .bank_bytes
  mov   ax, 3960
  call  set_start
  mov   ax, 18
  call  wait_until

  ; 2. 80x25 text, the start address 8192 - 3 * 80 words, a red border
  mov   ax, 0x0003
  int   0x10
  xor   di, di
  xor   cx, cx                  ; the cell
.cells:
  mov   ax, cx
  xor   dx, dx
  mov   bx, 80
  div   bx                      ; AX: the row in the 16 KB
  xor   dx, dx
  mov   bx, 26
  div   bx
  mov   al, dl
  add   al, 'A'
  mov   ah, dl
  and   ah, 0x07
  or    ah, 0x08                ; bright colours 8-15
  stosw
  inc   cx
  cmp   cx, 8192
  jb    .cells
  mov   ax, 8192 - 3 * 80
  call  set_start
  mov   dx, 0x3d9
  mov   al, 0x04
  out   dx, al
  mov   ax, 36
  call  wait_until

  ; 3. 80x100: the CRTC as the tweaked mode programs it
  mov   ax, 0x0003
  int   0x10
  mov   dx, 0x3d8
  mov   al, 0x09                ; 80 columns, video on, no blink: 16 background colours
  out   dx, al
  mov   si, crtc_80x100
.crtc:
  lodsw
  cmp   al, 0xff
  je    .crtc_done
  mov   dx, 0x3d4
  out   dx, al
  inc   dx
  mov   al, ah
  out   dx, al
  jmp   .crtc
.crtc_done:
  xor   di, di
  xor   cx, cx                  ; the cell, 80 x 100
.half_blocks:
  mov   ax, cx
  xor   dx, dx
  mov   bx, 80
  div   bx                      ; AX: the row, DX: the column
  and   al, 0x0f
  shl   al, 1
  shl   al, 1
  shl   al, 1
  shl   al, 1                   ; background from the row
  and   dl, 0x0f
  or    al, dl                  ; foreground from the column
  mov   ah, al
  mov   al, 0xde                ; the right half block
  stosw
  inc   cx
  cmp   cx, 8000
  jb    .half_blocks
  mov   ax, 54
  call  wait_until

  ; 4. 40x25 text, a green border
  mov   ax, 0x0001
  int   0x10
  mov   dx, 0x3d9
  mov   al, 0x02
  out   dx, al
  mov   si, msg_40
  call  print
  jmp   done

; AX: the CRTC start address
set_start:
  push  ax
  push  dx
  mov   dx, 0x3d4
  push  ax
  mov   al, 0x0c
  out   dx, al
  inc   dx
  pop   ax
  push  ax
  mov   al, ah
  out   dx, al
  dec   dx
  mov   al, 0x0d
  out   dx, al
  inc   dx
  pop   ax
  out   dx, al
  pop   dx
  pop   ax
  ret

; waits until the BIOS clock (0 at the start) reaches AX
wait_until:
  push  bx
  mov   bx, ax
.wait:
  call  ticks
  cmp   ax, bx
  jb    .wait
  pop   bx
  ret

crtc_80x100:
  db    0x04, 0x7f              ; vertical total: 128 rows
  db    0x06, 0x64              ; 100 rows shown
  db    0x07, 0x70              ; vertical sync at row 112
  db    0x09, 0x01              ; 2 lines a row
  db    0xff, 0xff

msg_40 db 'cga: 40x25 text, green border', 0

TEST_END
