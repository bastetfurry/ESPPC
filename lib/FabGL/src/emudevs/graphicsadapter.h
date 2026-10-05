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


#pragma once


/**
 * @file
 *
 * @brief This file contains fabgl::GraphicsAdapter definition.
 */



#include <stdlib.h>
#include <stdint.h>

#include "fabgl.h"



namespace fabgl {



class GraphicsAdapter {

public:

  enum class Emulation {
    None,
    
    // CGA Color Text Mode (PC BIOS 00h/01h)
    // Requires 2 * 40 * 25 = 2000 bytes
    PC_Text_40x25_16Colors,
    
    // CGA Color Text Mode (PC BIOS 02h/03h)
    // This is not visually like CGA because use 640x400 screen with 8x16 fonts, while should be 640x200 with 8x8 fonts
    // Requires 2 * 80 * 25 = 4000 bytes
    PC_Text_80x25_16Colors,
    
    // CGA 320x200, 4 Colors Graphics Mode (PC BIOS 04h/05h)
    // Requires 320 / 4 * 200 = 16000 bytes
    PC_Graphics_320x200_4Colors,
    
    // CGA 640x200, 2 Colors Graphics Mode (PC BIOS 06h)
    // Requires 640 / 8 * 200 = 16000 bytes
    PC_Graphics_640x200_2Colors,
    

    // --- ESPPC local addition: EGA planar graphics modes ---
    //
    // These do not read a linear frame buffer. EGA pixels live in four bit planes, which the
    // emulator keeps interleaved as one uint32_t per byte-column (see src/ega.h), so the video
    // buffer for these modes is set with setEGAVideoBuffer() rather than setVideoBuffer().

    // EGA 640x350, 16 Colors (PC BIOS 10h)
    PC_Graphics_EGA_640x350_16Colors,

    // EGA 640x350, 2 Colors (PC BIOS 0Fh) -- same planar layout, two entry palette
    PC_Graphics_EGA_640x350_2Colors,

    // EGA 640x200, 16 Colors (PC BIOS 0Eh)
    PC_Graphics_EGA_640x200_16Colors,

    // EGA 320x200, 16 Colors (PC BIOS 0Dh)
    PC_Graphics_EGA_320x200_16Colors,

    // EGA 80x25 Color Text (PC BIOS 02h/03h on an EGA).
    //
    // Unlike the CGA text modes this does not read a linear character/attribute buffer and a
    // fixed ROM font. The EGA keeps characters in plane 0, attributes in plane 1 and the font
    // itself in plane 2, which is why software can redefine the character set: the renderer
    // simply draws whatever glyphs are in plane 2 at the time.
    PC_Text_EGA_80x25_16Colors,

    // --- ESPPC local addition: Tandy 1000 16 colour modes ---
    //
    // Linear frame buffers like CGA, two pixels per byte (high nibble first), with the lines of a
    // character row spread over 8 KB banks. Every pixel goes through the 16 palette registers.

    // Tandy 320x200, 16 Colors (PC BIOS 09h). 32 KB: four banks, 160 bytes a line.
    PC_Graphics_Tandy_320x200_16Colors,

    // Tandy 160x200, 16 Colors (PC BIOS 08h). 16 KB: two banks, 80 bytes a line, every pixel
    // shown two pixels wide.
    PC_Graphics_Tandy_160x200_16Colors,

    // --- ESPPC local addition: Plantronics ColorPlus ---
    //
    // A CGA with a second 16 KB plane: plane 0 at B8000, plane 1 at BC000, both in the CGA layout
    // (even and odd lines 8 KB apart, 80 bytes a line). The video buffer is plane 0; plane 1 is
    // 16 KB after it.

    // Plantronics 320x200, 16 Colors: 2 bits of each pixel in each plane.
    PC_Graphics_Plantronics_320x200_16Colors,

    // Plantronics 640x200, 4 Colors: 1 bit of each pixel in each plane, through the CGA palette.
    PC_Graphics_Plantronics_640x200_4Colors,

    // --- ESPPC local addition: CGA on a composite colour monitor ---
    //
    // The CGA graphics modes as an NTSC monitor shows them with the colour burst on: every nibble
    // of video memory (four dots of 640x200, or two pixels of 320x200) becomes one artifact colour,
    // so the picture is 160x200 in 16 colours. Same memory layout as the CGA modes; the colour
    // table is picked with setCompositeTable().
    PC_Graphics_CGA_Composite,
  };


  GraphicsAdapter();
  ~GraphicsAdapter();

