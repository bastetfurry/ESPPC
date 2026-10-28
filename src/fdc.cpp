#include "fdc.h"
#include "machine.h"

#include <stdio.h>
#include <string.h>


// main status register
#define MSR_RQM       0x80      // the data register is ready for the next byte
#define MSR_DIO       0x40      // direction: 1 = controller to CPU (result phase)
#define MSR_NDM       0x20      // execution phase in non-DMA mode (never set here)
#define MSR_CB        0x10      // controller busy: a command is in progress

// ST0
#define ST0_IC_ABNORMAL 0x40    // abnormal termination
#define ST0_IC_INVALID  0x80    // invalid command, or SENSE INTERRUPT STATUS with nothing pending
#define ST0_IC_READY    0xC0    // ready line changed: what the polls after a reset report
#define ST0_SE          0x20    // seek end
#define ST0_EC          0x10    // equipment check: recalibrate found no track 0
#define ST0_NR          0x08    // not ready

// ST1
#define ST1_EN        0x80      // end of cylinder: the sectors ran out before the DMA count
#define ST1_OR        0x10      // overrun: the DMA did not take the data
#define ST1_ND        0x04      // no data: sector not found
#define ST1_NW        0x02      // not writable
#define ST1_MA        0x01      // missing address mark

// ST3
#define ST3_WP        0x40
#define ST3_RY        0x20
#define ST3_T0        0x10
#define ST3_TS        0x08

// DOR
#define DOR_RESET     0x04      // 0 = held in reset
#define DOR_DMA       0x08      // DMA and IRQ enabled

// The waits, in emulated instructions. Real ones are milliseconds; all that matters here is that a
// driver which issues the command and then starts waiting for the interrupt does not miss it.
constexpr int DELAY_RESET = 200;
constexpr int DELAY_SEEK  = 2000;
constexpr int DELAY_RW    = 800;
constexpr int DELAY_ID    = 200;

constexpr int SECTOR_SIZE = 512;


// bytes a command takes, by its low five bits; 0 = not implemented (invalid)
static const uint8_t COMMAND_LENGTH[32] = {
  0, 0, 9, 3, 2, 9, 9, 2,     // 00-07: -, -, read track, specify, sense drive status, write, read, recalibrate
  1, 9, 2, 0, 9, 6, 0, 3,     // 08-0F: sense int, write deleted, read ID, -, read deleted, format, -, seek
  1, 0, 0, 0, 0, 0, 0, 0,     // 10-17: version
  0, 0, 0, 0, 0, 0, 0, 0,
};


void FDC765::reset()
{
  m_dor        = DOR_RESET | DOR_DMA;
  m_changed    = 0x03;           // as after power on: no disk has been seen in either drive yet
  memset(m_pcn, 0, sizeof(m_pcn));
  controllerReset();
  m_resetSense = 0;              // the power on reset does not interrupt
}


// what the reset line does: the command in progress is dropped and the controller waits for a new
// one. Afterwards it raises its interrupt, and four SENSE INTERRUPT STATUS polls each report one
// drive's ready line changed -- drivers issue exactly those four after every reset.
void FDC765::controllerReset()
{
  m_phase       = Phase::Command;
  m_cmdPos      = 0;
  m_cmdLen      = 0;
  m_resultLen   = 0;
  m_resultPos   = 0;
  m_delay       = 0;
  m_completeIRQ = false;
  m_seeking     = false;
  m_intPending  = 0;
  m_resetSense  = 4;
}


bool FDC765::driveGeometry(int drive, int * cyls, int * heads, int * spt)
{
  if (drive > 1 || !m_machine->disk(drive))
    return false;
  *cyls  = m_machine->diskCylinders(drive);
  *heads = m_machine->diskHeads(drive);
  *spt   = m_machine->diskSectors(drive);
  return *cyls > 0 && *heads > 0 && *spt > 0;
}


void FDC765::write(int port, uint8_t value)
{
  switch (port) {

    case 0x3f2:
    {
      bool wasReset = !(m_dor & DOR_RESET);
      m_dor = value;
      if (!(value & DOR_RESET)) {
        controllerReset();
        m_resetSense = 0;
      } else if (wasReset) {
        // leaving reset: interrupt once the controller is up
        controllerReset();
        m_completeIRQ = true;
        m_delay       = DELAY_RESET;
        m_phase       = Phase::Command;
      }
      break;
    }

    case 0x3f5:
      if (!(m_dor & DOR_RESET) || m_phase != Phase::Command)
        break;      // not taking bytes: in reset, executing, or results waiting to be read
      if (m_cmdPos == 0) {
        m_cmdLen = COMMAND_LENGTH[value & 0x1f];
        if (m_cmdLen == 0) {
          // not a command this controller knows: one result byte, "invalid"
          m_result[0] = ST0_IC_INVALID;
          m_resultLen = 1;
          m_resultPos = 0;
          m_phase     = Phase::Result;
          break;
        }
      }
      m_cmd[m_cmdPos++] = value;
      if (m_cmdPos == m_cmdLen) {
        m_cmdPos = 0;
        startCommand();
      }
      break;

    // 3F7h: the data rate on an AT. A 360K disk at 250 kbit/s or a 1.44M one at 500 makes no
    // difference to an image, so it is accepted and ignored.
    default:
      break;
  }
}


