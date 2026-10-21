#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_io_expander_tca9554.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "node_io.h"

static const char *TAG = "node_io";

#define PWM_MAX         1000
#define PWM_RES         LEDC_TIMER_13_BIT
#define PWM_DUTY_FULL   ((1 << 13) - 1)
#define PWM_FREQ_HZ     5000
#define NVS_NS          "node_io"
#define SAVE_DELAY_US   (2000 * 1000)   /* 防抖：最后一次改动 2s 后落盘，避免拖滑条时频繁写 flash */

#ifdef CONFIG_NODE_RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 1
#else
#define RELAY_ACTIVE_LOW 0
#endif
#ifdef CONFIG_NODE_EXP_ACTIVE_LOW
#define EXP_ACTIVE_LOW 1
#else
#define EXP_ACTIVE_LOW 0
#endif
#ifdef CONFIG_NODE_PWM_ACTIVE_LOW
#define PWM_ACTIVE_LOW 1
#else
#define PWM_ACTIVE_LOW 0
#endif

typedef struct {
    uint8_t  type;
    bool     exp;       /* 继电器在 IO 扩展上，gpio 为扩展引脚号 */
    int      gpio;
    int      ledc_ch;
    uint16_t value;
} io_ch_t;

static io_ch_t s_ch[MP_MAX_CH];
static uint8_t s_n;
static esp_timer_handle_t s_save_timer;
static esp_io_expander_handle_t s_exp;

static void save_cb(void *arg);
static void restore(void);

/* 落盘格式：带通道类型，布局变了（改了 GPIO 配置）就不恢复 */
typedef struct __attribute__((packed)) {
    uint8_t  n;
    uint8_t  type[MP_MAX_CH];
    uint16_t value[MP_MAX_CH];
} saved_t;

/* 解析 "4,5,6" 形式的 GPIO 列表 */
static int parse_gpios(const char *list, int out[], int max)
{
    int n = 0;
    const char *p = list;
    while (*p && n < max) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }
        out[n++] = (int)v;
        p = end;
    }
    return n;
}

static esp_err_t add_relay(int gpio)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        ESP_LOGE(TAG, "relay gpio %d invalid", gpio);
        return ESP_ERR_INVALID_ARG;
    }
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, RELAY_ACTIVE_LOW ? 1 : 0);
    s_ch[s_n++] = (io_ch_t){.type = MP_CH_RELAY, .gpio = gpio, .ledc_ch = -1};
    return ESP_OK;
}

static esp_err_t expander_init(void)
{
#ifdef CONFIG_NODE_EXP_I2C_SDA
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_NODE_EXP_I2C_SDA,
        .scl_io_num = CONFIG_NODE_EXP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) {
        return err;
    }
    /* TCA9554 与 TCA9554A 地址段不同，两个都试 */
    err = esp_io_expander_new_i2c_tca9554(bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &s_exp);
    if (err != ESP_OK) {
        err = esp_io_expander_new_i2c_tca9554(bus, ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000, &s_exp);
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t add_exp_relay(int pin)
{
    if (pin < 0 || pin > 7) {
        ESP_LOGE(TAG, "expander pin %d invalid", pin);
        return ESP_ERR_INVALID_ARG;
    }
    /* 输入模式下不能设电平，先切输出 */
    esp_err_t err = esp_io_expander_set_dir(s_exp, 1U << pin, IO_EXPANDER_OUTPUT);
    if (err == ESP_OK) {
        err = esp_io_expander_set_level(s_exp, 1U << pin, EXP_ACTIVE_LOW ? 1 : 0);
    }
    if (err != ESP_OK) {
        return err;
    }
    s_ch[s_n++] = (io_ch_t){.type = MP_CH_RELAY, .exp = true, .gpio = pin, .ledc_ch = -1};
    return ESP_OK;
}

static esp_err_t add_pwm(int gpio, int ledc_ch)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio) || ledc_ch >= LEDC_CHANNEL_MAX) {
        ESP_LOGE(TAG, "pwm gpio %d / ledc channel %d invalid", gpio, ledc_ch);
        return ESP_ERR_INVALID_ARG;
    }
    ledc_channel_config_t cfg = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_ch,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .flags.output_invert = PWM_ACTIVE_LOW,
    };
    esp_err_t err = ledc_channel_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    s_ch[s_n++] = (io_ch_t){.type = MP_CH_PWM, .gpio = gpio, .ledc_ch = ledc_ch};
    return ESP_OK;
}

