/*
  ESPPC - getting an SD card out of a state it was left in by a reset. See sdrecover.h.
*/


#include "driver/gpio.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

#include "sdrecover.h"


namespace {

struct Pins {
  gpio_num_t miso, mosi, clk, cs;

  // SPI mode 0, MSB first, a few hundred kHz
  uint8_t xfer(uint8_t out)
  {
    uint8_t in = 0;
    for (int b = 7; b >= 0; --b) {
      gpio_set_level(mosi, (out >> b) & 1);
      ets_delay_us(2);
      gpio_set_level(clk, 1);
      in = (in << 1) | gpio_get_level(miso);
      ets_delay_us(2);
      gpio_set_level(clk, 0);
    }
    return in;
  }

  // clocks 0xFF until the card answers 0xFF (not busy) or timeoutMs has passed
  bool waitReady(int timeoutMs)
  {
    int64_t until = esp_timer_get_time() + timeoutMs * 1000LL;
    while (esp_timer_get_time() < until)
      if (xfer(0xff) == 0xff)
        return true;
    return false;
  }
};

}


void sdRecoverCard(int miso, int mosi, int clk, int cs)
{
  Pins p = { (gpio_num_t) miso, (gpio_num_t) mosi, (gpio_num_t) clk, (gpio_num_t) cs };

  gpio_reset_pin(p.mosi);
  gpio_reset_pin(p.clk);
  gpio_reset_pin(p.cs);
  gpio_set_direction(p.mosi, GPIO_MODE_OUTPUT);
  gpio_set_direction(p.clk, GPIO_MODE_OUTPUT);
  gpio_set_direction(p.cs, GPIO_MODE_OUTPUT);
  gpio_set_direction(p.miso, GPIO_MODE_INPUT);
  gpio_set_pull_mode(p.miso, GPIO_PULLUP_ONLY);     // no effect on input-only pins, harmless

  gpio_set_level(p.clk, 0);
  gpio_set_level(p.mosi, 1);

  // deselected: at least 74 clocks, as the card wants before anything
  gpio_set_level(p.cs, 1);
  for (int i = 0; i < 20; ++i)
    p.xfer(0xff);

  // selected: end a multi-block write that a reset cut off, and let the card finish programming
  gpio_set_level(p.cs, 0);
  p.xfer(0xfd);                 // stop tran token
  p.xfer(0xff);
  p.waitReady(500);

  // and a multi-block read: CMD12, then its response and busy
  static const uint8_t CMD12[] = { 0x40 | 12, 0, 0, 0, 0, 0x61 };
  for (uint8_t b : CMD12)
    p.xfer(b);
  p.xfer(0xff);                 // CMD12 has a stuff byte before its response
  for (int i = 0; i < 8; ++i)
    if (!(p.xfer(0xff) & 0x80))
      break;
  p.waitReady(500);

  gpio_set_level(p.cs, 1);
  for (int i = 0; i < 4; ++i)
    p.xfer(0xff);
}
