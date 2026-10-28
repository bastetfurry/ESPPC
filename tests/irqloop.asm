; Timer interrupts reach a loop that spends most of its time in a BIOS call with interrupts off.
;
; MS-DOS 5 waits for F5/F8 at "Starting MS-DOS..." by calling INT 16h in a loop until the BIOS
; clock has moved on. Interrupts used to be taken only at the end of a 16 instruction batch, and
; only with IF set there, so when the loop was a multiple of 16 instructions long and the batch
; boundaries fell inside the handler, the timer tick never came (fixed in 05947ad).
;
; That needs a loop of the right length and the right alignment, so here are all of them: an inner
; block of 0 to 15 NOPs, with interrupts off so the window where IF is set stays a few instructions
; wide, changes the loop's length; an outer block of 0 to 15 NOPs before it changes where the
; batches meet it. Each of the 256 variants waits for one tick of the BIOS clock.
;
; pctool: expect=20:PASS: irqloop
; pctool: time=20

%include "lib.inc"

test_main:
  mov   si, msg_start
  call  print
  xor   di, di                  ; inner NOPs skipped, 0..15
next_inner:
  xor   cx, cx                  ; outer NOPs skipped, 0..15
next_outer:
  call  ticks
  inc   ax
  mov   bp, ax                  ; wait until the clock reaches this
  lea   bx, [inner_sled + di]
  lea   dx, [outer_sled]
  add   dx, cx
  jmp   dx

outer_sled:
  times 15 nop
wait_loop:
  mov   ah, 0x02                ; INT 16h AH=02h, shift flags: the BIOS call DOS makes
  int   0x16
  cli
  jmp   bx
inner_sled:
  times 15 nop
  sti
  cmp   [0x46c], bp
  jb    wait_loop

  inc   cx
  cmp   cx, 16
  jb    next_outer
  mov   al, '.'
  mov   ah, 0x0e
  xor   bx, bx
  int   0x10
  inc   di
  cmp   di, 16
  jb    next_inner

  call  newline
  mov   si, msg_pass
  call  print
  jmp   done

msg_start db 'irqloop: timer ticks in an INT 16h loop, 256 lengths and alignments ', 0
msg_pass  db 'PASS: irqloop', 13, 10, 0

TEST_END
