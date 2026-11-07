// storage.cpp - the game data volume.
//
// Board: the micro-SD module on its own SPI bus (SPI3), FAT32, mounted at
// /sd. tools/re1tool.py writes the tree as /re1/{config.ini,usa/,save/}.
// QEMU: the same tree in a host directory, reached through simcall
// semihosting (semihost_vfs.cpp).
#include "board.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if CONFIG_RE1_TARGET_BOARD
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#endif

static const char* TAG = "storage";

#if CONFIG_RE1_TARGET_BOARD
static sdmmc_card_t* s_card = nullptr;

// ---------------------------------------------------------------------------
// FATFS disk I/O with retries. IDF's own sdmmc diskio gives up on the first
// failed transfer; on this wiring a read now and then times out (0x107), and
// FATFS then hands the game a short or failed read - a background or mask
// that keeps the previous camera's pixels. A failed sector transfer is tried
// again after a short, growing pause; the card's status is polled in between
// so an SPI card that lost the thread resynchronises.
// ---------------------------------------------------------------------------
static const int kSdTries = 6;
static unsigned s_sdRetries = 0, s_sdRecovered = 0, s_sdFailed = 0;

static DSTATUS RetryInit(BYTE)
{
    return sdmmc_get_status(s_card) == ESP_OK ? 0 : STA_NOINIT;
}

static DSTATUS RetryStatus(BYTE)
{
    return 0;
}

template <typename Op>
static DRESULT WithRetries(const char* what, uint32_t sector, unsigned count, Op op)
{
    esp_err_t err = ESP_OK;
    for (int attempt = 0; attempt < kSdTries; ++attempt) {
        if (attempt > 0) {
            ++s_sdRetries;
            vTaskDelay(pdMS_TO_TICKS(2 << attempt));        // 4, 8 .. 64 ms
            (void)sdmmc_get_status(s_card);
        }
        err = op();
        if (err == ESP_OK) {
            if (attempt > 0) {
                ++s_sdRecovered;
                ESP_LOGW(TAG, "SD %s of sector %u+%u recovered after %d retries (%u so far)",
                         what, (unsigned)sector, count, attempt, s_sdRecovered);
            }
            return RES_OK;
        }
    }
    ++s_sdFailed;
    ESP_LOGE(TAG, "SD %s of sector %u+%u failed after %d tries (%s)", what, (unsigned)sector,
             count, kSdTries, esp_err_to_name(err));
    return RES_ERROR;
}

static DRESULT RetryRead(BYTE, BYTE* buff, uint32_t sector, unsigned count)
{
    return WithRetries("read", sector, count,
                       [&] { return sdmmc_read_sectors(s_card, buff, sector, count); });
}

// Every write is read back and compared. A card can accept a write and not
// keep it - on 2026-11-07 a card that read and wrote perfectly in a PC reader
// lost every write made over SPI here (likely its supply sagging while it
// programmed), and FATFS, told it had worked, reported the saves as written.
// Writes are rare (saves, config.ini), so the read costs nothing that shows.
static unsigned s_sdVerifyFailed = 0;

static DRESULT RetryWrite(BYTE, const BYTE* buff, uint32_t sector, unsigned count)
{
    DRESULT r = WithRetries("write", sector, count,
                            [&] { return sdmmc_write_sectors(s_card, buff, sector, count); });
    if (r != RES_OK) return r;
    static uint8_t* s_check = nullptr;   // one sector at a time, PSRAM
    if (s_check == nullptr) s_check = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (s_check == nullptr) return r;
    for (unsigned i = 0; i < count; ++i) {
        if (sdmmc_read_sectors(s_card, s_check, sector + i, 1) != ESP_OK ||
            memcmp(s_check, buff + (size_t)i * 512, 512) != 0) {
            int at = 0;
            while (at < 512 && s_check[at] == buff[(size_t)i * 512 + at]) ++at;
            ++s_sdVerifyFailed;
            ESP_LOGE(TAG, "SD write of sector %u did not stick: byte %d wrote 0x%02x, card has 0x%02x"
                          " (%u such writes so far)", (unsigned)(sector + i), at,
                     buff[(size_t)i * 512 + (at < 512 ? at : 0)], s_check[at < 512 ? at : 0],
                     s_sdVerifyFailed);
            return RES_ERROR;   // FATFS then fails the write instead of reporting it done
        }
    }
    return r;
}

