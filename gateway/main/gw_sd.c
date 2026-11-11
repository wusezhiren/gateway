#include "gw_sd.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "gw_audio.h"

static const char *TAG = "gw_sd";
static sdmmc_card_t *s_card;
static bool s_mounted;

esp_err_t gw_sd_init(void)
{
    if (s_mounted) return ESP_OK;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_15;
    slot.cmd = GPIO_NUM_7;
    slot.d0 = GPIO_NUM_4;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "mounted /sdcard");
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool gw_sd_is_mounted(void) { return s_mounted; }

void gw_sd_status(void)
{
    if (!s_mounted) {
        printf("sdcard: not mounted\n");
        return;
    }
    printf("sdcard: mounted /sdcard name=%s capacity=%lluMB speed=%ukHz\n",
           s_card->cid.name, (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024ULL * 1024ULL)),
           (unsigned)s_card->max_freq_khz);
}

esp_err_t gw_sd_upload_file(const char *name, const uint8_t *data, size_t len)
{
    if (!s_mounted || !name || !data || !len || len > 2 * 1024 * 1024 ||
        (strcmp(name, "zai-ne.wav") &&
        (strcmp(name, "_MODEL_INFO_p0") && strcmp(name, "wn9_data_p0") &&
         strcmp(name, "_MODEL_INFO_p1") && strcmp(name, "wn9_data_p1") &&
         strcmp(name, "_MODEL_INFO_p2") && strcmp(name, "wn9_data_p2") &&
         strcmp(name, "_MODEL_INFO_") && strcmp(name, "wn9_index") && strcmp(name, "wn9_data")))) {
        return ESP_ERR_INVALID_ARG;
    }
    mkdir("/sdcard/srmodels", 0775);
    mkdir("/sdcard/srmodels/wn9s_nihaoxiaozhi", 0775);
    char path[128];
    if (!strcmp(name, "zai-ne.wav")) snprintf(path, sizeof(path), "/sdcard/%s", name);
    else snprintf(path, sizeof(path), "/sdcard/srmodels/wn9s_nihaoxiaozhi/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t written = fwrite(data, 1, len, f);
    int close_err = fclose(f);
    return written == len && close_err == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_sd_play_wake_reply(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen("/sdcard/zai-ne.wav", "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long size = ftell(f);
    if (size < 44 || size > 128 * 1024 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    uint8_t *data = malloc(size);
    if (!data) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t read_bytes = fread(data, 1, size, f);
    fclose(f);
    esp_err_t err = read_bytes == (size_t)size ? gw_audio_play_wav_sync(data, size) : ESP_FAIL;
    free(data);
    return err;
}
