/*
  PCEmulator - text mode user interface

  The host side screens (power-on, machine selection, Wi-Fi, downloads) drawn as an 80x25 text
  screen on the emulator's own CGA text renderer, in the style of a classic Award BIOS setup.

  This replaces FabGL's graphical InputBox, which needed a 30-50 KB frame buffer of its own and
  kept about 16 KB of drawing code in IRAM. Here the screen is 4000 bytes of character/attribute
  cells, the same layout the guest's text mode uses, and input is keyboard only.
*/


#pragma once


#include <stdint.h>
#include <functional>

#include "fabgl.h"
#include "emudevs/graphicsadapter.h"


// colour attributes, CGA order: high nibble background, low nibble foreground
enum : uint8_t {
  TUI_SCREEN      = 0x1F,   // white on blue: setup screen body
  TUI_LABEL       = 0x1E,   // yellow on blue: item names
  TUI_VALUE       = 0x1F,   // white on blue: values
  TUI_DIM         = 0x17,   // grey on blue: help text, frames
  TUI_HILITE      = 0x4F,   // white on red: the selection bar
  TUI_HILITE_DIM  = 0x4E,   // yellow on red
  TUI_DIALOG      = 0x4F,   // white on red: pop up boxes
  TUI_DIALOG_HEAD = 0x4E,   // yellow on red: pop up titles
  TUI_DIALOG_SEL  = 0x1F,   // white on blue: selected item inside a pop up
  TUI_DIALOG_FLD  = 0x70,   // black on grey: input fields
  TUI_POST        = 0x07,   // grey on black: POST screen
  TUI_POST_BRIGHT = 0x0F,   // white on black
  TUI_POST_OK     = 0x0A,   // green on black
  TUI_POST_FAIL   = 0x0C,   // red on black
  TUI_POST_NOTE   = 0x0E,   // yellow on black
};


class TextUI {

public:

  // key as the screens see it: the virtual key plus its ASCII translation (0 if none)
  struct Key {
    fabgl::VirtualKey vk;
    uint8_t           ascii;
  };

  // one step of a progress box; update() returns false once the user has pressed Esc on an
  // abortable box, and the work is expected to stop
  class Progress {
  public:
    bool update(int percent, char const * format, ...);
  private:
    friend class TextUI;
    TextUI * m_ui;
    int      m_x, m_y, m_w;
    bool     m_abortable;
    bool     m_aborted;
  };

  TextUI();
  ~TextUI();

  // takes over the adapter with a text screen of its own; end() gives it back unconfigured, so
  // the machine can set whatever mode it needs from scratch
  bool begin(fabgl::GraphicsAdapter * adapter);
  void end();

  // step aside for something that drives the VGA output itself (the remaining FabGL dialogs), and
  // come back afterwards with the screen exactly as it was
  void suspend();
  void resume();

  // --- drawing ---

  void clear(uint8_t attr);
  void put(int x, int y, uint8_t ch, uint8_t attr);
  void setAttr(int x, int y, uint8_t attr);
  void fill(int x, int y, int w, int h, uint8_t ch, uint8_t attr);
  // text clipped to width (and padded with spaces to it) when width > 0
  void text(int x, int y, char const * s, uint8_t attr, int width = 0);
  void textf(int x, int y, uint8_t attr, char const * format, ...);
  void centre(int y, char const * s, uint8_t attr);
  // frame with single or double lines; the inside is left alone
  void frame(int x, int y, int w, int h, uint8_t attr, bool doubleLine);
  // frame with a filled inside and a drop shadow, as every pop up has
  void box(int x, int y, int w, int h, uint8_t attr, bool doubleLine = true);
  void shadow(int x, int y, int w, int h);

  void showCursor(int x, int y);
  void hideCursor();

  // save / restore the whole screen, for pop ups over something that must come back
  void save(uint8_t * dest);
  void restore(uint8_t const * src);
  static constexpr int SCREENBYTES = 80 * 25 * 2;

  // --- input ---

  // next key press, or false after timeoutMS (-1 waits for ever)
  bool getKey(Key * key, int timeoutMS = -1);
  void flushKeys();

  // --- dialogs, all drawn over whatever is on screen and restoring it afterwards ---

  // lines separated by '\n'; buttons separated by ';' (nullptr = none, the box just stays up).
  // Returns the button index, -1 for Esc. The last button is the default. autoSeconds > 0
  // presses the default after that long.
  int  message(char const * title, char const * text, char const * buttons = "OK", int autoSeconds = 0);

  // item list; returns the chosen index or -1 for Esc
  int  menu(char const * title, char const * prompt, char const * const * items, int count, int selected = 0);

  // returns true for Enter, false for Esc; buf holds the text either way
  bool textInput(char const * title, char const * prompt, char * buf, int maxLength, bool password = false);

  // runs work() with a progress box up; the box goes away when work() returns
  void progress(char const * title, bool abortable, std::function<void(Progress &)> work);

  // --- the POST style log screen used while the host starts up ---

  void postBegin();
  // starts a line "label ....", finished later by postResult()
  void postItem(char const * label);
  void postResult(char const * result, uint8_t attr);
  void postLine(char const * s, uint8_t attr = TUI_POST);
  int  postRow() const                         { return m_postRow; }
  void postFooter(char const * s);

private:

  struct SavedScreen;

  fabgl::GraphicsAdapter * m_adapter;
  uint8_t *                m_cells;
  bool                     m_active;
  int                      m_postRow;
  int                      m_postCol;
};
