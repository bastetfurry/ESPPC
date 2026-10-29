; INT 13h AH=00h resets the floppy controller even for a drive without a disk image.
;
; Reset and status are answered by the controller, which a PC has without drives. The BIOS used to
; time out (AH=80h) on a reset of a floppy drive with no image: CheckIt 3.0's hard disk test resets
; drive 0 first and, on a machine booting from its hard disk without floppies, reported "Your
; controller card has a problem. Error: 80h". The test boots from A:, so drive B: is the one without.
;
; pctool: expect=5:PASS: fdreset
; pctool: time=5

%include "lib.inc"

test_main:
  mov   si, msg_start
  call  print

  mov   ah, 0x00                ; reset, drive B:
  mov   dl, 0x01
  int   0x13
  jc    .fail
  test  ah, ah
  jnz   .fail

  mov   ah, 0x02                ; but reading from it still fails
  mov   al, 1
  mov   cx, 0x0001
  xor   dh, dh
  mov   dl, 0x01
  mov   bx, buffer
  int   0x13
  jnc   .fail_read

  call  newline
  mov   si, msg_pass
  call  print
  jmp   done

.fail_read:
  mov   si, msg_read
  jmp   .show
.fail:
  mov   si, msg_fail
.show:
  call  newline
  call  print
  mov   al, ah
  xor   ah, ah
  call  print_hex
  jmp   done

msg_start db 'fdreset: reset of a floppy drive without an image ', 0
msg_pass  db 'PASS: fdreset', 13, 10, 0
msg_fail  db 'FAIL: fdreset, reset returned AH=', 0
msg_read  db 'FAIL: fdreset, read from B: worked, AH=', 0

buffer    times 512 db 0

TEST_END
