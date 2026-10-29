; DMA channel 0 counts the DRAM refresh, and INT 14h leaves the 8237 alone.
;
; The BIOS sets up the refresh as IBM's does: PIT timer 1 asks for a cycle of DMA channel 0 every
; 15 us, the channel from address 0 with a count of FFFFh and auto-init. CheckIt 3.0's System Board
; test failed "DMA Controller(s)" because channel 0 stood still (and, on machines without the Tandy
; DAC or hwdisk, because nothing answered at ports 0-0Fh at all).
;
; Channel 0's address goes up and its count down by one per cycle, so they always add up to FFFFh.
; INT 14h used to initialise a COM port that is not there (base address 0) at ports 0-7: COM3 here,
; and the sum after it is checked too.
;
; pctool: expect=5:PASS: dma
; pctool: time=5

%include "lib.inc"

test_main:
  mov   si, msg_start
  call  print

  call  read_ch0
  mov   bp, ax                  ; the address, to see it move
  call  check_sum
  jne   .fail_sum

  mov   cx, 1000
.wait:
  loop  .wait
  call  read_ch0
  cmp   ax, bp
  je    .fail_still
  call  check_sum
  jne   .fail_sum

  mov   ah, 0x00                ; INT 14h AH=00h, initialise COM3, which has no port
  mov   al, 0xe3
  mov   dx, 2
  int   0x14
  test  ah, 0x80
  jz    .fail_int14
  call  read_ch0
  call  check_sum
  jne   .fail_sum

  call  newline
  mov   si, msg_pass
  call  print
  jmp   done

.fail_still:
  mov   si, msg_still
  jmp   .fail
.fail_int14:
  mov   si, msg_int14
  jmp   .fail
.fail_sum:
  mov   si, msg_sum
.fail:
  call  newline
  call  print
  mov   ax, [addr]
  call  print_hex
  mov   al, ' '
  mov   ah, 0x0e
  xor   bx, bx
  int   0x10
  mov   ax, [count]
  call  print_hex
  jmp   done

; channel 0's current address in AX and [addr], its count in [count]. The address is read first, so
; the count may have moved on by a cycle or two since: check_sum allows for that.
read_ch0:
  cli
  out   0x0c, al                ; clear the flip-flop
  in    al, 0x00
  mov   ah, al
  in    al, 0x00
  xchg  al, ah
  mov   [addr], ax
  in    al, 0x01
  mov   ah, al
  in    al, 0x01
  xchg  al, ah
  mov   [count], ax
  sti
  mov   ax, [addr]
  ret

; ZF set when [addr] + [count] is FFFFh, or a few less
check_sum:
  push  ax
  mov   ax, [addr]
  add   ax, [count]
  not   ax                      ; 0 when the sum is FFFFh
  and   ax, 0xfffc              ; and 0 for up to 3 less
  pop   ax
  ret

msg_start db 'dma: channel 0 refresh, INT 14h without a port ', 0
msg_pass  db 'PASS: dma', 13, 10, 0
msg_still db 'FAIL: dma, channel 0 does not move: ', 0
msg_int14 db 'FAIL: dma, INT 14h found a COM3: ', 0
msg_sum   db 'FAIL: dma, channel 0 address and count do not add up: ', 0

addr      dw 0
count     dw 0

TEST_END
