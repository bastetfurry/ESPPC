/*
  XTHDC: the IBM PC/XT fixed disk adapter (a Xebec 1210), for machines with "hwdisk 1".

  Local to this project -- not part of FabGL.
 */


#pragma once


#include <stdint.h>


class Machine;


// The XT hard disk controller, at 320h-323h with IRQ 5 and DMA channel 3, on the hd0 and hd1 images.
//
// For software with its own hard disk driver, such as SCO Xenix 86; INT 13h does not come through
// here and stays the fast path. Good enough rather than exact: every command completes at once, the
// interrupt a short, fixed number of instructions later, and formatting does not erase data. Sector
// numbers in the command block start at 0, as on the real controller.
//
//   320h  data: the command block, data bytes without DMA, and the completion and sense bytes
//   321h  read: status (REQ, I/O, C/D, BUSY, interrupt); write: controller reset
//   322h  read: drive type switches; write: select the controller, which starts a command
//   323h  write: bit 0 DMA enable, bit 1 IRQ enable
class XTHDC {

public:

  void init(Machine * machine)          { m_machine = machine; reset(); }

  void reset();

  void write(int port, uint8_t value);
  uint8_t read(int port);

  void tick(int n)                      { if (m_delay > 0 && (m_delay -= n) <= 0) complete(); }

private:

  enum class Phase : uint8_t {
    Idle,       // not selected
    Command,    // taking the six byte command block
    Busy,       // working: nothing to transfer until the delay has run
    DataOut,    // taking data bytes through 320h
    DataIn,     // giving data bytes through 320h
    Status,     // the completion byte is waiting at 320h
  };

  void execute();
  void complete()                       { m_delay = 0; execute(); }
  void endCommand(uint8_t error);
  void dataPhase(Phase phase, int bytes);
  void dataDone();
  uint8_t checkPosition(uint32_t * pos);
  void advance();
  void transferDMA(bool write);
  void sectorIn();
  bool sectorOut();
  bool driveGeometry(int drive, int * cyls, int * heads, int * spt);

  Machine * m_machine;

  Phase     m_phase;
  uint8_t   m_mask;              // 323h: bit 0 DMA, bit 1 IRQ
  bool      m_intPending;        // status bit 5, until the completion byte is read
  int       m_delay;             // instructions until the command block is acted on

  uint8_t   m_cmd[6];
  uint8_t   m_cmdPos;

  // the block the command works on, moved on sector by sector
  uint8_t   m_drive;
  uint8_t   m_head;
  uint16_t  m_cyl;
  uint8_t   m_sector;
  int       m_count;             // sectors left

  uint8_t   m_buf[512];          // the sector buffer, and the bytes of a data phase
  int       m_bufLen;
  int       m_bufPos;

  uint8_t   m_error;             // of the last command, for the status byte and REQUEST SENSE
  bool      m_senseValid;        // the address in the sense bytes means something
  uint8_t   m_senseDrive;
  uint8_t   m_senseHead;
  uint16_t  m_senseCyl;
  uint8_t   m_senseSector;
};