  void setEmulation(Emulation emulation);
  Emulation emulation()                                    { return m_emulation; }

  bool enableVideo(bool value);

  void setVideoBuffer(void const * videoBuffer);

  // ESPPC: EGA planar modes. banks is the bank pointer array from the EGA engine, stride is
  // the number of byte-columns per scanline, and palette is 16 entries of 6 bit EGA RGB (rgbRGB),
  // which map one to one onto FabGL's RGB222.
  void setEGAVideoBuffer(uint32_t * const * banks, int stride, uint32_t bankMask, uint32_t offsetMask);
  void setEGAPalette(uint8_t const * palette16);
  void setEGAStartAddress(uint32_t byteOffset)             { m_egaStart = byteOffset; }

  // EGA line compare (CRTC 18h with bit 8 in 07h): the split screen. The scanlines after this one
  // are drawn from address 0, whatever the start address; a value past the last line turns it off.
  // Software keeps a status bar there while it page flips the part above.
  void setEGALineCompare(int line)                         { m_egaLineCompare = line; }

  // EGA text: character height in scanlines (14 for mode 3 on an EGA, 8 for 43 line mode)
  void setEGACharHeight(int h)                             { m_egaCharHeight = h; }

  // EGA text: 40 or 80 columns. The 40 column modes are the same 640 pixel scanline with every
  // pixel doubled, which is what halving the dot clock does on the real card.
  void setEGATextColumns(int c)                            { m_egaTextColumns = c; m_columns = c; }

  // text modes methods
  void setCursorShape(int start, int end);
  void setCursorPos(int row, int column);
  void setCursorVisible(bool value)                        { m_cursorVisible = value; }
  void setBit7Blink(bool value)                            { m_bit7blink = value; }
  int getTextColumns()                                     { return m_columns; }
  int getTextRows()                                        { return m_rows; }

  // PC graphics modes methods
  void setPCGraphicsBackgroundColorIndex(int colorIndex);
  void setPCGraphicsForegroundColorIndex(int colorIndex);
  void setPCGraphicsPaletteInUse(int paletteIndex);

  // ESPPC: the Tandy palette registers (4 bit IRGB, an index into the 16 CGA colours) and the
  // palette mask the pixel value is ANDed with first. Takes effect at once in a Tandy mode.
  void setTandyPalette(uint8_t const * palette16, uint8_t mask);

  // ESPPC: an on-screen drive activity LED, for boards without a free real one. 3x3 pixels in
  // the lower right corner of every mode, dim green when idle and bright for a moment after each
  // driveActivity(). mode: 0 = off, 1 = one LED for all drives, 2 = one each for A, B, C and D.
  // It is drawn into the line buffers only, never into guest memory.
  void setDriveLEDMode(int mode);
  void driveActivity(int drive);

  // ESPPC: which artifact colours the composite mode shows: 0 = from 640x200, 1 = from
  // 320x200 palette 0, 2 = from 320x200 palette 1
  void setCompositeTable(int table);
  int getGraphWidth()                                      { return m_VGADCtrl.getViewPortWidth(); }
  int getGraphHeight()                                     { return m_VGADCtrl.getViewPortHeight(); }

  bool VSync()                                             { return m_VGADCtrl.VSync(); }

  // for the debug monitor: what the adapter believes it is currently driving
  bool videoEnabled() const                                { return m_videoEnabled; }
  int  viewPortWidth()                                     { return m_VGADCtrl.getViewPortWidth(); }
  int  viewPortHeight()                                    { return m_VGADCtrl.getViewPortHeight(); }
  int  egaCharHeight() const                               { return m_egaCharHeight; }
  int  egaTextColumns() const                              { return m_egaTextColumns; }
  uint8_t egaPaletteEntry(int i) const                     { return m_egaPalette[i & 15]; }

  // ESPPC: the state an overlay (the SysReq menu) has to put back exactly as it found it
  uint8_t const * videoBuffer() const                      { return m_videoBuffer; }
  int  cursorRow() const                                   { return m_cursorRow; }
  int  cursorCol() const                                   { return m_cursorCol; }
  int  cursorStart() const                                 { return m_cursorStart; }
  int  cursorEnd() const                                   { return m_cursorEnd; }
  bool cursorVisible() const                               { return m_cursorVisible; }
  bool bit7Blink() const                                   { return m_bit7blink; }
  uint32_t egaStartAddress() const                         { return m_egaStart; }
  // raw VGA byte the renderer would emit for attribute colour i, 0 if the table is missing
  uint16_t egaRawPair(int i) const                         { return m_egaPal2 ? m_egaPal2[(i & 15) * 0x11] : 0; }


private:

