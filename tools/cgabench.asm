; CGABENCH.COM: CGA pixel speed, without a language runtime in the way.
;
; Mode 4 (320x200, 4 colours), every pixel set one at a time from left to right and top to bottom,
; as a PSET does it: the byte address from x and y, a read-modify-write of the two bits. One round
; fills the screen four times, in colours 1, 2, 3 and 0 (256,000 pixels); the count of rounds is
; the command line argument (default 1). The time comes from the BIOS clock (18.2 ticks a second),
; printed in text mode with the pixels per second.
;
;   nasm -f bin -o cgabench.com cgabench.asm
;   cgabench [rounds]

  cpu   8086
  org   0x100

start:
  ; rounds from the command line
  mov   si, 0x81
  xor   bx, bx
.skip:
  lodsb
  cmp   al, ' '
  je    .skip
.digit:
  sub   al, '0'
  cmp   al, 9
  ja    .parsed
  cbw
  xchg  ax, bx
  mov   cx, 10
  mul   cx
  add   bx, ax
  lodsb
  jmp   .digit
.parsed:
  test  bx, bx
  jnz   .have
  inc   bx
.have:
  mov   [rounds], bx

  mov   ax, 0x0004              ; 320x200, 4 colours
  int   0x10
  mov   ax, 0xb800
  mov   es, ax

  call  ticks
  mov   [t0], ax

  mov   bp, [rounds]
round:
  mov   byte [colour], 1
fill:
  xor   si, si                  ; y
row:
  mov   ax, si                  ; row start: (y / 2) * 80, plus 2000h on odd lines
  shr   ax, 1
  mov   bl, 80
  mul   bl
  test  si, 1
  jz    .even
  add   ax, 0x2000
.even:
  mov   di, ax
  xor   cx, cx                  ; x
pixel:
  mov   bx, cx                  ; byte: x / 4
  shr   bx, 1
  shr   bx, 1
  push  cx
  and   cl, 3                   ; bits: (3 - x % 4) * 2 from the right
  xor   cl, 3
  shl   cl, 1
  mov   al, 3
  shl   al, cl
  not   al
  mov   ah, [colour]
  shl   ah, cl
  and   al, [es:di + bx]
  or    al, ah
  mov   [es:di + bx], al
  pop   cx
  inc   cx
  cmp   cx, 320
  jb    pixel
  inc   si
  cmp   si, 200
  jb    row
  inc   byte [colour]
  and   byte [colour], 3
  jnz   fill                    ; 1, 2, 3, then 0 ends the round
  dec   bp
  jnz   round

  call  ticks
  sub   ax, [t0]
  mov   [elapsed], ax

  mov   ax, 0x0003
  int   0x10
  push  cs
  pop   es

  mov   dx, msg_rounds
  call  print
  mov   ax, [rounds]
  call  print_dec
  mov   dx, msg_ticks
  call  print
  mov   ax, [elapsed]
  call  print_dec

  ; milliseconds: ticks * 54.925 = ticks * 2197 / 40
  mov   dx, msg_time
  call  print
  mov   ax, [elapsed]
  mov   cx, 2197
  mul   cx
  mov   cx, 40
  div   cx
  mov   [ms], ax
  xor   dx, dx
  mov   cx, 1000
  div   cx                      ; AX seconds, DX milliseconds
  call  print_dec
  mov   al, '.'
  call  putc
  mov   ax, dx
  mov   cl, 100
  div   cl                      ; three digits, with the zeros
  add   al, '0'
  call  putc
  mov   al, ah
  xor   ah, ah
  mov   cl, 10
  div   cl
  add   ax, '00'
  push  ax
  call  putc
  pop   ax
  mov   al, ah
  call  putc

  ; thousand pixels a second, one decimal: rounds * 2560000 / ms
  mov   dx, msg_rate
  call  print
  mov   cx, [ms]
  jcxz  .end
  mov   ax, [rounds]
  mov   bx, 40000
  mul   bx                      ; DX:AX = rounds * 40000
  mov   bx, 6
.times64:
  shl   ax, 1
  rcl   dx, 1
  dec   bx
  jnz   .times64                ; DX:AX = rounds * 2560000
  call  div32                   ; DX:AX / CX
  mov   cx, 10
  call  div32                   ; and its tenths in BX
  push  bx
  call  print_dec32
  mov   al, '.'
  call  putc
  pop   ax
  add   al, '0'
  call  putc
.end:
  mov   dx, msg_end
  call  print
  mov   ax, 0x4c00
  int   0x21

; the BIOS clock's low word
ticks:
  push  ds
  xor   ax, ax
  mov   ds, ax
  mov   ax, [0x46c]
  pop   ds
  ret

; the $ terminated string at DS:DX
print:
  push  ax
  mov   ah, 0x09
  int   0x21
  pop   ax
  ret

putc:
  push  ax
  push  dx
  mov   dl, al
  mov   ah, 0x02
  int   0x21
  pop   dx
  pop   ax
  ret

; DX:AX = DX:AX / CX, remainder in BX
div32:
  mov   bx, ax
  mov   ax, dx
  xor   dx, dx
  div   cx
  xchg  ax, bx
  div   cx
  xchg  dx, bx
  ret

; AX unsigned, in decimal
print_dec:
  push  dx
  xor   dx, dx
  call  print_dec32
  pop   dx
  ret

; DX:AX unsigned, in decimal
print_dec32:
  push  ax
  push  bx
  push  cx
  push  dx
  push  si
  mov   si, 10
  xor   cx, cx
.divide:
  mov   bx, ax                  ; DX:AX / 10, in two steps
  mov   ax, dx
  xor   dx, dx
  div   si
  xchg  ax, bx
  div   si
  push  dx
  inc   cx
  mov   dx, bx
  or    bx, ax
  jnz   .divide
.emit:
  pop   ax
  add   al, '0'
  call  putc
  loop  .emit
  pop   si
  pop   dx
  pop   cx
  pop   bx
  pop   ax
  ret

msg_rounds db 'CGABENCH: $'
msg_ticks  db ' x 256000 pixels in $'
msg_time   db ' ticks, $'
msg_rate   db ' s, k pixels/s: $'
msg_end    db 13, 10, '$'

rounds     dw 1
t0         dw 0
elapsed    dw 0
ms         dw 0
colour     db 1
