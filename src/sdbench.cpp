/*
  SD card write benchmark - not the emulator.

      pio run -e sdbench -t upload
      <read the serial console at 115200>

  Measures how fast the card takes a file written the way the image download writes it, against
  variations: block size, internal (DMA capable) or PSRAM buffer, appending, syncing. The download
  measured about 45 KB/s of card writes with VGA output running; this runs without it, so the two
  together say whether the card path or the rest of the system is the brake.

  Writes /SD/sdbench.tmp (4 MB) and deletes it again.
*/

#include <Arduino.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

extern "C" {
#include "esp_spiram.h"
}
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "fabgl.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"


static constexpr char const * PATH = "/SD/sdbench.tmp";
static constexpr int TOTAL = 1024 * 1024;


static void run(char const * label, uint8_t * buf, int block, bool append, int syncEvery, int startOffset)
{
  remove(PATH);
  if (startOffset) {
    // an unaligned start, as a resumed download has
    int fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    write(fd, buf, startOffset);
    close(fd);
  }
  int fd = open(PATH, O_WRONLY | O_CREAT | (append || startOffset ? O_APPEND : O_TRUNC), 0666);
  if (fd < 0) {
    printf("%-44s cannot open\n", label);
    return;
  }
  int64_t t0 = esp_timer_get_time(), worst = 0;
  int written = 0, sinceSync = 0;
  while (written < TOTAL) {
    int64_t t = esp_timer_get_time();
    if (write(fd, buf, block) != block) {
      printf("%-44s write failed at %d\n", label, written);
      break;
    }
    t = esp_timer_get_time() - t;
    if (t > worst)
      worst = t;
    written   += block;
    sinceSync += block;
    if (syncEvery && sinceSync >= syncEvery) {
      fsync(fd);
      sinceSync = 0;
    }
  }
  close(fd);
  int64_t us = esp_timer_get_time() - t0;
  printf("%-44s %5d KB/s   worst write %4d ms\n", label, (int) ((int64_t) written * 1000000 / us / 1024), (int) (worst / 1000));
}


// as FileBrowser::mountSDCard() does it,
// but with the clock limit as a parameter and the card at hand for its info
static sdmmc_card_t * mount(int freqKHz)
{
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = HSPI_HOST;
  host.max_freq_khz = freqKHz;

  static bool busUp = false;
  if (!busUp) {
    // the chip package moves MISO and MOSI, as in FileBrowser::mountSDCard()
    int miso = 16, mosi = 17;
    switch (fabgl::getChipPackage()) {
      case fabgl::ChipPackage::ESP32PICOD4: miso = 2;  mosi = 12; break;
      case fabgl::ChipPackage::ESP32D0WDQ5: miso = 35; mosi = 12; break;
      default: break;
    }
    printf("pins: MISO %d MOSI %d CLK 14 CS 13\n", miso, mosi);
    spi_bus_config_t bus = { };
    bus.mosi_io_num = mosi;
    bus.miso_io_num = miso;
    bus.sclk_io_num = GPIO_NUM_14;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    spi_bus_initialize(HSPI_HOST, &bus, 2);
    busUp = true;
  }
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = GPIO_NUM_13;
  slot.host_id = HSPI_HOST;

  esp_vfs_fat_sdmmc_mount_config_t mc = { };
  mc.format_if_mount_failed = false;
  mc.max_files = 8;
  mc.allocation_unit_size = 16 * 1024;

  sdmmc_card_t * card = nullptr;
  esp_err_t r = esp_vfs_fat_sdspi_mount("/SD", &host, &slot, &mc, &card);
  if (r != ESP_OK) {
    printf("mount at %d kHz failed: %s\n", freqKHz, esp_err_to_name(r));
    return nullptr;
  }
  return card;
}


void setup()
{
  Serial.begin(115200);
  delay(500);
  printf("\nSD card write benchmark, %d KB per run\n", TOTAL / 1024);

  if (esp_spiram_init() != ESP_OK) {
    printf("no PSRAM\n");
    return;
  }
  esp_spiram_init_cache();

  auto dma16 = (uint8_t *) heap_caps_malloc(16384, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  auto psram = (uint8_t *) SOC_EXTRAM_DATA_LOW;
  for (int i = 0; i < 65536; ++i)
    psram[i] = i;
  memcpy(dma16, psram, 16384);

  static const int FREQS[] = { 20000, 19000, 20000, 19000, 18000, 20000 };
  for (int f : FREQS) {
    auto card = mount(f);
    if (!card)
      continue;
    printf("--- limit %d kHz: card says max %d kHz, running at %d kHz\n", f, card->csd.tr_speed / 1000, card->max_freq_khz);
    if (f == 19000)
      sdmmc_card_print_info(stdout, card);
    run("16 KB internal, append", dma16, 16384, true, 0, 0);
    if (false) {
      run("16 KB PSRAM, truncate", psram, 16384, false, 0, 0);
      run("16 KB internal, append, unaligned start", dma16, 16384, true, 0, 1000);
    }
    remove(PATH);
    esp_vfs_fat_sdcard_unmount("/SD", card);
  }
  printf("done\n");
}


void loop()
{
  delay(1000);
}
