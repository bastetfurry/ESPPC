; The frame of a test: a boot floppy of its own, so the tests need no DOS.
;
; The BIOS loads the boot sector to 0000:7C00. It loads the rest of the test (track 0, sectors 2
; to 18, 8.5 KB) through INT 13h, sets the BIOS clock to 0 so timings do not depend on when the
; test runs, and jumps to test_main, which the test defines after including this file. A test
; reports with PASS/FAIL lines on the screen, which tests/run.sh checks through pctool:
; "; pctool:" lines in the test give its pctool arguments (expect= and framesum= checks).

  cpu   8086
  org   0x7c00

boot:
  cli
  xor   ax, ax
  mov   ds, ax
  mov   es, ax
  mov   ss, ax
  mov   sp, 0x7c00
  sti
  cld
  mov   [boot_drive], dl

  mov   ax, 0x0211              ; read 17 sectors
  mov   bx, 0x7e00
  mov   cx, 0x0002              ; track 0, sector 2
  xor   dh, dh                  ; head 0, DL the boot drive
  int   0x13
  jc    .load_failed

  mov   ah, 0x01                ; INT 1Ah AH=01h: the BIOS clock to 0
  xor   cx, cx
  xor   dx, dx
  int   0x1a

  jmp   test_main

.load_failed:
  mov   si, msg_load_failed
  call  print
.hang:
  hlt
  jmp   .hang

msg_load_failed db 'FAIL: loading the test through INT 13h', 13, 10, 0
boot_drive      db 0

; print the zero terminated string at DS:SI with the BIOS teletype
print:
  push  ax
  push  bx
  push  si
.next:
  lodsb
  test  al, al
  jz    .done
  mov   ah, 0x0e
  xor   bx, bx
  int   0x10
  jmp   .next
.done:
  pop   si
  pop   bx
  pop   ax
  ret

; AX as four hex digits
print_hex:
  push  ax
  push  bx
  push  cx
  mov   cx, 4
.digit:
  rol   ax, 1
  rol   ax, 1
  rol   ax, 1
  rol   ax, 1
  push  ax
  and   al, 0x0f
  add   al, '0'
  cmp   al, '9'
  jbe   .out
  add   al, 'A' - '9' - 1
.out:
  mov   ah, 0x0e
  xor   bx, bx
  int   0x10
  pop   ax
  loop  .digit
  pop   cx
  pop   bx
  pop   ax
  ret

; AX as an unsigned decimal number
print_dec:
  push  ax
  push  bx
  push  cx
  push  dx
  mov   bx, 10
  xor   cx, cx
.divide:
  xor   dx, dx
  div   bx
  push  dx
  inc   cx
  test  ax, ax
  jnz   .divide
.emit:
  pop   ax
  add   al, '0'
  mov   ah, 0x0e
  xor   bx, bx
  int   0x10
  loop  .emit
  pop   dx
  pop   cx
  pop   bx
  pop   ax
  ret

newline:
  push  ax
  push  bx
  mov   ax, 0x0e0d
  xor   bx, bx
  int   0x10
  mov   ax, 0x0e0a
  int   0x10
  pop   bx
  pop   ax
  ret

; the BIOS clock's low word, in AX (it runs at 18.2 ticks a second)
ticks:
  push  ds
  xor   ax, ax
  mov   ds, ax
  mov   ax, [0x46c]
  pop   ds
  ret

; stop here for good: the result stays on the screen for pctool to read
done:
  cli
.hang:
  hlt
  jmp   .hang

  times 510 - ($ - $$) db 0
  dw    0xaa55

; the test follows from here (sector 2), up to 17 sectors of it; see TEST_END
%macro TEST_END 0
  %if ($ - $$) > 18 * 512
    %error "the test is longer than the 17 sectors the boot sector loads"
  %endif
%endmacro
