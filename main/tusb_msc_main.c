/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* DESCRIPTION:
 * This example contains code to make ESP32 based device recognizable by USB-hosts as a USB Mass Storage Device.
 * It either allows the embedded application i.e. example to access the partition or Host PC accesses the partition over USB MSC.
 * They can't be allowed to access the partition at the same time.
 * For different scenarios and behaviour, Refer to README of this example.
 */

#include <errno.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_console.h"
#include "esp_check.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "esp_ota_ops.h"
#include "spi_api.h"
#include "ota_c6_sdcard.h"

#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SDMMC
#include "sdmmc_cmd.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif // CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
#endif

/*
 * We warn if a secondary serial console is enabled. A secondary serial console is always output-only and
 * hence not very useful for interactive console applications. If you encounter this warning, consider disabling
 * the secondary serial console in menuconfig unless you know what you are doing.
 */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#if !CONFIG_ESP_CONSOLE_SECONDARY_NONE
#warning "A secondary serial console is not useful when using the console component. Please disable it in menuconfig."
#endif
#endif

static const char *TAG = "example_main";
static esp_console_repl_t *repl = NULL;
static tinyusb_msc_storage_handle_t storage_hdl = NULL;
static volatile tinyusb_msc_mount_point_t storage_mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
static volatile bool host_was_mounted = false;

static SemaphoreHandle_t _wait_console_smp = NULL;

/* TinyUSB descriptors
   ********************************************************************* */
#define EPNUM_MSC       1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

enum {
    EDPT_CTRL_OUT = 0x00,
    EDPT_CTRL_IN  = 0x80,

    EDPT_MSC_OUT  = 0x01,
    EDPT_MSC_IN   = 0x81,
};

static tusb_desc_device_t descriptor_config = {
    .bLength = sizeof(descriptor_config),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // This is Espressif VID. This needs to be changed according to Users / Customers
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static uint8_t const msc_fs_configuration_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0
};

static uint8_t const msc_hs_configuration_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 512),
};
#endif // TUD_OPT_HIGH_SPEED

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },  // 0: is supported language is English (0x0409)
    "CTAG",                      // 1: Manufacturer
    "CTAG-TBD",               // 2: Product
    "123456",                       // 3: Serials
    "TBDDISK",                  // 4. MSC
};
/*********************************************************************** TinyUSB descriptors*/

#define BASE_PATH "/data" // base path to mount the partition

#define PROMPT_STR CONFIG_IDF_TARGET
static int console_unmount(int argc, char **argv);
static int console_read(int argc, char **argv);
static int console_write(int argc, char **argv);
static int console_size(int argc, char **argv);
static int console_status(int argc, char **argv);
static int console_exit(int argc, char **argv);
const esp_console_cmd_t cmds[] = {
    {
        .command = "read",
        .help = "read BASE_PATH/README.MD and print its contents",
        .hint = NULL,
        .func = &console_read,
    },
    {
        .command = "write",
        .help = "create file BASE_PATH/README.MD if it does not exist",
        .hint = NULL,
        .func = &console_write,
    },
    {
        .command = "size",
        .help = "show storage size and sector size",
        .hint = NULL,
        .func = &console_size,
    },
    {
        .command = "expose",
        .help = "Expose Storage to Host",
        .hint = NULL,
        .func = &console_unmount,
    },
    {
        .command = "status",
        .help = "Status of storage exposure over USB",
        .hint = NULL,
        .func = &console_status,
    },
    {
        .command = "exit",
        .help = "exit from application",
        .hint = NULL,
        .func = &console_exit,
    }
};

static bool storage_is_mounted_to_app(void)
{
    return storage_mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP;
}

// Expose storage to the USB host.
static int console_unmount(int argc, char **argv)
{
    if (!storage_is_mounted_to_app()) {
        ESP_LOGE(TAG, "storage is already exposed");
        return -1;
    }
    ESP_LOGI(TAG, "Expose storage to USB host...");
    ESP_ERROR_CHECK(tinyusb_msc_set_storage_mount_point(storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB));
    return 0;
}

// read BASE_PATH/README.MD and print its contents
static int console_read(int argc, char **argv)
{
    if (!storage_is_mounted_to_app()) {
        ESP_LOGE(TAG, "storage exposed over USB. Application can't read from storage.");
        return -1;
    }
    ESP_LOGD(TAG, "read from storage:");
    const char *filename = BASE_PATH "/README.MD";
    FILE *ptr = fopen(filename, "r");
    if (ptr == NULL) {
        ESP_LOGE(TAG, "Filename not present - %s", filename);
        return -1;
    }
    char buf[1024];
    while (fgets(buf, 1000, ptr) != NULL) {
        printf("%s", buf);
    }
    fclose(ptr);
    return 0;
}