uint8_t FDC765::read(int port)
{
  switch (port) {

    case 0x3f2:
      return m_dor;

    case 0x3f4:
    {
      if (!(m_dor & DOR_RESET))
        return 0x00;
      uint8_t busy = m_seeking ? (1 << m_seekDrive) : 0;
      switch (m_phase) {
        case Phase::Command:   return MSR_RQM | (m_cmdPos ? MSR_CB : 0) | busy;
        case Phase::Execution: return MSR_CB | busy;
        case Phase::Result:    return MSR_RQM | MSR_DIO | MSR_CB | busy;
      }
      return 0;
    }

    case 0x3f5:
      if (m_phase != Phase::Result)
        return 0xff;
      {
        uint8_t v = m_result[m_resultPos++];
        if (m_resultPos >= m_resultLen)
          m_phase = Phase::Command;
        return v;
      }

    case 0x3f7:
      return (m_changed & (1 << (m_dor & 3))) ? 0x80 : 0x00;

    default:
      return 0xff;
  }
}


// the command bytes are all in
void FDC765::startCommand()
{
  int drive = m_cmd[1] & 3;

  // The polls after a reset are only answered while they come in a row. An AT BIOS asks four
  // times, the XT BIOS and the drivers written for an XT once, and then seek; left queued, the
  // reset's remaining answers came back for the seek's SENSE INTERRUPT STATUS instead of its own,
  // and SCO Xenix 86 failed its first read with ST0 = C1.
  if ((m_cmd[0] & 0x1f) != 0x08)
    m_resetSense = 0;

  switch (m_cmd[0] & 0x1f) {

    // SPECIFY: step rate, head load and unload times, DMA mode. Nothing to time here, and the
    // controller only does DMA. No result, no interrupt.
    case 0x03:
      break;

    // SENSE DRIVE STATUS: ST3 at once, no interrupt
    case 0x04:
    {
      int cyls, heads, spt;
      bool present = driveGeometry(drive, &cyls, &heads, &spt);
      m_result[0] = (m_cmd[1] & 7) | (present ? ST3_RY : 0) | (m_pcn[drive] == 0 ? ST3_T0 : 0) |
                    (present && heads > 1 ? ST3_TS : 0);
      m_resultLen = 1;
      m_resultPos = 0;
      m_phase     = Phase::Result;
      break;
    }

    // RECALIBRATE and SEEK: done in the background, the controller takes new commands meanwhile;
    // the end is an interrupt, read back with SENSE INTERRUPT STATUS. The head positions are only
    // kept for the status registers: a transfer goes where its C/H/R say.
    case 0x07:
    case 0x0f:
    {
      bool recal  = (m_cmd[0] & 0x1f) == 0x07;
      int  target = recal ? 0 : m_cmd[2];
      int  cyls, heads, spt;
      uint8_t st0 = ST0_SE | (m_cmd[1] & 7);
      if (drive > 1)
        st0 |= ST0_IC_ABNORMAL | ST0_EC;       // no such drive: recalibrate never sees track 0
      else
        m_pcn[drive] = target;
      if (driveGeometry(drive, &cyls, &heads, &spt))
        m_changed &= ~(1 << drive);            // a step with a disk in clears the change line
      m_st0[drive]  = st0;
      m_seekDrive   = drive;
      m_seeking     = true;
      m_completeIRQ = true;
      m_delay       = DELAY_SEEK;
      break;
    }

    // SENSE INTERRUPT STATUS: why the controller interrupted
    case 0x08:
      m_resultPos = 0;
      if (m_resetSense) {
        int d = 4 - m_resetSense--;
        m_result[0] = ST0_IC_READY | d;
        m_result[1] = m_pcn[d];
        m_resultLen = 2;
      } else if (m_intPending) {
        int d = __builtin_ctz(m_intPending);
        m_intPending &= ~(1 << d);
        m_result[0] = m_st0[d];
        m_result[1] = m_pcn[d];
        m_resultLen = 2;
      } else {
        m_result[0] = ST0_IC_INVALID;
        m_resultLen = 1;
      }
      m_phase = Phase::Result;
      break;

    // READ DATA, READ DELETED DATA, READ TRACK (from sector 1, the way an image has it)
    case 0x06:
    case 0x0c:
    case 0x02:
      transfer(false);
      break;

    // WRITE DATA, WRITE DELETED DATA
    case 0x05:
    case 0x09:
      transfer(true);
      break;

    case 0x0a:
      readID();
      break;

    case 0x0d:
      formatTrack();
      break;

    // VERSION: 80h is a plain uPD765A, which is what a PC/XT has
    case 0x10:
      m_result[0] = 0x80;
      m_resultLen = 1;
      m_resultPos = 0;
      m_phase     = Phase::Result;
      break;
  }
}


