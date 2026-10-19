/*
 * 串口调试命令。App 接入前，用这些命令验证组网和控制：
 *   nodes                      列出节点
 *   set <idx> <ch> <value>     设置通道
 *   pair [seconds]             打开配对窗口（默认 60s，0 关闭）
 *   unpair <idx>               解除配对
 *   wifi [status]              查看 Wi-Fi 状态
 *   wifi set <ssid> [pass]     配置路由器
 *   wifi clear                 清除路由器配置
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "gw_link.h"
#include "gw_wifi.h"

static const char *TAG = "gw_console";

static const char *ch_type_name(uint8_t type)
{
    switch (type) {
    case MP_CH_RELAY: return "relay";
    case MP_CH_PWM:   return "pwm";
    default:          return "?";
    }
}

static int cmd_nodes(int argc, char **argv)
{
    static gw_node_t nodes[GW_MAX_NODES];
    int64_t now = esp_timer_get_time();
    int count = 0;

    gw_link_snapshot(nodes);
    for (int i = 0; i < GW_MAX_NODES; i++) {
        const gw_node_t *n = &nodes[i];
        if (!n->used) {
            continue;
        }
        count++;
        printf("[%d] " MACSTR " %-16s %s v%s %s", i, MAC2STR(n->mac), n->name, n->project, n->fw_version,
               n->online ? "online " : "OFFLINE");
        if (n->last_seen_us) {
            printf(" rssi %d up %lus seen %llds ago", n->rssi, (unsigned long)n->uptime_s,
                   (now - n->last_seen_us) / 1000000);
        }
        printf("\n");
        for (int c = 0; c < n->n_ch; c++) {
            printf("      ch%d %-5s = %u (max %u)\n", c, ch_type_name(n->ch[c].type), n->value[c], n->ch[c].max);
        }
    }
    if (count == 0) {
        printf("no nodes, use 'pair' to open pairing window\n");
    }
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc != 4) {
        printf("usage: set <idx> <ch> <value>\n");
        return 1;
    }
    esp_err_t err = gw_link_set(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]));
    if (err != ESP_OK) {
        printf("set failed: %s\n", err == ESP_ERR_INVALID_STATE ? "node offline" : esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int cmd_pair(int argc, char **argv)
{
    uint32_t seconds = argc > 1 ? strtoul(argv[1], NULL, 10) : 60;
    gw_link_pair(seconds);
    return 0;
}

static int cmd_unpair(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: unpair <idx>\n");
        return 1;
    }
    esp_err_t err = gw_link_unpair(atoi(argv[1]));
    if (err != ESP_OK) {
        printf("unpair failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "set") == 0) {
        esp_err_t err = gw_wifi_set_credentials(argv[2], argc > 3 ? argv[3] : "");
        printf("%s\n", err == ESP_OK ? "saved, connecting..." : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        esp_err_t err = gw_wifi_set_credentials(NULL, NULL);
        printf("%s\n", err == ESP_OK ? "cleared" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        gw_wifi_status_t st;
        gw_wifi_get_status(&st);
        if (!st.configured) {
            printf("router: not configured\n");
        } else {
            printf("router: \"%s\" %s %s\n", st.ssid, st.connected ? "connected" : "connecting", st.ip);
        }
        printf("channel: %d, pairing: %lus left\n", st.channel, (unsigned long)gw_link_pair_remaining());
        return 0;
    }
    printf("usage: wifi [status] | wifi set <ssid> [pass] | wifi clear\n");
    return 1;
}

void gw_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "gw>";
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl));
#else
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl));
#endif

    const esp_console_cmd_t cmds[] = {
        {.command = "nodes",  .help = "List paired nodes", .func = cmd_nodes},
        {.command = "set",    .help = "set <idx> <ch> <value>", .func = cmd_set},
        {.command = "pair",   .help = "pair [seconds], open pairing window (default 60, 0 closes)", .func = cmd_pair},
        {.command = "unpair", .help = "unpair <idx>", .func = cmd_unpair},
        {.command = "wifi",   .help = "wifi [status] | wifi set <ssid> [pass] | wifi clear", .func = cmd_wifi},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    ESP_ERROR_CHECK(esp_console_register_help_command());
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "console ready, type 'help'");
}
