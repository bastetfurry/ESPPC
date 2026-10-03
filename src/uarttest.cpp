// U1 wiring test for the Olimex ESP32-SBC-FabGL - not the emulator.
//   pio run -e uarttest -t upload
//
// U1 is the 8 pin "Access_Bus" header, 3.3V levels:
//   1 +5V  2 +3.3V  3 GND  4 ESP_EN  5 TXD (GPIO2)  6 RXD (GPI34)  7 RTS (GPIO0)  8 CTS (GPI39)
//
// Sends "AB" on U1 TXD forever at 38400 8N1. Whatever arrives on U1 RXD is echoed to the USB
// console, so both directions can be checked.

#include <Arduino.h>

#define U1_RX 34
#define U1_TX 2

void setup()
{
  Serial.begin(115200);
  Serial1.begin(38400, SERIAL_8N1, U1_RX, U1_TX);
  Serial.println("\nU1 test: sending \"AB\" at 38400 8N1 on GPIO2, echoing GPI34 here");
}

void loop()
{
  if (Serial1.availableForWrite() >= 2)
    Serial1.write("AB", 2);
  while (Serial1.available()) {
    int ch = Serial1.read();
    Serial.printf("U1 RX: 0x%02X '%c'\r\n", ch, ch >= 32 && ch < 127 ? ch : '.');
  }
}