// the command in progress has run its time
void FDC765::complete()
{
  m_delay = 0;
  if (m_seeking) {
    m_seeking     = false;
    m_intPending |= 1 << m_seekDrive;
  } else if (m_phase == Phase::Execution) {
    m_phase = Phase::Result;
  }
  if (m_completeIRQ && (m_dor & DOR_DMA))
    m_machine->fdcInterrupt();
  m_completeIRQ = false;
}


// execution done: the results are ready after delay instructions, with an interrupt
void FDC765::finish(int delay, int resultBytes)
{
  m_resultLen   = resultBytes;
  m_resultPos   = 0;
  m_phase       = Phase::Execution;
  m_completeIRQ = true;
  m_delay       = delay;
}


void FDC765::setResultCHRN(int cyl, int head, int sector, int n)
{
  m_result[3] = cyl;
  m_result[4] = head;
  m_result[5] = sector;
  m_result[6] = n;
}


// READ DATA / WRITE DATA: cmd, HD/US, C, H, R, N, EOT, GPL, DTL
//
// Sectors R..EOT of the track, then on to head 1 with MT, until the DMA reaches terminal count. The
// position in the image is C/H/R as the command gives them (H from the ID, the head select bit is
// what the drive does): no modelled head, so a driver that seeks and then reads finds its sector
// even if it was asked for a cylinder the seek did not go to. Lenient, but nothing that works on
// real hardware breaks for it.
void FDC765::transfer(bool write)
{
  int  drive   = m_cmd[1] & 3;
  int  head    = (m_cmd[1] >> 2) & 1;
  int  cyl     = m_cmd[2];
  int  idHead  = m_cmd[3];
  int  sector  = (m_cmd[0] & 0x1f) == 0x02 ? 1 : m_cmd[4];
  int  n       = m_cmd[5];
  int  eot     = m_cmd[6];
  bool mt      = m_cmd[0] & 0x80;

  uint8_t st0 = m_cmd[1] & 7, st1 = 0, st2 = 0;
  int cyls, heads, spt;

  m_result[0] = m_result[1] = m_result[2] = 0;

  if (!driveGeometry(drive, &cyls, &heads, &spt)) {
    // nothing in the drive: a real controller waits for an index pulse that never comes, and the
    // driver times out. Reporting it is kinder and ends up the same way.
    m_result[0] = ST0_IC_ABNORMAL | ST0_NR | st0;
    setResultCHRN(cyl, idHead, sector, n);
    finish(DELAY_RW, 7);
    return;
  }

  if (!(m_dor & DOR_DMA) || n != 2) {
    // no DMA to take the data, or sectors that are not 512 bytes, which an image does not have
    m_result[0] = ST0_IC_ABNORMAL | st0;
    m_result[1] = n != 2 ? ST1_ND : ST1_OR;
    setResultCHRN(cyl, idHead, sector, n);
    finish(DELAY_RW, 7);
    return;
  }

  FILE * file = m_machine->disk(drive);
  uint8_t buf[SECTOR_SIZE];
  m_machine->graphicsAdapter()->driveActivity(drive);

  while (true) {

    if (sector < 1 || sector > spt || cyl >= cyls || head >= heads) {
      st0 |= ST0_IC_ABNORMAL;
      st1 |= ST1_ND;
      break;
    }

    uint32_t pos = ((uint32_t)(cyl * heads + head) * spt + (sector - 1)) * SECTOR_SIZE;
    bool tc = false;
    if (write) {
      int got = m_machine->dmaFromMemory(2, buf, SECTOR_SIZE, &tc);
      if (got < SECTOR_SIZE)
        memset(buf + got, 0, SECTOR_SIZE - got);   // the DMA ran out mid-sector: the rest is zeros
      if (fseek(file, pos, SEEK_SET) != 0 || fwrite(buf, 1, SECTOR_SIZE, file) != SECTOR_SIZE) {
        st0 |= ST0_IC_ABNORMAL;
        st1 |= ST1_NW;                             // an image in the flash, opened read-only
        break;
      }
    } else {
      if (fseek(file, pos, SEEK_SET) != 0 || fread(buf, 1, SECTOR_SIZE, file) != SECTOR_SIZE) {
        st0 |= ST0_IC_ABNORMAL;
        st1 |= ST1_ND;
        break;
      }
      m_machine->dmaToMemory(2, buf, SECTOR_SIZE, &tc);
    }

    // the next sector, which is also what the result reports
    bool lastOnTrack = sector == eot;
    if (lastOnTrack) {
      sector = 1;
      if (mt && head == 0) {
        head   = 1;
        idHead = 1;
      } else {
        if (mt) {
          head   = 0;
          idHead = 0;
        }
        ++cyl;
      }
    } else
      ++sector;

    // terminal count ends the command normally, wherever it falls
    if (tc)
      break;

    // the last sector done and the DMA still wants more: end of cylinder, which is how a real
    // controller ends every transfer that runs to EOT without terminal count
    if (lastOnTrack && !(mt && head == 1)) {
      st0 |= ST0_IC_ABNORMAL;
      st1 |= ST1_EN;
      break;
    }
  }

  m_result[0] = st0;
  m_result[1] = st1;
  m_result[2] = st2;
  setResultCHRN(cyl, idHead, sector, n);
  finish(DELAY_RW, 7);
}


