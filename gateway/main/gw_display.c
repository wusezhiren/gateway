#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "sdkconfig.h"
#include "gw_display.h"

static const char *TAG = "gw_display";

/* 引脚与面板参数取自板级原理图和厂家例程 */
#define I2C_PORT        I2C_NUM_1
#define PIN_I2C_SDA     17
#define PIN_I2C_SCL     18
#define LCD_SPI_HOST    SPI2_HOST
#define PIN_LCD_SCLK    1
#define PIN_LCD_MOSI    0
#define PIN_LCD_DC      2
#define PIN_LCD_CS      46
#define LCD_PCLK_HZ     (80 * 1000 * 1000)
#define LCD_X_GAP       20      /* 面板 240x320 显存里 280 宽的可视区偏移 */
#define DRAW_LINES      40

static i2c_master_bus_handle_t s_i2c;
static esp_io_expander_handle_t s_exp;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static lv_indev_t *s_touch;

static esp_err_t expander_init(void)
{
    esp_err_t err = esp_io_expander_new_i2c_tca9554(s_i2c, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &s_exp);
    if (err != ESP_OK) {
        err = esp_io_expander_new_i2c_tca9554(s_i2c, ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000, &s_exp);
    }
    if (err != ESP_OK) {
        return err;
    }
    /* P1、P2：屏幕电源与背光，全部拉高 */
    esp_io_expander_set_dir(s_exp, IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, IO_EXPANDER_OUTPUT);
    return esp_io_expander_set_level(s_exp, IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 1);
}

static esp_err_t panel_init(esp_lcd_panel_io_handle_t *io_out)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = GW_LCD_W * DRAW_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, io_out),
                        TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(*io_out, &panel_cfg, &s_panel), TAG, "st7789");
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0);
    return esp_lcd_panel_disp_on_off(s_panel, true);
}

static esp_lcd_touch_handle_t touch_init(void)
{
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &tp_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_touch_config_t cfg = {
        .x_max = GW_LCD_W,
        .y_max = GW_LCD_H,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,     /* 中断脚未确认，轮询读取 */
        .flags = {.swap_xy = 1, .mirror_x = 0, .mirror_y = 1},
    };
    esp_lcd_touch_handle_t tp = NULL;
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &cfg, &tp) != ESP_OK) {
        return NULL;
    }
    /* 关闭自动休眠：轮询模式下芯片睡着后 I2C 无应答 */
    const uint8_t dis_auto_sleep = 1;
    esp_lcd_panel_io_tx_param(tp_io, 0xFE, &dis_auto_sleep, 1);
    return tp;
}

esp_err_t gw_display_init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c bus");
    if (expander_init() != ESP_OK) {
        ESP_LOGW(TAG, "tca9554 not found, lcd power may stay off");
    }

    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(panel_init(&io), TAG, "panel");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = s_panel,
        .buffer_size = GW_LCD_W * DRAW_LINES,
        .double_buffer = true,
        .hres = GW_LCD_W,
        .vres = GW_LCD_H,
        .rotation = {.swap_xy = true, .mirror_x = false, .mirror_y = true},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        return ESP_FAIL;
    }

    esp_lcd_touch_handle_t tp = touch_init();
    if (tp) {
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = s_disp, .handle = tp};
        s_touch = lvgl_port_add_touch(&touch_cfg);
    }
    ESP_LOGI(TAG, "display ready, touch %s", s_touch ? "ok" : "NOT FOUND");
    return ESP_OK;
}

lv_display_t *gw_display_get(void)
{
    return s_disp;
}

lv_indev_t *gw_display_touch(void)
{
    return s_touch;
}

bool gw_display_lock(int timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void gw_display_unlock(void)
{
    lvgl_port_unlock();
}

void gw_display_set_on(bool on)
{
#if CONFIG_GW_LCD_BL_EXP_PIN >= 0
    if (s_exp) {
        esp_io_expander_set_level(s_exp, 1U << CONFIG_GW_LCD_BL_EXP_PIN, on ? 1 : 0);
    }
#endif
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, on);
    }
}

esp_err_t gw_display_exp_set(int pin, int level)
{
    if (!s_exp || pin < 0 || pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_io_expander_set_dir(s_exp, 1U << pin, IO_EXPANDER_OUTPUT);
    return esp_io_expander_set_level(s_exp, 1U << pin, level ? 1 : 0);
}
