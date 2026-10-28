; The CGA's vertical retrace, measured with the PIT: 262 lines of 76 PIT ticks, 19912 ticks a frame
; (59.92 Hz), as on the card.
;
; The status register 3DAh was a counter (every eighth read "drawing", the rest "retrace") and the
; CGA Compatibility Tester measured about 32000 Hz; it follows a raster model now (1eb0636), and
; the PIT, which counted 0.15% slow, is exact to about 2 ppm (ef3bfa6). This measures as the
; tester does: PIT channel 0 in mode 2 (so it counts down by one), latched at the start of
; vertical retrace, frame by frame, averaged over eight. Then the same in 320x200 (mode 4), whose
; CRTC values the BIOS mode set loads.
;
; pctool: expect=3:PASS: retrace 80x25
; pctool: expect=3:PASS: retrace 320x200
; pctool: time=3

%include "lib.inc"

FRAMES equ 8

test_main:
  call  measure                 ; 80x25, as booted
  push  ax
  mov   ax, 0x0004              ; 320x200
  int   0x10
  call  measure
  push  ax
  mov   ax, 0x0003              ; back to text for the report, which clears the screen
  int   0x10
  mov   si, msg_title
  call  print
  pop   bx
  pop   ax
  mov   si, msg_text
  call  report
  mov   ax, bx
  mov   si, msg_graphics
  call  report
  jmp   done

; AX: the ticks of a frame, SI: the mode's name. Prints them, and PASS or FAIL for 19912 +- 25.
report:
  call  print_dec
  cmp   ax, 19912 - 25
  jb    .fail
  cmp   ax, 19912 + 25
  ja    .fail
  push  si
  mov   si, msg_pass
  jmp   .out
.fail:
  push  si
  mov   si, msg_fail
.out:
  call  print
  pop   si
  call  print
  call  newline
  ret

; AX: the PIT ticks of one frame, averaged over FRAMES
measure:
  cli
  mov   al, 0x34                ; channel 0, low then high byte, mode 2
  out   0x43, al
  xor   al, al
  out   0x40, al
  out   0x40, al
  sti
  xor   bx, bx                  ; DX:BX, the sum
  xor   dx, dx
  call  retrace_start
  call  latch
  mov   di, ax
  mov   cx, FRAMES
.frame:
  call  retrace_start
  call  latch
  sub   di, ax                  ; counted down: the previous count minus this one
  add   bx, di
  adc   dx, 0
  mov   di, ax
  loop  .frame
  mov   ax, bx
  mov   cx, FRAMES
  div   cx
  ret

; waits for the start of vertical retrace (status bit 3 rising)
retrace_start:
  push  ax
  push  dx
  mov   dx, 0x3da
.in_retrace:
  in    al, dx
  test  al, 8
  jnz   .in_retrace
.drawing:
  in    al, dx
  test  al, 8
  jz    .drawing
  pop   dx
  pop   ax
  ret

; AX: PIT channel 0's count, latched
latch:
  cli
  xor   al, al                  ; latch channel 0
  out   0x43, al
  in    al, 0x40
  mov   ah, al
  in    al, 0x40
  xchg  al, ah
  sti
  ret

msg_title    db 'retrace: PIT ticks a frame, 19912 on the card', 13, 10, 0
msg_text     db '80x25', 0
msg_graphics db '320x200', 0
msg_pass     db ' PASS: retrace ', 0
msg_fail     db ' FAIL: retrace ', 0

TEST_END
