; Single stepping: INT 1 comes after an instruction that began with TF set, not before it.
;
; The core used to take INT 1 at the start of an instruction whenever TF was set. A handler that
; IRETs back with TF set got the trap again at once, before the instruction it returned to, and
; never left it: CheckIt 3.0's "CPU Interrupt Bug" test (System Board) hung there.
;
; Here IRET sets TF and the handler logs the address of each trap. As on the 8086, the instruction
; after the IRET runs before the first trap, POP SS holds the trap back for one more instruction,
; and the POPF that clears TF still traps after it.
;
; pctool: expect=5:PASS: trap
; pctool: time=5

%include "lib.inc"

test_main:
  mov   si, msg_start
  call  print
  mov   word [4], trap_handler  ; INT 1 (DS is 0, and so is CS)
  mov   [6], cs
  mov   di, log

  pushf                         ; for the POPF that clears TF
  pushf
  pop   ax
  or    ax, 0x100
  push  ax
  push  cs
  mov   ax, t0
  push  ax
  iret                          ; sets TF: no trap after this one
t0:
  nop                           ; trap, at t1
t1:
  push  ss                      ; trap, at t2
t2:
  pop   ss                      ; no trap: it holds interrupts for one instruction
  nop                           ; trap, at t3
t3:
  popf                          ; clears TF, and still traps, at t4
t4:
  nop                           ; no trap

  mov   si, expected
  mov   cx, (log_end - log) / 2
  push  di
  mov   di, log
  repe  cmpsw
  pop   di
  jne   .fail
  cmp   di, log + 8
  jne   .fail
  call  newline
  mov   si, msg_pass
  call  print
  jmp   done

.fail:
  call  newline
  mov   si, msg_fail
  call  print
  mov   si, log
.show:
  cmp   si, di
  jae   .shown
  lodsw
  call  print_hex
  mov   al, ' '
  mov   ah, 0x0e
  xor   bx, bx
  int   0x10
  jmp   .show
.shown:
  jmp   done

; logs the address the trap returns to, up to the end of the log
trap_handler:
  push  bp
  mov   bp, sp
  cmp   di, log_end
  jae   .full
  push  ax
  mov   ax, [bp + 2]
  stosw
  pop   ax
.full:
  pop   bp
  iret

msg_start db 'trap: INT 1 after the instructions run with TF set ', 0
msg_pass  db 'PASS: trap', 13, 10, 0
msg_fail  db 'FAIL: trap, INT 1 at ', 0

expected  dw t1, t2, t3, t4, 0, 0, 0, 0
log       times 8 dw 0
log_end:

TEST_END
