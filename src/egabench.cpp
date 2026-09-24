/*
  EGA planar path benchmark -- the spike that decides whether EGA is worth building.

  Not part of the emulator. Build and run with:

      pio run -e egabench -t upload
      <read the serial console at 115200>

  The question this answers: an EGA stores pixels in four bit planes behind the Graphics
  Controller, so a guest write to 0xA0000 cannot be a store into a linear buffer the way CGA is
  today. It has to go through latches, set/reset, an ALU, a bit mask and the Sequencer's map mask.
  Windows 3.0's EGA driver does every blit through that path, so if a planar write is dramatically
  more expensive than today's linear write, EGA is not worth building on this board.

  So: measure the cost of one planar access against the cost of today's linear video write, on the
  real hardware, and convert both into the time a full-screen repaint would take.

  It also measures the proposed scanline renderer's inner loop, which has to fit inside the VGA
  ISR's per-line budget.
 */

#include <Arduino.h>

#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "esp32-hal-psram.h"
extern "C" {
#include "esp_spiram.h"
}

#include "emudevs/i8086.h"

#include "ega.h"

using fabgl::i8086;


static constexpr int MODE10_WIDTH   = 640;
static constexpr int MODE10_HEIGHT  = 350;
static constexpr int MODE10_STRIDE  = MODE10_WIDTH / 8;                    // 80 byte-columns
static constexpr int MODE10_BYTES   = MODE10_STRIDE * MODE10_HEIGHT;       // 28000 per plane

// one full-screen repaint in mode 10h touches this many byte-columns
static constexpr int FULLSCREEN_OPS = MODE10_BYTES;


static EGA * ega;

// today's path, for the baseline: a plain linear byte buffer like Machine::s_videoMemory
static uint8_t * linearVideo;

// renderer lookup tables
static uint32_t * spread[4];      // plane byte -> 8 packed 4-bit pixel indices, swizzle baked in
static uint16_t * pal2;           // two packed indices -> two raw VGA pixel bytes
static uint8_t  * scanlineBuf;    // one 640 byte scanline, as the VGA ISR would hand us


static void report(char const * what, int64_t usec, int ops)
{
  double nsPerOp = (double)usec * 1000.0 / (double)ops;
  // what a 640x350 full-screen repaint would cost at this rate
  double fullscreenMs = nsPerOp * FULLSCREEN_OPS / 1e6;
  printf("  %-44s %7.1f ns/op   fullscreen %7.2f ms  (%5.1f fps)\n",
         what, nsPerOp, fullscreenMs, 1000.0 / fullscreenMs);
}


// ---- Graphics Controller / Sequencer setup helpers -------------------------------------------

static void gc(uint8_t index, uint8_t value)
{
  ega->selectGraphics(index);
  ega->writeGraphics(value);
}

static void seq(uint8_t index, uint8_t value)
{
  ega->selectSequencer(index);
  ega->writeSequencer(value);
}

// the register state a normal "just write pixels" mode 10h program leaves behind
static void setupPlainWriteMode0()
{
  gc(0, 0x00);   // Set/Reset
  gc(1, 0x00);   // Enable Set/Reset: none, so data comes from the CPU
  gc(3, 0x00);   // rotate 0, ALU = replace
  gc(5, 0x00);   // write mode 0, read mode 0
  gc(8, 0xff);   // Bit Mask: all bits from the CPU
  seq(2, 0x0f);  // Map Mask: all four planes
}

// the register state Windows 3.0 uses for a masked colour fill: the colour comes from Set/Reset
// and only the bits inside the Bit Mask are touched, so every write is a read-modify-write
static void setupSetResetMasked()
{
  gc(0, 0x09);   // Set/Reset = colour 9 (light blue)
  gc(1, 0x0f);   // Enable Set/Reset on all four planes
  gc(3, 0x00);   // rotate 0, ALU = replace
  gc(5, 0x00);   // write mode 0
  gc(8, 0x3c);   // Bit Mask: a partial mask, the expensive case
  seq(2, 0x0f);
}

static void setupWriteMode2()
{
  gc(1, 0x00);
  gc(3, 0x00);
  gc(5, 0x02);   // write mode 2: low nibble of the byte is the colour
  gc(8, 0xff);
  seq(2, 0x0f);
}