// create file BASE_PATH/README.MD if it does not exist
static int console_write(int argc, char **argv)
{
    if (!storage_is_mounted_to_app()) {
        ESP_LOGE(TAG, "storage exposed over USB. Application can't write to storage.");
        return -1;
    }
    ESP_LOGD(TAG, "write to storage:");
    const char *filename = BASE_PATH "/README.MD";
    FILE *fd = fopen(filename, "r");
    if (!fd) {
        ESP_LOGW(TAG, "README.MD doesn't exist yet, creating");
        fd = fopen(filename, "w");
        fprintf(fd, "Mass Storage Devices are one of the most common USB devices. It use Mass Storage Class (MSC) that allow access to their internal data storage.\n");
        fprintf(fd, "In this example, ESP chip will be recognised by host (PC) as Mass Storage Device.\n");
        fprintf(fd, "Upon connection to USB host (PC), the example application will initialize the storage module and then the storage will be seen as removable device on PC.\n");
        fclose(fd);
    }
    return 0;
}

// Show storage size and sector size
static int console_size(int argc, char **argv)
{
    if (!storage_is_mounted_to_app()) {
        ESP_LOGE(TAG, "storage exposed over USB. Application can't access storage");
        return -1;
    }
    uint32_t sec_count = 0;
    uint32_t sec_size = 0;
    ESP_ERROR_CHECK(tinyusb_msc_get_storage_capacity(storage_hdl, &sec_count));
    ESP_ERROR_CHECK(tinyusb_msc_get_storage_sector_size(storage_hdl, &sec_size));
    printf("Storage Capacity %lluMB\n", ((uint64_t) sec_count) * sec_size / (1024 * 1024));
    return 0;
}

// Show storage status
static int console_status(int argc, char **argv)
{
    printf("storage exposed over USB: %s\n", storage_is_mounted_to_app() ? "No" : "Yes");
    return 0;
}

// Exit from application
static int console_exit(int argc, char **argv)
{
    if (storage_hdl != NULL) {
        ESP_ERROR_CHECK(tinyusb_msc_delete_storage(storage_hdl));
        storage_hdl = NULL;
    }
    tinyusb_driver_uninstall();

    xSemaphoreGive(_wait_console_smp);

    printf("Application Exit\n");

    return 0;
}

void boot_into_slot(int slot) { // slot 0 or 1
    esp_partition_subtype_t st = (slot == 0)
        ? ESP_PARTITION_SUBTYPE_APP_OTA_0
        : ESP_PARTITION_SUBTYPE_APP_OTA_1;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, st, NULL);
    if (!p) return;
    printf("Try to boot into %s\n", p->label);
    if (esp_ota_set_boot_partition(p) == ESP_OK) esp_restart();
    printf("Boot into %s\n not successful", p->label);
}

// IDF 6 reports explicit target mount points. A host eject remounts the FAT
// filesystem to the application; only then is local C6 update access safe.
static void storage_mount_changed_cb(tinyusb_msc_storage_handle_t handle,
                                     tinyusb_msc_event_t *event,
                                     void *arg)
{
    (void)handle;
    (void)arg;

    if (event->id == TINYUSB_MSC_EVENT_MOUNT_START) {
        ESP_LOGI(TAG, "MSC mount transition starting: target=%s",
                 event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP");
        return;
    }

    if (event->id != TINYUSB_MSC_EVENT_MOUNT_COMPLETE) {
        ESP_LOGE(TAG, "MSC mount transition failed: event=%d target=%s",
                 (int)event->id,
                 event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP");
        return;
    }

    storage_mount_point = event->mount_point;
    ESP_LOGI(TAG, "MSC mount transition complete: target=%s",
             event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP");

    if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
        host_was_mounted = true;
        return;
    }

    if (host_was_mounted) {
        host_was_mounted = false;
        ota_c6_sd_perform(true, BASE_PATH "/c6_fw");
        boot_into_slot(0);
    }
}

#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH
static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    ESP_LOGI(TAG, "Initializing wear levelling");

    const esp_partition_t *data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition. Check the partition table.");
        return ESP_ERR_NOT_FOUND;
    }

    return wl_mount(data_partition, wl_handle);
}
#else  // CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH
typedef enum {
    SD_MOUNT_UHS_SDR50,
    SD_MOUNT_HS_1BIT,
} sd_mount_mode_t;