esp_err_t node_io_init(mp_ch_desc_t desc[MP_MAX_CH], uint8_t *n_ch)
{
    int gpios[MP_MAX_CH];
    int n;

    s_n = 0;
    n = parse_gpios(CONFIG_NODE_RELAY_GPIOS, gpios, MP_MAX_CH);
    for (int i = 0; i < n; i++) {
        if (add_relay(gpios[i]) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    n = parse_gpios(CONFIG_NODE_EXP_RELAY_PINS, gpios, MP_MAX_CH - s_n);
    if (n > 0) {
        esp_err_t err = expander_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "io expander init failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    for (int i = 0; i < n; i++) {
        if (add_exp_relay(gpios[i]) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    n = parse_gpios(CONFIG_NODE_PWM_GPIOS, gpios, MP_MAX_CH - s_n);
    if (n > 0) {
        ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = PWM_RES,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = PWM_FREQ_HZ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t err = ledc_timer_config(&timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    for (int i = 0; i < n; i++) {
        esp_err_t err = add_pwm(gpios[i], i);
        if (err != ESP_OK) {
            return err;
        }
    }

    memset(desc, 0, sizeof(mp_ch_desc_t) * MP_MAX_CH);
    for (int i = 0; i < s_n; i++) {
        desc[i].type = s_ch[i].type;
        desc[i].max = s_ch[i].type == MP_CH_PWM ? PWM_MAX : 1;
        ESP_LOGI(TAG, "ch%d %s %s %d", i, s_ch[i].type == MP_CH_PWM ? "pwm" : "relay",
                 s_ch[i].exp ? "exp pin" : "gpio", s_ch[i].gpio);
    }
    *n_ch = s_n;

    const esp_timer_create_args_t args = {.callback = save_cb, .name = "io_save"};
    esp_err_t err = esp_timer_create(&args, &s_save_timer);
    if (err != ESP_OK) {
        return err;
    }
    restore();
    return ESP_OK;
}

static void apply(uint8_t ch, uint16_t value)
{
    io_ch_t *c = &s_ch[ch];
    if (c->type == MP_CH_RELAY) {
        value = value ? 1 : 0;
        if (c->exp) {
            esp_io_expander_set_level(s_exp, 1U << c->gpio, EXP_ACTIVE_LOW ? !value : value);
        } else {
            gpio_set_level(c->gpio, RELAY_ACTIVE_LOW ? !value : value);
        }
    } else {
        if (value > PWM_MAX) {
            value = PWM_MAX;
        }
        ledc_set_duty(LEDC_LOW_SPEED_MODE, c->ledc_ch, (uint32_t)value * PWM_DUTY_FULL / PWM_MAX);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, c->ledc_ch);
    }
    c->value = value;
}

static void save_cb(void *arg)
{
    saved_t st = {.n = s_n};
    for (int i = 0; i < s_n; i++) {
        st.type[i] = s_ch[i].type;
        st.value[i] = s_ch[i].value;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, "state", &st, sizeof(st)) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "save state failed");
    }
    nvs_close(h);
}

static void restore(void)
{
    saved_t st;
    size_t len = sizeof(st);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_get_blob(h, "state", &st, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(st) || st.n != s_n) {
        return;
    }
    for (int i = 0; i < s_n; i++) {
        if (st.type[i] != s_ch[i].type) {
            return;
        }
    }
    for (int i = 0; i < s_n; i++) {
        apply(i, st.value[i]);
    }
    ESP_LOGI(TAG, "restored %d channel values", s_n);
}

esp_err_t node_io_set(uint8_t ch, uint16_t value)
{
    if (ch >= s_n) {
        return ESP_ERR_INVALID_ARG;
    }
    apply(ch, value);
    ESP_LOGI(TAG, "ch%d = %u", ch, s_ch[ch].value);
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, SAVE_DELAY_US);
    return ESP_OK;
}

void node_io_get(uint16_t value[MP_MAX_CH])
{
    memset(value, 0, sizeof(uint16_t) * MP_MAX_CH);
    for (int i = 0; i < s_n; i++) {
        value[i] = s_ch[i].value;
    }
}
