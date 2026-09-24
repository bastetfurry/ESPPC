' ==============================================================
'  EGATEST.BAS
'
'  Video adapter test for the ESP32 PC/XT emulator (PCEmulator).
'  Runs automatically from AUTOEXEC.BAT on this disk.
'
'  Walks every CGA and EGA screen mode QBasic can reach. For each
'  one it reports whether the mode came up at all, times a run of
'  full screen repaints, and draws a pattern so the picture itself
'  can be eyeballed for geometry, colour and line doubling.
'
'  The numbers also go to A:\RESULTS.TXT so they can be read back
'  on the host instead of copied off the screen by hand.
'
'  SCREEN 1 and 2 are the CGA modes the emulator supports today,
'  so they are the baseline. SCREEN 7, 8, 9 and 10 are the EGA
'  modes being added; before EGA exists they should simply come
'  back "not available" rather than hang or corrupt anything.
' ==============================================================

ON ERROR GOTO ErrTrap

DIM nm$(12), st$(12), fl$(12)
nCount = 0

SCREEN 0
WIDTH 80
CLS

PRINT "=================================================="
PRINT "  ESP32 PCEmulator  --  video adapter test"
PRINT "=================================================="
PRINT

' ---- what the BIOS says is installed -------------------------

DEF SEG = &H40
vmode = PEEK(&H49)
vcols = PEEK(&H4A)
eqlo = PEEK(&H10)
egain = PEEK(&H87)
DEF SEG

PRINT "BIOS video mode   (40:49) : &H"; HEX$(vmode)
PRINT "Text columns      (40:4A) : "; vcols
PRINT "Equipment byte    (40:10) : &H"; HEX$(eqlo)
PRINT "EGA info byte     (40:87) : &H"; HEX$(egain)
PRINT
IF (egain AND 8) = 0 THEN
  PRINT "40:87 bit 3 clear  -> claims EGA is active,";
ELSE
  PRINT "40:87 bit 3 set    -> claims EGA is not active,";
END IF
PRINT ((egain \ 2) AND 3) * 64 + 64; "KB video memory."
PRINT
PRINT "Do not trust that. This BIOS copies 40:87 from a fixed"
PRINT "table in ROM at POST, so the value is there whether or not"
PRINT "an EGA exists -- see vidmode_opt in tools/bios.asm. The"
PRINT "real detection is INT 10h AH=12h BL=10h, which QBasic 1.1"
PRINT "cannot issue. So the mode probe below is the actual test:"
PRINT "a mode that is really there will draw, one that is not"
PRINT "will come back 'not available'."
PRINT
PRINT "Each mode draws colour bars, a moire grid and a box frame."
PRINT "Check for: correct width, no line doubling artefacts, and"
PRINT "16 distinct colours in the EGA modes."
PRINT
PRINT "Press any key to begin...";
DO: LOOP WHILE INKEY$ = ""
PRINT

' ---- the mode list -------------------------------------------

md = 1: mw = 320: mh = 200: mc = 4: mn$ = "SCREEN 1   CGA 320x200x4": GOSUB TestMode
md = 2: mw = 640: mh = 200: mc = 2: mn$ = "SCREEN 2   CGA 640x200x2": GOSUB TestMode
md = 7: mw = 320: mh = 200: mc = 16: mn$ = "SCREEN 7   EGA 320x200x16": GOSUB TestMode
md = 8: mw = 640: mh = 200: mc = 16: mn$ = "SCREEN 8   EGA 640x200x16": GOSUB TestMode
md = 9: mw = 640: mh = 350: mc = 16: mn$ = "SCREEN 9   EGA 640x350x16": GOSUB TestMode
md = 10: mw = 640: mh = 350: mc = 2: mn$ = "SCREEN 10  EGA 640x350 mono": GOSUB TestMode

' ---- summary --------------------------------------------------

