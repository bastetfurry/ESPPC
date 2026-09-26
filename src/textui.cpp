/*
  PCEmulator - text mode user interface, see textui.h
*/


#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "esp_timer.h"

#include "esp_heap_caps.h"

#include "textui.h"


using fabgl::VirtualKey;
using fabgl::GraphicsAdapter;


// CP437 line drawing
namespace {
  struct FrameChars { uint8_t tl, tr, bl, br, h, v; };
  constexpr FrameChars SINGLE = { 0xDA, 0xBF, 0xC0, 0xD9, 0xC4, 0xB3 };
  constexpr FrameChars DOUBLE = { 0xC9, 0xBB, 0xC8, 0xBC, 0xCD, 0xBA };

  constexpr int COLS = 80;
  constexpr int ROWS = 25;

  constexpr uint8_t BAR_FULL  = 0xDB;
  constexpr uint8_t BAR_EMPTY = 0xB0;

  bool isEnter(VirtualKey vk)   { return vk == VirtualKey::VK_RETURN || vk == VirtualKey::VK_KP_ENTER; }
}


// What a pop up covers, put back when it closes. On the heap: setup() runs on the Arduino loop
// task's 8 KB stack, and a message box over a progress box would otherwise hold 8 KB of copies.
struct TextUI::SavedScreen {
  TextUI *  ui;
  uint8_t * cells;
  SavedScreen(TextUI * owner) : ui(owner), cells((uint8_t*) malloc(TextUI::SCREENBYTES)) {
    if (cells)
      ui->save(cells);
  }
  ~SavedScreen() {
    if (cells) {
      ui->restore(cells);
      free(cells);
    }
  }
  // leave what is on screen now in place
  void keep() {
    free(cells);
    cells = nullptr;
  }
};


TextUI::TextUI()
  : m_adapter(nullptr),
    m_cells(nullptr),
    m_active(false),
    m_postRow(0),
    m_postCol(0)
{
}


TextUI::~TextUI()
{
  end();
}


bool TextUI::begin(GraphicsAdapter * adapter)
{
  m_adapter = adapter;

  // read by the scanline interrupt, so it has to be internal RAM
  if (!m_cells)
    m_cells = (uint8_t*) heap_caps_malloc(SCREENBYTES, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!m_cells)
    return false;
  clear(TUI_POST);

  // Keyboard: virtual keys with a queue, which is what getKey() reads. The emulated 8042 takes the
  // same controller over later and switches virtual keys off again (i8042::init()).
  if (!fabgl::PS2Controller::initialized())
    fabgl::PS2Controller::begin(fabgl::PS2Preset::KeyboardPort0_MousePort1, fabgl::KbdMode::CreateVirtualKeysQueue);
  else
    fabgl::PS2Controller::keyboard()->enableVirtualKeys(true, true);

  resume();
  return true;
}


void TextUI::end()
{
  if (!m_adapter)
    return;
  suspend();
  // Hand the adapter back with no emulation selected, the state a fresh one is in, so the
  // machine's first mode set configures everything itself.
  m_adapter->setEmulation(GraphicsAdapter::Emulation::None);
  m_adapter->setCursorVisible(false);
  // The emulated 8042 turns virtual keys off anyway (i8042::init()), but from the emulator task,
  // where the deleted converter task's stack is only reclaimed once the idle task gets to run.
  // Doing it here, on the setup task, gives the memory back straight away.
  if (fabgl::PS2Controller::keyboard())
    fabgl::PS2Controller::keyboard()->enableVirtualKeys(false, false);
  vTaskDelay(20 / portTICK_PERIOD_MS);
  heap_caps_free(m_cells);
  m_cells   = nullptr;
  m_adapter = nullptr;
}


void TextUI::suspend()
{
  if (m_adapter && m_active) {
    m_adapter->enableVideo(false);
    m_active = false;
  }
}