#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
static sd_pwr_ctrl_handle_t s_sd_pwr_ctrl_handle = NULL;
#endif

// ESP-Hosted initializes SDMMC slot 1 before app_main(). Keep the shared host
// controller alive and initialize/deinitialize only slot 0 for the SD card.
static esp_err_t sdmmc_host_init_noop(void)
{
    return ESP_OK;
}

static const char *sd_mount_mode_name(sd_mount_mode_t mode)
{
    return mode == SD_MOUNT_UHS_SDR50 ? "UHS-I SDR50 4-bit phase 2" : "HS 1-bit phase 0";
}

static esp_err_t ensure_sd_power_control(void)
{
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    if (s_sd_pwr_ctrl_handle != NULL) {
        return ESP_OK;
    }

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID,
    };
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_sd_pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SD on-chip LDO power control (0x%x)", ret);
    }
    return ret;
#else
    return ESP_OK;
#endif
}

static void sd_power_cycle(int settle_ms)
{
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    if (ensure_sd_power_control() == ESP_OK) {
        esp_err_t ret = sd_pwr_ctrl_set_io_voltage(s_sd_pwr_ctrl_handle, 3300);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to force SD IO voltage to 3.3V (0x%x)", ret);
        }
    }
#endif

    gpio_reset_pin(CONFIG_EXAMPLE_PIN_SD_RESET);
    gpio_set_direction(CONFIG_EXAMPLE_PIN_SD_RESET, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_EXAMPLE_PIN_SD_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    gpio_set_level(CONFIG_EXAMPLE_PIN_SD_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(settle_ms));
}

static void deinit_sdmmc_host(const sdmmc_host_t *host)
{
    if (host->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host->deinit_p(host->slot);
    } else {
        (*host->deinit)();
    }
}

static void release_sdmmc(sdmmc_card_t **card)
{
    if (card == NULL || *card == NULL) {
        return;
    }
    deinit_sdmmc_host(&(*card)->host);
    free(*card);
    *card = NULL;
}

static bool is_uhs_active(const sdmmc_card_t *card)
{
    return card != NULL && card->real_freq_khz > SDMMC_FREQ_HIGHSPEED;
}

static void log_card_mode(const sdmmc_card_t *card)
{
    uint32_t bus_width = card->is_mmc
                             ? (1u << card->log_bus_width)
                             : (card->ssr.cur_bus_width ? 4u : 1u);
    ESP_LOGI(TAG,
             "SD mode: real=%d kHz limit=%" PRIu32 " kHz bus=%" PRIu32 "-bit card_uhs=%d active_uhs=%d ddr=%d ocr=0x%08" PRIx32,
             card->real_freq_khz,
             card->max_freq_khz,
             bus_width,
             (int)card->is_uhs1,
             (int)is_uhs_active(card),
             (int)card->is_ddr,
             card->ocr);
}

