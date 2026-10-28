// Host build: no I/O expander on the bus
#pragma once
#include <stdint.h>
#define MCP_PORTA 0
#define MCP_PORTB 1
namespace fabgl {
enum class MCPDir { Input, Output };
class MCP23S17 {
public:
  bool begin(int = -1, int = -1, int = -1, int = -1, int = -1, int = 0) { return false; }
  void end()                                         { }
  bool available()                                   { return false; }
  void setPortDir(int, uint8_t)                      { }
  uint8_t getPortDir(int)                            { return 0xff; }
  void enablePortPullUp(int, uint8_t)                { }
  uint8_t getPortPullUp(int)                         { return 0; }
  void writePort(int, uint8_t)                       { }
  uint8_t readPort(int)                              { return 0xff; }
  void configureGPIO(int, MCPDir, bool = false)      { }
  void writeGPIO(int, bool)                          { }
  bool readGPIO(int)                                 { return false; }
  void setINTActiveHigh(bool)                        { }
  bool getINTActiveHigh()                            { return false; }
};
}