static DRESULT RetryIoctl(BYTE, BYTE cmd, void* buff)
{
    switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *((DWORD*)buff) = s_card->csd.capacity; return RES_OK;
    case GET_SECTOR_SIZE:  *((WORD*)buff) = s_card->csd.sector_size; return RES_OK;
    default:               return RES_ERROR;
    }
}

// ---------------------------------------------------------------------------
// Why a card does not mount. The mount reports only a FATFS code (13: "no
// file system"); this reads the card's first sector, and the first
// partition's boot sector, straight from the card and says what is there.
// Read-only: nothing is written to the card.
// ---------------------------------------------------------------------------
static uint32_t Le32(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static const char* Printable(char* out, const uint8_t* p, int n)
{
    for (int i = 0; i < n; ++i) out[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
    out[n] = '\0';
    return out;
}

// What a boot sector says it is, or nullptr when it is not one.
static const char* VolumeKind(const uint8_t* s)
{
    if (s[510] != 0x55 || s[511] != 0xAA) return nullptr;
    if (memcmp(s + 3, "EXFAT   ", 8) == 0) return "exFAT";
    if (memcmp(s + 3, "NTFS    ", 8) == 0) return "NTFS";
    if (memcmp(s + 82, "FAT32", 5) == 0) return "FAT32";
    if (memcmp(s + 54, "FAT", 3) == 0) return "FAT12/16";
    return nullptr;
}

static void DumpSector(const char* what, const uint8_t* s)
{
    ESP_LOGW(TAG, "card check: %s, first 32 bytes:", what);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, s, 32, ESP_LOG_WARN);
    ESP_LOGW(TAG, "card check: %s, last 66 bytes:", what);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, s + 446, 66, ESP_LOG_WARN);
}

static void DescribeVolume(const char* where, const uint8_t* s, uint32_t sector)
{
    char oem[9];
    const char* kind = VolumeKind(s);
    if (kind == nullptr) {
        bool zero = true, ones = true;
        for (int i = 0; i < 512; ++i) {
            zero = zero && s[i] == 0x00;
            ones = ones && s[i] == 0xFF;
        }
        ESP_LOGE(TAG, "card check: %s (sector %u) is not a boot sector%s", where, (unsigned)sector,
                 zero ? " - it is all zeros" : ones ? " - it is all 0xFF" : "");
        DumpSector(where, s);
        return;
    }
    const unsigned bytesPerSector = s[11] | s[12] << 8;
    ESP_LOGW(TAG, "card check: %s (sector %u) is %s, OEM '%s', %u bytes/sector", where,
             (unsigned)sector, kind, Printable(oem, s + 3, 8), bytesPerSector);
    if (strcmp(kind, "exFAT") == 0 || strcmp(kind, "NTFS") == 0)
        ESP_LOGE(TAG, "card check: the board reads FAT32 only - format the card FAT32 and "
                      "copy the game again with tools/re1tool.py sdcard");
    else if (bytesPerSector != 512)
        ESP_LOGE(TAG, "card check: %u-byte sectors - FATFS here expects 512", bytesPerSector);
    else
        DumpSector(where, s);  // a FAT boot sector FATFS still refused: show it
}