static void setupWriteMode1()
{
  gc(5, 0x01);   // write mode 1: straight latch copy, the fast screen-to-screen blit
  seq(2, 0x0f);
}


// ---- renderer LUT construction ---------------------------------------------------------------

// A fake "raw pixel" builder. The real one is VGADirectController::createRawPixel(), which ORs the
// sync bits with 2 bits per colour channel. The exact value is irrelevant to the timing; what
// matters is that the renderer does the same number of lookups and stores.
static uint8_t fakeRawPixel(int index)
{
  return (uint8_t)(0xc0 | (index * 3));
}

static void buildRendererLUTs()
{
  for (int p = 0; p < 4; ++p) {
    spread[p] = (uint32_t*) heap_caps_malloc(256 * sizeof(uint32_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    for (int b = 0; b < 256; ++b) {
      uint32_t v = 0;
      for (int k = 0; k < 8; ++k)
        if ((b >> (7 - k)) & 1)
          // pixel k goes to output byte (k ^ 2): FabGL's scanline byte order is swizzled, and
          // baking it into the table keeps the inner loop to plain sequential stores
          v |= 1u << (4 * (k ^ 2) + p);
      spread[p][b] = v;
    }
  }

  pal2 = (uint16_t*) heap_caps_malloc(256 * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  for (int b = 0; b < 256; ++b)
    pal2[b] = (uint16_t)fakeRawPixel(b & 0x0f) | ((uint16_t)fakeRawPixel(b >> 4) << 8);
}


// the proposed inner loop: one 32-bit load gets all four planes of eight pixels
static void IRAM_ATTR renderScanline(uint32_t const * src, uint8_t * dest)
{
  uint32_t const * L0 = spread[0];
  uint32_t const * L1 = spread[1];
  uint32_t const * L2 = spread[2];
  uint32_t const * L3 = spread[3];
  uint16_t const * P  = pal2;

  auto d16 = (uint16_t*) dest;

  for (int col = 0; col < MODE10_STRIDE; ++col) {
    uint32_t w = *src++;
    uint32_t i = L0[w & 0xff] | L1[(w >> 8) & 0xff] | L2[(w >> 16) & 0xff] | L3[w >> 24];
    d16[0] = P[i & 0xff];
    d16[1] = P[(i >> 8) & 0xff];
    d16[2] = P[(i >> 16) & 0xff];
    d16[3] = P[i >> 24];
    d16 += 4;
  }
}




// ---- 8086 interpreter harness -----------------------------------------------------------------
//
// The planar numbers above are only half the story. A guest cannot issue writes faster than the
// interpreter can decode the instructions that make them, so what decides whether EGA is usable is
// the planar cost *relative to the cost of interpreting the instruction around it*. This runs the
// real fabgl::i8086 core over a real "rep stosw" and times it with ES pointing first at ordinary
// RAM and then at the EGA window, which is exactly the comparison a guest program experiences.

static uint8_t * guestRAM;

static uint8_t benchReadPort(void *, int)             { return 0xff; }
static void    benchWritePort(void *, int, uint8_t)   { }
static bool    benchInterrupt(void *, int)            { return false; }

// routed to the EGA planar engine
static void    egaWriteVideo8(void *, int addr, uint8_t value)    { ega->write8(addr - 0xa0000, value); }
static void    egaWriteVideo16(void *, int addr, uint16_t value)  { ega->write16(addr - 0xa0000, value); }
static uint8_t egaReadVideo8(void *, int addr)                    { return ega->read8(addr - 0xa0000); }
static uint16_t egaReadVideo16(void *, int addr)                  { return ega->read16(addr - 0xa0000); }

// what Machine::writeVideoMemory8() does today: a straight store into a linear buffer
static void    linWriteVideo8(void *, int addr, uint8_t value)    { if (addr >= 0xb0000) linearVideo[addr - 0xb0000] = value; }
static void    linWriteVideo16(void *, int addr, uint16_t value)  { if (addr >= 0xb0000) *(uint16_t*)(linearVideo + addr - 0xb0000) = value; }
static uint8_t linReadVideo8(void *, int addr)                    { return addr >= 0xb0000 ? linearVideo[addr - 0xb0000] : 0xff; }
static uint16_t linReadVideo16(void *, int addr)                  { return addr >= 0xb0000 ? *(uint16_t*)(linearVideo + addr - 0xb0000) : 0xffff; }

static void useEGACallbacks()
{
  i8086::setCallbacks(nullptr, benchReadPort, benchWritePort,
                      egaWriteVideo8, egaWriteVideo16, egaReadVideo8, egaReadVideo16, benchInterrupt);
}

static void useLinearCallbacks()
{
  i8086::setCallbacks(nullptr, benchReadPort, benchWritePort,
                      linWriteVideo8, linWriteVideo16, linReadVideo8, linReadVideo16, benchInterrupt);
}

// runs "cld; rep stosw" filling 'words' words at destSeg:0000, returns microseconds
static int64_t timeRepStosw(uint16_t destSeg, int words)
{
  // code at 0x1000:0000
  constexpr uint32_t CODE = 0x10000;
  uint8_t * c = guestRAM + CODE;
  int n = 0;
  c[n++] = 0xfc;                      // cld
  c[n++] = 0xf3; c[n++] = 0xab;       // rep stosw

  i8086::reset();
  i8086::setCS(0x1000);
  i8086::setIP(0x0000);
  i8086::setES(destSeg);
  i8086::setAX(0x1234);
  i8086::setCX((uint16_t)words);
  i8086::setDI(0x0000);

  int64_t t0 = esp_timer_get_time();
  // cld, then one step per rep iteration: the core rewinds IP and decrements CX itself
  i8086::step();
  while (i8086::CX() != 0)
    i8086::step();
  return esp_timer_get_time() - t0;
}

static void runInterpreterBench()
{
  printf("\n  --- 8086 interpreter, the denominator ---\n\n");

  if (esp_spiram_init() != ESP_OK) {
    printf("  PSRAM init failed, skipping interpreter benchmark\n");
    return;
  }
  #ifndef BOARD_HAS_PSRAM
  esp_spiram_init_cache();
  #endif

  // same core/PSRAM split the emulator uses (Machine::init)
  guestRAM = (uint8_t*)(SOC_EXTRAM_DATA_LOW + (xPortGetCoreID() == 1 ? 2 * 1024 * 1024 : 0));
  memset(guestRAM, 0, 1024 * 1024);

  i8086::setMemory(guestRAM);
  i8086::setVideoMemStart(0xa0000);

  constexpr int WORDS = 14000;        // 28000 bytes = one mode 10h screen

  // destination 0x2000:0000 is ordinary RAM, so this is the interpreter cost with no video path
  useLinearCallbacks();
  int64_t tRAM = timeRepStosw(0x2000, WORDS);

  // 0xb800:0000 through the linear callbacks: exactly what the emulator does today for CGA
  i8086::setVideoMemStart(0xb0000);
  int64_t tCGA = timeRepStosw(0xb800, WORDS);

  // 0xa000:0000 through the EGA planar engine
  i8086::setVideoMemStart(0xa0000);
  useEGACallbacks();
  setupPlainWriteMode0();
  int64_t tEGA = timeRepStosw(0xa000, WORDS);

  // and the Windows masked read-modify-write pattern, driven by real instructions:
  // "rep stosw" with Set/Reset supplying the colour and a partial Bit Mask
  setupSetResetMasked();
  int64_t tEGAMasked = timeRepStosw(0xa000, WORDS);

  printf("  rep stosw, %d words (one mode 10h screen):\n\n", WORDS);
  printf("  %-44s %8.2f ms  %7.1f ns/word\n", "to ordinary RAM (interpreter only)",
         tRAM / 1000.0, (double)tRAM * 1000.0 / WORDS);
  printf("  %-44s %8.2f ms  %7.1f ns/word\n", "to CGA linear video memory (today)",
         tCGA / 1000.0, (double)tCGA * 1000.0 / WORDS);
  printf("  %-44s %8.2f ms  %7.1f ns/word\n", "to EGA planar video memory",
         tEGA / 1000.0, (double)tEGA * 1000.0 / WORDS);
  printf("  %-44s %8.2f ms  %7.1f ns/word\n", "to EGA, Set/Reset + Bit Mask (Windows)",
         tEGAMasked / 1000.0, (double)tEGAMasked * 1000.0 / WORDS);
  printf("\n  EGA costs %.2fx a CGA-style screen fill, and %.2fx a plain RAM fill.\n",
         (double)tEGA / (double)tCGA, (double)tEGA / (double)tRAM);
  printf("  Effective guest fill rate: %.2f MB/s (CGA today: %.2f MB/s)\n",
         WORDS * 2.0 / tEGA, WORDS * 2.0 / tCGA);
}


// ---- correctness check ------------------------------------------------------------------------
//
// The hot path is branchless and mask-driven, which is easy to get subtly wrong in a way that only
// shows up as corrupted pixels inside Windows months later. So check it against the straightforward
// switch-based implementation over random register states before trusting any timing.

static bool validatePlanarEngine()
{
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 16; };

  int failures = 0;

  for (int trial = 0; trial < 20000; ++trial) {
    uint8_t  setReset  = rnd() & 0x0f;
    uint8_t  enableSR  = rnd() & 0x0f;
    uint8_t  rotFunc   = rnd() & 0x1f;      // rotate count + ALU function
    uint8_t  mode      = rnd() & 0x03;      // write mode
    uint8_t  bitMask   = rnd() & 0xff;
    uint8_t  mapMask   = rnd() & 0x0f;
    uint32_t latch     = ((uint32_t)rnd() << 16) | rnd();
    uint32_t initial   = ((uint32_t)rnd() << 16) | rnd();
    uint8_t  value     = rnd() & 0xff;
    uint32_t off       = rnd() & 0x0fff;

    gc(0, setReset);
    gc(1, enableSR);
    gc(3, rotFunc);
    gc(5, mode);
    gc(8, bitMask);
    seq(2, mapMask);

    *ega->bankPtr(off) = initial;
    ega->setLatch(latch);
    ega->write8(off, value);
    uint32_t fast = *ega->bankPtr(off);

    *ega->bankPtr(off) = initial;
    ega->setLatch(latch);
    ega->write8Ref(off, value);
    uint32_t ref = *ega->bankPtr(off);

    if (fast != ref) {
      if (failures < 5)
        printf("  MISMATCH sr=%x esr=%x rot/fn=%02x mode=%d bm=%02x mm=%x val=%02x "
               "lat=%08x init=%08x -> fast=%08x ref=%08x\n",
               setReset, enableSR, rotFunc, mode, bitMask, mapMask, value,
               (unsigned)latch, (unsigned)initial, (unsigned)fast, (unsigned)ref);
      ++failures;
    }
  }

  if (failures)
    printf("  planar engine validation: %d/20000 MISMATCHES\n\n", failures);
  else
    printf("  planar engine validation: 20000/20000 states match the reference\n\n");

  return failures == 0;
}


// ---- the benchmarks ---------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(2000);

  printf("\n\n===== EGA planar path benchmark =====\n\n");

  ega = new EGA;
  if (!ega->allocVRAM(4)) {   // worst case, as mode 10h needs
    printf("  FAILED to allocate EGA VRAM (4 banks of 32 KB internal)\n");
    return;
  }

  linearVideo = (uint8_t*) heap_caps_malloc(65536, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  scanlineBuf = (uint8_t*) heap_caps_malloc(MODE10_WIDTH, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  buildRendererLUTs();

  if (!linearVideo || !scanlineBuf || !pal2 || !spread[3]) {
    printf("  FAILED to allocate benchmark buffers\n");
    return;
  }

  printf("  free internal after allocations: %d bytes (largest %d)\n\n",
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  validatePlanarEngine();

  printf("  one full-screen repaint in mode 10h = %d byte-columns\n", FULLSCREEN_OPS);
  printf("  (the same repaint in CGA 640x200x2 = %d bytes, for scale)\n\n", 640 / 8 * 200);

  constexpr int ROUNDS = 40;
  int64_t t0, t1;

  // --- baseline: today's linear CGA-style video write ---
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int i = 0; i < FULLSCREEN_OPS; ++i)
      linearVideo[i & 0xffff] = (uint8_t)i;
  t1 = esp_timer_get_time();
  report("BASELINE linear write (today's CGA path)", t1 - t0, ROUNDS * FULLSCREEN_OPS);

  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int i = 0; i < FULLSCREEN_OPS; i += 2)
      *(uint16_t*)(linearVideo + (i & 0xfffe)) = (uint16_t)i;
  t1 = esp_timer_get_time();
  report("BASELINE linear write16 (today's CGA path)", t1 - t0, ROUNDS * FULLSCREEN_OPS / 2);

  printf("\n");

  // --- planar writes, by the register state the guest leaves behind ---
  setupPlainWriteMode0();
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int i = 0; i < FULLSCREEN_OPS; ++i)
      ega->write8(i, (uint8_t)i);
  t1 = esp_timer_get_time();
  report("EGA write mode 0, plain (rep stosb fill)", t1 - t0, ROUNDS * FULLSCREEN_OPS);

  setupPlainWriteMode0();
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int i = 0; i < FULLSCREEN_OPS; i += 2)
      ega->write16(i, (uint16_t)i);
  t1 = esp_timer_get_time();
  report("EGA write mode 0, plain, 16 bit (rep stosw)", t1 - t0, ROUNDS * FULLSCREEN_OPS / 2);

  setupWriteMode2();
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int i = 0; i < FULLSCREEN_OPS; ++i)
      ega->write8(i, (uint8_t)i);
  t1 = esp_timer_get_time();
  report("EGA write mode 2 (colour per pixel)", t1 - t0, ROUNDS * FULLSCREEN_OPS);

  setupWriteMode1();
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int i = 0; i < FULLSCREEN_OPS; ++i)
      ega->write8(i, 0);
  t1 = esp_timer_get_time();
  report("EGA write mode 1 (latch copy blit)", t1 - t0, ROUNDS * FULLSCREEN_OPS);

  printf("\n");

  // --- reads ---
  setupPlainWriteMode0();
  volatile uint32_t sink = 0;
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int i = 0; i < FULLSCREEN_OPS; ++i)
      sink += ega->read8(i);
  t1 = esp_timer_get_time();
  report("EGA read mode 0 (loads latches)", t1 - t0, ROUNDS * FULLSCREEN_OPS);

  gc(5, 0x08);   // read mode 1, colour compare
  gc(2, 0x09);
  gc(7, 0x0f);
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int i = 0; i < FULLSCREEN_OPS; ++i)
      sink += ega->read8(i);
  t1 = esp_timer_get_time();
  report("EGA read mode 1 (colour compare)", t1 - t0, ROUNDS * FULLSCREEN_OPS);
  (void)sink;

  printf("\n");

  // --- the one that actually matters: Windows 3.0's masked read-modify-write ---
  setupSetResetMasked();
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int i = 0; i < FULLSCREEN_OPS; ++i) {
      ega->read8(i);              // load the latches
      ega->write8(i, 0xff);       // merge through Set/Reset + Bit Mask
    }
  t1 = esp_timer_get_time();
  report("EGA masked RMW (Windows blit pattern)", t1 - t0, ROUNDS * FULLSCREEN_OPS);

  printf("\n");

  // --- the renderer's inner loop, against the VGA ISR budget ---
  constexpr int LINEROUNDS = 20000;
  auto src = ega->bankPtr(0);
  t0 = esp_timer_get_time();
  for (int r = 0; r < LINEROUNDS; ++r)
    renderScanline(src, scanlineBuf);
  t1 = esp_timer_get_time();
  double usPerLine = (double)(t1 - t0) / LINEROUNDS;
  printf("  %-44s %7.2f us/scanline\n", "RENDERER 640px 16-colour planar", usPerLine);
  printf("  %-44s %7.2f us   (%.0f%% of budget)\n",
         "  vs 640x350@70Hz per-scanline budget", 31.78, usPerLine / 31.78 * 100.0);
  printf("  %-44s %7.2f ms/frame\n", "  whole 350-line frame", usPerLine * 350 / 1000.0);

  runInterpreterBench();

  printf("\n===== done =====\n");
}


void loop()
{
  delay(1000);
}
