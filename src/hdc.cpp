#include "hdc.h"
#include "machine.h"

#include <stdio.h>
#include <string.h>


// 321h, status
#define ST_REQ        0x01      // a byte is wanted or ready at 320h
#define ST_IO         0x02      // direction: 1 = controller to CPU
#define ST_CD         0x04      // 1 = command or status byte, 0 = data
#define ST_BUSY       0x08
#define ST_INT        0x20      // the controller has interrupted

// 323h
#define MASK_DMA      0x01
#define MASK_IRQ      0x02

// error codes, in the completion byte's command (bit 1) and the first sense byte
#define ERR_NONE           0x00
#define ERR_NOT_READY      0x04
#define ERR_NOT_FOUND      0x14   // target sector not found
#define ERR_BAD_COMMAND    0x20
#define ERR_BAD_ADDRESS    0x21   // illegal disk address

// From the command block to the start of the work. Long enough for a driver that sends the block
// and only then enables DMA or starts waiting for the interrupt.
constexpr int DELAY_COMMAND = 400;

constexpr int SECTOR_SIZE = 512;


void XTHDC::reset()
{
  m_phase      = Phase::Idle;
  m_mask       = 0;
  m_intPending = false;
  m_delay      = 0;
  m_cmdPos     = 0;
  m_bufLen     = 0;
  m_bufPos     = 0;
  m_error      = ERR_NONE;
  m_senseValid = false;
  m_senseDrive = m_senseHead = m_senseSector = 0;
  m_senseCyl   = 0;
}


// hd0 and hd1 are the controller's drives 0 and 1
bool XTHDC::driveGeometry(int drive, int * cyls, int * heads, int * spt)
{
  if (drive > 1 || !m_machine->disk(2 + drive))
    return false;
  *cyls  = m_machine->diskCylinders(2 + drive);
  *heads = m_machine->diskHeads(2 + drive);
  *spt   = m_machine->diskSectors(2 + drive);
  return *cyls > 0 && *heads > 0 && *spt > 0;
}


void XTHDC::write(int port, uint8_t value)
{
  switch (port) {

    case 0x320:
      if (m_phase == Phase::Command) {
        m_cmd[m_cmdPos++] = value;
        if (m_cmdPos == sizeof(m_cmd)) {
          m_phase = Phase::Busy;
          m_delay = DELAY_COMMAND;
        }
      } else if (m_phase == Phase::DataOut) {
        m_buf[m_bufPos++] = value;
        if (m_bufPos >= m_bufLen)
          dataDone();
      }
      break;

    case 0x321:
      reset();
      break;

    // select: the controller takes a new command block, whatever it was doing
    case 0x322:
      m_phase      = Phase::Command;
      m_cmdPos     = 0;
      m_delay      = 0;
      m_intPending = false;
      break;

    case 0x323:
      m_mask = value;
      break;
  }
}


uint8_t XTHDC::read(int port)
{
  switch (port) {

    case 0x320:
      if (m_phase == Phase::DataIn) {
        uint8_t v = m_buf[m_bufPos++];
        if (m_bufPos >= m_bufLen)
          dataDone();
        return v;
      }
      if (m_phase == Phase::Status) {
        // the completion byte: bit 1 error, bit 5 the drive. Reading it ends the command.
        m_phase      = Phase::Idle;
        m_intPending = false;
        return (m_error ? 0x02 : 0x00) | (m_drive << 5);
      }
      return 0xff;

    case 0x321:
    {
      uint8_t st = m_intPending ? ST_INT : 0;
      switch (m_phase) {
        case Phase::Idle:    break;
        case Phase::Command: st |= ST_BUSY | ST_CD | ST_REQ;         break;
        case Phase::Busy:    st |= ST_BUSY;                          break;
        case Phase::DataOut: st |= ST_BUSY | ST_REQ;                 break;
        case Phase::DataIn:  st |= ST_BUSY | ST_REQ | ST_IO;         break;
        case Phase::Status:  st |= ST_BUSY | ST_REQ | ST_IO | ST_CD; break;
      }
      return st;
    }

    // the drive type switches, two bits a drive: type 0 for both. The geometry a driver uses comes
    // from the BIOS's parameter table at INT 41h, which follows the image.
    case 0x322:
      return 0x00;

    default:
      return 0xff;
  }
}


// the command is over: the completion byte is ready, and the interrupt with it if enabled
void XTHDC::endCommand(uint8_t error)
{
  m_error = error;
  if (error) {
    m_senseValid  = true;
    m_senseDrive  = m_drive;
    m_senseHead   = m_head;
    m_senseCyl    = m_cyl;
    m_senseSector = m_sector;
  }
  m_phase = Phase::Status;
  if (m_mask & MASK_IRQ) {
    m_intPending = true;
    m_machine->hdcInterrupt();
  }
}