  void cleanupFont();

  void freeLUT();
  void setupLUT();

  void setFont(FontInfo const * font);

  void createCursorGlyph(int width, int height, int start, int end);

  void setupEmulation(Emulation emulation);

  static void drawScanline_PC_Text_80x25_16Colors(void * arg, uint8_t * dest, int scanLine);
  static void drawScanline_PC_Text_40x25_16Colors(void * arg, uint8_t * dest, int scanLine);
  static void drawScanline_PC_Graphics_320x200_4Colors(void * arg, uint8_t * dest, int scanLine);
  static void drawScanline_PC_Graphics_640x200_2Colors(void * arg, uint8_t * dest, int scanLine);
  static void drawScanline_EGA_640_16Colors(void * arg, uint8_t * dest, int scanLine);
  static void drawScanline_EGA_320_16Colors(void * arg, uint8_t * dest, int scanLine);
  static void drawScanline_EGA_Text_80x25(void * arg, uint8_t * dest, int scanLine);
  static void drawScanlineWithLED(void * arg, uint8_t * dest, int scanLine);
  void installScanlineCallback();
  static void drawScanline_Tandy_320x200_16Colors(void * arg, uint8_t * dest, int scanLine);
  static void drawScanline_Tandy_160x200_16Colors(void * arg, uint8_t * dest, int scanLine);
  static void drawScanline_Plantronics_320x200_16Colors(void * arg, uint8_t * dest, int scanLine);
  static void drawScanline_Plantronics_640x200_4Colors(void * arg, uint8_t * dest, int scanLine);

  void setupEGALUT();
  void freeEGALUT();


  VGADirectController m_VGADCtrl;
  Emulation           m_emulation;
  uint8_t const *     m_videoBuffer;

  uint8_t *           m_rawLUT;

  uint32_t            m_frameCounter;

  // text mode parameters
  FontInfo            m_font;
  int16_t             m_columns;
  int16_t             m_rows;
  int16_t             m_cursorRow;
  int16_t             m_cursorCol;
  uint8_t             m_cursorStart;  // cursor shape scanline start
  uint8_t             m_cursorEnd;    // cursor shape scanline end
  bool                m_cursorVisible;
  uint8_t *           m_cursorGlyph;
  bool                m_fontInFlash;  // m_font.data is the original, not a RAM copy (see setFont())
  bool                m_bit7blink;

  // PC graphics parameters
  int8_t              m_PCGraphicsBackgroundColorIndex; // used as background color index on 320x200
  int8_t              m_PCGraphicsForegroundColorIndex; // used as foreground color index on 640x200
  int8_t              m_PCGraphicsPaletteInUse;         // 0 = palette 0 low intensity, 1 = palette 0 high intensity, 2 = palette 1 low intensity, 3 = palette 2 high intensity

  bool                m_videoEnabled;

  // --- ESPPC: EGA planar mode state ---
  uint32_t * const *  m_egaBanks;      // four banks of EGA_BANK_ENTRIES uint32, planes interleaved
  int32_t             m_egaStride;     // byte-columns per scanline
  uint32_t            m_egaBankMask;   // banks - 1; video memory is sized to the mode
  uint32_t            m_egaOffsetMask; // banks * EGA_BANK_ENTRIES - 1
  uint32_t            m_egaStart;      // first byte-column of the displayed page
  int32_t             m_egaLineCompare;// split screen: lines after this one start from address 0
  int16_t             m_egaCharHeight; // scanlines per character in EGA text
  int16_t             m_egaTextColumns;
  uint8_t             m_egaPalette[16];
  uint32_t *          m_egaSpread[4];  // plane byte -> eight packed 4 bit pixel indices
  uint16_t *          m_egaPal2;       // two packed indices -> two raw VGA pixel bytes

  // --- ESPPC: Tandy mode state ---
  uint8_t             m_tandyPalette[16];
  uint8_t             m_tandyMask;

  // --- ESPPC: composite ---
  int8_t              m_compositeTable;

  // --- ESPPC: drive LED ---
  DrawScanlineCallback m_modeCallback;     // the mode's own renderer, called by drawScanlineWithLED
  int8_t              m_modeLines;         // its scanlines per call
  int8_t              m_driveLEDMode;
  uint8_t             m_ledRawOn;
  uint8_t             m_ledRawOff;
  volatile int64_t    m_ledUntil[4];       // esp_timer time until which each drive's LED is lit

};



};  // fabgl namespace