void TextUI::resume()
{
  if (!m_adapter || m_active)
    return;
  m_adapter->setEmulation(GraphicsAdapter::Emulation::PC_Text_80x25_16Colors);
  m_adapter->setVideoBuffer(m_cells);
  m_adapter->setBit7Blink(false);          // all sixteen background colours
  m_adapter->setCursorVisible(false);
  m_adapter->enableVideo(true);
  // whatever ran in between may have switched them off (FabGL's InputBox does on its way out)
  if (fabgl::PS2Controller::keyboard())
    fabgl::PS2Controller::keyboard()->enableVirtualKeys(true, true);
  flushKeys();
  m_active = true;
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// drawing


void TextUI::clear(uint8_t attr)
{
  fill(0, 0, COLS, ROWS, ' ', attr);
}


void TextUI::put(int x, int y, uint8_t ch, uint8_t attr)
{
  if (x < 0 || y < 0 || x >= COLS || y >= ROWS)
    return;
  uint8_t * c = m_cells + (y * COLS + x) * 2;
  c[0] = ch;
  c[1] = attr;
}


void TextUI::setAttr(int x, int y, uint8_t attr)
{
  if (x < 0 || y < 0 || x >= COLS || y >= ROWS)
    return;
  m_cells[(y * COLS + x) * 2 + 1] = attr;
}


void TextUI::fill(int x, int y, int w, int h, uint8_t ch, uint8_t attr)
{
  for (int r = y; r < y + h; ++r)
    for (int c = x; c < x + w; ++c)
      put(c, r, ch, attr);
}


void TextUI::text(int x, int y, char const * s, uint8_t attr, int width)
{
  int n = 0;
  for (; s && *s && (width <= 0 || n < width); ++s, ++n)
    put(x + n, y, (uint8_t) *s, attr);
  for (; n < width; ++n)
    put(x + n, y, ' ', attr);
}


void TextUI::textf(int x, int y, uint8_t attr, char const * format, ...)
{
  char buf[COLS + 1];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  text(x, y, buf, attr);
}


void TextUI::centre(int y, char const * s, uint8_t attr)
{
  text((COLS - (int) strlen(s)) / 2, y, s, attr);
}


void TextUI::frame(int x, int y, int w, int h, uint8_t attr, bool doubleLine)
{
  FrameChars const & f = doubleLine ? DOUBLE : SINGLE;
  put(x, y, f.tl, attr);
  put(x + w - 1, y, f.tr, attr);
  put(x, y + h - 1, f.bl, attr);
  put(x + w - 1, y + h - 1, f.br, attr);
  for (int c = x + 1; c < x + w - 1; ++c) {
    put(c, y, f.h, attr);
    put(c, y + h - 1, f.h, attr);
  }
  for (int r = y + 1; r < y + h - 1; ++r) {
    put(x, r, f.v, attr);
    put(x + w - 1, r, f.v, attr);
  }
}


void TextUI::box(int x, int y, int w, int h, uint8_t attr, bool doubleLine)
{
  fill(x, y, w, h, ' ', attr);
  frame(x, y, w, h, attr, doubleLine);
  shadow(x, y, w, h);
}


// the shadow keeps the characters underneath and darkens them, as BIOS setups do
void TextUI::shadow(int x, int y, int w, int h)
{
  for (int r = y + 1; r <= y + h; ++r)
    for (int c = x + w; c < x + w + 2; ++c)
      setAttr(c, r, 0x08);
  for (int c = x + 2; c < x + w; ++c)
    setAttr(c, y + h, 0x08);
}


void TextUI::showCursor(int x, int y)
{
  m_adapter->setCursorPos(y, x);
  m_adapter->setCursorVisible(true);
}


void TextUI::hideCursor()
{
  m_adapter->setCursorVisible(false);
}


void TextUI::save(uint8_t * dest)
{
  memcpy(dest, m_cells, SCREENBYTES);
}


void TextUI::restore(uint8_t const * src)
{
  memcpy(m_cells, src, SCREENBYTES);
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// input


bool TextUI::getKey(Key * key, int timeoutMS)
{
  auto kbd = fabgl::PS2Controller::keyboard();
  if (!kbd)
    return false;
  int64_t until = timeoutMS < 0 ? -1 : esp_timer_get_time() + (int64_t) timeoutMS * 1000;
  fabgl::VirtualKeyItem item;
  while (true) {
    int wait = -1;
    if (until >= 0) {
      int64_t left = until - esp_timer_get_time();
      if (left <= 0)
        return false;
      wait = (int)(left / 1000) + 1;
    }
    if (!kbd->getNextVirtualKey(&item, wait))
      continue;                       // timed out: the loop above decides
    if (!item.down)
      continue;                       // presses only
    key->vk    = item.vk;
    key->ascii = item.ASCII;
    return true;
  }
}


void TextUI::flushKeys()
{
  auto kbd = fabgl::PS2Controller::keyboard();
  if (kbd)
    kbd->emptyVirtualKeyQueue();
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// dialogs


namespace {

  // splits "a;b;c" into pointers into a copy; returns the count
  int splitList(char * s, char sep, char const ** out, int max)
  {
    int n = 0;
    while (s && *s && n < max) {
      out[n++] = s;
      char * e = strchr(s, sep);
      if (!e)
        break;
      *e = 0;
      s  = e + 1;
    }
    return n;
  }

}


int TextUI::message(char const * title, char const * msg, char const * buttons, int autoSeconds)
{
  SavedScreen saved(this);

  char textCopy[400];
  strlcpy(textCopy, msg ? msg : "", sizeof(textCopy));
  char const * lines[12];
  int lineCount = splitList(textCopy, '\n', lines, 12);

  char buttonCopy[80];
  strlcpy(buttonCopy, buttons ? buttons : "", sizeof(buttonCopy));
  char const * btn[4];
  int btnCount = buttons ? splitList(buttonCopy, ';', btn, 4) : 0;

  // size to the content
  int w = title ? (int) strlen(title) + 6 : 20;
  for (int i = 0; i < lineCount; ++i)
    w = fabgl::imax(w, (int) strlen(lines[i]) + 6);
  int btnWidth = 0;
  for (int i = 0; i < btnCount; ++i)
    btnWidth += strlen(btn[i]) + 4 + 2;
  w = fabgl::imin(fabgl::imax(w, btnWidth + 4), COLS - 4);
  int h = lineCount + (btnCount ? 5 : 3);
  int x = (COLS - w) / 2;
  int y = (ROWS - h) / 2;

  box(x, y, w, h, TUI_DIALOG);
  if (title) {
    char t[COLS];
    snprintf(t, sizeof(t), " %s ", title);
    text(x + (w - (int) strlen(t)) / 2, y, t, TUI_DIALOG_HEAD);
  }
  for (int i = 0; i < lineCount; ++i)
    text(x + (w - (int) strlen(lines[i])) / 2, y + 1 + i + (btnCount ? 1 : 0), lines[i], TUI_DIALOG);

  int sel = btnCount - 1;       // the last button is the default, as with FabGL's InputBox
  int result = -1;

  if (btnCount == 0) {
    // informational: stays up, the caller carries on or never returns
    saved.keep();
    return -1;
  }

  auto drawButtons = [&]() {
    int bx = x + (w - btnWidth + 2) / 2;
    for (int i = 0; i < btnCount; ++i) {
      uint8_t a = (i == sel) ? TUI_DIALOG_SEL : TUI_DIALOG;
      textf(bx, y + h - 2, a, "[ %s ]", btn[i]);
      bx += strlen(btn[i]) + 4 + 2;
    }
  };

  int64_t deadline = autoSeconds > 0 ? esp_timer_get_time() + (int64_t) autoSeconds * 1000000 : -1;
  while (true) {
    drawButtons();
    Key k;
    int wait = -1;
    if (deadline >= 0) {
      int64_t left = deadline - esp_timer_get_time();
      if (left <= 0) {
        result = btnCount - 1;
        break;
      }
      wait = (int)(left / 1000) + 1;
    }
    if (!getKey(&k, wait))
      continue;
    deadline = -1;              // any key stops the automatic answer
    if (k.vk == VirtualKey::VK_LEFT || k.vk == VirtualKey::VK_KP_LEFT)
      sel = (sel + btnCount - 1) % btnCount;
    else if (k.vk == VirtualKey::VK_RIGHT || k.vk == VirtualKey::VK_KP_RIGHT || k.vk == VirtualKey::VK_TAB)
      sel = (sel + 1) % btnCount;
    else if (isEnter(k.vk)) {
      result = sel;
      break;
    } else if (k.vk == VirtualKey::VK_ESCAPE) {
      result = -1;
      break;
    } else if (k.ascii) {
      // first letter of a button chooses it, like Y/N in a BIOS
      for (int i = 0; i < btnCount; ++i)
        if (toupper(btn[i][0]) == toupper(k.ascii)) {
          sel = result = i;
          goto done;
        }
    }
  }
done:
  return result;
}


int TextUI::menu(char const * title, char const * prompt, char const * const * items, int count, int selected)
{
  if (count <= 0)
    return -1;

  SavedScreen saved(this);

  int w = fabgl::imax(title ? (int) strlen(title) + 6 : 0, prompt ? (int) strlen(prompt) + 4 : 0);
  for (int i = 0; i < count; ++i)
    w = fabgl::imax(w, (int) strlen(items[i]) + 6);
  w = fabgl::imin(w, COLS - 4);
  int visible = fabgl::imin(count, 14);
  int top     = prompt ? 2 : 1;
  int h       = visible + top + 2;
  int x       = (COLS - w) / 2;
  int y       = (ROWS - h) / 2;

  box(x, y, w, h, TUI_DIALOG);
  if (title) {
    char t[COLS];
    snprintf(t, sizeof(t), " %s ", title);
    text(x + (w - (int) strlen(t)) / 2, y, t, TUI_DIALOG_HEAD);
  }
  if (prompt)
    text(x + 2, y + 1, prompt, TUI_DIALOG);
  text(x + 2, y + h - 1, " \x18\x19:Move  Enter:Select  Esc:Cancel ", TUI_DIALOG_HEAD);

  int sel    = fabgl::imax(0, fabgl::imin(selected, count - 1));
  int scroll = fabgl::imax(0, sel - visible + 1);
  int result = -1;

  while (true) {
    for (int i = 0; i < visible; ++i) {
      int idx = scroll + i;
      uint8_t a = (idx == sel) ? TUI_DIALOG_SEL : TUI_DIALOG;
      text(x + 2, y + top + i, "", a, w - 4);
      text(x + 3, y + top + i, items[idx], a, w - 6);
    }
    // scroll hints
    put(x + w - 1, y + top, scroll > 0 ? 0x1E : 0xBA, TUI_DIALOG);
    put(x + w - 1, y + top + visible - 1, scroll + visible < count ? 0x1F : 0xBA, TUI_DIALOG);

    Key k;
    getKey(&k);
    switch (k.vk) {
      case VirtualKey::VK_UP:       case VirtualKey::VK_KP_UP:       sel = fabgl::imax(0, sel - 1); break;
      case VirtualKey::VK_DOWN:     case VirtualKey::VK_KP_DOWN:     sel = fabgl::imin(count - 1, sel + 1); break;
      case VirtualKey::VK_PAGEUP:   case VirtualKey::VK_KP_PAGEUP:   sel = fabgl::imax(0, sel - visible); break;
      case VirtualKey::VK_PAGEDOWN: case VirtualKey::VK_KP_PAGEDOWN: sel = fabgl::imin(count - 1, sel + visible); break;
      case VirtualKey::VK_HOME:     case VirtualKey::VK_KP_HOME:     sel = 0; break;
      case VirtualKey::VK_END:      case VirtualKey::VK_KP_END:      sel = count - 1; break;
      case VirtualKey::VK_ESCAPE:
              return -1;
      default:
        if (isEnter(k.vk)) {
          result = sel;
                  return result;
        }
        break;
    }
    if (sel < scroll)
      scroll = sel;
    else if (sel >= scroll + visible)
      scroll = sel - visible + 1;
  }
}


bool TextUI::textInput(char const * title, char const * prompt, char * buf, int maxLength, bool password)
{
  SavedScreen saved(this);

  int fieldWidth = fabgl::imin(fabgl::imax(maxLength + 1, 20), COLS - 10);
  int w = fabgl::imax(fieldWidth + 6, prompt ? (int) strlen(prompt) + 6 : 0);
  w = fabgl::imax(w, title ? (int) strlen(title) + 6 : 0);
  int h = 6;
  int x = (COLS - w) / 2;
  int y = (ROWS - h) / 2;

  box(x, y, w, h, TUI_DIALOG);
  if (title) {
    char t[COLS];
    snprintf(t, sizeof(t), " %s ", title);
    text(x + (w - (int) strlen(t)) / 2, y, t, TUI_DIALOG_HEAD);
  }
  if (prompt)
    text(x + 3, y + 1, prompt, TUI_DIALOG);
  text(x + 2, y + h - 1, " Enter:OK  Esc:Cancel ", TUI_DIALOG_HEAD);

  int fx  = x + 3;
  int fy  = y + 3;
  int len = strlen(buf);
  bool ok = false;

  while (true) {
    // the field scrolls when the text is wider than it
    int first = fabgl::imax(0, len - (fieldWidth - 1));
    for (int i = 0; i < fieldWidth; ++i) {
      int idx = first + i;
      uint8_t ch = idx < len ? (password ? '*' : (uint8_t) buf[idx]) : ' ';
      put(fx + i, fy, ch, TUI_DIALOG_FLD);
    }
    showCursor(fx + len - first, fy);

    Key k;
    getKey(&k);
    if (isEnter(k.vk)) {
      ok = true;
      break;
    } else if (k.vk == VirtualKey::VK_ESCAPE) {
      break;
    } else if (k.vk == VirtualKey::VK_BACKSPACE) {
      if (len > 0)
        buf[--len] = 0;
    } else if (k.ascii >= 0x20 && k.ascii < 0x7f && len < maxLength) {
      buf[len++] = k.ascii;
      buf[len]   = 0;
    }
  }

  hideCursor();
  return ok;
}


void TextUI::progress(char const * title, bool abortable, std::function<void(Progress &)> work)
{
  SavedScreen saved(this);

  int w = 64;
  int h = 6;
  int x = (COLS - w) / 2;
  int y = (ROWS - h) / 2;

  box(x, y, w, h, TUI_DIALOG);
  if (title && title[0]) {
    char t[COLS];
    snprintf(t, sizeof(t), " %s ", title);
    text(x + (w - (int) strlen(t)) / 2, y, t, TUI_DIALOG_HEAD);
  }
  if (abortable)
    text(x + 2, y + h - 1, " Esc:Abort ", TUI_DIALOG_HEAD);

  Progress p;
  p.m_ui        = this;
  p.m_x         = x;
  p.m_y         = y;
  p.m_w         = w;
  p.m_abortable = abortable;
  p.m_aborted   = false;
  flushKeys();

  work(p);

}


bool TextUI::Progress::update(int percent, char const * format, ...)
{
  char line[COLS];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof(line), format, ap);
  va_end(ap);

  m_ui->text(m_x + 3, m_y + 2, line, TUI_DIALOG, m_w - 6);

  int barWidth = m_w - 6;
  int filled   = fabgl::imax(0, fabgl::imin(barWidth, percent * barWidth / 100));
  for (int i = 0; i < barWidth; ++i)
    m_ui->put(m_x + 3 + i, m_y + 3, i < filled ? BAR_FULL : BAR_EMPTY, TUI_DIALOG);

  if (m_abortable && !m_aborted) {
    Key k;
    while (m_ui->getKey(&k, 0))
      if (k.vk == VirtualKey::VK_ESCAPE)
        m_aborted = true;
  }
  return !m_aborted;
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// POST screen


void TextUI::postBegin()
{
  clear(TUI_POST);
  hideCursor();
  // the banner block, where a BIOS puts its name and the vendor's
  text(0, 0, "ESP32 PC Emulator BIOS", TUI_POST_BRIGHT);
  text(0, 1, "www.fabgl.com - by Fabrizio Di Vittorio", TUI_POST);
  text(0, 2, "Modified by Alexandra Bastet Stehr - lionscade.de", TUI_POST);
  m_postRow = 4;
  m_postCol = 0;
}


void TextUI::postItem(char const * label)
{
  if (m_postRow >= ROWS - 2) {
    // scroll the log part up, keeping the banner and the footer row
    memmove(m_cells + 4 * COLS * 2, m_cells + 5 * COLS * 2, (ROWS - 2 - 5) * COLS * 2);
    fill(0, ROWS - 3, COLS, 1, ' ', TUI_POST);
    m_postRow = ROWS - 3;
  }
  char dots[COLS];
  snprintf(dots, sizeof(dots), "%-34s", label);
  for (char * d = dots + strlen(label); *d; ++d)
    *d = (d - dots) % 2 ? '.' : ' ';
  text(0, m_postRow, dots, TUI_POST);
  m_postCol = 35;
}


void TextUI::postResult(char const * result, uint8_t attr)
{
  text(m_postCol, m_postRow, result, attr, COLS - m_postCol);
  ++m_postRow;
  m_postCol = 0;
}


void TextUI::postLine(char const * s, uint8_t attr)
{
  postItem("");
  text(0, m_postRow, s, attr, COLS);
  ++m_postRow;
  m_postCol = 0;
}


void TextUI::postFooter(char const * s)
{
  text(0, ROWS - 1, s, TUI_POST, COLS);
}
