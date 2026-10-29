; An EGA mode set leaves the display registers as the BIOS does: start address 0, the offset at
; the mode's width, no pel panning.
;
; Our mode set does not program the CRTC itself, so what the last program left there applied to the
; next one. Commander Keen leaves its scrolling in the start address (3604h), the offset (18h) and
; the panning; Monkey Island, started after it, writes only the start address's high byte and got
; Keen's low byte with it: its logo came out cut in two and shifted sideways.
;
; Here Keen's values go in, the mode is set again, only the high byte of the start address is
; written, and a line is drawn at offset 0. The frame must be the one without Keen's values.
;
; pctool: video=ega
; pctool: egarom=../data/fs/ega.bin
; pctool: framesum=6:927e9dc402e1268d
; pctool: time=6

%include "lib.inc"

test_main:
  mov   ax, 0x000d              ; 320x200 in 16 colours
  int   0x10

%ifndef CLEAN
  mov   dx, 0x3d4               ; what Commander Keen leaves behind
  mov   ax, 0x1813              ; offset 18h: 384 pixels a line
  out   dx, ax
  mov   ax, 0x360c              ; start address 3604h
  out   dx, ax
  mov   ax, 0x040d
  out   dx, ax
  mov   dx, 0x3da               ; pel panning 3
  in    al, dx
  mov   dx, 0x3c0
  mov   al, 0x33
  out   dx, al
  mov   al, 0x03
  out   dx, al

  mov   ax, 0x000d              ; the next program's mode set
  int   0x10
%endif

  mov   dx, 0x3d4               ; it writes only the high byte of its start address
  mov   ax, 0x000c
  out   dx, ax

  mov   dx, 0x3c4               ; a line in plane 0 at offset 0, and a mark on line 100
  mov   ax, 0x0102
  out   dx, ax
  mov   ax, 0xa000
  mov   es, ax
  xor   di, di
  mov   cx, 40
  mov   al, 0xff
  rep   stosb
  mov   byte [es:100 * 40 + 5], 0xf0
  jmp   done

TEST_END
