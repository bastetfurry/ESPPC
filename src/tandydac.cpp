/*
  Tandy 1000 SL/TL DAC -- see tandydac.h
 */


#include "tandydac.h"


static constexpr int TANDY_CLOCK = 3579545;


TandyDAC::Voice::Voice()
  : head(0),
    tail(0),
    m_step(0),
    m_acc(0),
    m_last(0)
{
}


void TandyDAC::Voice::setFrequency(int value)
{
  m_step = (value > 0 && sampleRate()) ? (uint32_t)(((uint64_t)value << 16) / sampleRate()) : 0;
}


// Runs in the sound generator's interrupt: internal RAM only. Nearest neighbour resampling, which
// is what the real DAC's sample and hold sounds like anyway. An empty ring (the emulator task fell
// behind, or the block ended) holds the last sample instead of clicking.
int TandyDAC::Voice::getSample()
{
  m_acc += m_step;
  while (m_acc >= 0x10000) {
    m_acc -= 0x10000;
    if (tail != head) {
      m_last = (int)ring[tail & (RINGSIZE - 1)] - 128;
      tail = tail + 1;
    }
  }
  return m_last * volume() / 128;
}


TandyDAC::TandyDAC()
  : m_active(false),
    m_dma(false),
    m_autoInit(false),
    m_base(0),
    m_length(0),
    m_copied(0),
    m_startTail(0),
    m_blocksDone(0),
    m_lookahead(64)
{
}


void TandyDAC::attach(fabgl::SoundGenerator * soundGen)
{
  soundGen->attach(&m_voice);
  m_voice.setVolume(0);
  m_voice.enable(true);
}


void TandyDAC::begin(uint32_t length, int divider, int volume)
{
  m_length     = length;
  m_copied     = 0;
  m_startTail  = m_voice.head;     // the ring is empty after stop(), so tail == head
  m_blocksDone = 0;
  setVolume(volume);
  setDivider(divider);
  m_active     = true;
}


void TandyDAC::start(uint32_t addr, uint32_t length, int divider, int volume)
{
  stop();
  if (length == 0 || divider <= 0)
    return;
  m_dma      = false;
  m_autoInit = false;
  m_base     = addr & 0xfffff;
  begin(length, divider, volume);
}


void TandyDAC::startDMA(uint8_t page, uint16_t address, uint16_t count, bool autoInit, int divider, int volume)
{
  stop();
  if (divider <= 0)
    return;
  m_dma      = true;
  m_autoInit = autoInit;
  m_base     = ((uint32_t)page << 16) | address;
  begin((uint32_t)count + 1, divider, volume);
}


void TandyDAC::setDivider(int divider)
{
  int rate = divider > 0 ? TANDY_CLOCK / divider : 0;
  // rates far above what the output can carry are set by some software as "off"
  if (rate > 49000)
    rate = 0;
  m_voice.setFrequency(rate);
  // about 15 ms of samples, and never less than a pump interval's worth
  m_lookahead = rate / 64 > 64 ? rate / 64 : 64;
}


// BIOS volume 0..7 onto FabGL's 0..127, a little under full so the SN76496 voices beside it do not
// push the mixer into rescaling everything
void TandyDAC::setVolume(int volume)
{
  m_voice.setVolume((volume & 7) * 14);
}


void TandyDAC::stop()
{
  m_active = false;
  m_voice.setFrequency(0);
  m_voice.setVolume(0);
  m_voice.tail = m_voice.head;     // drop what is queued; only safe with the rate at 0
}


uint32_t TandyDAC::dmaOffset() const
{
  if (!m_active || m_length == 0)
    return 0;
  uint32_t p = played();
  return m_autoInit ? p % m_length : (p < m_length ? p : m_length);
}


int TandyDAC::pump(uint8_t const * memory)
{
  if (!m_active)
    return 0;

  // copy, but only m_lookahead ahead of playback, and only one block unless auto-initialised
  uint32_t played = this->played();
  uint32_t limit  = played + m_lookahead;
  if (!m_autoInit && limit > m_length)
    limit = m_length;
  uint32_t head = m_voice.head;
  while (m_copied < limit && (head - m_voice.tail) < Voice::RINGSIZE) {
    uint32_t off  = m_copied % m_length;
    uint32_t addr = m_dma ? ((m_base & 0xf0000) | ((m_base + off) & 0xffff)) : (m_base + off);
    m_voice.ring[head & (Voice::RINGSIZE - 1)] = memory[addr & 0xfffff];
    ++head;
    ++m_copied;
  }
  m_voice.head = head;

  // blocks heard to the end since the last call
  uint32_t done  = played / m_length;
  int      ended = (int)(done - m_blocksDone);
  m_blocksDone   = done;

  if (!m_autoInit && played >= m_length) {
    m_active = false;
    m_voice.setFrequency(0);
    return 1;
  }
  return ended;
}