static esp_err_t try_init_sdmmc(sdmmc_card_t **card, sd_mount_mode_t mode)
{
    ESP_LOGI(TAG, "Initializing SD card for MSC (%s)", sd_mount_mode_name(mode));
    sd_power_cycle(500);

    esp_err_t ret = ensure_sd_power_control();
    if (ret != ESP_OK) {
        return ret;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
    host.max_freq_khz = mode == SD_MOUNT_UHS_SDR50 ? SDMMC_FREQ_SDR50 : SDMMC_FREQ_HIGHSPEED;
    host.input_delay_phase = mode == SD_MOUNT_UHS_SDR50 ? SDMMC_DELAY_PHASE_2 : SDMMC_DELAY_PHASE_0;
    host.init = &sdmmc_host_init_noop;
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    host.pwr_ctrl_handle = s_sd_pwr_ctrl_handle;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = mode == SD_MOUNT_UHS_SDR50 ? 4 : 1;
    if (mode == SD_MOUNT_UHS_SDR50) {
        slot_config.flags |= SDMMC_SLOT_FLAG_UHS1;
    }

#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = CONFIG_EXAMPLE_PIN_CLK;
    slot_config.cmd = CONFIG_EXAMPLE_PIN_CMD;
    slot_config.d0 = CONFIG_EXAMPLE_PIN_D0;
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
    slot_config.d1 = CONFIG_EXAMPLE_PIN_D1;
    slot_config.d2 = CONFIG_EXAMPLE_PIN_D2;
    slot_config.d3 = CONFIG_EXAMPLE_PIN_D3;
    slot_config.d4 = GPIO_NUM_NC;
    slot_config.d5 = GPIO_NUM_NC;
    slot_config.d6 = GPIO_NUM_NC;
    slot_config.d7 = GPIO_NUM_NC;
#endif
#endif

    sdmmc_card_t *sd_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (sd_card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = (*host.init)();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC host init failed (0x%x)", ret);
        free(sd_card);
        return ret;
    }

    ret = sdmmc_host_init_slot(host.slot, &slot_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC slot init failed (0x%x)", ret);
        deinit_sdmmc_host(&host);
        free(sd_card);
        return ret;
    }

    ret = sdmmc_card_init(&host, sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed in %s mode (0x%x)", sd_mount_mode_name(mode), ret);
        deinit_sdmmc_host(&host);
        free(sd_card);
        return ret;
    }

    sdmmc_card_print_info(stdout, sd_card);
    log_card_mode(sd_card);
    *card = sd_card;
    return ESP_OK;
}

static esp_err_t storage_init_sdmmc(sdmmc_card_t **card)
{
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
    const int max_uhs_attempts = 5;
    esp_err_t last_error = ESP_FAIL;

    for (int attempt = 1; attempt <= max_uhs_attempts; ++attempt) {
        last_error = try_init_sdmmc(card, SD_MOUNT_UHS_SDR50);
        if (last_error != ESP_OK) {
            ESP_LOGW(TAG, "UHS-I init attempt %d/%d failed (0x%x)",
                     attempt, max_uhs_attempts, last_error);
            break;
        }

        if (is_uhs_active(*card)) {
            if (attempt > 1) {
                ESP_LOGI(TAG, "SD UHS recovered after MSC init attempt %d/%d",
                         attempt, max_uhs_attempts);
            }
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "SD initialized below UHS speed on attempt %d/%d; real=%d kHz limit=%" PRIu32 " kHz card_uhs=%d",
                 attempt,
                 max_uhs_attempts,
                 (*card)->real_freq_khz,
                 (*card)->max_freq_khz,
                 (int)(*card)->is_uhs1);
        release_sdmmc(card);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#else
    ESP_LOGW(TAG, "4-bit pins are disabled at build time; skipping UHS-I attempts");
#endif

    ESP_LOGW(TAG, "Falling back to conservative SD mode for MSC: %s",
             sd_mount_mode_name(SD_MOUNT_HS_1BIT));
    return try_init_sdmmc(card, SD_MOUNT_HS_1BIT);
}
#endif  // CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing storage...");

    _wait_console_smp = xSemaphoreCreateBinary();
    if (_wait_console_smp == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    tinyusb_msc_storage_config_t storage_cfg = {
        .fat_fs = {
            .base_path = BASE_PATH,
            .config = VFS_FAT_MOUNT_DEFAULT_CONFIG(),
            .do_not_format = true,
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };

#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH
    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));
    storage_cfg.medium.wl_handle = wl_handle;
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&storage_cfg, &storage_hdl));
#else // CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH
    static sdmmc_card_t *card = NULL;
    ESP_ERROR_CHECK(storage_init_sdmmc(&card));
    storage_cfg.medium.card = card;
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_sdmmc(&storage_cfg, &storage_hdl));
#endif

    ESP_ERROR_CHECK(tinyusb_msc_set_storage_callback(storage_mount_changed_cb, NULL));

    ESP_LOGI(TAG, "USB MSC initialization");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.port = TINYUSB_PORT_HIGH_SPEED_0;
    tusb_cfg.task = TINYUSB_TASK_CUSTOM(4096, 10, 0);
    tusb_cfg.descriptor.device = &descriptor_config;
    tusb_cfg.descriptor.full_speed_config = msc_fs_configuration_desc;
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
#if TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.high_speed_config = msc_hs_configuration_desc;
    tusb_cfg.descriptor.qualifier = &device_qualifier;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB MSC initialization DONE");

    // start spi_api
    spi_start();

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    /* Prompt to be printed before each line.
     * This can be customized, made dynamic, etc.
     */
    repl_config.prompt = PROMPT_STR ">";
    repl_config.max_cmdline_length = 64;

    // Init console based on menuconfig settings
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

    // USJ console can be set only on esp32p4, having separate USB PHYs for USB_OTG and USJ
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) && defined(CONFIG_IDF_TARGET_ESP32P4)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

#else
#error Unsupported console type
#endif

    for (int count = 0; count < sizeof(cmds) / sizeof(esp_console_cmd_t); count++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[count]));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    xSemaphoreTake(_wait_console_smp, portMAX_DELAY);
    ESP_ERROR_CHECK(esp_console_stop_repl(repl));
    vSemaphoreDelete(_wait_console_smp);


}
