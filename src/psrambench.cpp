/*
  PSRAM benchmark for the proposed LIM-EMS card.

  Not part of the emulator. Build and run with:

      pio run -e psrambench -t upload
      <read the serial console at 115200>

  The EMS design bank-switches by copying: when the guest remaps an expanded-memory page, a
  16 KB page is copied between the PSRAM-backed EMS store and the guest RAM page frame. That
  keeps the CPU core untouched and costs nothing on ordinary memory accesses, but it means a
  remap costs up to 32 KB of PSRAM-to-PSRAM copying (write the outgoing page back, read the
  incoming one in). This measures that number before we build a card on top of the assumption.

  It also measures raw sequential PSRAM read/write bandwidth, which is the figure that decides
  whether a larger video memory (EGA/VGA) could ever live in PSRAM instead of internal RAM.

  Pages are cycled across the whole 512 KB store rather than hammering one buffer, so the
  32 KB PSRAM cache cannot hide the real cost.
*/

#include <Arduino.h>

#include "esp32-hal-psram.h"
extern "C" {
#include "esp_spiram.h"
}
#include "esp_timer.h"
#include "esp_heap_caps.h"


static constexpr size_t KB        = 1024;
static constexpr size_t MB        = 1024 * 1024;

static constexpr size_t RAM_SIZE  = 1 * MB;      // guest RAM, as in machine.h
static constexpr size_t EMS_SIZE  = 512 * KB;    // proposed EMS store
static constexpr size_t EMS_PAGE  = 16 * KB;     // LIM page size
static constexpr int    EMS_PAGES = EMS_SIZE / EMS_PAGE;   // 32

// page frame lives in the UMA hole at 0xD0000, inside guest RAM
static constexpr size_t FRAME_OFF = 0xD0000;


// mirrors Machine::init(): core 0 may only use the lower 2 MB, core 1 only the higher 2 MB
static uint8_t * psramBase()
{
  return (uint8_t *)(SOC_EXTRAM_DATA_LOW + (xPortGetCoreID() == 1 ? 2 * MB : 0));
}


static void report(char const * what, int64_t usec, size_t totalBytes, int ops)
{
  double mbs = (double)totalBytes / (double)usec;         // bytes/us == MB/s
  printf("  %-38s %8.1f MB/s   %8.1f us/op\n", what, mbs, (double)usec / ops);
}