// READ ID: the first ID field that comes past. With no rotation, that is sector 1.
void FDC765::readID()
{
  int drive = m_cmd[1] & 3;
  int head  = (m_cmd[1] >> 2) & 1;
  int cyls, heads, spt;

  m_result[1] = m_result[2] = 0;
  if (!driveGeometry(drive, &cyls, &heads, &spt)) {
    m_result[0] = ST0_IC_ABNORMAL | ST0_NR | (m_cmd[1] & 7);
    m_result[1] = ST1_MA;
  } else if (head >= heads || m_pcn[drive] >= cyls) {
    m_result[0] = ST0_IC_ABNORMAL | (m_cmd[1] & 7);
    m_result[1] = ST1_MA;
  } else
    m_result[0] = m_cmd[1] & 7;
  setResultCHRN(m_pcn[drive], head, 1, 2);
  finish(DELAY_ID, 7);
}


// FORMAT TRACK: cmd, HD/US, N, SC, GPL, D. The DMA supplies C, H, R, N for each of the SC
// sectors, and each sector is filled with D. The track is the drive's present cylinder, as on the
// real controller, since the format command has no cylinder of its own.
void FDC765::formatTrack()
{
  int drive = m_cmd[1] & 3;
  int head  = (m_cmd[1] >> 2) & 1;
  int n     = m_cmd[2];
  int sc    = m_cmd[3];
  int fill  = m_cmd[5];
  int cyls, heads, spt;

  uint8_t st0 = m_cmd[1] & 7, st1 = 0;
  int cyl = m_pcn[drive];

  if (!driveGeometry(drive, &cyls, &heads, &spt)) {
    st0 |= ST0_IC_ABNORMAL | ST0_NR;
  } else if (!(m_dor & DOR_DMA) || n != 2) {
    st0 |= ST0_IC_ABNORMAL;
    st1 |= ST1_OR;
  } else {
    FILE * file = m_machine->disk(drive);
    uint8_t buf[SECTOR_SIZE];
    memset(buf, fill, SECTOR_SIZE);
    m_machine->graphicsAdapter()->driveActivity(drive);
    for (int i = 0; i < sc; ++i) {
      uint8_t id[4];
      bool tc = false;
      if (m_machine->dmaFromMemory(2, id, 4, &tc) < 4)
        break;
      int r = id[2];
      // the sector numbers come from the ID list; ones the image has no room for are skipped
      if (r >= 1 && r <= spt && cyl < cyls && head < heads) {
        uint32_t pos = ((uint32_t)(cyl * heads + head) * spt + (r - 1)) * SECTOR_SIZE;
        if (fseek(file, pos, SEEK_SET) != 0 || fwrite(buf, 1, SECTOR_SIZE, file) != SECTOR_SIZE) {
          st0 |= ST0_IC_ABNORMAL;
          st1 |= ST1_NW;
          break;
        }
      }
      if (tc)
        break;
    }
  }

  m_result[0] = st0;
  m_result[1] = st1;
  m_result[2] = 0;
  setResultCHRN(cyl, head, 1, n);
  finish(DELAY_RW, 7);
}
