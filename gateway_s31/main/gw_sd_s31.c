#include "gw_sd_s31.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static sdmmc_card_t *s_card;

esp_err_t gw_sd_s31_init(void)
{
    if (s_card) return ESP_OK;

    gpio_config_t power = {
        .pin_bit_mask = 1ULL << 60,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&power);
    if (err != ESP_OK) return err;
    err = gpio_set_level(GPIO_NUM_60, 0);
    if (err != ESP_OK) return err;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_24;
    slot.cmd = GPIO_NUM_25;
    slot.d0 = GPIO_NUM_20;
    slot.d1 = GPIO_NUM_21;
    slot.d2 = GPIO_NUM_22;
    slot.d3 = GPIO_NUM_23;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &s_card);
}
