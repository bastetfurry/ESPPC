; The keyboard, both ways software reads it:
;
;   1. through the BIOS: a line typed on the keyboard, read with INT 16h and compared
;   2. from the 8042 directly, as games do: an INT 9 handler of the test's own reads port 60h, and
;      pressing "a" must arrive as scancode 1Eh and its release as 9Eh, set 2 from the keyboard
;      translated to set 1 by the 8042
;
; pctool: type=0.5:hello world\n
; pctool: key=2:a
; pctool: expect=3:PASS: keyboard bios
; pctool: expect=3:PASS: keyboard port 60h
; pctool: time=3

%include "lib.inc"

test_main:
  mov   si, msg_prompt
  call  print
  mov   di, line
.key:
  xor   ah, ah                  ; INT 16h AH=00h: wait for a key
  int   0x16
  cmp   al, 13
  je    .line_done
  mov   [di], al
  inc   di
  mov   ah, 0x0e
  xor   bx, bx
  int   0x10
  jmp   .key
.line_done:
  mov   byte [di], 0
  call  newline
  mov   si, line
  mov   di, expected
.compare:
  mov   al, [si]
  cmp   al, [di]
  jne   .bios_fail
  inc   si
  inc   di
  test  al, al
  jnz   .compare
  mov   si, msg_bios_pass
  jmp   .bios_out
.bios_fail:
  mov   si, msg_bios_fail
.bios_out:
  call  print

  ; 2. our own INT 9: every byte from port 60h into the buffer
  cli
  mov   word [4 * 9], int9
  mov   word [4 * 9 + 2], 0
  sti
  call  ticks                   ; a quarter second for the Enter key's release, which the BIOS
  add   ax, 5                   ; did not take, then forget it
  mov   bx, ax
.settle:
  call  ticks
  cmp   ax, bx
  jb    .settle
  mov   byte [count], 0
  mov   si, msg_wait
  call  print
.wait:
  cmp   byte [count], 2
  jb    .wait
  mov   al, [codes]
  xor   ah, ah
  call  print_hex
  mov   al, ' '
  mov   ah, 0x0e
  xor   bx, bx
  int   0x10
  mov   al, [codes + 1]
  xor   ah, ah
  call  print_hex
  cmp   word [codes], 0x9e1e    ; 1Eh, then 9Eh
  jne   .port_fail
  mov   si, msg_port_pass
  jmp   .port_out
.port_fail:
  mov   si, msg_port_fail
.port_out:
  call  print
  jmp   done

int9:
  push  ax
  push  bx
  in    al, 0x60
  xor   bh, bh
  mov   bl, [cs:count]
  cmp   bl, 8
  jae   .full
  mov   [cs:codes + bx], al
  inc   byte [cs:count]
.full:
  mov   al, 0x20                ; EOI
  out   0x20, al
  pop   bx
  pop   ax
  iret

count         db 0
codes         times 8 db 0
line          times 64 db 0
expected      db 'hello world', 0
msg_prompt    db 'keyboard: type a line: ', 0
msg_bios_pass db 'PASS: keyboard bios', 13, 10, 0
msg_bios_fail db 'FAIL: keyboard bios, expected "hello world"', 13, 10, 0
msg_wait      db 'press a: ', 0
msg_port_pass db ' PASS: keyboard port 60h', 13, 10, 0
msg_port_fail db ' FAIL: keyboard port 60h, expected 001E 009E', 13, 10, 0

TEST_END