void XTHDC::dataPhase(Phase phase, int bytes)
{
  m_phase  = phase;
  m_bufLen = bytes;
  m_bufPos = 0;
}


// where the current sector is in the image, or the error that says why there is none
uint8_t XTHDC::checkPosition(uint32_t * pos)
{
  int cyls, heads, spt;
  if (!driveGeometry(m_drive, &cyls, &heads, &spt))
    return ERR_NOT_READY;
  if (m_cyl >= cyls || m_head >= heads)
    return ERR_BAD_ADDRESS;
  if (m_sector >= spt)
    return ERR_NOT_FOUND;
  *pos = ((uint32_t)(m_cyl * heads + m_head) * spt + m_sector) * SECTOR_SIZE;
  return ERR_NONE;
}


// a multi-sector command runs on over the heads and then the cylinders, as the controller does
void XTHDC::advance()
{
  int cyls, heads, spt;
  if (!driveGeometry(m_drive, &cyls, &heads, &spt))
    return;
  if (++m_sector >= spt) {
    m_sector = 0;
    if (++m_head >= heads) {
      m_head = 0;
      ++m_cyl;
    }
  }
}


// one sector from the image into the buffer, for a read without DMA; on an error the command ends
// there instead
void XTHDC::sectorIn()
{
  uint32_t pos;
  uint8_t  err = checkPosition(&pos);
  FILE *   file = m_machine->disk(2 + m_drive);
  if (!err && (fseek(file, pos, SEEK_SET) != 0 || fread(m_buf, 1, SECTOR_SIZE, file) != SECTOR_SIZE))
    err = ERR_NOT_FOUND;
  if (err) {
    endCommand(err);
    return;
  }
  dataPhase(Phase::DataIn, SECTOR_SIZE);
}


// the buffer to the image at the current sector, for a write without DMA
bool XTHDC::sectorOut()
{
  uint32_t pos;
  uint8_t  err = checkPosition(&pos);
  FILE *   file = m_machine->disk(2 + m_drive);
  if (!err && (fseek(file, pos, SEEK_SET) != 0 || fwrite(m_buf, 1, SECTOR_SIZE, file) != SECTOR_SIZE))
    err = ERR_NOT_FOUND;
  if (err) {
    endCommand(err);
    return false;
  }
  return true;
}


// a whole read or write through DMA channel 3, then the completion
void XTHDC::transferDMA(bool write)
{
  FILE * file = m_machine->disk(2 + m_drive);
  m_machine->graphicsAdapter()->driveActivity(2 + m_drive);
  for (; m_count > 0; --m_count) {
    uint32_t pos;
    uint8_t  err = checkPosition(&pos);
    if (err) {
      endCommand(err);
      return;
    }
    bool tc = false;
    if (write) {
      int got = m_machine->dmaFromMemory(3, m_buf, SECTOR_SIZE, &tc);
      if (got < SECTOR_SIZE)
        memset(m_buf + got, 0, SECTOR_SIZE - got);
      if (fseek(file, pos, SEEK_SET) != 0 || fwrite(m_buf, 1, SECTOR_SIZE, file) != SECTOR_SIZE) {
        endCommand(ERR_NOT_FOUND);
        return;
      }
    } else {
      if (fseek(file, pos, SEEK_SET) != 0 || fread(m_buf, 1, SECTOR_SIZE, file) != SECTOR_SIZE) {
        endCommand(ERR_NOT_FOUND);
        return;
      }
      m_machine->dmaToMemory(3, m_buf, SECTOR_SIZE, &tc);
    }
    advance();
    if (tc && m_count > 1)
      break;      // the DMA was programmed for less than the command asked: stop where it did
  }
  endCommand(ERR_NONE);
}


