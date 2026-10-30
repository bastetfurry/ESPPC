/*
 * ESPPC: the CPU core's per instruction code (sections .iram_cpu.*, CPU_IRAM in i8086.cpp) in IRAM
 * after all other IRAM code, linked in by platformio.ini.
 *
 * IRAM is made of 32 KB blocks, and two cores fetching code from the same block take turns. The
 * linker put step() and stepEx() in the first block, where the scanline renderers and the VGA
 * interrupt run from on the other core: the CPU core lost 12% in EGA text mode (the monitor's
 * 'n gaps', a loop next to step() against one elsewhere). After the rest it is two blocks away.
 */
SECTIONS
{
  .iram0.cpu :
  {
    . = ALIGN(4);
    *(.iram_cpu .iram_cpu.*)
  } > iram0_0_seg
}
INSERT AFTER .iram0.text;
