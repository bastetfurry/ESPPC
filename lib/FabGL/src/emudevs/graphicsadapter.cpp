/*
  Created by Fabrizio Di Vittorio (fdivitto2013@gmail.com) - <http://www.fabgl.com>
  Copyright (c) 2019-2022 Fabrizio Di Vittorio.
  All rights reserved.


* Please contact fdivitto2013@gmail.com if you need a commercial license.


* This library and related software is available under GPL v3.

  FabGL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  FabGL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with FabGL.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "graphicsadapter.h"




#pragma GCC optimize ("O2")


namespace fabgl {



static const RGB222 CGAPalette[16] = {
  RGB222(0, 0, 0),    // black
  RGB222(0, 0, 2),    // blue
  RGB222(0, 2, 0),    // green
  RGB222(0, 2, 2),    // cyan
  RGB222(2, 0, 0),    // red
  RGB222(2, 0, 2),    // magenta
  RGB222(2, 1, 0),    // brown
  RGB222(2, 2, 2),    // light gray
  RGB222(1, 1, 1),    // dark gray
  RGB222(1, 1, 3),    // light blue
  RGB222(1, 3, 1),    // light green
  RGB222(1, 3, 3),    // light cyan
  RGB222(3, 1, 1),    // light red
  RGB222(3, 1, 3),    // light magenta
  RGB222(3, 3, 1),    // yellow
  RGB222(3, 3, 3),    // white
};



static const RGB222 CGAGraphics4ColorsPalette[6][4] = {
  // low intensity PC graphics 4 colors palette
  {
    RGB222(0, 0, 0),    // background (not used)
    RGB222(0, 2, 0),    // green
    RGB222(2, 0, 0),    // red
    RGB222(2, 1, 0),    // brown
  },
  // high intensity PC graphics 4 colors palette
  {
    RGB222(0, 0, 0),    // background (not used)
    RGB222(1, 3, 1),    // light green
    RGB222(3, 1, 1),    // light red
    RGB222(3, 3, 1),    // yellow
  },
  // low intensity PC graphics 4 colors alternate palette
  {
    RGB222(0, 0, 0),    // background (not used)
    RGB222(0, 2, 2),    // cyan
    RGB222(2, 0, 2),    // magenta
    RGB222(2, 2, 2),    // light gray
  },
  // low intensity PC graphics 4 colors alternate palette
  {
    RGB222(0, 0, 0),    // background (not used)
    RGB222(1, 3, 3),    // light cyan
    RGB222(3, 1, 3),    // light magenta
    RGB222(3, 3, 3),    // white
  },
  // low intensity PC graphics 4 colors mode 5 palette (colour burst off: cyan, red, light gray)
  {
    RGB222(0, 0, 0),    // background (not used)
    RGB222(0, 2, 2),    // cyan
    RGB222(2, 0, 0),    // red
    RGB222(2, 2, 2),    // light gray
  },
  // high intensity PC graphics 4 colors mode 5 palette
  {
    RGB222(0, 0, 0),    // background (not used)
    RGB222(1, 3, 3),    // light cyan
    RGB222(3, 1, 1),    // light red
    RGB222(3, 3, 3),    // white
  }
};


// Default EGA palette as the EGA BIOS programs it for modes 0Dh/0Eh/10h: the familiar 16 CGA
// colours. Stored as FabGL RGB222 components already combined, one entry per attribute value.
static const RGB222 EGADefaultPalette[16] = {
  RGB222(0, 0, 0), RGB222(0, 0, 2), RGB222(0, 2, 0), RGB222(0, 2, 2),
  RGB222(2, 0, 0), RGB222(2, 0, 2), RGB222(2, 1, 0), RGB222(2, 2, 2),
  RGB222(1, 1, 1), RGB222(1, 1, 3), RGB222(1, 3, 1), RGB222(1, 3, 3),
  RGB222(3, 1, 1), RGB222(3, 1, 3), RGB222(3, 3, 1), RGB222(3, 3, 3),
};


void GraphicsAdapter::setupEmulation(Emulation emulation)
{
  static const struct {
    bool                 text;
    FontInfo const *     font;                 // valid when text = true
    uint8_t              cursorStart;          // valid when text = true
    uint8_t              cursorEnd;            // valid when text = true
    uint8_t              scanlinesPerCallback;
    DrawScanlineCallback scanlineCallback;
    char const *         modeline;
    int16_t              viewportWidth;
    int16_t              viewportHeight;
  } emulationInfo[] = {

    // Emulation::PC_Text_40x25_16Colors (CGA, Text Mode, 40x25x16)
    {  true,  &FONT_8x8,  5,  7, 4,      drawScanline_PC_Text_40x25_16Colors, QVGA_320x240_60Hz, 320, 200 },

    // Emulation::PC_Text_80x25_16Colors (CGA, Text Mode, 80x25x16)
    {  true, &FONT_8x16, 13, 15, 8,      drawScanline_PC_Text_80x25_16Colors,  VGA_640x480_60Hz, 640, 400 },

    // Emulation::PC_Graphics_320x200_4Colors (CGA, Graphics Mode, 320x200x4)
    { false,    nullptr,  0,  0, 1, drawScanline_PC_Graphics_320x200_4Colors, QVGA_320x240_60Hz, 320, 200 },

    // Emulation::PC_Graphics_640x200_2Colors (CGA, Graphics Mode, 640x200x2)
    { false,    nullptr,  0,  0, 1, drawScanline_PC_Graphics_640x200_2Colors, VGA_640x240_60Hz,  640, 200 },

    // Emulation::PC_Graphics_HGC_720x348 (Hercules, Graphics Mode, 720x348x2)
    { false,    nullptr,  0,  0, 2,     drawScanline_PC_Graphics_HGC_720x348, VGA_720x350_70Hz,  -1,  -1 },

    // --- PCEmulator local addition: EGA planar modes ---
    // The 350 line modes are driven on the 640x480 60Hz timing with the image centred, the same
    // trade the CGA 80x25 text mode above already makes. 640x350 is a real timing and was used
    // here at first, but it shares its 31.5kHz/70Hz sync rates with the 720x400 text mode, so
    // panels label it that, and 70Hz flickers visibly on them. 640x200 uses the doublescan 640x240
    // timing, which doubles the lines in hardware exactly as the CGA 640x200 mode already does,
    // and 320x200 uses the QVGA timing like CGA 320x200.
    //
    // Two scanlines per callback, not one: at one the scanline interrupt fires 24500 times a
    // second for these modes, which is a lot of overhead for the renderer to work against. Two is
    // a power of two, which VGADirectController::ISRHandler requires, and divides 350 and 200.

    // Emulation::PC_Graphics_EGA_640x350_16Colors (EGA, Graphics Mode, 640x350x16, BIOS 10h)
    { false,    nullptr,  0,  0, 2,           drawScanline_EGA_640_16Colors, VGA_640x480_60Hz, 640, 350 },

    // Emulation::PC_Graphics_EGA_640x350_2Colors (EGA, Graphics Mode, 640x350x2, BIOS 0Fh)
    { false,    nullptr,  0,  0, 2,           drawScanline_EGA_640_16Colors, VGA_640x480_60Hz, 640, 350 },

    // Emulation::PC_Graphics_EGA_640x200_16Colors (EGA, Graphics Mode, 640x200x16, BIOS 0Eh)
    { false,    nullptr,  0,  0, 2,           drawScanline_EGA_640_16Colors, VGA_640x240_60Hz, 640, 200 },

    // Emulation::PC_Graphics_EGA_320x200_16Colors (EGA, Graphics Mode, 320x200x16, BIOS 0Dh)
    { false,    nullptr,  0,  0, 1,           drawScanline_EGA_320_16Colors, QVGA_320x240_60Hz, 320, 200 },

    // Emulation::PC_Text_EGA_80x25_16Colors (EGA, Text Mode, 80x25, 8x14 glyphs from plane 2).
    // 25 rows of 14 scanlines is 350, which is a real timing, so no scaling is needed. This is
    // marked as a graphics mode in the table because the font does not come from FontInfo.
    // Emulation::PC_Text_EGA_80x25_16Colors -- deliberately the same shape as the CGA 80x25 entry
    // above: 640x400 on the 640x480 timing with a 16 line cell. A real EGA text mode is 8x14 on
    // 350 lines, but that timing looks poor on the panels this runs on, which is exactly why the
    // CGA text mode already makes the same trade.
    //
    // 8 scanlines per callback. This value MUST be a power of two: VGADirectController::ISRHandler
    // selects the line buffer with "scanLine & (linesCount - 1)", so anything else aliases onto
    // indices past the end of the buffer array and corrupts the heap. 8 also divides the 16 line
    // cell, so a call never spans two character rows, which is what lets the character, attribute
    // and colours be fetched once and reused across all eight scanlines.
    { false,    nullptr,  0,  0, 8,             drawScanline_EGA_Text_80x25, VGA_640x480_60Hz, 640, 400 },

    // --- PCEmulator local addition: Tandy 1000 16 colour modes, on the QVGA timing like CGA
    // 320x200. One scanline per callback, like the CGA modes: the renderers are a table lookup
    // per source byte and cheap enough for it.

    // Emulation::PC_Graphics_Tandy_320x200_16Colors (Tandy, BIOS 09h)
    { false,    nullptr,  0,  0, 1,     drawScanline_Tandy_320x200_16Colors, QVGA_320x240_60Hz, 320, 200 },

    // Emulation::PC_Graphics_Tandy_160x200_16Colors (Tandy, BIOS 08h)
    { false,    nullptr,  0,  0, 1,     drawScanline_Tandy_160x200_16Colors, QVGA_320x240_60Hz, 320, 200 },

  };

  if (emulation != Emulation::None) {
    auto info = emulationInfo + (int)emulation - 1;

    m_VGADCtrl.setDrawScanlineCallback(info->scanlineCallback, this);
    m_VGADCtrl.setScanlinesPerCallBack(info->scanlinesPerCallback);
    m_VGADCtrl.setResolution(info->modeline, info->viewportWidth, info->viewportHeight);

    if (info->text) {
      setFont(info->font);
      setCursorShape(info->cursorStart, info->cursorEnd);
      m_columns = m_VGADCtrl.getViewPortWidth() / m_font.width;
      m_rows    = m_VGADCtrl.getViewPortHeight() / m_font.height;
    } else if (emulation == Emulation::PC_Text_EGA_80x25_16Colors) {
      // A text mode that does not use FontInfo: the glyphs come from plane 2. The geometry still
      // has to be published, because the cursor position is computed against getTextColumns().
      m_columns = m_egaTextColumns;
      m_rows    = m_VGADCtrl.getViewPortHeight() / m_egaCharHeight;
    }
  }
}


GraphicsAdapter::GraphicsAdapter()
  : m_VGADCtrl(false),
    m_emulation(Emulation::None),
    m_videoBuffer(nullptr),
    m_rawLUT(nullptr),
    m_cursorRow(0),
    m_cursorCol(0),
    m_cursorStart(0),
    m_cursorEnd(0),
    m_cursorVisible(false),
    m_cursorGlyph(nullptr),
    m_bit7blink(true),
    m_PCGraphicsBackgroundColorIndex(0),
    m_PCGraphicsForegroundColorIndex(15),
    m_PCGraphicsPaletteInUse(0),
    m_videoEnabled(true),
    m_egaBanks(nullptr),
    m_egaStride(80),
    m_egaBankMask(3),
    m_egaOffsetMask(0x7fff),
    m_egaStart(0),
    m_egaCharHeight(16),
    m_egaTextColumns(80),
    m_egaPal2(nullptr),
    m_tandyMask(0x0f)
{
  for (int i = 0; i < 16; ++i)
    m_tandyPalette[i] = i;
  for (int i = 0; i < 4; ++i)
    m_egaSpread[i] = nullptr;
  for (int i = 0; i < 16; ++i)
    m_egaPalette[i] = i;
  m_font.data = nullptr;
  m_VGADCtrl.begin();
}


GraphicsAdapter::~GraphicsAdapter()
{
  enableVideo(false);
  cleanupFont();
  freeLUT();
  freeEGALUT();
  if (m_cursorGlyph)
    heap_caps_free(m_cursorGlyph);
}


bool GraphicsAdapter::enableVideo(bool value)
{
  if (value == m_videoEnabled)
    return m_videoEnabled;

  m_videoEnabled = value;

  if (m_videoEnabled) {
    setupEmulation(m_emulation);
    m_VGADCtrl.run();
  } else {
    m_VGADCtrl.end();
  }

  return !m_videoEnabled;
}


void GraphicsAdapter::setEmulation(Emulation emulation)
{
  if (m_emulation != emulation) {

    bool videoWasEnabled = enableVideo(false);
    freeLUT();

    m_emulation = emulation;
    if (m_emulation != Emulation::None) {
      setupEmulation(emulation);
      setupLUT();
      enableVideo(videoWasEnabled);
    } else {
      // PCEmulator: back to the unconfigured state for real. The host's setup screens run the
      // text mode before the machine starts, and without this its internal RAM copy of the font
      // stayed allocated for good, even on an EGA machine that never draws with it again.
      cleanupFont();
    }
  }
}


void GraphicsAdapter::freeLUT()
{
  if (m_rawLUT)
    heap_caps_free(m_rawLUT);
  m_rawLUT = nullptr;
}


void GraphicsAdapter::setupLUT()
{
  switch (m_emulation) {

    case Emulation::None:
      break;

    case Emulation::PC_Text_80x25_16Colors:
    case Emulation::PC_Text_40x25_16Colors:
      // each LUT item contains half pixel (an index to 16 colors palette)
      if (!m_rawLUT)
        m_rawLUT = (uint8_t*) heap_caps_malloc(16, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      for (int i = 0; i < 16; ++i)
        m_rawLUT[i]  = m_VGADCtrl.createRawPixel(CGAPalette[i]);
      break;

    case Emulation::PC_Graphics_320x200_4Colors:
      // each LUT item contains four pixels (decodes as four raw bytes)
      if (!m_rawLUT)
        m_rawLUT = (uint8_t*) heap_caps_malloc(256 * 4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      for (int i = 0; i < 256; ++i) {
        for (int j = 0; j < 4; ++j) {
          int pixel = (i >> (6 - j * 2)) & 0b11;
          uint8_t rawPixel = m_VGADCtrl.createRawPixel(pixel == 0 ? CGAPalette[m_PCGraphicsBackgroundColorIndex] : CGAGraphics4ColorsPalette[m_PCGraphicsPaletteInUse][pixel]);
          m_rawLUT[(i * 4) + (j ^ 2)] = rawPixel;
        }
      }
      break;

    case Emulation::PC_Graphics_640x200_2Colors:
      // each LUT item contains eight pixels (decodes as eight raw bytes)
      if (!m_rawLUT)
        m_rawLUT = (uint8_t*) heap_caps_malloc(256 * 8, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      for (int i = 0; i < 256; ++i) {
        for (int j = 0; j < 8; ++j) {
          bool pixel = (i >> (7 - j)) & 1;
          uint8_t rawPixel = m_VGADCtrl.createRawPixel(pixel ? CGAPalette[m_PCGraphicsForegroundColorIndex] : RGB222(0, 0, 0));
          m_rawLUT[(i * 8) + (j ^ 2)] = rawPixel;
        }
      }
      break;

    case Emulation::PC_Graphics_HGC_720x348:
      // each LUT item contains eight pixels (decodes as eight raw bytes)
      if (!m_rawLUT)
        m_rawLUT = (uint8_t*) heap_caps_malloc(256 * 8, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      for (int i = 0; i < 256; ++i) {
        for (int j = 0; j < 8; ++j) {
          bool pixel = (i >> (7 - j)) & 1;
          uint8_t rawPixel = m_VGADCtrl.createRawPixel(RGB222(pixel * 3, pixel * 3, pixel * 3));
          m_rawLUT[(i * 8) + (j ^ 2)] = rawPixel;
        }
      }
      break;

    case Emulation::PC_Graphics_EGA_640x350_16Colors:
    case Emulation::PC_Graphics_EGA_640x350_2Colors:
    case Emulation::PC_Graphics_EGA_640x200_16Colors:
    case Emulation::PC_Graphics_EGA_320x200_16Colors:
    case Emulation::PC_Text_EGA_80x25_16Colors:
      setupEGALUT();
      break;

    // PCEmulator: Tandy. The line buffer is byte swizzled in 32 bit words (pixel x goes to byte
    // x ^ 2, see the CGA 320x200 case above), and the tables are laid out so the renderers can
    // store whole words in order.
    case Emulation::PC_Graphics_Tandy_320x200_16Colors:
      // one source byte -> two raw pixels, the first (high nibble) in the low byte. A word is two
      // source bytes: the first one's pair goes to the upper half, which is bytes 2 and 3.
      if (!m_rawLUT)
        m_rawLUT = (uint8_t*) heap_caps_malloc(256 * 2, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      for (int i = 0; i < 256; ++i) {
        uint8_t hi = m_VGADCtrl.createRawPixel(CGAPalette[m_tandyPalette[(i >> 4) & m_tandyMask] & 15]);
        uint8_t lo = m_VGADCtrl.createRawPixel(CGAPalette[m_tandyPalette[i & m_tandyMask] & 15]);
        ((uint16_t*)m_rawLUT)[i] = hi | (lo << 8);
      }
      break;

    case Emulation::PC_Graphics_Tandy_160x200_16Colors:
      // one source byte -> four raw pixels, each source pixel twice: output pixels 0 and 1 (the
      // high nibble) land in bytes 2 and 3, pixels 2 and 3 in bytes 0 and 1
      if (!m_rawLUT)
        m_rawLUT = (uint8_t*) heap_caps_malloc(256 * 4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      for (int i = 0; i < 256; ++i) {
        uint32_t hi = m_VGADCtrl.createRawPixel(CGAPalette[m_tandyPalette[(i >> 4) & m_tandyMask] & 15]);
        uint32_t lo = m_VGADCtrl.createRawPixel(CGAPalette[m_tandyPalette[i & m_tandyMask] & 15]);
        ((uint32_t*)m_rawLUT)[i] = lo | (lo << 8) | (hi << 16) | (hi << 24);
      }
      break;

  }
}


// PCEmulator: build the tables the planar renderers use.
//
// A 16 colour planar pixel takes one bit from each of four planes, so it cannot be expanded with
// a single table the way the CGA modes are. Two stages instead:
//
//   m_egaSpread[p][b]  scatters the eight bits of plane p's byte into the p-th bit of eight
//                      nibbles, so ORing the four planes' entries gives eight 4 bit pixel
//                      indices packed into one uint32_t
//   m_egaPal2[b]       turns one byte of that word (two indices) into two raw VGA pixel bytes
//
// FabGL's scanline byte order is swizzled, and baking that into the spread tables keeps the
// inner loop to plain sequential 16 bit stores.
void GraphicsAdapter::setupEGALUT()
{
  for (int p = 0; p < 4; ++p) {
    if (!m_egaSpread[p])
      m_egaSpread[p] = (uint32_t*) heap_caps_malloc(256 * sizeof(uint32_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    for (int b = 0; b < 256; ++b) {
      uint32_t v = 0;
      for (int k = 0; k < 8; ++k)
        if ((b >> (7 - k)) & 1)
          v |= 1u << (4 * (k ^ 2) + p);
      m_egaSpread[p][b] = v;
    }
  }

  if (!m_egaPal2)
    m_egaPal2 = (uint16_t*) heap_caps_malloc(256 * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

  bool mono   = (m_emulation == Emulation::PC_Graphics_EGA_640x350_2Colors);

  // A palette register holds 6 bits, and what they mean depends on the monitor mode the scan
  // rate selects. In the 200 line modes the EGA drives a CGA style monitor: bits 2-0 are R G B
  // and bit 4 is intensity, the rest ignored, and the monitor turns dark yellow into brown. In
  // the 350 line modes every bit is a colour line, rgbRGB with the capitals worth two thirds and
  // the lower case one third -- which is exactly RGB222.
  bool line200 = (m_emulation == Emulation::PC_Graphics_EGA_320x200_16Colors ||
                  m_emulation == Emulation::PC_Graphics_EGA_640x200_16Colors);

  uint8_t raw[16];
  for (int i = 0; i < 16; ++i) {
    uint8_t v = m_egaPalette[i];
    RGB222 c;
    if (mono)
      c = RGB222((i & 1) * 3, (i & 1) * 3, (i & 1) * 3);
    else if (line200)
      c = EGADefaultPalette[(v & 0x07) | ((v >> 1) & 0x08)];
    else
      c = RGB222(((v >> 1) & 2) | ((v >> 5) & 1),
                 ( v       & 2) | ((v >> 4) & 1),
                 ((v << 1) & 2) | ((v >> 3) & 1));
    raw[i] = m_VGADCtrl.createRawPixel(c);
  }

  for (int b = 0; b < 256; ++b)
    m_egaPal2[b] = (uint16_t)raw[b & 0x0f] | ((uint16_t)raw[b >> 4] << 8);
}


void GraphicsAdapter::freeEGALUT()
{
  for (int p = 0; p < 4; ++p) {
    if (m_egaSpread[p])
      heap_caps_free(m_egaSpread[p]);
    m_egaSpread[p] = nullptr;
  }
  if (m_egaPal2)
    heap_caps_free(m_egaPal2);
  m_egaPal2 = nullptr;
}


void GraphicsAdapter::setEGAVideoBuffer(uint32_t * const * banks, int stride, uint32_t bankMask, uint32_t offsetMask)
{
  m_egaBanks      = banks;
  m_egaStride     = stride;
  m_egaBankMask   = bankMask;
  m_egaOffsetMask = offsetMask;
}


void GraphicsAdapter::setEGAPalette(uint8_t const * palette16)
{
  for (int i = 0; i < 16; ++i)
    m_egaPalette[i] = palette16[i];
  if (m_egaPal2)
    setupEGALUT();
}


void GraphicsAdapter::setPCGraphicsBackgroundColorIndex(int colorIndex)
{
  m_PCGraphicsBackgroundColorIndex = colorIndex;
  setupLUT();
}


void GraphicsAdapter::setPCGraphicsForegroundColorIndex(int colorIndex)
{
  m_PCGraphicsForegroundColorIndex = colorIndex;
  setupLUT();
}


void GraphicsAdapter::setPCGraphicsPaletteInUse(int paletteIndex)
{
  m_PCGraphicsPaletteInUse = paletteIndex;
  setupLUT();
}


void GraphicsAdapter::setTandyPalette(uint8_t const * palette16, uint8_t mask)
{
  memcpy(m_tandyPalette, palette16, 16);
  m_tandyMask = mask & 0x0f;
  if (m_emulation == Emulation::PC_Graphics_Tandy_320x200_16Colors ||
      m_emulation == Emulation::PC_Graphics_Tandy_160x200_16Colors)
    setupLUT();
}


void GraphicsAdapter::setVideoBuffer(void const * videoBuffer)
{
  m_videoBuffer = (uint8_t*) videoBuffer;
}


void GraphicsAdapter::cleanupFont()
{
  if (m_font.data) {
    heap_caps_free((void*)m_font.data);
    m_font.data = nullptr;
  }
}


void GraphicsAdapter::setFont(FontInfo const * font)
{
  cleanupFont();
  if (font) {
    m_font = *font;
    // copy font data into internal RAM
    auto size = 256 * ((m_font.width + 7) / 8) * m_font.height;
    m_font.data = (uint8_t const *) heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    memcpy((void*)m_font.data, font->data, size);
  }
}


void GraphicsAdapter::setCursorShape(int start, int end)
{
  if (start != m_cursorStart || end != m_cursorEnd) {
    m_cursorStart = start;
    m_cursorEnd   = end;

    // readapt start->end to the actual font height to make sure the cursor is always visible
    if (start <= end && end >= m_font.height) {
      int h = end - start;
      end   = m_font.height - 1;
      start = end - h;
    }

    int charWidthInBytes = (m_font.width + 7) / 8;
    if (!m_cursorGlyph)
      m_cursorGlyph = (uint8_t*) heap_caps_malloc(charWidthInBytes * m_font.height, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    memset(m_cursorGlyph, 0, charWidthInBytes * m_font.height);
    if (end >= start && start >= 0 && start < m_font.height && end < m_font.height)
      memset(m_cursorGlyph + (start * charWidthInBytes), 0xff, (end - start + 1) * charWidthInBytes);
  }
}


void GraphicsAdapter::setCursorPos(int row, int column)
{
  m_cursorRow = row;
  m_cursorCol = column;
}


void IRAM_ATTR GraphicsAdapter::drawScanline_PC_Text_40x25_16Colors(void * arg, uint8_t * dest, int scanLine)
{
  auto ga = (GraphicsAdapter*) arg;

  constexpr int CHARWIDTH        = 8;
  constexpr int CHARHEIGHT       = 8;
  constexpr int CHARWIDTHINBYTES = (CHARWIDTH + 7) / 8;
  constexpr int CHARSIZEINBYTES  = CHARWIDTHINBYTES * CHARHEIGHT;
  constexpr int COLUMNS          = 40;
  constexpr int SCREENWIDTH      = 320;
  constexpr int LINES            = 8;

  if (scanLine == 0)
    ++ga->m_frameCounter;

  int charScanline = scanLine & (CHARHEIGHT - 1);
  int textRow      = scanLine / CHARHEIGHT;

  uint8_t const * fontData = ga->m_font.data + (charScanline * CHARWIDTHINBYTES);

  uint8_t * rawLUT = ga->m_rawLUT;

  uint8_t const * curItem = ga->m_videoBuffer + (textRow * COLUMNS * 2);

  bool showCursor = ga->m_cursorVisible && ga->m_cursorRow == textRow && ((ga->m_frameCounter & 0x1f) < 0xf);
  int cursorCol = ga->m_cursorCol;

  bool bit7blink = ga->m_bit7blink;
  bool blinktime = bit7blink && !((ga->m_frameCounter & 0x3f) < 0x1f);

  for (int textCol = 0; textCol < COLUMNS; ++textCol) {

    int charIdx  = *curItem++;
    int charAttr = *curItem++;

    bool blink = false;
    if (bit7blink) {
      blink = blinktime && (charAttr & 0x80);
      charAttr &= 0x7f;
    }

    uint8_t bg = rawLUT[charAttr >> 4];
    uint8_t fg = blink ? bg : rawLUT[charAttr & 0xf];

    const uint8_t colors[2] = { bg, fg };

    uint8_t const * charBitmapPtr = fontData + charIdx * CHARSIZEINBYTES;

    auto destptr = dest;

    if (showCursor && textCol == cursorCol) {

      uint8_t const * cursorBitmapPtr = ga->m_cursorGlyph + (charScanline * CHARWIDTHINBYTES);

      for (int charRow = 0; charRow < LINES / 2; ++charRow) {

        uint32_t charBitmap = *charBitmapPtr | *cursorBitmapPtr;

        *(destptr + 0) = colors[(bool)(charBitmap & 0x20)];
        *(destptr + 1) = colors[(bool)(charBitmap & 0x10)];
        *(destptr + 2) = colors[(bool)(charBitmap & 0x80)];
        *(destptr + 3) = colors[(bool)(charBitmap & 0x40)];
        *(destptr + 4) = colors[(bool)(charBitmap & 0x02)];
        *(destptr + 5) = colors[(bool)(charBitmap & 0x01)];
        *(destptr + 6) = colors[(bool)(charBitmap & 0x08)];
        *(destptr + 7) = colors[(bool)(charBitmap & 0x04)];

        destptr += SCREENWIDTH;
        charBitmapPtr += CHARWIDTHINBYTES;
        cursorBitmapPtr += CHARWIDTHINBYTES;
      }

    } else {

      for (int charRow = 0; charRow < LINES / 2; ++charRow) {

        uint32_t charBitmap = *charBitmapPtr;

        *(destptr + 0) = colors[(bool)(charBitmap & 0x20)];
        *(destptr + 1) = colors[(bool)(charBitmap & 0x10)];
        *(destptr + 2) = colors[(bool)(charBitmap & 0x80)];
        *(destptr + 3) = colors[(bool)(charBitmap & 0x40)];
        *(destptr + 4) = colors[(bool)(charBitmap & 0x02)];
        *(destptr + 5) = colors[(bool)(charBitmap & 0x01)];
        *(destptr + 6) = colors[(bool)(charBitmap & 0x08)];
        *(destptr + 7) = colors[(bool)(charBitmap & 0x04)];

        destptr += SCREENWIDTH;
        charBitmapPtr += CHARWIDTHINBYTES;
      }

    }

    dest += 8;

  }
}


// PCEmulator: text mode pixel expansion, four pixels per 32 bit word. TEXT_NIBMASK[n] has 0xff in
// the byte of every pixel whose bit is set in glyph nibble n (bit 3 = leftmost), with FabGL's
// pairwise byte swizzle (pixel i lives in byte i ^ 2) already applied. A glyph row is then two
// word stores, (fg & mask) | (bg & ~mask), instead of eight table lookups and eight byte stores.
// The text renderers written the old way took about 245 us of a 244 us budget, so any extra load
// on the other core (disk I/O in particular) made them overrun and the screen glitched.
static const DRAM_ATTR uint32_t TEXT_NIBMASK[16] = {
  0x00000000, 0x0000ff00, 0x000000ff, 0x0000ffff,
  0xff000000, 0xff00ff00, 0xff0000ff, 0xff00ffff,
  0x00ff0000, 0x00ffff00, 0x00ff00ff, 0x00ffffff,
  0xffff0000, 0xffffff00, 0xffff00ff, 0xffffffff,
};

// The same for the 40 column modes, where every pixel is doubled: TEXT_PAIRMASK[v] covers two bits
// of the glyph (bit 1 = left) as four pixels, one word.
static const DRAM_ATTR uint32_t TEXT_PAIRMASK[4] = {
  0x00000000, 0x0000ffff, 0xffff0000, 0xffffffff,
};


void IRAM_ATTR GraphicsAdapter::drawScanline_PC_Text_80x25_16Colors(void * arg, uint8_t * dest, int scanLine)
{
  auto ga = (GraphicsAdapter*) arg;

  constexpr int CHARWIDTH        = 8;
  constexpr int CHARHEIGHT       = 16;
  constexpr int CHARWIDTHINBYTES = (CHARWIDTH + 7) / 8;
  constexpr int CHARSIZEINBYTES  = CHARWIDTHINBYTES * CHARHEIGHT;
  constexpr int COLUMNS          = 80;
  constexpr int SCREENWIDTH      = 640;
  constexpr int LINES            = 16;

  if (scanLine == 0)
    ++ga->m_frameCounter;

  int charScanline = scanLine & (CHARHEIGHT - 1);
  int textRow      = scanLine / CHARHEIGHT;

  uint8_t const * fontData = ga->m_font.data + (charScanline * CHARWIDTHINBYTES);

  uint8_t * rawLUT = ga->m_rawLUT;

  uint8_t const * curItem = ga->m_videoBuffer + (textRow * COLUMNS * 2);

  bool showCursor = ga->m_cursorVisible && ga->m_cursorRow == textRow && ((ga->m_frameCounter & 0x1f) < 0xf);
  int cursorCol = ga->m_cursorCol;

  bool bit7blink = ga->m_bit7blink;
  bool blinktime = bit7blink && !((ga->m_frameCounter & 0x3f) < 0x1f);

  for (int textCol = 0; textCol < COLUMNS; ++textCol) {

    int charIdx  = *curItem++;
    int charAttr = *curItem++;

    bool blink = false;
    if (bit7blink) {
      blink = blinktime && (charAttr & 0x80);
      charAttr &= 0x7f;
    }

    uint8_t bg = rawLUT[charAttr >> 4];
    uint8_t fg = blink ? bg : rawLUT[charAttr & 0xf];

    uint32_t fgw = fg * 0x01010101u;
    uint32_t bgw = bg * 0x01010101u;

    uint8_t const * charBitmapPtr = fontData + charIdx * CHARSIZEINBYTES;
    uint8_t const * cursorBitmapPtr = (showCursor && textCol == cursorCol) ? ga->m_cursorGlyph + (charScanline * CHARWIDTHINBYTES) : nullptr;

    auto destptr = dest;

    for (int charRow = 0; charRow < LINES / 2; ++charRow) {
      uint8_t charBitmap = *charBitmapPtr;
      if (cursorBitmapPtr) {
        charBitmap |= *cursorBitmapPtr;
        cursorBitmapPtr += CHARWIDTHINBYTES;
      }
      uint32_t m0 = TEXT_NIBMASK[charBitmap >> 4];
      uint32_t m1 = TEXT_NIBMASK[charBitmap & 0x0f];
      ((uint32_t *) destptr)[0] = (fgw & m0) | (bgw & ~m0);
      ((uint32_t *) destptr)[1] = (fgw & m1) | (bgw & ~m1);
      destptr += SCREENWIDTH;
      charBitmapPtr += CHARWIDTHINBYTES;
    }

    dest += 8;

  }
}


// offset 0x0000 for even scan lines (bit 0 = 0), lines 0, 2, 4...
// offset 0x2000 for odd scan lines  (bit 0 = 1), lines 1, 3, 5...
void IRAM_ATTR GraphicsAdapter::drawScanline_PC_Graphics_320x200_4Colors(void * arg, uint8_t * dest, int scanLine)
{
  constexpr int WIDTH         = 320;
  constexpr int PIXELSPERBYTE = 4;
  constexpr int WIDTHINBYTES  = WIDTH / PIXELSPERBYTE;

  auto ga = (GraphicsAdapter*) arg;

  auto src = ga->m_videoBuffer + ((scanLine & 1) << 13) + WIDTHINBYTES * (scanLine >> 1);

  auto dest32 = (uint32_t*) dest;

  auto LUT32 = (uint32_t*) ga->m_rawLUT;

  for (int col = 0; col < WIDTH; col += PIXELSPERBYTE) {
    *dest32++ = LUT32[*src++];
  }
}


// offset 0x0000 for even scan lines (bit 0 = 0), lines 0, 2, 4...
// offset 0x2000 for odd scan lines  (bit 0 = 1), lines 1, 3, 5...
void IRAM_ATTR GraphicsAdapter::drawScanline_PC_Graphics_640x200_2Colors(void * arg, uint8_t * dest, int scanLine)
{
  constexpr int WIDTH         = 640;
  constexpr int PIXELSPERBYTE = 8;
  constexpr int WIDTHINBYTES  = WIDTH / PIXELSPERBYTE;

  auto ga = (GraphicsAdapter*) arg;

  auto src = ga->m_videoBuffer + ((scanLine & 1) << 13) + WIDTHINBYTES * (scanLine >> 1);

  auto dest64 = (uint64_t*) dest;

  auto LUT64 = (uint64_t*) ga->m_rawLUT;

  for (int col = 0; col < WIDTH; col += PIXELSPERBYTE) {
    *dest64++ = LUT64[*src++];
  }
}


// offset 0x0000 for lines 0, 4, 8, 12...
// offset 0x2000 for lines 1, 5, 9, 13...
// offset 0x4000 for lines 2, 6, 10, 14...
// offset 0x6000 for lines 3, 7, 11, 15...
void IRAM_ATTR GraphicsAdapter::drawScanline_PC_Graphics_HGC_720x348(void * arg, uint8_t * dest, int scanLine)
{
  constexpr int WIDTH         = 720;
  constexpr int PIXELSPERBYTE = 8;
  constexpr int WIDTHINBYTES  = WIDTH / PIXELSPERBYTE;

  auto ga = (GraphicsAdapter*) arg;

  auto src = ga->m_videoBuffer + ((scanLine & 0b11) << 13) + WIDTHINBYTES * (scanLine >> 2);

  auto dest64 = (uint64_t*) dest;

  auto LUT64 = (uint64_t*) ga->m_rawLUT;

  for (int col = 0; col < WIDTH; col += PIXELSPERBYTE)
    *dest64++ = LUT64[*src++];

  ++scanLine;
  src = ga->m_videoBuffer + ((scanLine & 0b11) << 13) + WIDTHINBYTES * (scanLine >> 2);

  for (int col = 0; col < WIDTH; col += PIXELSPERBYTE)
    *dest64++ = LUT64[*src++];
}




// PCEmulator: EGA planar renderers.
//
// One 32 bit load brings in all four planes of eight pixels, because the emulator stores the
// planes interleaved (src/ega.h). Four table lookups and three ORs turn that into eight packed
// 4 bit colour indices, and four more lookups turn those into eight raw VGA bytes.
//
// VRAM is banked so it can be allocated on a fragmented heap, so the byte-column offset is
// resolved through the bank table on each iteration -- a shift, a load and a mask.

// These two must match EGA_BANK_SHIFT and EGA_BANK_ENTRIES in src/ega.h. They are constants
// rather than values carried in the adapter because this indexes once per byte-column on the
// scanline hot path.
#define EGA_BANK_SHIFT_R   12
#define EGA_BANK_INDEXMASK 4095

#define EGA_RENDER_BANK(o)  (ga->m_egaBanks[((o) >> EGA_BANK_SHIFT_R) & ga->m_egaBankMask][(o) & EGA_BANK_INDEXMASK])


void IRAM_ATTR GraphicsAdapter::drawScanline_EGA_640_16Colors(void * arg, uint8_t * dest, int scanLine)
{
  constexpr int WIDTHINBYTES = 640 / 8;
  constexpr int SCREENWIDTH  = 640;
  constexpr int LINES        = 2;         // scanlines per call, see the mode table

  auto ga = (GraphicsAdapter*) arg;

  // bank 0 too: EGA::allocVRAM() nulls every bank when it runs out, and this runs from the
  // scanline ISR, where following one of those is a panic rather than a glitch
  if (!ga->m_egaBanks || !ga->m_egaBanks[0] || !ga->m_egaPal2)
    return;

  uint32_t const * L0 = ga->m_egaSpread[0];
  uint32_t const * L1 = ga->m_egaSpread[1];
  uint32_t const * L2 = ga->m_egaSpread[2];
  uint32_t const * L3 = ga->m_egaSpread[3];
  uint16_t const * P  = ga->m_egaPal2;

  // FabGL's line buffers are contiguous, so the next scanline is one viewport width along
  for (int line = 0; line < LINES; ++line) {

    uint32_t off = (ga->m_egaStart + (scanLine + line) * ga->m_egaStride) & ga->m_egaOffsetMask;
    auto     d32 = (uint32_t*) (dest + line * SCREENWIDTH);

    // A line is 80 consecutive byte-columns and a bank is EGA_BANK_INDEXMASK + 1 of them, so it
    // lies in one bank or runs into the next exactly once (the offset mask can also wrap it back
    // to the start, which is the same case). Resolving the bank per run rather than per column,
    // and storing two palette pairs as one word, took the longest call in Windows from 59 to
    // 55 us, against 63.6 us between calls at 640x480.
    for (int col = 0; col < WIDTHINBYTES; ) {
      uint32_t idx = off & EGA_BANK_INDEXMASK;
      int      run = fabgl::imin(WIDTHINBYTES - col, (int)(EGA_BANK_INDEXMASK + 1 - idx));
      uint32_t const * src = &ga->m_egaBanks[(off >> EGA_BANK_SHIFT_R) & ga->m_egaBankMask][idx];

      for (int k = 0; k < run; ++k) {
        uint32_t w = src[k];
        uint32_t i = L0[w & 0xff] | L1[(w >> 8) & 0xff] | L2[(w >> 16) & 0xff] | L3[w >> 24];
        d32[0] = (uint32_t)P[i & 0xff]         | ((uint32_t)P[(i >> 8) & 0xff] << 16);
        d32[1] = (uint32_t)P[(i >> 16) & 0xff] | ((uint32_t)P[i >> 24] << 16);
        d32 += 2;
      }

      col += run;
      off  = (off + run) & ga->m_egaOffsetMask;
    }
  }
}


void IRAM_ATTR GraphicsAdapter::drawScanline_EGA_320_16Colors(void * arg, uint8_t * dest, int scanLine)
{
  constexpr int WIDTHINBYTES = 320 / 8;

  auto ga = (GraphicsAdapter*) arg;

  // bank 0 too: EGA::allocVRAM() nulls every bank when it runs out, and this runs from the
  // scanline ISR, where following one of those is a panic rather than a glitch
  if (!ga->m_egaBanks || !ga->m_egaBanks[0] || !ga->m_egaPal2)
    return;

  uint32_t const * L0 = ga->m_egaSpread[0];
  uint32_t const * L1 = ga->m_egaSpread[1];
  uint32_t const * L2 = ga->m_egaSpread[2];
  uint32_t const * L3 = ga->m_egaSpread[3];
  uint16_t const * P  = ga->m_egaPal2;

  uint32_t off  = (ga->m_egaStart + scanLine * ga->m_egaStride) & ga->m_egaOffsetMask;
  auto     d16  = (uint16_t*) dest;

  for (int col = 0; col < WIDTHINBYTES; ++col) {
    uint32_t w = EGA_RENDER_BANK(off);
    off = (off + 1) & ga->m_egaOffsetMask;

    uint32_t i = L0[w & 0xff] | L1[(w >> 8) & 0xff] | L2[(w >> 16) & 0xff] | L3[w >> 24];

    d16[0] = P[i & 0xff];
    d16[1] = P[(i >> 8) & 0xff];
    d16[2] = P[(i >> 16) & 0xff];
    d16[3] = P[i >> 24];
    d16 += 4;
  }
}


// PCEmulator: EGA text.
//
// Characters come from plane 0, attributes from plane 1, and the glyph bitmap from plane 2 --
// which is the whole point: a program that rewrites plane 2 changes the font, and this renderer
// picks it up with no further work. Plane 2 holds 32 bytes per character, of which the first
// m_egaCharHeight are used.
void IRAM_ATTR GraphicsAdapter::drawScanline_EGA_Text_80x25(void * arg, uint8_t * dest, int scanLine)
{
  constexpr int SCREENWIDTH = 640;
  constexpr int LINES       = 8;          // scanlines rendered per call, see the mode table

  auto ga = (GraphicsAdapter*) arg;

  // bank 0 too: EGA::allocVRAM() nulls every bank when it runs out, and this runs from the
  // scanline ISR, where following one of those is a panic rather than a glitch
  if (!ga->m_egaBanks || !ga->m_egaBanks[0] || !ga->m_egaPal2)
    return;

  int columns    = ga->m_egaTextColumns;
  int charWidth  = (columns >= 80) ? 8 : 16;      // the 40 column modes double every pixel
  int charHeight = ga->m_egaCharHeight;

  int textRow  = scanLine / charHeight;
  int glyphRow = scanLine % charHeight;

  if (scanLine == 0)
    ++ga->m_frameCounter;

  bool showCursor = ga->m_cursorVisible && ga->m_cursorRow == textRow &&
                    ((ga->m_frameCounter & 0x1f) < 0xf);
  int  cursorCol  = ga->m_cursorCol;

  bool bit7blink = ga->m_bit7blink;
  bool blinktime = bit7blink && !((ga->m_frameCounter & 0x3f) < 0x1f);

  uint16_t const * P        = ga->m_egaPal2;
  uint32_t const   omask    = ga->m_egaOffsetMask;
  uint32_t const   bmask    = ga->m_egaBankMask;
  uint32_t * const * banks  = ga->m_egaBanks;

  uint32_t rowBase = (ga->m_egaStart + (uint32_t)textRow * columns) & omask;

  for (int col = 0; col < columns; ++col) {

    // fetched once per character, then used for every scanline in this call
    uint32_t o = (rowBase + col) & omask;
    uint32_t w = banks[(o >> EGA_BANK_SHIFT_R) & bmask][o & EGA_BANK_INDEXMASK];

    uint8_t ch   = (uint8_t) w;             // plane 0: character
    uint8_t attr = (uint8_t)(w >> 8);       // plane 1: attribute

    bool blink = false;
    if (bit7blink) {
      blink = blinktime && (attr & 0x80);
      attr &= 0x7f;
    }

    uint8_t fg = (uint8_t) P[(attr & 0x0f) * 0x11];
    uint8_t bg = (uint8_t) P[(attr >> 4) * 0x11];
    if (blink)
      fg = bg;

    bool cursorHere = showCursor && col == cursorCol;
    uint8_t * out = dest + col * charWidth;

    // Plane 2 holds the glyphs, 32 bytes per character -- rewrite it and the font changes. A
    // character's 32 bytes never straddle a bank (banks are EGA_BANK_INDEXMASK + 1 entries, a
    // multiple of 32), so the bank is resolved once and the rows are read through one pointer.
    uint32_t go = ((uint32_t)ch * 32 + glyphRow) & omask;
    uint32_t const * glyph = &banks[(go >> EGA_BANK_SHIFT_R) & bmask][go & EGA_BANK_INDEXMASK];

    if (charWidth == 8) {

      uint32_t fgw = fg * 0x01010101u;
      uint32_t bgw = bg * 0x01010101u;

      for (int i = 0; i < LINES; ++i) {
        uint8_t bits = (uint8_t)(glyph[i] >> 16);
        if (cursorHere && glyphRow + i >= charHeight - 2)
          bits = 0xff;
        uint32_t * p = (uint32_t *)(out + i * SCREENWIDTH);
        uint32_t m0  = TEXT_NIBMASK[bits >> 4];
        uint32_t m1  = TEXT_NIBMASK[bits & 0x0f];
        p[0] = (fgw & m0) | (bgw & ~m0);
        p[1] = (fgw & m1) | (bgw & ~m1);
      }

    } else {

      // 40 columns: every pixel doubled. Half the characters but the same 640 pixels per line, so
      // the same work -- this path is as tight as the 80 column one and gets the same treatment.
      uint32_t fgw = fg * 0x01010101u;
      uint32_t bgw = bg * 0x01010101u;

      for (int i = 0; i < LINES; ++i) {
        uint8_t bits = (uint8_t)(glyph[i] >> 16);
        if (cursorHere && glyphRow + i >= charHeight - 2)
          bits = 0xff;
        uint32_t * p = (uint32_t *)(out + i * SCREENWIDTH);
        for (int k = 0; k < 4; ++k) {
          uint32_t m = TEXT_PAIRMASK[(bits >> (6 - 2 * k)) & 3];
          p[k] = (fgw & m) | (bgw & ~m);
        }
      }

    }
  }
}


// PCEmulator: Tandy 320x200x16 (BIOS 09h). A character row is four scanlines, and line n of the
// row lives in bank n (n * 8 KB), 160 bytes a line. Two source bytes make one 32 bit store.
void IRAM_ATTR GraphicsAdapter::drawScanline_Tandy_320x200_16Colors(void * arg, uint8_t * dest, int scanLine)
{
  constexpr int WIDTHINBYTES = 320 / 2;

  auto ga = (GraphicsAdapter*) arg;

  auto src    = ga->m_videoBuffer + ((scanLine & 3) << 13) + WIDTHINBYTES * (scanLine >> 2);
  auto dest32 = (uint32_t*) dest;
  auto LUT16  = (uint16_t const *) ga->m_rawLUT;

  for (int col = 0; col < WIDTHINBYTES; col += 2, src += 2)
    *dest32++ = ((uint32_t)LUT16[src[0]] << 16) | LUT16[src[1]];
}


// PCEmulator: Tandy 160x200x16 (BIOS 08h). Two scanlines to a character row, so two banks, 80 bytes
// a line; each source byte becomes four output pixels, one 32 bit store.
void IRAM_ATTR GraphicsAdapter::drawScanline_Tandy_160x200_16Colors(void * arg, uint8_t * dest, int scanLine)
{
  constexpr int WIDTHINBYTES = 160 / 2;

  auto ga = (GraphicsAdapter*) arg;

  auto src    = ga->m_videoBuffer + ((scanLine & 1) << 13) + WIDTHINBYTES * (scanLine >> 1);
  auto dest32 = (uint32_t*) dest;
  auto LUT32  = (uint32_t const *) ga->m_rawLUT;

  for (int col = 0; col < WIDTHINBYTES; ++col)
    *dest32++ = LUT32[*src++];
}


};  // fabgl namespace
