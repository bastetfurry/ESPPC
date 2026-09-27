/*
  Texas Instruments SN76496, the three voice sound chip of the Tandy 1000 (port C0h).

  Local to this project -- not part of FabGL.

  Three square wave tone channels and one noise channel, each with a 4 bit attenuator. The chip is
  write only. A byte with bit 7 set latches a register and carries its low four bits; a byte
  with bit 7 clear carries the upper six bits of a tone divider:

      1 cc t dddd    latch: channel cc, t = 1 volume / 0 tone (noise control on channel 3)
      0 x dddddd     data:  bits 9..4 of the latched tone divider

  Tone frequency is 3579545 / (32 * N) for the 10 bit divider N, and attenuation goes in 2 dB steps,
  15 being off. The voices are played on FabGL's SoundGenerator. The noise channel is an
  approximation: FabGL's noise generator is white noise at the sample rate, so the shift rate and the
  periodic mode are not modelled.
 */


#pragma once


#include "fabgl.h"


class SN76496 {

public:

  SN76496();

  // attaches the four voices to the sound generator; call once, before any write()
  void attach(fabgl::SoundGenerator * soundGen);

  // silence every voice, as at power on
  void reset();

  // a byte written to the chip's port
  void write(uint8_t value);

private:

  void updateTone(int channel);
  void updateVolume(int channel);

  fabgl::SquareWaveformGenerator m_tone[3];
  fabgl::NoiseWaveformGenerator  m_noise;

  uint16_t m_divider[3];     // 10 bit tone dividers
  uint8_t  m_attenuation[4]; // 0 = loudest, 15 = off; channel 3 is the noise
  uint8_t  m_noiseControl;
  uint8_t  m_latch;          // latched register: channel << 1 | volume
};
