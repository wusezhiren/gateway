# ESP32-S31 网关替换计划

## 目标与约束

- 目标板：VIEWE PLCM-UES31S043H800V480C-U（ESP32-S31-WROOM-3、ES8389、双模拟麦克风、4.3 寸 RGB/GT911、SDMMC）。
- 保持 `components/meshproto` 的 ESP-NOW 帧和节点行为、现有 Android/HTTP/MQTT/OTA 契约；不能让两块网关同时使用相同发现名与节点配对身份。
- `gateway/` 继续作为 Korvo 的稳定固件；`gateway_s31/` 独立配置、独立构建目录和板级实现。语音公共组件先放在本仓库的 `components/voice_core/`，成熟后原样迁往独立仓库；架构与任务模型见 [VOICE_ARCHITECTURE.md](VOICE_ARCHITECTURE.md)。
- S31 预览目标使用 `~/esp/esp-idf-master` 和 `idf.py --preview`。记录验证过的 master 提交和组件锁版本；旧 S3/C3 保持各自已验证 IDF 环境。

## 工程边界

| 层 | 位置 / 接口 | 职责 |
| --- | --- | --- |
| 共享协议 | `components/meshproto/` | ESP-NOW 报文、校验；不嵌入板级代码 |
| 板级 | `gateway_s31/main/board/`（待建） | BSP 的 ES8389、GT911/RGB、SD、按键、电源与引脚；对上暴露采集/播放接口 |
| 可移植语音 | `components/voice_core/`（待建） | 会话、VAD、编解码、协议与 MCP；板级端口注入，后续独立仓库 |
| 语音适配 | `gateway_s31/main/adapters/`（待建） | 音频端口与节点 MCP 执行器，连接语音核心和 S31 网关 |
| 网关服务 | `gateway_s31/main/services/`（待建） | Wi-Fi、ESP-NOW、配网、API、MQTT、OTA；复用现有契约 |
| UI | `gateway_s31/main/ui/`（待建） | 800×480 横屏 LVGL、触摸和绑定确认 |

暂不创建空目录或把 Korvo 的 GPIO、ES8311/ES7210、SPI 屏初始化复制进 S31。每个模块可独立编译、验收后再接入 `app_main`。S31 的 SD 卡初始实现位于 `main/gw_sd_s31.c`，后续纳入 `board/`。

## 分阶段验收

1. **环境基线**：master 的 `--preview --list-targets` 包含 S3/S31/C3；S31 最小工程构建成功。已完成软件构建，尚无板上验证。整理 `sdkconfig.defaults`（Flash/PSRAM/分区/日志）和 BSP 版本，再烧录最小固件确认启动与 SD 卡挂载。
2. **模块契约与音频硬件**：先固定 `voice_core` 公开接口、FreeRTOS 任务所有权和模拟测试，再引入厂家 `viewesmart/bsp_ues31s043h800v480c_u`，分别验证 ES8389 双麦 16 kHz 录音（声道、峰值、无削波）及扬声器播放；先做手动触发的短录回放，不自动开机录放。处理 BSP 共享 I2S/I2C 生命周期及双工冲突。
3. **本地编解码**：将双声道转换为明确选择的单声道，验证 16 kHz/60 ms Opus 编码与 24 kHz Opus 解码、重采样播放；检查内存、连续多轮的资源释放和音质。
4. **单工联网语音（首期目标）**：用与旧网关相同的 NVS 凭据模型和云端协议接入 Wi-Fi/TLS/WebSocket；先手动触发一轮录音→STT→MCP→TTS，再从 SD 卡加载 WakeNet，验证真实唤醒、静音判停与连续多轮。录音与播放互斥，TTS 播放期间不监听声学唤醒。云端凭据仅在设备 NVS 或本地忽略文件，不放进固件或日志。
5. **全双工与打断（后续独立阶段）**：在音频端口确认同时录放稳定后，启用播放期间二次唤醒、旧会话取消与完整指令交接，至少两轮连续回归，无资源泄漏或回声误唤醒；不作为首期单工完成条件。
6. **替换网关**：迁移 ESP-NOW 配对、节点 SET/OTA、HTTP/MQTT/Android 兼容、显示绑定确认和独立 OTA 分区；进行节点/手机端到端验证，再决定切换旧网关。配对身份、token、设备 MAC 的迁移方式需在替换前明确。

## 当前状态与下一步

- 已下载厂家资料；曾启动一次音频 BSP 构建，但中途停止，试验代码与生成的未完成依赖锁已从源码区撤回，**没有音频编译结果或板上录放验证**。不把该试验视为完成的语音链路。
- 本机当前未检测到 S31 的 CH340 串口。第一阶段需接板后确定 `/dev/serial/by-id/` 对应设备，再观察启动日志；不对 Korvo 串口烧录。
- 下一步先固定配置与 BSP 引入方式，核对厂商 BSP 音频句柄生命周期，再实现可手动触发的录音/播放诊断；每阶段通过后才进入下一阶段。

厂家资料：<https://gitee.com/VIEWESMART/PLCM-UES31S043H800V480C-U>。