static void benchTask(void * arg)
{
  int core = xPortGetCoreID();
  uint8_t * base  = psramBase();
  uint8_t * guest = base;                      // guest RAM     [base, base+1MB)
  uint8_t * frame = guest + FRAME_OFF;         // EMS page frame inside guest RAM
  uint8_t * store = base + RAM_SIZE;           // EMS store     [base+1MB, base+1.5MB)

  printf("\n=== core %d ===\n", core);
  printf("  psram base  %p\n", base);
  printf("  guest RAM   %p .. %p\n", guest, guest + RAM_SIZE);
  printf("  page frame  %p (guest 0x%05X)\n", frame, (unsigned)FRAME_OFF);
  printf("  EMS store   %p .. %p  (%d pages of %u KB)\n\n",
         store, store + EMS_SIZE, EMS_PAGES, (unsigned)(EMS_PAGE / KB));

  // touch everything once so nothing is measuring first-access effects
  memset(store, 0x5a, EMS_SIZE);
  memset(frame, 0x00, EMS_PAGE);

  constexpr int ROUNDS = 200;   // 200 * 32 pages = 6400 page copies

  // --- 1. page in: EMS store -> page frame (both PSRAM), cycling all 32 pages ---
  int64_t t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int p = 0; p < EMS_PAGES; ++p)
      memcpy(frame, store + p * EMS_PAGE, EMS_PAGE);
  int64_t t1 = esp_timer_get_time();
  report("page in  (16KB psram->psram)", t1 - t0, (size_t)ROUNDS * EMS_PAGES * EMS_PAGE,
         ROUNDS * EMS_PAGES);

  // --- 2. page out: page frame -> EMS store ---
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int p = 0; p < EMS_PAGES; ++p)
      memcpy(store + p * EMS_PAGE, frame, EMS_PAGE);
  t1 = esp_timer_get_time();
  report("page out (16KB psram->psram)", t1 - t0, (size_t)ROUNDS * EMS_PAGES * EMS_PAGE,
         ROUNDS * EMS_PAGES);

  // --- 3. the real cost of one EMS remap: write back + read in ---
  t0 = esp_timer_get_time();
  for (int r = 0; r < ROUNDS; ++r)
    for (int p = 0; p < EMS_PAGES; ++p) {
      memcpy(store + p * EMS_PAGE, frame, EMS_PAGE);                    // evict
      memcpy(frame, store + ((p + 1) % EMS_PAGES) * EMS_PAGE, EMS_PAGE); // load
    }
  t1 = esp_timer_get_time();
  report("FULL REMAP (32KB: out + in)", t1 - t0, (size_t)ROUNDS * EMS_PAGES * EMS_PAGE * 2,
         ROUNDS * EMS_PAGES);

  // --- 4. reference: sequential PSRAM read (the number that matters for EGA/VGA video RAM) ---
  volatile uint32_t sink = 0;
  t0 = esp_timer_get_time();
  for (int r = 0; r < 20; ++r) {
    uint32_t const * p = (uint32_t const *)store;
    for (size_t i = 0; i < EMS_SIZE / 4; ++i)
      sink += p[i];
  }
  t1 = esp_timer_get_time();
  report("psram sequential read", t1 - t0, (size_t)20 * EMS_SIZE, 20);
  (void)sink;

  // --- 5. reference: sequential PSRAM write ---
  t0 = esp_timer_get_time();
  for (int r = 0; r < 20; ++r)
    memset(store, r, EMS_SIZE);
  t1 = esp_timer_get_time();
  report("psram sequential write (memset)", t1 - t0, (size_t)20 * EMS_SIZE, 20);

  // --- 6. reference: internal RAM copy, to show the PSRAM penalty ---
  uint8_t * ia = (uint8_t *)heap_caps_malloc(16 * KB, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  uint8_t * ib = (uint8_t *)heap_caps_malloc(16 * KB, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (ia && ib) {
    memset(ia, 0x33, 16 * KB);
    t0 = esp_timer_get_time();
    for (int r = 0; r < ROUNDS * EMS_PAGES; ++r)
      memcpy(ib, ia, 16 * KB);
    t1 = esp_timer_get_time();
    report("reference: 16KB internal->internal", t1 - t0,
           (size_t)ROUNDS * EMS_PAGES * 16 * KB, ROUNDS * EMS_PAGES);
  } else
    printf("  (internal RAM reference skipped: allocation failed)\n");
  free(ia);
  free(ib);

  printf("\n  -> a 16KB page copy is the cost of one EMS page map (fn 44h).\n");
  printf("  -> FULL REMAP is the worst case when the outgoing page must be written back.\n\n");

  xSemaphoreGive((SemaphoreHandle_t)arg);
  vTaskDelete(NULL);
}


void setup()
{
  Serial.begin(115200);
  delay(500);

  disableCore0WDT();
  delay(100);
  disableCore1WDT();

  printf("\n\n\nPSRAM benchmark for the LIM-EMS design\n");
  printf("======================================\n");

  if (esp_spiram_init() != ESP_OK) {
    printf("esp_spiram_init() FAILED - no PSRAM?\n");
    return;
  }
  #ifndef BOARD_HAS_PSRAM
  esp_spiram_init_cache();
  #endif

  printf("PSRAM size: %u bytes (%.1f MB)\n", (unsigned)esp_spiram_get_size(),
         esp_spiram_get_size() / 1048576.0);

  // Run on both cores: each may only touch its own 2 MB half. Strictly one at a time and
  // waiting on a semaphore, never a fixed delay -- two cores hammering PSRAM at once contend
  // for the same bus and roughly halve every figure, which silently poisons the results.
  TaskHandle_t h;
  SemaphoreHandle_t done = xSemaphoreCreateBinary();

  xTaskCreatePinnedToCore(benchTask, "bench0", 4000, done, 5, &h, 0);
  xSemaphoreTake(done, portMAX_DELAY);

  xTaskCreatePinnedToCore(benchTask, "bench1", 4000, done, 5, &h, 1);
  xSemaphoreTake(done, portMAX_DELAY);

  printf("done.\n");
}


void loop()
{
  vTaskDelete(NULL);
}