static void DescribeUnmountableCard(sdmmc_host_t host, const sdspi_device_config_t& dev)
{
    sdspi_dev_handle_t handle = -1;
    if (sdspi_host_init() != ESP_OK) return;
    if (sdspi_host_init_device(&dev, &handle) != ESP_OK) {
        sdspi_host_deinit();
        return;
    }
    host.slot = handle;
    host.dma_aligned_buffer = nullptr;
    sdmmc_card_t card = {};
    uint8_t* s = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    esp_err_t err = s != nullptr ? sdmmc_card_init(&host, &card) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        sdmmc_card_print_info(stdout, &card);
        // Is the whole card blank (erased, or a card whose controller lost
        // its map) or only its start? Sample sectors across it.
        const uint32_t total = card.csd.capacity;
        const uint32_t span[2] = {total < 8192 ? total : 8192, total};
        const char* const name[2] = {"the first 8192 sectors", "the whole card"};
        for (int pass = 0; pass < 2; ++pass) {
            unsigned blank = 0, zero = 0, data = 0, bad = 0;
            uint32_t firstData = UINT32_MAX;
            for (int i = 0; i < 64; ++i) {
                const uint32_t sector = (uint32_t)((uint64_t)span[pass] * i / 64);
                if (sdmmc_read_sectors(&card, s, sector, 1) != ESP_OK) { ++bad; continue; }
                bool ff = true, z = true;
                for (int b = 0; b < 512; ++b) { ff = ff && s[b] == 0xFF; z = z && s[b] == 0x00; }
                if (ff) ++blank;
                else if (z) ++zero;
                else { ++data; if (sector < firstData) firstData = sector; }
            }
            ESP_LOGW(TAG, "card check: 64 sectors across %s (%u): %u erased (0xFF), %u zero, "
                          "%u with data, %u unreadable", name[pass], (unsigned)span[pass], blank,
                     zero, data, bad);
            if (data) ESP_LOGW(TAG, "card check:   first with data: sector %u", (unsigned)firstData);
            if (pass == 1 && blank == 64)
                // Seen on 2026-11-07: a prepared 1 GB card that reads fine
                // in a PC answered here as a blank 4 GB SDHC card. Cheap
                // controllers keep the card's real name, size and map in
                // their flash; when they cannot reach it they come up with
                // their built-in defaults and read 0xFF everywhere.
                ESP_LOGE(TAG, "card check: every sector reads erased. A card that is not empty in a "
                              "PC but looks like this here (wrong name/size too) cannot reach its "
                              "flash - check the adapter's supply: VCC on 5 V (not 3.3 V), "
                              "GND and VCC wires seated");
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "card check: the card does not initialise (%s)", esp_err_to_name(err));
    } else if ((err = sdmmc_read_sectors(&card, s, 0, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "card check: sector 0 does not read (%s)", esp_err_to_name(err));
    } else if (VolumeKind(s) != nullptr) {
        DescribeVolume("sector 0, no partition table", s, 0);
    } else if (s[510] != 0x55 || s[511] != 0xAA) {
        DescribeVolume("sector 0", s, 0);
    } else {
        uint32_t first = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t* e = s + 446 + 16 * i;
            if (e[4] == 0) continue;
            ESP_LOGW(TAG, "card check: partition %d: type 0x%02X, sectors %u + %u", i + 1, e[4],
                     (unsigned)Le32(e + 8), (unsigned)Le32(e + 12));
            if (e[4] == 0xEE) ESP_LOGE(TAG, "card check: GPT partition table - use MBR (FAT32)");
            if (first == 0) first = Le32(e + 8);
        }
        if (first == 0) {
            ESP_LOGE(TAG, "card check: the partition table is empty");
        } else if ((err = sdmmc_read_sectors(&card, s, first, 1)) != ESP_OK) {
            ESP_LOGE(TAG, "card check: sector %u does not read (%s)", (unsigned)first,
                     esp_err_to_name(err));
        } else {
            DescribeVolume("partition 1", s, first);
        }
    }
    heap_caps_free(s);
    sdspi_host_remove_device(handle);
    sdspi_host_deinit();
}

