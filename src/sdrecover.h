/*
  PCEmulator - getting an SD card out of a state it was left in by a reset
*/


#pragma once


// The card keeps its power over a reset of the ESP32 (esp_restart(), or EN from the USB serial),
// and with it whatever it was doing: in the middle of a multi-block write, or busy programming.
// It then ignores the driver's initialisation, the mount times out, and only a power cycle helped.
// This talks to it on the bare pins first, before the SPI driver has them: clocks with the card
// deselected, the stop token for an open multi-block write, clocks until it is no longer busy, and
// CMD12 (stop transmission). Harmless for a card that was fine.
void sdRecoverCard(int miso, int mosi, int clk, int cs);
