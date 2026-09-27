 

#pragma once

#include "fabgl.h"

#include <stdint.h>


#define I8086_SHOW_OPCODE_STATS 0

#ifndef I80186MODE
  #define I80186MODE 0
#endif



namespace fabgl {


class i8086 {

public:

  // callbacks

  typedef void (*WritePort)(void * context, int address, uint8_t value);
  typedef uint8_t (*ReadPort)(void * context, int address);
  typedef void (*WriteVideoMemory8)(void * context, int address, uint8_t value);
  typedef void (*WriteVideoMemory16)(void * context, int address, uint16_t value);
  typedef uint8_t (*ReadVideoMemory8)(void * context, int address);
  typedef uint16_t (*ReadVideoMemory16)(void * context, int address);
  typedef bool (*Interrupt)(void * context, int num);


  static void setCallbacks(void * context, ReadPort readPort, WritePort writePort, WriteVideoMemory8 writeVideoMemory8, WriteVideoMemory16 writeVideoMemory16, ReadVideoMemory8 readVideoMemory8, ReadVideoMemory16 readVideoMemory16,Interrupt interrupt) {
    s_context            = context;
    s_readPort           = readPort;
    s_writePort          = writePort;
    s_writeVideoMemory8  = writeVideoMemory8;
    s_writeVideoMemory16 = writeVideoMemory16;
    s_readVideoMemory8   = readVideoMemory8;
    s_readVideoMemory16  = readVideoMemory16;
    s_interrupt          = interrupt;
  }

  static void setMemory(uint8_t * memory) { s_memory = memory; }

  // PCEmulator: start of the address range routed to the video memory callbacks.
  // 0xA0000 is the IBM default; a machine with more than 640KB of conventional memory sets
  // 0xB0000 so that 0xA0000-0xAFFFF becomes ordinary RAM (see Machine::setRamSize).
  static void setVideoMemStart(int value) { s_videoMemStart = value; }

  static void reset();

  // PCEmulator: the register accessors are inline. They used to be out of line in i8086.cpp,
  // which made every AL(), CS() or setAX() from the BIOS helpers and the machine's interrupt hook a
  // real call into flash. The register file layout is the one i8086.cpp works on: 16 bit registers
  // AX CX DX BX SP BP SI DI ES CS SS DS, the 8 bit ones overlaid on the first four.

  static void setAL(uint8_t value)                        { s_regs[0] = value; }
  static void setAH(uint8_t value)                        { s_regs[1] = value; }
  static void setCL(uint8_t value)                        { s_regs[2] = value; }
  static void setCH(uint8_t value)                        { s_regs[3] = value; }
  static void setDL(uint8_t value)                        { s_regs[4] = value; }
  static void setDH(uint8_t value)                        { s_regs[5] = value; }
  static void setBL(uint8_t value)                        { s_regs[6] = value; }
  static void setBH(uint8_t value)                        { s_regs[7] = value; }

  static uint8_t AL()                                     { return s_regs[0]; }
  static uint8_t AH()                                     { return s_regs[1]; }
  static uint8_t CL()                                     { return s_regs[2]; }
  static uint8_t CH()                                     { return s_regs[3]; }
  static uint8_t DL()                                     { return s_regs[4]; }
  static uint8_t DH()                                     { return s_regs[5]; }
  static uint8_t BL()                                     { return s_regs[6]; }
  static uint8_t BH()                                     { return s_regs[7]; }

  static void setAX(uint16_t value)                       { reg16(0)  = value; }
  static void setCX(uint16_t value)                       { reg16(1)  = value; }
  static void setDX(uint16_t value)                       { reg16(2)  = value; }
  static void setBX(uint16_t value)                       { reg16(3)  = value; }
  static void setSP(uint16_t value)                       { reg16(4)  = value; }
  static void setDI(uint16_t value)                       { reg16(7)  = value; }
  static void setES(uint16_t value)                       { reg16(8)  = value; }
  static void setCS(uint16_t value)                       { reg16(9)  = value; }
  static void setSS(uint16_t value)                       { reg16(10) = value; }
  static void setDS(uint16_t value)                       { reg16(11) = value; }
  static void setIP(uint16_t value)                       { s_regIP   = value; }

  static uint16_t AX()                                    { return reg16(0); }
  static uint16_t CX()                                    { return reg16(1); }
  static uint16_t DX()                                    { return reg16(2); }
  static uint16_t BX()                                    { return reg16(3); }
  static uint16_t SP()                                    { return reg16(4); }
  static uint16_t BP()                                    { return reg16(5); }
  static uint16_t SI()                                    { return reg16(6); }
  static uint16_t DI()                                    { return reg16(7); }
  static uint16_t ES()                                    { return reg16(8); }
  static uint16_t CS()                                    { return reg16(9); }
  static uint16_t SS()                                    { return reg16(10); }
  static uint16_t DS()                                    { return reg16(11); }

  // flags, in i8086.cpp's order: CF PF AF ZF SF TF IF DF OF
  static bool flagCF()                                    { return s_flags[0]; }
  static bool flagPF()                                    { return s_flags[1]; }
  static bool flagAF()                                    { return s_flags[2]; }
  static bool flagZF()                                    { return s_flags[3]; }
  static bool flagSF()                                    { return s_flags[4]; }
  static bool flagTF()                                    { return s_flags[5]; }
  static bool flagIF()                                    { return s_flags[6]; }
  static bool flagDF()                                    { return s_flags[7]; }
  static bool flagOF()                                    { return s_flags[8]; }

  static void setFlagCF(bool value)                       { s_flags[0] = value; }
  static void setFlagZF(bool value)                       { s_flags[3] = value; }

  static uint16_t IP()                                    { return s_regIP; }

  static bool halted()                                    { return s_halted; }

  static bool IRQ(uint8_t interrupt_num);

  static void step();


  // The register file i8086.cpp emulates on (regs8/regs16 there point at s_regs), IP and the
  // flags. Public only because file level code in i8086.cpp works on them directly: everything
  // else goes through the accessors above.
  alignas(4) static uint8_t s_regs[48];
  static uint16_t           s_regIP;
  static uint8_t            s_flags[10];

private:

  static uint16_t & reg16(int index)                      { return ((uint16_t *) s_regs)[index]; }

  static uint8_t WMEM8(int addr, uint8_t value);
  static uint16_t WMEM16(int addr, uint16_t value);
  static uint8_t RMEM8(int addr);
  static uint16_t RMEM16(int addr);

  static uint16_t make_flags();
  static void set_flags(int new_flags);

  static void pc_interrupt(uint8_t interrupt_num);

  static uint8_t raiseDivideByZeroInterrupt();

  static void stepEx(uint8_t const * opcode_stream);
  static bool stepExCold(uint8_t const * opcode_stream, uint16_t i_data0, uint16_t i_data2, int32_t & op_result);


  static void *             s_context;
  static ReadPort           s_readPort;
  static WritePort          s_writePort;
  static WriteVideoMemory8  s_writeVideoMemory8;
  static WriteVideoMemory16 s_writeVideoMemory16;
  static ReadVideoMemory8   s_readVideoMemory8;
  static ReadVideoMemory16  s_readVideoMemory16;
  static Interrupt          s_interrupt;

  static bool               s_pendingIRQ;
  static uint8_t            s_pendingIRQIndex;
  static uint8_t *          s_memory;
  static int                s_videoMemStart;
  static bool               s_halted;

};


} // namespace fabgl