// A second card on the same wires answers even while ours is deselected: a
// card left in the display module's own SD slot with its SD_CS pin tied low
// or open. Both cards then drive MISO, and the stronger one is the card the
// board sees - not the one in the adapter.
static void ProbeSecondCard(sdmmc_host_t host, sdspi_device_config_t dev)
{
    const gpio_num_t ours = dev.gpio_cs;
    gpio_reset_pin(ours);
    gpio_set_direction(ours, GPIO_MODE_OUTPUT);
    gpio_set_level(ours, 1);
    dev.gpio_cs = SDSPI_SLOT_NO_CS;

    sdspi_dev_handle_t handle = -1;
    if (sdspi_host_init() != ESP_OK) return;
    if (sdspi_host_init_device(&dev, &handle) == ESP_OK) {
        host.slot = handle;
        host.dma_aligned_buffer = nullptr;
        sdmmc_card_t card = {};
        if (sdmmc_card_init(&host, &card) == ESP_OK) {
            ESP_LOGE(TAG, "card check: a card answers with our CS (GPIO %d) released: '%s', %u MB. "
                          "A second card shares the SD wires (the display module's own SD slot?) "
                          "- take it out or wire its SD_CS high",
                     (int)ours, card.cid.name,
                     (unsigned)((uint64_t)card.csd.capacity * card.csd.sector_size >> 20));
        } else {
            ESP_LOGW(TAG, "card check: no card answers with our CS (GPIO %d) released - the card "
                          "above is the one behind CS", (int)ours);
        }
        sdspi_host_remove_device(handle);
    }
    sdspi_host_deinit();
}

static void InstallRetryingDiskio(void)
{
    static const ff_diskio_impl_t impl = {
        .init = &RetryInit,
        .status = &RetryStatus,
        .read = &RetryRead,
        .write = &RetryWrite,
        .ioctl = &RetryIoctl,
    };
    const BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv == 0xFF) {
        ESP_LOGW(TAG, "SD drive not found; reads are not retried");
        return;
    }
    ff_diskio_register(pdrv, &impl);
}
#endif

int board_storage_init(void)
{
#if CONFIG_RE1_TARGET_QEMU
    if (!board_semihost_vfs_register(BOARD_MOUNT, CONFIG_RE1_QEMU_DATA_DIR)) {
        ESP_LOGE(TAG, "semihosting VFS failed (run QEMU with -semihosting)");
        return 0;
    }
#else
    spi_bus_config_t bus = {};
    bus.mosi_io_num = CONFIG_RE1_SD_MOSI;
    bus.miso_io_num = CONFIG_RE1_SD_MISO;
    bus.sclk_io_num = CONFIG_RE1_SD_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 32 * 1024;
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 init failed: %s", esp_err_to_name(err));
        return 0;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = CONFIG_RE1_SD_KHZ;
    // One bounce buffer for the card's lifetime. The game's buffers are in
    // PSRAM, which this DMA cannot reach, so every transfer is staged in
    // internal RAM; left to itself the driver allocates up to 8 KB of it per
    // call, and with the game holding internal RAM nearly full that fails
    // (ESP_ERR_NO_MEM) and the read is lost. 16 sectors = the host's chunk.
    static void* s_bounce = nullptr;
    if (s_bounce == nullptr)
        s_bounce = heap_caps_malloc(host.unaligned_multi_block_rw_max_chunk_size * 512,
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    host.dma_aligned_buffer = s_bounce;

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = (gpio_num_t)CONFIG_RE1_SD_CS;
    dev.host_id = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    // Every sound streamed from the card holds a file open for as long as its
    // bank lives (port/common/Audio.cpp); with 12, a room's streams left no
    // handle for a save - it failed silently and the slots read back empty.
    // A file object is ~0.6 KB (FATFS_SECTOR_512), in PSRAM.
    mount.max_files = 32;
    mount.allocation_unit_size = 32 * 1024;

    // A card left mid-transfer by a reset (flashing, the reset button) can
    // fail its first handshake or FAT read; it answers again after a pause.
    for (int attempt = 1; attempt <= 3; ++attempt) {
        err = esp_vfs_fat_sdspi_mount(BOARD_MOUNT, &host, &dev, &mount, &s_card);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "SD mount attempt %d failed (%s), retrying", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s - FAT32 card with /re1 from re1tool.py?",
                 esp_err_to_name(err));
        DescribeUnmountableCard(host, dev);
        ProbeSecondCard(host, dev);
        return 0;
    }
    sdmmc_card_print_info(stdout, s_card);
    InstallRetryingDiskio();
#endif

    struct stat st;
    if (stat(BOARD_DATA_ROOT "/usa", &st) != 0) {
        ESP_LOGE(TAG, "%s/usa not found - prepare the card with tools/re1tool.py", BOARD_DATA_ROOT);
        return 0;
    }
    ESP_LOGI(TAG, "game data at %s", BOARD_DATA_ROOT);
    return 1;
}
