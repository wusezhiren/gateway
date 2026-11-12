# ESP32-S31 网关工程

独立于 `gateway/`（Korvo ESP32-S3）的 ESP-IDF 工程，目标板为 VIEWE PLCM-UES31S043H800V480C-U。共享仓库中的 `components/meshproto` 协议组件，不修改现有节点协议。

当前处于板级移植阶段。模组外设通过 `idf.py menuconfig` → `S31 Gateway` → `Board peripherals` 独立开关：RGB LCD/GT911、ES8389 音频、SD、ADC 按键、WS2812、蜂鸣器、UART1、RS485、CAN。关闭模块后对应初始化代码不会编译或运行，以减少资源占用。现有固件中的 Wi-Fi、ESP-NOW、MQTT、HTTP、OTA、屏幕和语音服务尚未全部接入此工程，目前不能替换 Korvo 网关。

## 开发环境

厂家示例要求支持 `esp32s31` 的 ESP-IDF master 预览版；本机的 `release/v6.0` 不包含该目标。已在本机安装 `/home/yjh/esp/esp-idf-master`（验证时为 IDF 6.2 开发版）。在本目录运行：

```sh
. ~/esp/esp-idf-master/export.sh
idf.py --preview -B build_s31 -DIDF_TARGET=esp32s31 build
```

使用新构建目录时显式指定 `IDF_TARGET`；若用 `idf.py --preview set-target esp32s31`，它会先清理对应构建目录。目标检查：`idf.py --preview --list-targets`。

烧录前请先确认 Type-C CH340 串口设备及板上启动模式。厂商示例和引脚资料：<https://gitee.com/VIEWESMART/PLCM-UES31S043H800V480C-U>。


下一阶段的顺序与验收条件见 [移植计划](PLAN.md)；可移植语音、FreeRTOS 任务与 MCP 模块边界见 [语音架构](VOICE_ARCHITECTURE.md)。当前工程仅有 SD 卡板级引导，以及已通过主机契约测试的 `voice_core` 会话/MCP 接口；v1 上行报文编码已移到独立的 `listen_clouds` 仓库。尚未接入 S31 音频、FreeRTOS 任务或云端会话，不会在开机时自动录音或播放。
