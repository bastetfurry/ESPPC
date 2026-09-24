/*
  EGA planar memory engine for the PCEmulator machine.

  This is local to this project -- it is not part of FabGL.

  An EGA stores its pixels in four bit planes that all answer to the same CPU address window at
  0xA0000. A byte written there does not land in memory: it passes through the Graphics Controller
  (set/reset, ALU function, rotate, bit mask) and the Sequencer's map mask, combined with four
  latch registers that were loaded by the CPU's previous *read* from the same address.

  The obvious way to model that is four plane arrays and a loop over the planes. That costs four
  byte loads, four byte stores and a branch per plane on every single guest write, and the guest
  writes to video memory constantly.

  So the planes are interleaved instead: one uint32_t per byte-column, plane N living in byte lane
  N. All four latches are a single 32-bit load, and every Graphics Controller operation -- set/reset
  expansion, the ALU, the bit mask, the map mask -- becomes one branchless 32-bit ALU op on all four
  planes at once. The renderer wants the same layout, because a scanline needs all four planes of a
  byte-column together.

  Memory is split into four banks of 32 KB so that it can be allocated on a fragmented heap. The
  largest free internal block measured on the target board is about 110 KB, which is just under the
  112 KB that mode 10h needs as a single allocation. Four 32 KB blocks always fit.

  Addressable offset range is 0..0x7FFF (32 K byte-columns per plane), which covers mode 10h's
  28000. A real EGA has 64 KB per plane; the extra is off-screen memory we cannot afford.
 */


#pragma once


#include <stdint.h>
#include <stddef.h>


// bytes per bank and number of banks -- 4 * 8192 uint32 = 4 * 32 KB = 128 KB total
#define EGA_BANK_ENTRIES  8192
#define EGA_BANK_SHIFT    13
#define EGA_BANK_COUNT    4
#define EGA_OFFSET_MASK   ((EGA_BANK_ENTRIES * EGA_BANK_COUNT) - 1)   // 0x7FFF


class EGA {

public:

  EGA();
  ~EGA();

  // Allocates video memory in internal RAM, returns false if any allocation fails.
  //
  // banks must be 1, 2 or 4 so that the bank index can be masked rather than compared, and is
  // chosen from what the mode actually needs: mode 0Dh addresses 8000 byte-columns, 0Eh 16000,
  // and 0Fh/10h 28000. Allocating the worst case for every mode wasted 64 KB of internal RAM in
  // 640x200, which is the mode a lot of EGA software actually uses.
  bool allocVRAM(int banks);
  void freeVRAM();
  bool hasVRAM()                                 { return m_bank[0] != nullptr; }

  // bank table for the renderer in GraphicsAdapter, with the masks it needs to index it
  uint32_t * const * banks() const               { return m_bank; }
  uint32_t           bankMask() const            { return m_bankMask; }
  uint32_t           offsetMask() const          { return m_offsetMask; }

  void clearVRAM();

  void reset();

  // validation hook: the benchmark compares write8() against write8Ref()
  uint32_t latch() const                         { return m_latch; }
  void     setLatch(uint32_t v)                  { m_latch = v; }

  // --- register file access ---------------------------------------------------------------

  // Sequencer, ports 0x3c4 (index) / 0x3c5 (data)
  void    writeSequencer(uint8_t value);
  uint8_t readSequencer();
  void    selectSequencer(uint8_t index)         { m_seqIndex = index & 0x07; }

  // Graphics Controller, ports 0x3ce (index) / 0x3cf (data)
  void    writeGraphics(uint8_t value);
  uint8_t readGraphics();
  void    selectGraphics(uint8_t index)          { m_gcIndex = index & 0x0f; }

  // register values, for the mode-detection and renderer side
  uint8_t seqReg(int i) const                    { return m_seqReg[i & 0x07]; }
  uint8_t gcReg(int i) const                     { return m_gcReg[i & 0x0f]; }

  // --- planar memory access ---------------------------------------------------------------
  // off is relative to 0xA0000

  inline __attribute__((always_inline)) uint32_t * bankPtr(uint32_t off)
  {
    off &= m_offsetMask;
    return m_bank[(off >> EGA_BANK_SHIFT) & m_bankMask] + (off & (EGA_BANK_ENTRIES - 1));
  }

  inline __attribute__((always_inline)) uint8_t read8(uint32_t off)
  {
    uint32_t v = *bankPtr(off);

    // every CPU read loads the four latches, whatever the read mode
    m_latch = v;

    if (m_readMode == 0)
      return (uint8_t)(v >> m_readMapShift);

    // read mode 1: colour compare. A result bit is set where every plane the Colour Don't Care
    // register cares about matches the corresponding Colour Compare bit.
    uint32_t d = (v ^ m_colorCompare32) & m_colorDontCare32;
    return (uint8_t)~(d | (d >> 8) | (d >> 16) | (d >> 24));
  }

