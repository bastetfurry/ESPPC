/*
  MM58174: the National Semiconductor MM58174A real time clock, for machines with "clock mm58174".

  Local to this project -- not part of FabGL.
 */


#pragma once


#include <stdint.h>
#include <time.h>


// National MM58174A, a clock chip with sixteen 4-bit registers, here at 70h-7Fh in place of the AT's
// MC146818. It is the clock SCO Xenix 86 reads through its rtc driver (/dev/clock); DOS and the
// BIOS do not know it, they expect the AT clock.
//
//   0  test (write only)              8  units of days
//   1  tenths of seconds              9  tens of days
//   2  units of seconds               A  day of the week, 1-7
//   3  tens of seconds                B  units of months
//   4  units of minutes               C  tens of months
//   5  tens of minutes                D  leap year (write only)
//   6  units of hours (24 hour)       E  start/stop: bit 0 = 1 runs the clock
//   7  tens of hours                  F  interrupt select (write), status (read)
//
// The time is the host's, plus whatever offset the guest has set. The chip counts no years. Xenix
// keeps its own year in the three low bits of register F as an offset from 1984, so it can only
// see 1984 to 1991. The bits read back as written; at power on they hold the year in that range
// whose calendar matches this one (same weekday on 1 January, same leap year), so day, month and
// weekday are right. The chip's interrupt is not wired.
//
// Battery backed: it keeps running, and keeps what was set, across a reset of the machine.
class MM58174 {

public:

  // power on
  void init();

  void write(int reg, uint8_t value);
  uint8_t read(int reg);

private:

  void current(tm * t, int * tenths);
  void store(tm * t);

  bool     m_running;
  int32_t  m_offset;      // seconds added to the host time
  tm       m_stopped;     // the registers while stopped, where writes go until the clock starts
  uint8_t  m_regF;        // interrupt select as written; Xenix's year in bits 2-0
};
