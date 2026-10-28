; Display page 1 in 80x25: shown, written to, and with its cursor.
;
; The BIOS gave the CRTC the page's start in bytes rather than words, so page 1 showed page 2; the
; cursor address lacked the page's start; and only the first page was cleared, so text written to
; another one came out black on black (75dcd41). Here page 1 is made active, a line goes to it
; with the teletype, and the CRTC's cursor registers are read back: the page's start (800h words)
; plus the cell. The frame checksum checks that the page is the one drawn, text and cursor.
;
; pctool: expect=2:PASS: pages cursor
; pctool: framesum=2:6d7c29625f526c55
; pctool: time=2

%include "lib.inc"

test_main:
  mov   ax, 0x0501              ; page 1 active
  int   0x10
  mov   ah, 0x02                ; its cursor to row 2, column 0
  mov   bh, 1
  mov   dx, 0x0200
  int   0x10
  mov   si, msg_page
  call  print1
  mov   ah, 0x02                ; and to row 4 for the result
  mov   bh, 1
  mov   dx, 0x0400
  int   0x10
  mov   si, msg_result
  call  print1

  mov   dx, 0x3d4               ; the CRTC's cursor address
  mov   al, 0x0e
  out   dx, al
  inc   dx
  in    al, dx
  mov   ah, al
  dec   dx
  mov   al, 0x0f
  out   dx, al
  inc   dx
  in    al, dx
  mov   bx, ax

  mov   ax, bx
  call  print_hex1
  cmp   bx, 0x0800 + 4 * 80 + RESULT_LEN
  jne   .fail
  mov   si, msg_pass
  jmp   .out
.fail:
  mov   si, msg_fail
.out:
  call  print1
  jmp   done

; as print and print_hex, but on page 1
print1:
  push  ax
  push  bx
  push  si
.next:
  lodsb
  test  al, al
  jz    .done
  mov   ah, 0x0e
  mov   bh, 1
  int   0x10
  jmp   .next
.done:
  pop   si
  pop   bx
  pop   ax
  ret

print_hex1:
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
  mov   bh, 1
  int   0x10
  pop   ax
  loop  .digit
  pop   cx
  pop   bx
  pop   ax
  ret

msg_page   db 'pages: this line is on display page 1', 0
msg_result db 'cursor at ', 0
RESULT_LEN equ $ - msg_result - 1
msg_pass   db ' PASS: pages cursor', 0
msg_fail   db ' FAIL: pages cursor (expected 094Ah)', 0

TEST_END