  // The hot path.
  //
  // This is measurably faster than the mask-driven branchless form in ega.cpp (write8Ref): the
  // write mode and ALU function stay the same for long runs, so the branches predict well, while
  // the branchless version has to compute every write mode's source term on every write. Both are
  // kept and the benchmark cross-checks them over random register states.
  inline __attribute__((always_inline)) void write8(uint32_t off, uint8_t value)
  {
    uint32_t * p = bankPtr(off);

    // Fast path for the configuration an ordinary fill leaves behind: write mode 0, no rotate,
    // ALU replace, no set/reset, every bit and every plane enabled. The whole Graphics Controller
    // then reduces to splatting the byte across the four planes, which is what a "rep stosw" into
    // an unmasked EGA screen actually means.
    if (m_simpleWrite) {
      *p = (uint32_t)value * 0x01010101u;
      return;
    }

    uint32_t lat = m_latch;
    uint32_t src;

    switch (m_writeMode) {

      case 0:
      {
        // rotate, then replace the planes that have Enable Set/Reset set with the Set/Reset value
        uint8_t v = m_rotate ? (uint8_t)((value >> m_rotate) | (value << (8 - m_rotate))) : value;
        src = (uint32_t)v * 0x01010101u;
        src = (src & m_srMask) | m_srBlend;
        src = alu(src, lat);
        src = (src & m_bitMask32) | (lat & ~m_bitMask32);
        break;
      }

      case 1:
        // straight latch copy: ALU, rotate and bit mask are all bypassed
        src = lat;
        break;

      case 2:
        // the low nibble of the written byte gives each plane a solid 0x00 or 0xff
        src = s_expand[value & 0x0f];
        src = alu(src, lat);
        src = (src & m_bitMask32) | (lat & ~m_bitMask32);
        break;

      default:
      {
        // write mode 3 is VGA only; an EGA treats it as mode 0 with no set/reset
        uint8_t v = m_rotate ? (uint8_t)((value >> m_rotate) | (value << (8 - m_rotate))) : value;
        src = (uint32_t)v * 0x01010101u;
        src = alu(src, lat);
        src = (src & m_bitMask32) | (lat & ~m_bitMask32);
        break;
      }
    }

    // the Sequencer's map mask decides which planes are actually committed; the planes it blocks
    // keep what is in memory, which is not necessarily what is in the latches
    *p = (src & m_mapMask32) | (*p & m_mapMaskInv);
  }

  // Independent mask-driven implementation. Not on the hot path -- it exists so the benchmark can
  // check write8() against a second implementation written a completely different way.
  void write8Ref(uint32_t off, uint8_t value);

  inline __attribute__((always_inline)) uint16_t read16(uint32_t off)
  {
    // two byte cycles on real hardware, and the second one leaves the latches loaded
    uint8_t lo = read8(off);
    uint8_t hi = read8(off + 1);
    return (uint16_t)lo | ((uint16_t)hi << 8);
  }

  inline __attribute__((always_inline)) void write16(uint32_t off, uint16_t value)
  {
    // note both byte cycles use the same latch contents -- latches only reload on a read
    write8(off,     (uint8_t)value);
    write8(off + 1, (uint8_t)(value >> 8));
  }


private:

  inline __attribute__((always_inline)) uint32_t alu(uint32_t src, uint32_t lat)
  {
    switch (m_aluFunc) {
      case 1:  return src & lat;
      case 2:  return src | lat;
      case 3:  return src ^ lat;
      default: return src;
    }
  }

  void recomputeMasks();

  // expands a plane nibble so that plane N becomes 0xff or 0x00 in byte lane N
  static const uint32_t s_expand[16];

  uint32_t * m_bank[EGA_BANK_COUNT];
  uint32_t   m_bankMask;          // banks - 1, so the bank index is a mask not a compare
  uint32_t   m_offsetMask;        // banks * EGA_BANK_ENTRIES - 1

  uint32_t   m_latch;             // the four plane latches, one per byte lane

  // precomputed from the register file so the hot path never has to expand anything
  uint32_t   m_mapMask32;         // Sequencer index 2
  uint32_t   m_bitMask32;         // Graphics Controller index 8, replicated across the planes
  uint32_t   m_srBlend;           // Set/Reset value, masked by Enable Set/Reset
  uint32_t   m_srMask;            // planes where Enable Set/Reset is clear
  uint32_t   m_colorCompare32;    // Graphics Controller index 2
  uint32_t   m_colorDontCare32;   // Graphics Controller index 7

  uint32_t   m_mapMaskInv;        // ~m_mapMask32, precomputed to keep the hot path free of NOTs
  uint32_t   m_effBitMask32;      // Bit Mask, forced to 0 in write mode 1 so the latch wins
  uint32_t   m_effBitMaskInv;
  uint32_t   m_srcSel;            // all ones in write modes 0/3, zero in write mode 2
  uint32_t   m_srcSelInv;
  uint32_t   m_aluA, m_aluB, m_aluC;   // branchless Graphics Controller ALU function

  bool       m_simpleWrite;       // true when the Graphics Controller is doing nothing at all

  uint8_t    m_writeMode;
  uint8_t    m_readMode;
  uint8_t    m_rotate;
  uint8_t    m_aluFunc;
  uint8_t    m_readMapShift;      // Read Map Select as a shift, so read8 needs no multiply

  uint8_t    m_seqIndex;
  uint8_t    m_gcIndex;
  uint8_t    m_seqReg[8];
  uint8_t    m_gcReg[16];

};
