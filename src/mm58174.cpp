#include "mm58174.h"

#include <sys/time.h>


static bool isLeap(int year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}


// 0 = Sunday
static int weekdayOfJan1(int year)
{
  int y = year - 1;
  return (1 + y + y / 4 - y / 100 + y / 400) % 7;
}


// The year 1984-1991 with the same calendar as this one, or failing that one with the same leap
// status. Xenix reads its year from register F as 1984 plus bits 2-0.
static int equivalentYear(int year)
{
  int fallback = 0;
  for (int y = 1984; y <= 1991; ++y)
    if (isLeap(y) == isLeap(year)) {
      if (weekdayOfJan1(y) == weekdayOfJan1(year))
        return y;
      if (!fallback)
        fallback = y;
    }
  return fallback;
}


void MM58174::init()
{
  m_running = true;
  m_offset  = 0;

  time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);
  m_stopped = t;
  m_regF    = (equivalentYear(1900 + t.tm_year) - 1984) & 7;
}


// the time the registers show
void MM58174::current(tm * t, int * tenths)
{
  if (!m_running) {
    *t      = m_stopped;
    *tenths = 0;
    return;
  }
  timeval tv;
  gettimeofday(&tv, nullptr);
  time_t s = tv.tv_sec + m_offset;
  localtime_r(&s, t);
  *tenths = tv.tv_usec / 100000;
}


// the registers changed to t: held while stopped, otherwise the clock runs on from there
void MM58174::store(tm * t)
{
  if (m_running) {
    t->tm_isdst = -1;
    m_offset = (int32_t) (mktime(t) - time(nullptr));
  } else
    m_stopped = *t;
}


uint8_t MM58174::read(int reg)
{
  tm  t;
  int tenths;
  current(&t, &tenths);

  int v;
  switch (reg & 15) {
    case 0x1: v = tenths;                 break;
    case 0x2: v = t.tm_sec % 10;          break;
    case 0x3: v = t.tm_sec / 10;          break;
    case 0x4: v = t.tm_min % 10;          break;
    case 0x5: v = t.tm_min / 10;          break;
    case 0x6: v = t.tm_hour % 10;         break;
    case 0x7: v = t.tm_hour / 10;         break;
    case 0x8: v = t.tm_mday % 10;         break;
    case 0x9: v = t.tm_mday / 10;         break;
    case 0xA: v = t.tm_wday + 1;          break;
    case 0xB: v = (t.tm_mon + 1) % 10;    break;
    case 0xC: v = (t.tm_mon + 1) / 10;    break;
    case 0xF: v = m_regF & 7;             break;
    default:  return 0xff;                // write only
  }
  // only D3-D0 are driven, the upper half of the bus floats high
  return 0xf0 | v;
}


void MM58174::write(int reg, uint8_t value)
{
  value &= 15;
  reg   &= 15;

  if (reg == 0xE) {
    bool run = value & 1;
    if (run == m_running)
      return;
    if (run) {
      // starts from what the registers hold, at a whole second
      m_running = true;
      store(&m_stopped);
    } else {
      int tenths;
      current(&m_stopped, &tenths);
      m_running = false;
    }
    return;
  }
  if (reg == 0xF) {
    m_regF = value;
    return;
  }

  tm  t;
  int tenths;
  current(&t, &tenths);
  switch (reg) {
    case 0x2: t.tm_sec  = t.tm_sec  / 10 * 10 + value;         break;
    case 0x3: t.tm_sec  = value * 10 + t.tm_sec % 10;          break;
    case 0x4: t.tm_min  = t.tm_min  / 10 * 10 + value;         break;
    case 0x5: t.tm_min  = value * 10 + t.tm_min % 10;          break;
    case 0x6: t.tm_hour = t.tm_hour / 10 * 10 + value;         break;
    case 0x7: t.tm_hour = value * 10 + t.tm_hour % 10;         break;
    case 0x8: t.tm_mday = t.tm_mday / 10 * 10 + value;         break;
    case 0x9: t.tm_mday = value * 10 + t.tm_mday % 10;         break;
    case 0xB: t.tm_mon  = (t.tm_mon + 1) / 10 * 10 + value - 1; break;
    case 0xC: t.tm_mon  = value * 10 + (t.tm_mon + 1) % 10 - 1; break;
    default:  return;   // test, tenths, weekday (follows the date here) and leap year: no effect
  }
  store(&t);
}
