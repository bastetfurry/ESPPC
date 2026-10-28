; A reboot from inside the keyboard interrupt, without an EOI, as SCO Xenix 86 does it after
; "shutdown": the keyboard and the timer must work on the next boot.
;
; The jump to the reset vector does not go through the emulator's reset, so the PIC kept IRQ 1 in
; service through POST, and a scancode left in the 8042 kept its buffer full: no keyboard, and the
; hard disk's IRQ 5 blocked behind it (fixed in 1111e96: ICW1 starts the PIC afresh, POST empties
; the 8042). The first boot leaves a marker at 9000:0000, which POST does not clear, and hooks
; INT 9 to jump to FFFF:0000 on a key press; the second finds the marker and checks that timer
; ticks arrive and a key gets through, each with a time limit.
;
; pctool: key=1:SPACE
; pctool: key=3:k
; pctool: expect=6:PASS: reboot timer
; pctool: expect=6:PASS: reboot keyboard
; pctool: time=6

%include "lib.inc"

MARKER equ 0x7e57

test_main:
  push  ds
  mov   ax, 0x9000
  mov   ds, ax
  cmp   word [0], MARKER
  mov   word [0], 0
  pop   ds
  je    second_boot

  push  ds
  mov   ax, 0x9000
  mov   ds, ax
  mov   word [0], MARKER
  pop   ds
  cli
  mov   word [4 * 9], reboot_int9
  mov   word [4 * 9 + 2], 0
  sti
  mov   si, msg_first
  call  print
.wait:
  jmp   .wait

; the key press: straight to the reset vector, with IRQ 1 in service and no EOI
reboot_int9:
  in    al, 0x60
  test  al, 0x80
  jnz   .release
  jmp   0xffff:0x0000
.release:
  mov   al, 0x20
  out   0x20, al
  iret

second_boot:
  mov   si, msg_second
  call  print

  ; the timer: 9 ticks (half a second), or give up after 65536 rounds
  xor   cx, cx
.timer:
  call  ticks
  cmp   ax, 9
  jae   .timer_ok
  loop  .timer
  mov   si, msg_timer_fail
  jmp   .timer_out
.timer_ok:
  mov   si, msg_timer_pass
.timer_out:
  call  print

  ; the keyboard: a key within 3 seconds (55 ticks) of the clock
.key:
  mov   ah, 0x01                ; INT 16h AH=01h: a key waiting?
  int   0x16
  jnz   .key_ok
  call  ticks
  cmp   ax, 9 + 55
  jb    .key
  mov   si, msg_key_fail
  jmp   .key_out
.key_ok:
  xor   ah, ah
  int   0x16
  mov   si, msg_key_pass
.key_out:
  call  print
  jmp   done

msg_first      db 'reboot: press a key to reboot from inside INT 9', 13, 10, 0
msg_second     db 'reboot: after the reboot', 13, 10, 0
msg_timer_pass db 'PASS: reboot timer', 13, 10, 0
msg_timer_fail db 'FAIL: reboot timer, the BIOS clock does not move', 13, 10, 0
msg_key_pass   db 'PASS: reboot keyboard', 13, 10, 0
msg_key_fail   db 'FAIL: reboot keyboard, no key came through', 13, 10, 0

TEST_END