// the command block has had its time: do it
void XTHDC::execute()
{
  m_drive  = (m_cmd[1] >> 5) & 1;
  m_head   = m_cmd[1] & 0x1f;
  m_cyl    = ((m_cmd[2] & 0xc0) << 2) | m_cmd[3];
  m_sector = m_cmd[2] & 0x3f;
  m_count  = m_cmd[4] ? m_cmd[4] : 256;

  int  cyls, heads, spt;
  bool present = driveGeometry(m_drive, &cyls, &heads, &spt);
  bool dma     = m_mask & MASK_DMA;

  switch (m_cmd[0]) {

    // TEST DRIVE READY, RECALIBRATE, the diagnostics
    case 0x00:
    case 0x01:
    case 0xe0:
    case 0xe3:
    case 0xe4:
      endCommand(present ? ERR_NONE : ERR_NOT_READY);
      break;

    // REQUEST SENSE: four bytes, error and the address it happened at. Always through 320h.
    case 0x03:
      m_buf[0] = m_error | (m_senseValid ? 0x80 : 0x00);
      m_buf[1] = (m_senseDrive << 5) | m_senseHead;
      m_buf[2] = ((m_senseCyl >> 2) & 0xc0) | m_senseSector;
      m_buf[3] = m_senseCyl & 0xff;
      dataPhase(Phase::DataIn, 4);
      break;

    // FORMAT DRIVE, FORMAT BAD TRACK: nothing to do to an image. Erasing all of it would hold the
    // emulator for as long as the SD card takes to write the whole image, and the data on a freshly
    // formatted disk is not something software relies on.
    case 0x04:
    case 0x07:
      endCommand(present ? ERR_NONE : ERR_NOT_READY);
      break;

    // READ VERIFY: whether the sectors exist
    case 0x05:
    {
      uint8_t err = ERR_NONE;
      for (; m_count > 0 && !err; --m_count) {
        uint32_t pos;
        err = checkPosition(&pos);
        if (!err)
          advance();
      }
      endCommand(err);
      break;
    }

    // FORMAT TRACK: the track's sectors are cleared, which is cheap and what a later read expects
    case 0x06:
    {
      if (!present) {
        endCommand(ERR_NOT_READY);
        break;
      }
      memset(m_buf, 0, SECTOR_SIZE);
      m_sector = 0;
      uint8_t err = ERR_NONE;
      for (int s = 0; s < spt && !err; ++s) {
        m_sector = s;
        uint32_t pos;
        err = checkPosition(&pos);
        FILE * file = m_machine->disk(2 + m_drive);
        if (!err && (fseek(file, pos, SEEK_SET) != 0 || fwrite(m_buf, 1, SECTOR_SIZE, file) != SECTOR_SIZE))
          err = ERR_NOT_FOUND;
      }
      m_sector = 0;
      endCommand(err);
      break;
    }

    // READ
    case 0x08:
      if (!present)
        endCommand(ERR_NOT_READY);
      else if (dma)
        transferDMA(false);
      else {
        m_machine->graphicsAdapter()->driveActivity(2 + m_drive);
        sectorIn();
      }
      break;

    // WRITE
    case 0x0a:
      if (!present)
        endCommand(ERR_NOT_READY);
      else if (dma)
        transferDMA(true);
      else {
        m_machine->graphicsAdapter()->driveActivity(2 + m_drive);
        dataPhase(Phase::DataOut, SECTOR_SIZE);
      }
      break;

    // SEEK
    case 0x0b:
      endCommand(!present ? ERR_NOT_READY : (m_cyl >= cyls ? ERR_BAD_ADDRESS : ERR_NONE));
      break;

    // INITIALIZE DRIVE CHARACTERISTICS: eight bytes, cylinders, heads, reduced write current and
    // precompensation cylinders, ECC burst. Taken and not used: the geometry is the image's, which
    // is also what the BIOS's parameter table gives the driver to send here.
    case 0x0c:
      dataPhase(Phase::DataOut, 8);
      break;

    // READ ECC BURST LENGTH: there never is an error to correct
    case 0x0d:
      m_buf[0] = 0;
      dataPhase(Phase::DataIn, 1);
      break;

    // READ SECTOR BUFFER, WRITE SECTOR BUFFER
    case 0x0e:
      if (dma) {
        bool tc;
        m_machine->dmaToMemory(3, m_buf, SECTOR_SIZE, &tc);
        endCommand(ERR_NONE);
      } else
        dataPhase(Phase::DataIn, SECTOR_SIZE);
      break;
    case 0x0f:
      if (dma) {
        bool tc;
        m_machine->dmaFromMemory(3, m_buf, SECTOR_SIZE, &tc);
        endCommand(ERR_NONE);
      } else
        dataPhase(Phase::DataOut, SECTOR_SIZE);
      break;

    default:
      endCommand(ERR_BAD_COMMAND);
      break;
  }
}


// the last byte of a data phase through 320h has gone across
void XTHDC::dataDone()
{
  switch (m_cmd[0]) {

    // a read without DMA: the next sector, or the end
    case 0x08:
      advance();
      if (--m_count > 0)
        sectorIn();
      else
        endCommand(ERR_NONE);
      break;

    // a write without DMA: this sector to the image, then the next
    case 0x0a:
      if (!sectorOut())
        return;
      advance();
      if (--m_count > 0)
        dataPhase(Phase::DataOut, SECTOR_SIZE);
      else
        endCommand(ERR_NONE);
      break;

    // REQUEST SENSE: read, so the error is reported and gone
    case 0x03:
      m_error      = ERR_NONE;
      m_senseValid = false;
      m_phase      = Phase::Status;
      if (m_mask & MASK_IRQ) {
        m_intPending = true;
        m_machine->hdcInterrupt();
      }
      break;

    default:
      endCommand(ERR_NONE);
      break;
  }
}
