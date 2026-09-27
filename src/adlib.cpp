/*
  AdLib (OPL2) -- see adlib.h
 */


#include "adlib.h"
#include "opl/dbopl.h"

#include "esp_timer.h"
#include "soc/soc.h"

#include <new>


// A register write that tells the synth task to reset the chip rather than write a register.
static constexpr uint16_t RESET_COMMAND = 0xffff;


// In the sound generator's interrupt: internal RAM only. An empty ring (the synth task fell behind,
// the video core being busy) holds the last sample rather than clicking.
int AdLib::Voice::getSample()
{
  if (tail != head) {
    m_last = ring[tail & (RINGSIZE - 1)];
    tail = tail + 1;
  }
  return m_last * volume() / 128;
}


AdLib::AdLib()
  : m_ready(false),
    m_chip(nullptr),
    m_core(1),
    m_task(nullptr),
    m_queue(),
    m_queueHead(0),
    m_queueTail(0),
    m_address(0),
    m_timer1Count(0),
    m_timer2Count(0),
    m_timerControl(0),
    m_timer1Start(0),
    m_timer2Start(0),
    m_statusFlags(0)
{
}


bool AdLib::begin(fabgl::SoundGenerator * soundGen, int core)
{
  m_core = core < 0 ? 1 : core;
  soundGen->attach(&m_voice);
  m_voice.setVolume(100);
  m_voice.enable(true);
  // the lowest priority above idle: the scanline interrupt and everything else come first
  if (xTaskCreatePinnedToCore(synthTask, "adlib", 2560, this, 1, &m_task, m_core) != pdPASS) {
    m_task = nullptr;
    return false;
  }
  return true;
}


void AdLib::reset()
{
  m_address      = 0;
  m_timerControl = 0;
  m_statusFlags  = 0;
  queueWrite(RESET_COMMAND);
}


void AdLib::queueWrite(uint16_t regval)
{
  uint32_t head = m_queueHead;
  if (head - m_queueTail >= QUEUESIZE)
    return;                               // synth task far behind: drop rather than wait
  m_queue[head & (QUEUESIZE - 1)] = regval;
  m_queueHead = head + 1;
}


void AdLib::writeData(uint8_t value)
{
  switch (m_address) {
    case 0x02:
      m_timer1Count = value;
      break;
    case 0x03:
      m_timer2Count = value;
      break;
    case 0x04:
      if (value & 0x80) {
        // IRQ reset: clears the expired flags, touches nothing else
        m_statusFlags = 0;
        return;
      }
      // starting a timer (bit 0 or 1 going from 0 to 1) begins its count now
      if ((value & 0x01) && !(m_timerControl & 0x01))
        m_timer1Start = esp_timer_get_time();
      if ((value & 0x02) && !(m_timerControl & 0x02))
        m_timer2Start = esp_timer_get_time();
      m_timerControl = value;
      return;
  }
  queueWrite(((uint16_t)m_address << 8) | value);
}


// Status: bit 7 = an unmasked timer has expired, bit 6 = timer 1 expired, bit 5 = timer 2 expired.
// Timer 1 counts from its register to 256 in steps of 80 us, timer 2 in steps of 320 us. The low
// bits read 06h on an OPL2, which is what tells it apart from an OPL3 for software that cares.
uint8_t AdLib::readStatus()
{
  int64_t now = esp_timer_get_time();
  if ((m_timerControl & 0x01) && !(m_timerControl & 0x40) &&
      now - m_timer1Start >= (int64_t)(256 - m_timer1Count) * 80)
    m_statusFlags |= 0x40;
  if ((m_timerControl & 0x02) && !(m_timerControl & 0x20) &&
      now - m_timer2Start >= (int64_t)(256 - m_timer2Count) * 320)
    m_statusFlags |= 0x20;
  return (m_statusFlags ? 0x80 : 0x00) | m_statusFlags | 0x06;
}


void AdLib::synthTask(void * arg)
{
  ((AdLib*) arg)->synthLoop();
}


// The synth task: set the chip up (once; the tables are in flash, the rest is some floating point
// for the envelope rates), then keep the ring AHEAD samples full, a block at a time, applying the
// queued register writes before each block. With the ring full it sleeps a tick, which is what
// leaves the core to everything else.
//
// The chip's state (4.4 KB, touched on every sample) lives in PSRAM rather than internal RAM, which
// is too scarce for it: with it there, the SysReq menu's text mode could no longer allocate its video
// buffers and the machine hung. The ESP32's PSRAM workaround gives each core its own 2 MB half; guest
// RAM is in the emulator core's half and nothing uses the video core's, so the chip goes to the start
// of that one -- and is constructed here, in the task, so that only this core ever touches it. Its
// 4.4 KB stay in this core's cache anyway.
void AdLib::synthLoop()
{
  void * home = (void *)(SOC_EXTRAM_DATA_LOW + (m_core == 1 ? 2 * 1024 * 1024 : 0));
  m_chip = new (home) DBOPL::Chip(false);     // OPL2
  DBOPL::InitTables();
  m_chip->Setup(RATE);
  m_ready = true;

  static int32_t buf[BLOCK];

  while (true) {

    if (m_voice.head - m_voice.tail >= AHEAD) {
      vTaskDelay(1);
      continue;
    }

    while (m_queueTail != m_queueHead) {
      uint16_t w = m_queue[m_queueTail & (QUEUESIZE - 1)];
      m_queueTail = m_queueTail + 1;
      if (w == RESET_COMMAND) {
        for (int r = 0x20; r < 0x100; ++r)
          m_chip->WriteReg(r, 0);
        m_chip->WriteReg(0x01, 0);
        m_chip->WriteReg(0x08, 0);
        m_chip->WriteReg(0xbd, 0);
      } else
        m_chip->WriteReg(w >> 8, w & 0xff);
    }

    m_chip->GenerateBlock2(BLOCK, buf);

    // DBOPL's output is 16 bit scale, a single channel at full level reaching about 4000; the voice
    // takes -128..127. Shifted by 7 so a lone channel is not lost, and clipped for the rare moments
    // when several at full level add up past that.
    uint32_t head = m_voice.head;
    for (int i = 0; i < BLOCK; ++i) {
      int s = buf[i] >> 7;
      m_voice.ring[(head + i) & (RINGSIZE - 1)] = (int8_t)(s > 127 ? 127 : (s < -128 ? -128 : s));
    }
    m_voice.head = head + BLOCK;
  }
}
