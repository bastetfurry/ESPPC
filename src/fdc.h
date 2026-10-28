/*
  FDC765: the NEC uPD765 floppy disk controller, for machines with "hwdisk 1".

  Local to this project -- not part of FabGL.
 */


#pragma once


#include <stdint.h>


class Machine;


// NEC uPD765 floppy disk controller, as the PC has it at 3F0h-3F7h with IRQ 6 and DMA channel 2.
//
// Only for software that drives the controller itself: an operating system with its own floppy
// driver (SCO Xenix 86), or a disk copier. INT 13h does not come through here, it reads the image
// directly on the C++ side, which stays the fast path. Both work on the same image files.
//
// Good enough rather than exact, like the rest of the machine: a sector is where the command's
// C/H/R say it is, without a modelled head position or rotation, the data moves in one go when the
// command is complete, and the interrupt follows a short, fixed number of instructions later.
// Only DMA transfers are implemented, which is what every PC driver uses; 512 byte sectors only.
//
//   3F2h  digital output register: drive select, controller reset, DMA and IRQ enable, motors
//   3F4h  main status register (read)
//   3F5h  data: commands in, results out
//   3F7h  digital input register (read): bit 7 the disk change line, as on an AT
class FDC765 {

public:

  void init(Machine * machine)          { m_machine = machine; reset(); }

  // power on
  void reset();

  void write(int port, uint8_t value);
  uint8_t read(int port);

  // n instructions have run: counts down to the end of a command in progress
  void tick(int n)                      { if (m_delay > 0 && (m_delay -= n) <= 0) complete(); }

  // a new image went into a drive (0 or 1), for the disk change line
  void mediaChanged(int drive)          { if (drive >= 0 && drive < 4) m_changed |= 1 << drive; }

private:

  enum class Phase : uint8_t { Command, Execution, Result };

  void controllerReset();
  void startCommand();
  void complete();
  void finish(int delay, int resultBytes);
  void transfer(bool write);
  void readID();
  void formatTrack();
  void setResultCHRN(int cyl, int head, int sector, int n);
  bool driveGeometry(int drive, int * cyls, int * heads, int * spt);

  Machine * m_machine;

  uint8_t   m_dor;               // last write to 3F2h
  Phase     m_phase;
  uint8_t   m_cmd[9];
  uint8_t   m_cmdLen;            // bytes the current command takes
  uint8_t   m_cmdPos;
  uint8_t   m_result[7];
  uint8_t   m_resultLen;
  uint8_t   m_resultPos;

  int       m_delay;             // instructions until the command in progress completes, 0 = none
  bool      m_completeIRQ;       // raise IRQ 6 when it does
  uint8_t   m_seekDrive;         // the drive a seek or recalibrate is moving, for the status bits
  bool      m_seeking;

  uint8_t   m_pcn[4];            // present cylinder of each drive
  uint8_t   m_intPending;        // bit per drive: a seek ended, waiting for SENSE INTERRUPT STATUS
  uint8_t   m_st0[4];            // ...and the ST0 it reports
  uint8_t   m_resetSense;        // after a reset: polls left to answer, one per drive
  uint8_t   m_changed;           // bit per drive: disk change line
};
