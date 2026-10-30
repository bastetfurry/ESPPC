 

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
  typedef int (*Acknowledge)(void * context);


  static void setCallbacks(void * context, ReadPort readPort, WritePort writePort, WriteVideoMemory8 writeVideoMemory8, WriteVideoMemory16 writeVideoMemory16, ReadVideoMemory8 readVideoMemory8, ReadVideoMemory16 readVideoMemory16,Interrupt interrupt) {
    s_state.context            = context;
    s_state.readPort           = readPort;
    s_state.writePort          = writePort;
    s_state.writeVideoMemory8  = writeVideoMemory8;
    s_state.writeVideoMemory16 = writeVideoMemory16;
    s_state.readVideoMemory8   = readVideoMemory8;
    s_state.readVideoMemory16  = readVideoMemory16;
    s_state.interrupt          = interrupt;
  }

  static void setMemory(uint8_t * memory) { s_state.memory = memory; }

  // ESPPC: start of the address range routed to the video memory callbacks.
  // 0xA0000 is the IBM default; a machine with more than 640KB of conventional memory sets
  // 0xB0000 so that 0xA0000-0xAFFFF becomes ordinary RAM (see Machine::setRamSize).
  static void setVideoMemStart(int value) { s_state.videoMemStart = value; }
  static int videoMemStart()              { return s_state.videoMemStart; }

  static void reset();

  // ESPPC: the register accessors are inline. They used to be out of line in i8086.cpp,
  // which made every AL(), CS() or setAX() from the BIOS helpers and the machine's interrupt hook a
  // real call into flash. The register file layout is the one i8086.cpp works on: 16 bit registers
  // AX CX DX BX SP BP SI DI ES CS SS DS, the 8 bit ones overlaid on the first four.

  static void setAL(uint8_t value)                        { s_state.regs[0] = value; }
  static void setAH(uint8_t value)                        { s_state.regs[1] = value; }
  static void setCL(uint8_t value)                        { s_state.regs[2] = value; }
  static void setCH(uint8_t value)                        { s_state.regs[3] = value; }
  static void setDL(uint8_t value)                        { s_state.regs[4] = value; }
  static void setDH(uint8_t value)                        { s_state.regs[5] = value; }
  static void setBL(uint8_t value)                        { s_state.regs[6] = value; }
  static void setBH(uint8_t value)                        { s_state.regs[7] = value; }

  static uint8_t AL()                                     { return s_state.regs[0]; }
  static uint8_t AH()                                     { return s_state.regs[1]; }
  static uint8_t CL()                                     { return s_state.regs[2]; }
  static uint8_t CH()                                     { return s_state.regs[3]; }
  static uint8_t DL()                                     { return s_state.regs[4]; }
  static uint8_t DH()                                     { return s_state.regs[5]; }
  static uint8_t BL()                                     { return s_state.regs[6]; }
  static uint8_t BH()                                     { return s_state.regs[7]; }

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
  static void setIP(uint16_t value)                       { s_state.regIP   = value; }

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
  static bool flagCF()                                    { return s_state.flags[0]; }
  static bool flagPF()                                    { return s_state.flags[1]; }
  static bool flagAF()                                    { return s_state.flags[2]; }
  static bool flagZF()                                    { return s_state.flags[3]; }
  static bool flagSF()                                    { return s_state.flags[4]; }
  static bool flagTF()                                    { return s_state.flags[5]; }
  static bool flagIF()                                    { return s_state.flags[6]; }
  static bool flagDF()                                    { return s_state.flags[7]; }
  static bool flagOF()                                    { return s_state.flags[8]; }

  static void setFlagCF(bool value)                       { s_state.flags[0] = value; }
  static void setFlagZF(bool value)                       { s_state.flags[3] = value; }

  static uint16_t IP()                                    { return s_state.regIP; }

  static bool halted()                                    { return s_state.halted; }

  static bool IRQ(uint8_t interrupt_num);

  // ESPPC: the INTR line from the interrupt controller. While it is up, the core takes an interrupt
  // at the first instruction boundary where IF is set, and only then asks the acknowledge callback
  // for the vector, as the 8259 gives it in the INTA cycle. The callback returns -1 when the
  // request has gone away meanwhile.
  static void setAcknowledgeCallback(Acknowledge value)   { s_state.acknowledge = value; }
  static void setINTR(bool value)                         { s_state.intr = value; }

  static void step();


  // ESPPC: everything the core touches on every instruction, behind one base address. Xtensa
  // loads a global's address with l32r from a literal pool in IRAM, 8 cycles more than a load
  // from internal RAM (the monitor's 'n'), and step() needed about ten of them per instruction;
  // in one struct a single base register reaches it all. Byte fields first: a byte load reaches
  // 255 bytes from its base, a 16 bit one 510, a 32 bit one 1020. Public only because file level
  // code in i8086.cpp works on it directly; everything else goes through the accessors above.
  struct State {
    // the register file (16 bit AX CX DX BX SP BP SI DI ES CS SS DS ZERO SCRATCH .. TMP, the 8
    // bit registers overlaid on the first four), the flags (CF PF AF ZF SF TF IF DF OF and a
    // dummy that is always 0) and IP
    alignas(4) uint8_t regs[48];
    uint8_t            flags[10];
    uint16_t           regIP;
    bool               trap;            // TF was set when the last instruction began
    bool               pendingIRQ;
    uint8_t            pendingIRQIndex;
    bool               intr;
    bool               halted;
    // the decoder's state, see i8086.cpp
    uint8_t            i_mod_size, i_d, i_w, raw_opcode_id, xlat_opcode_id, extra, rep_mode;
    uint8_t            seg_override_en, rep_override_en;
    uint16_t           seg_override;
    int32_t            regs_offset;
    uint32_t           op_source, op_dest, set_flags_type;
    int32_t            op_to_addr, op_from_addr;
    // the machine around it
    uint8_t *          memory;
    int                videoMemStart = 0xA0000;    // IBM default, see setVideoMemStart()
    void *             context;
    ReadPort           readPort;
    WritePort          writePort;
    WriteVideoMemory8  writeVideoMemory8;
    WriteVideoMemory16 writeVideoMemory16;
    ReadVideoMemory8   readVideoMemory8;
    ReadVideoMemory16  readVideoMemory16;
    Interrupt          interrupt;
    Acknowledge        acknowledge;
    // the decoder's tables, copied in from i8086.cpp by reset(): reached from the same base, they
    // cost no l32r of their own
    uint8_t            optcodes[256], parity[256], ex_data[256], xlat_ids[256], std_flags[256];
    uint8_t            i_mod_adder[256], base_size[256], i_w_adder[256];
    uint8_t            jxx_dec_a[8], jxx_dec_b[8], jxx_dec_c[8], jxx_dec_d[8], RM_BASE[8], RM_INDEX[8];
  };
  static State s_state;

private:

  static uint16_t & reg16(int index)                      { return ((uint16_t *) s_state.regs)[index]; }

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



};


} // namespace fabgl
