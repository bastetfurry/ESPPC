/*
  The DAC of the Tandy 1000 SL/TL/RL (in the PSSJ chip, next to the SN76496).

  Local to this project -- not part of FabGL.

  On the real machine the DAC is fed by DMA channel 1 and raises IRQ 7 at the end of a block.
  Software reaches it in two ways, and both end up here:

    - through the BIOS, INT 1Ah AH=81h..85h (KQ5's SL/TL driver): Machine::tandyDACBios() hands
      over a linear block with start()
    - by programming the DMA controller and the DAC's ports itself (MOD players): Machine keeps
      the 8237 registers and starts a transfer with startDMA() once the DAC is in playback mode
      with DMA enabled and channel 1 unmasked

  Either way the emulator task copies samples out of guest RAM into a small ring buffer, only a
  little ahead of playback, and a FabGL waveform generator plays the ring at the programmed rate.

  The split matters. Guest RAM is PSRAM, which only the emulator's core may touch, while the sound
  generator runs in an interrupt on either core -- so the generator only ever reads the ring, which
  lives in internal RAM. It also counts the samples it has actually played. Blocks end, and the
  DMA position the guest reads back moves, as samples are heard rather than as they are copied,
  and the copying stays only about 15 ms ahead: players mix into the part of their buffer that
  has just been played, and copying further ahead would pick up samples not yet mixed.
 */


#pragma once


#include "fabgl.h"


class TandyDAC {

public:

  TandyDAC();

  // attaches the voice to the sound generator; call once, before starting anything
  void attach(fabgl::SoundGenerator * soundGen);

  // BIOS path: plays length bytes of 8 bit unsigned samples from guest RAM at linear address addr,
  // once, at 3579545 / divider samples a second. volume is 0..7.
  void start(uint32_t addr, uint32_t length, int divider, int volume);

  // DMA path: plays from DMA channel 1 as programmed, page:address for count + 1 bytes, the
  // address wrapping within its 64 KB page as the 8237's does, and starting over at the end of the
  // block when autoInit is set.
  void startDMA(uint8_t page, uint16_t address, uint16_t count, bool autoInit, int divider, int volume);

  // the DAC's clock divider or amplitude changed while playing
  void setDivider(int divider);
  void setVolume(int volume);

  // stops at once, without reporting the end of a block
  void stop();

  bool busy() const                    { return m_active; }
  bool dmaMode() const                 { return m_active && m_dma; }

  // DMA path: bytes of the current block played so far, for the 8237's current address and count
  uint32_t dmaOffset() const;

  // Called regularly from the emulator task with guest RAM: tops the ring up. Returns how many
  // blocks have been played to the end since the last call (normally 0 or 1). A block that is not
  // auto-initialised stops the DAC when it ends.
  int pump(uint8_t const * memory);

private:

  class Voice : public fabgl::WaveformGenerator {
  public:
    static constexpr int RINGSIZE = 2048;          // power of two
    Voice();
    void setFrequency(int value);                  // samples a second, 0 = stopped
    int getSample();
    uint8_t           ring[RINGSIZE];
    volatile uint32_t head;                        // written by the emulator task
    volatile uint32_t tail;                        // written by the sound interrupt
  private:
    uint32_t m_step;                               // input samples per output sample, 16.16
    uint32_t m_acc;
    int      m_last;                               // the sample being held, -128..127
  };

  void begin(uint32_t length, int divider, int volume);
  uint32_t played() const              { return m_voice.tail - m_startTail; }

  Voice    m_voice;
  bool     m_active;
  bool     m_dma;          // source is a DMA channel (page wrap, auto-init) rather than linear
  bool     m_autoInit;
  uint32_t m_base;         // linear start (BIOS), or page << 16 | address (DMA)
  uint32_t m_length;       // bytes in a block
  uint32_t m_copied;       // bytes copied into the ring since the start
  uint32_t m_startTail;    // ring position at the start
  uint32_t m_blocksDone;   // blocks already reported by pump()
  uint32_t m_lookahead;    // how far copying may run ahead of playback
};
