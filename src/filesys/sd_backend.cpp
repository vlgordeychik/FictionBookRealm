#include "filesys/disk_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char* TAG_SD = "sd_backend";

/* ─── Paper S3 SD card pin mapping (SPI mode) ──────────────── */
#define SD_PIN_CLK   39
#define SD_PIN_MOSI  38
#define SD_PIN_MISO  40
#define SD_PIN_CS    47
#define SD_SPI_HOST  SPI2_HOST

/* ─── SD backend state ─────────────────────────────────────── */
static sdmmc_card_t* s_card       = nullptr;
static bool          s_initialized = false;
static uint32_t      s_sector_count = 0;
// Persistent DMA bounce buffer: FatFs sector buffers live in heap/PSRAM
// and may fail MALLOC_CAP_DMA under pressure (allocate_dma_buf NO_MEM).
static uint8_t*      s_dma_buf     = nullptr;
static constexpr size_t kDmaBufSize = 8192;

/* ─── Backend callbacks ────────────────────────────────────── */

static DSTATUS_FS sd_init(void) {
    if (s_initialized) return 0;

    ESP_LOGI(TAG_SD, "Initializing SD card (SPI, CLK=%d MOSI=%d MISO=%d CS=%d)",
             SD_PIN_CLK, SD_PIN_MOSI, SD_PIN_MISO, SD_PIN_CS);

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_PIN_MOSI;
    bus_cfg.miso_io_num = SD_PIN_MISO;
    bus_cfg.sclk_io_num = SD_PIN_CLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4096;

    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SD, "SPI bus init failed: %s", esp_err_to_name(ret));
        return STA_NOINIT_FS;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)SD_PIN_CS;
    slot_config.host_id = SD_SPI_HOST;

    static sdspi_dev_handle_t sd_handle;
    ret = sdspi_host_init_device(&slot_config, &sd_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SD, "SD device init failed: %s", esp_err_to_name(ret));
        spi_bus_free(SD_SPI_HOST);
        return STA_NOINIT_FS;
    }

    static sdmmc_card_t card;
    ret = sdmmc_card_init(&host, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SD, "SD card init failed: %s", esp_err_to_name(ret));
        spi_bus_free(SD_SPI_HOST);
        return STA_NOINIT_FS;
    }

    if (!s_dma_buf) {
        s_dma_buf = static_cast<uint8_t*>(
            heap_caps_malloc(kDmaBufSize, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!s_dma_buf) {
            // Fallback: any internal 8-bit memory (SDSPI can use it as bounce).
            s_dma_buf = static_cast<uint8_t*>(
                heap_caps_malloc(kDmaBufSize, MALLOC_CAP_8BIT));
        }
    }
    if (s_dma_buf) {
        card.host.dma_aligned_buffer = s_dma_buf;
        ESP_LOGI(TAG_SD, "SD DMA bounce buf=%p size=%u",
                 s_dma_buf, (unsigned)kDmaBufSize);
    } else {
        ESP_LOGW(TAG_SD, "SD DMA bounce alloc FAILED — reads may NO_MEM");
    }

    sdmmc_card_print_info(stdout, &card);
    s_card = &card;
    s_sector_count = card.csd.capacity;
    s_initialized = true;

    ESP_LOGI(TAG_SD, "SD card ready, %lu sectors", (unsigned long)s_sector_count);
    return 0;
}

static DSTATUS_FS sd_status(void) {
    if (s_initialized && s_card) return 0;
    return STA_NODISK_FS;
}

static DRESULT_FS sd_read(BYTE_FS* buff, LBA_t_FS sector, UINT_FS count) {
    if (!s_initialized || !s_card) return RES_NOTRDY_FS;

    esp_err_t ret = sdmmc_read_sectors(s_card, buff, sector, count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SD, "SD read failed at sector %lu: %s",
                 (unsigned long)sector, esp_err_to_name(ret));
        return RES_ERROR_FS;
    }
    return RES_OK_FS;
}

static DRESULT_FS sd_write(const BYTE_FS* buff, LBA_t_FS sector, UINT_FS count) {
    if (!s_initialized || !s_card) return RES_NOTRDY_FS;

    esp_err_t ret = sdmmc_write_sectors(s_card, buff, sector, count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SD, "SD write failed at sector %lu: %s",
                 (unsigned long)sector, esp_err_to_name(ret));
        return RES_ERROR_FS;
    }
    return RES_OK_FS;
}

static DRESULT_FS sd_ioctl(unsigned char cmd, void* buff) {
    if (!s_initialized || !s_card) return RES_NOTRDY_FS;

    switch (cmd) {
    case 0: /* CTRL_SYNC */
        return RES_OK_FS;
    case 1: { /* GET_SECTOR_COUNT */
        if (!buff) return RES_PARERR_FS;
        *(unsigned int*)buff = s_sector_count;
        return RES_OK_FS;
    }
    case 2: /* GET_SECTOR_SIZE */
        if (!buff) return RES_PARERR_FS;
        *(unsigned short*)buff = 512;
        return RES_OK_FS;
    case 3: /* GET_BLOCK_SIZE */
        if (!buff) return RES_PARERR_FS;
        *(unsigned int*)buff = 1;
        return RES_OK_FS;
    default:
        return RES_PARERR_FS;
    }
}

static const DiskBackend s_sd_backend = {
    sd_init,
    sd_status,
    sd_read,
    sd_write,
    sd_ioctl
};

/* ─── Public API ───────────────────────────────────────────── */

void sd_backend_init(void) {
    disk_backend_register(0, &s_sd_backend);
    sd_init();
}

void sd_backend_shutdown(void) {
    disk_backend_unregister(0);
    if (s_card) {
        s_card->host.dma_aligned_buffer = nullptr;
    }
    if (s_dma_buf) {
        heap_caps_free(s_dma_buf);
        s_dma_buf = nullptr;
    }
    s_card = nullptr;
    s_initialized = false;
    s_sector_count = 0;
}

int sd_backend_is_ready(void) {
    return s_initialized;
}

#ifdef __cplusplus
}
#endif
