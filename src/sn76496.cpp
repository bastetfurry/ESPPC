/*
  Texas Instruments SN76496 -- see sn76496.h
 */


#include "sn76496.h"


static constexpr int SN_CLOCK = 3579545;

// FabGL volume per attenuation step, 2 dB each: 31 * 10^(-step / 10), 15 = off. The mixer
// rescales everything once the enabled voices' volumes add up to more than 127, so each voice
// peaks at 31 and all four together stay under that.
static const uint8_t VOLUME[16] = { 31, 25, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 2, 1, 1, 0 };


// No reset() here: the generators get their sample rate from attach(), and setFrequency() divides
// by it.
SN76496::SN76496()
  : m_divider(),
    m_attenuation { 15, 15, 15, 15 },
    m_noiseControl(0),
    m_latch(0)
{
}


void SN76496::attach(fabgl::SoundGenerator * soundGen)
{
  for (int i = 0; i < 3; ++i) {
    soundGen->attach(&m_tone[i]);
    m_tone[i].enable(true);
  }
  soundGen->attach(&m_noise);
  m_noise.enable(true);
  reset();
}


void SN76496::reset()
{
  for (int i = 0; i < 3; ++i) {
    m_divider[i] = 0;
    m_tone[i].setFrequency(0);
  }
  for (int i = 0; i < 4; ++i) {
    m_attenuation[i] = 15;
    updateVolume(i);
  }
  m_noiseControl = 0;
  m_latch        = 0;
}


void SN76496::write(uint8_t value)
{
  if (value & 0x80) {
    m_latch = (value >> 4) & 0x07;
    int channel = m_latch >> 1;
    if (m_latch & 1) {
      m_attenuation[channel] = value & 0x0f;
      updateVolume(channel);
    } else if (channel < 3) {
      m_divider[channel] = (m_divider[channel] & 0x3f0) | (value & 0x0f);
      updateTone(channel);
    } else {
      m_noiseControl = value & 0x07;
    }
  } else {
    // a data byte goes to whatever register is latched
    int channel = m_latch >> 1;
    if (m_latch & 1) {
      m_attenuation[channel] = value & 0x0f;
      updateVolume(channel);
    } else if (channel < 3) {
      m_divider[channel] = (m_divider[channel] & 0x00f) | ((value & 0x3f) << 4);
      updateTone(channel);
    } else {
      m_noiseControl = value & 0x07;
    }
  }
}


void SN76496::updateTone(int channel)
{
  // A divider of 0 counts as 1024. Tones the sample rate cannot carry are silenced rather than
  // aliased: software sets tiny dividers for an inaudible, steady output (it is used to play
  // samples through the volume register), which would otherwise come out as a loud whine.
  int n = m_divider[channel] ? m_divider[channel] : 1024;
  int freq = SN_CLOCK / (32 * n);
  m_tone[channel].setFrequency(freq < 8000 ? freq : 0);
}


void SN76496::updateVolume(int channel)
{
  if (channel < 3)
    m_tone[channel].setVolume(VOLUME[m_attenuation[channel]]);
  else
    m_noise.setVolume(VOLUME[m_attenuation[channel]]);
}
