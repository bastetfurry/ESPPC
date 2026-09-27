/*
  AdLib: a Yamaha YM3812 (OPL2) FM synthesiser at ports 388h/389h.

  Local to this project -- not part of FabGL.

  The synthesis is DBOPL (src/opl, from DOSBox-X) and runs in its own task at the lowest priority on
  the video core, never on the 8086's core and never in an interrupt: the video always comes first,
  and a sound that falls behind only holds its last sample for a moment. The guest's register
  writes go into a queue; the task applies them between blocks of 64 samples (4 ms), fills a ring
  buffer a few blocks ahead, and a FabGL waveform generator plays the ring. FabGL's output rate is
  16384 Hz, the rate the chip is synthesised at, so there is no resampling.

  The status port and the two timers are emulated on the 8086's side, from host time: that is all
  games look at to detect the card (start timer 1, wait, see bits 7 and 6 of the status set).
 */


#pragma once


#include "fabgl.h"


namespace DBOPL { struct Chip; }


class AdLib {

public:

  AdLib();

  // starts the synthesiser task on the given core and attaches the voice; call once
  bool begin(fabgl::SoundGenerator * soundGen, int core);

  // silences the chip (all registers back to 0), as a power on does
  void reset();

  // the guest's port accesses
  void writeAddress(uint8_t value)     { m_address = value; }
  void writeData(uint8_t value);
  uint8_t readStatus();

private:

  static constexpr int RATE      = 16384;       // FabGL's output rate
  static constexpr int BLOCK     = 64;          // samples per synthesis step, 4 ms
  static constexpr int RINGSIZE  = 1024;        // samples, power of two; 62 ms
  static constexpr int AHEAD     = 512;         // keep this much queued, 31 ms
  static constexpr int QUEUESIZE = 256;         // register writes, power of two; plenty per block

  class Voice : public fabgl::WaveformGenerator {
  public:
    Voice() : ring(), head(0), tail(0), m_last(0) { }
    void setFrequency(int value)       { (void) value; }
    int getSample();
    int8_t            ring[RINGSIZE];
    volatile uint32_t head;                     // written by the synth task
    volatile uint32_t tail;                     // written by the sound interrupt
  private:
    int               m_last;
  };

  static void synthTask(void * arg);
  bool     m_ready;           // the chip is set up and the task running
  void synthLoop();
  void queueWrite(uint16_t regval);

  Voice             m_voice;
  DBOPL::Chip *     m_chip;                     // in the synth core's PSRAM half, see synthLoop()
  int               m_core;
  TaskHandle_t      m_task;

  // register writes on their way from the 8086 to the synth task (single producer, single consumer)
  uint16_t          m_queue[QUEUESIZE];
  volatile uint32_t m_queueHead;
  volatile uint32_t m_queueTail;

  uint8_t           m_address;

  // timers (registers 02h, 03h, 04h), worked out from host time when the status is read
  uint8_t           m_timer1Count;
  uint8_t           m_timer2Count;
  uint8_t           m_timerControl;
  int64_t           m_timer1Start;
  int64_t           m_timer2Start;
  uint8_t           m_statusFlags;              // bits 6 and 5 latched until reset through 04h
};