SCREEN 0
WIDTH 80
CLS
PRINT "=================================================="
PRINT "  Results"
PRINT "=================================================="
PRINT
PRINT "mode                        status         ms/fill"
PRINT "--------------------------------------------------"
FOR i = 1 TO nCount
  PRINT LEFT$(nm$(i) + SPACE$(28), 28);
  PRINT LEFT$(st$(i) + SPACE$(15), 15);
  PRINT fl$(i)
NEXT i
PRINT "--------------------------------------------------"
PRINT
PRINT "ms/fill is one full screen LINE ..,BF repaint,"
PRINT "averaged over"; nFills; "of them."
PRINT

' ---- write the numbers out so the host can read them ----------

ErrFlag = 0
OPEN "A:\RESULTS.TXT" FOR OUTPUT AS #1
IF ErrFlag <> 0 THEN
  PRINT "Could not write A:\RESULTS.TXT (disk is read only?)."
ELSE
  PRINT #1, "ESP32 PCEmulator video adapter test"
  PRINT #1, "date "; DATE$; "  time "; TIME$
  PRINT #1, ""
  PRINT #1, "BIOS video mode 40:49 = &H"; HEX$(vmode)
  PRINT #1, "text columns    40:4A = "; vcols
  PRINT #1, "equipment byte  40:10 = &H"; HEX$(eqlo)
  PRINT #1, "EGA info byte   40:87 = &H"; HEX$(egain)
  PRINT #1, ""
  PRINT #1, "mode                        status         ms/fill"
  FOR i = 1 TO nCount
    PRINT #1, LEFT$(nm$(i) + SPACE$(28), 28); LEFT$(st$(i) + SPACE$(15), 15); fl$(i)
  NEXT i
  PRINT #1, ""
  PRINT #1, "fills averaged per mode:"; nFills
  CLOSE #1
  PRINT "Wrote A:\RESULTS.TXT"
END IF

PRINT
PRINT "Press any key to return to DOS...";
DO: LOOP WHILE INKEY$ = ""
SYSTEM

' ==============================================================
'  Test one mode. Expects md, mw, mh, mc, mn$ to be set.
' ==============================================================

TestMode:
  nCount = nCount + 1
  nm$(nCount) = mn$
  fl$(nCount) = "-"

  ' A mode the adapter does not have makes SCREEN raise an error,
  ' which ErrTrap turns into ErrFlag and resumes past.
  ErrFlag = 0
  SCREEN md
  IF ErrFlag <> 0 THEN
    st$(nCount) = "not available"
    SCREEN 0
    WIDTH 80
    PRINT mn$; " -- not available (error"; ErrFlag; ")"
    RETURN
  END IF

  ' --- timed full screen repaints ---
  nFills = 10
  t0! = TIMER
  FOR r = 1 TO nFills
    LINE (0, 0)-(mw - 1, mh - 1), r MOD mc, BF
  NEXT r
  t1! = TIMER
  el! = t1! - t0!
  IF el! < 0 THEN el! = el! + 86400
  fl$(nCount) = STR$(INT(el! * 1000 / nFills))
  st$(nCount) = "ok"

  ' --- something to look at ---
  CLS
  bw = INT(mw / mc)
  FOR i = 0 TO mc - 1
    LINE (i * bw, 0)-((i + 1) * bw - 1, INT(mh / 4)), i, BF
  NEXT i

  ' moire grid: lots of individual short lines, unlike the block fills
  y0 = INT(mh / 4) + 2
  FOR x = 0 TO mw - 1 STEP 8
    LINE (x, y0)-(mw - 1 - x, mh - 1), (INT(x / 8) MOD (mc - 1)) + 1
  NEXT x

  ' frame, to make any geometry or centering error obvious
  LINE (0, 0)-(mw - 1, mh - 1), mc - 1, B

  LOCATE 2, 2
  PRINT mn$;
  LOCATE 3, 2
  PRINT "fill"; fl$(nCount); " ms   any key for next";

  DO: LOOP WHILE INKEY$ = ""

  SCREEN 0
  WIDTH 80
  RETURN

' ==============================================================

ErrTrap:
  ErrFlag = ERR
  RESUME NEXT
