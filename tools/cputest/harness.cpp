// Host side wrapper around FabGL's i8086 core for the CPU test harness (tools/cputest/run.py):
// plain memory (the video window moved out of reach), ports reading FFh, interrupts left to the
// core, and a C API that Python's ctypes can drive.

#include <stdlib.h>
#include <string.h>
#include "i8086.h"

using fabgl::i8086;

static uint8_t * mem;
static uint8_t  lastPortOut;

static void    wPort(void *, int, uint8_t v)   { lastPortOut = v; }
static uint8_t rPort(void *, int)              { return 0xff; }
static void    wV8(void *, int, uint8_t)       { }
static void    wV16(void *, int, uint16_t)     { }
static uint8_t rV8(void *, int)                { return 0xff; }
static uint16_t rV16(void *, int)              { return 0xffff; }
static bool    intr(void *, int)               { return false; }

extern "C" {

// 2 MB: the core does not wrap addresses at 1 MB the way the 8088 does, so it must not run off the end
void h_init()
{
  mem = (uint8_t *) calloc(1, 2 * 1024 * 1024);
  i8086::setMemory(mem);
  i8086::setCallbacks(nullptr, rPort, wPort, wV8, wV16, rV8, rV16, intr);
  i8086::setVideoMemStart(0x7fffffff);
  i8086::reset();
}

void h_clear()                         { memset(mem, 0, 2 * 1024 * 1024); }

// before every test: prefix counters, halt state and the like must not carry over from the last one
void h_reset()                         { i8086::reset(); }
void h_poke(uint32_t a, uint8_t v)     { mem[a] = v; }
uint8_t h_peek(uint32_t a)             { return mem[a]; }

// registers in the tests' order: ax bx cx dx cs ss ds es sp bp si di ip flags
static const int IDX[12] = { 0, 3, 1, 2, 9, 10, 11, 8, 4, 5, 6, 7 };

void h_set(int r, uint16_t v)
{
  if (r < 12)
    ((uint16_t *) i8086::s_regs)[IDX[r]] = v;
  else if (r == 12)
    i8086::setIP(v);
  else {
    uint8_t * f = i8086::s_flags;
    f[0] = !!(v & 0x001); f[1] = !!(v & 0x004); f[2] = !!(v & 0x010); f[3] = !!(v & 0x040);
    f[4] = !!(v & 0x080); f[5] = !!(v & 0x100); f[6] = !!(v & 0x200); f[7] = !!(v & 0x400);
    f[8] = !!(v & 0x800);
  }
}

uint16_t h_get(int r)
{
  if (r < 12)
    return ((uint16_t *) i8086::s_regs)[IDX[r]];
  if (r == 12)
    return i8086::IP();
  uint8_t * f = i8086::s_flags;
  return 0xf002 | f[0] << 0 | f[1] << 2 | f[2] << 4 | f[3] << 6 | f[4] << 7 | f[5] << 8 | f[6] << 9 |
         f[7] << 10 | f[8] << 11;
}

// One instruction: the core treats a prefix as a step of its own and repeats a REP string
// instruction one iteration per step with IP left on it, so keep stepping while IP is still inside
// the instruction's bytes. maxSteps bounds loops like a jump to itself.
int h_run(int length, int maxSteps)
{
  uint32_t start = i8086::IP();
  uint16_t cs    = ((uint16_t *) i8086::s_regs)[9];
  int n = 0;
  do {
    i8086::step();
    ++n;
  } while (n < maxSteps && ((uint16_t *) i8086::s_regs)[9] == cs &&
           (uint16_t)(i8086::IP() - start) < (uint16_t) length && !i8086::halted());
  return n;
}

}
