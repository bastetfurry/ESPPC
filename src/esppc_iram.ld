/*
 * ESPPC: the emulator core's IRAM code (sections .iram_emu.*, EMU_IRAM in fabutils.h: the CPU, the
 * video memory callbacks, the device ticks) in IRAM after all other IRAM code, linked in by
 * tools/esppc_link.py.
 *
 * IRAM is made of 32 KB blocks, and two cores fetching code from the same block take turns. The
 * linker put the CPU core in the first block, where the scanline renderers and the VGA interrupt
 * run from on the other core: it lost 12% in EGA text mode (the monitor's 'n gaps', a loop next to
 * step() against one elsewhere). After the rest it is two blocks away.
 */
SECTIONS
{
  .iram0.emu :
  {
    . = ALIGN(4);
    *(.iram_emu .iram_emu.*)
  } > iram0_0_seg
}
INSERT AFTER .iram0.text;
