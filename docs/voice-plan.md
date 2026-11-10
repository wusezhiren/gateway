# 原网关语音中控迭代计划

> 状态：待实施。本计划直接在本仓库的 `gateway/` 和 `android/` 中迭代，不替换现有网关固件架构或 Android App。

## 目标和边界

- 保留 ESP-NOW 网关、屏幕、节点配对及升级、Android App、本地 HTTP 和现有云端控制协议。旧节点仍由本仓库的网关配对和控制。
- 在 KS_S3_KORVO_V4（ESP32-S3 N16R8）同一块板、同一网关固件上加入“**小智小智**”本地唤醒、双麦收音、云端语音请求、扬声器回复和屏幕状态。语音工具复用现有节点控制入口，避免维护第二套节点协议。
- 首版借助 Wi-Fi 云端服务实现中文识别、意图和合成语音，树莓派/HA 不是必需依赖。断网时 App、屏幕、ESP-NOW 本地控制仍可用。离线语音命令另作后续阶段。
- 已刷 ESPHome 的 S3 彩灯板不属于本仓库的 ESP-NOW 节点，须通过另行实现的适配层/可选 HA 桥接才可由本语音网关控制；首版先验证旧网关已配对的节点。
- 多台语音终端以后再扩展：各终端独立唤醒、鉴权、房间上下文；仅主网关持有 ESP-NOW 配对与节点状态。

```text
Android App ──HTTP/原 MQTT──┐
                           ├── Korvo S3：原屏幕 + ESP-NOW 网关 ──旧协议── 已配对节点
“小智小智”──本地唤醒──双麦──┘                    ▲
                   │                      工具调用 / 结果回报
                   └── Wi-Fi ── 云端中文语音与 MCP ──┘

可选后续：HA 中的 ESPHome 彩灯 ←→ 桥接/适配层 ←→ 网关
```

## 资源与兼容性门槛

| 项目 | 当前依据 | 实施要求 |
|---|---|---|
| 代码 | `gateway/main/main.c`、`gw_cloud.c`、`gw_api.c`、`gw_link.h`；App 的 `GwApi.kt` | 通过原有控制接口执行语音命令；不阻塞 ESP-NOW 回调；保持原 API 和 App 兼容 |
| 音频硬件 | KS_S3_KORVO_V4 原理图：ES7210 双麦、ES8311 DAC、NS4150B 功放；I2C GPIO17/18、MCLK GPIO16、BCLK GPIO9、LRCK GPIO45、输入 GPIO10、输出 GPIO8、功放 GPIO48 | 开发前核对原理图、板级例程和实测引脚；分离录音、播放和音频任务 |
| 按键 | ADC 按键 GPIO5，REC 长按已有配网用途 | 保留 REC 长按语义；语音按键不能与配网冲突 |
| Flash | 16 MiB；双 3 MiB OTA 应用槽、4 MiB `node_fw`、约 5.875 MiB `storage` SPIFFS；加入 Opus 后镜像 0x2babf0，单槽余 0x45410（约 277 KiB） | 后续云端协议/模型继续测算实际占用；如需改分区，先设计迁移与恢复，不得默默缩减节点暂存或破坏 OTA |
| TF 卡（可选） | KS_S3_KORVO_V4 原理图有 J2 TF 卡座：SD_CLK GPIO15、SD_CMD GPIO7、SD_DATA0 GPIO4；当前网关源码未占用这三根 GPIO | 优先评估 SDMMC 1-bit 模式存放提示音及可从文件加载的模型；插卡后实测读速、启动及拔卡恢复。是否支持直接从文件加载唤醒模型取决于所选语音组件，不能预先假定可用 |
| RAM | 8 MiB Octal PSRAM；Wi-Fi、MQTT TLS、LVGL 和节点 OTA 已占内部 RAM | 测内部堆/PSRAM 余量、任务栈、音频延时和看门狗；DMA 缓冲按硬件约束分配 |
| 唤醒词 | 参考实现有“你好小智”模型，**并非**用户指定的“小智小智” | 确认目标词模型来源、许可、ESP-SR 版本、体积和检测效果；不可只改显示文字 |
| SDK | 网关已迁移到 ESP-IDF 6.0.1 并上板启动，原 5.4 环境仍保留 | 旧节点、App 和 OTA 的完整回归尚待实际设备/升级场景验证；迁移不等于直接替换网关架构 |

参考：[小智开源项目](https://github.com/78/xiaozhi-esp32)、[MCP 工具说明](https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md)、[自定义板说明](https://github.com/78/xiaozhi-esp32/blob/main/docs/custom-board.md)。MCP 工具调用负责请求设备动作，不承担语音识别，也不会自动接管 ESPHome 设备。

## 分阶段任务与验收

| 阶段 | 改动与依赖 | 验收 |
|---|---|---|
| 0 基线 | 在本仓库重编 `gateway/`；记录镜像大小、分区、可用内存；备份现有配置/回退路径，并验证旧节点、屏幕、App | 原功能正常，可回退；未有现场设备时先完成静态/编译检查，运行项标记待测 |
| 1 音频 | `gateway/main/gw_audio.[ch]` 与依赖：ES7210/ES8311、I2S 和功放控制 | 双麦可录到可辨语音、扬声器可播报；并发运行屏幕和 ESP-NOW 无明显退化 |
| 2 唤醒 | `gw_wake.[ch]`、模型资源与配置；验证“**小智小智**”模型 | 多人、距离、噪声下统计漏检/误唤醒；无模型时按键说话仅用于诊断，不能算本阶段完成 |
| 3 云端语音 | `gw_voice_transport.[ch]`；选定服务及协议、设备绑定、NVS 凭据、重连和超时 | 唤醒→中文识别→回复闭环；断网恢复；凭据不进入源码、日志和仓库 |
| 4 设备工具 | `gw_voice_tools.[ch]`；MCP 工具映射至现有网关接口，按 MAC+通道定位设备，校验类型和值域 | 可查询并控制旧网关配对的节点；离线、非法值和超时有真实结果；App 状态一致 |
| 5 中控和多端 | 现有 UI 加语音状态、App 增语音设置/静音；制定卫星语音终端路由 | REC 长按配网正常；多入口并发控制状态一致；卫星节点不另维护配对表 |
| 6 可选桥接 | 若需要控制已经迁入 HA 的 ESPHome 灯，再接入 HA/局域网适配层 | 双向状态正确且断线有反馈，不影响旧 ESP-NOW 节点 |

基线编译与上板记录（2026-11-10）：在 `gateway/` 使用 ESP-IDF 5.4 运行 `idf.py -B build_voice_baseline -DSDKCONFIG=build_voice_baseline/sdkconfig build`，全量编译通过；镜像大小 0x28a120，3 MiB 槽剩余 0x75ee0（约 15%）。旧 `build/` 缓存镜像为 0x291000，不能用旧产物推断本次改动。新接入的 Korvo 板使用 USB JTAG 串口（USB 序列号 `98:88:E0:14:75:E0`），已烧录该基线镜像并在串口确认：Flash 16 MiB、PSRAM 8 MiB 初始化及内存测试通过；`esp_s3_gateway v0.6.2` 从 `ota_0` 正常启动；ESP-NOW 初始化、屏幕与 CST816S 触摸初始化均正常；未配置路由器，进入配网并启动串口命令。节点实际控制、App、录音、播放及开机后剩余内存仍待实测。

TF 卡资源策略：卡用于可替换的大文件，不会扩展 Flash 中的 app 分区，也不能直接解决语音库代码超过 3 MiB 的问题。模型须先验证文件加载 API 与实时性；若加载时须占用 RAM/PSRAM，也要单独计入内存预算。卡缺失、损坏或版本不匹配时仅停用相关语音能力，原组网和 App 仍需正常启动。模型/提示音按版本校验，服务密钥保存在现有安全配置中而非明文放卡上。先用板载 Flash 完成音频最小闭环，再决定是否把资源移到 TF 卡。

控制反馈语义：目前 `gw_api_exec()` 对 `set` 返回成功表示网关接受/排队发送命令，不能直接声称执行器已生效；语音播报“已完成”前须等待节点状态回报或超时，并区分睡眠节点的延迟生效。

每阶段都检查网关镜像尺寸、OTA/USB 恢复和旧 App 回归。版本号在基线和分区方案通过后确定；未经现场验证不发布或覆盖正在使用的网关。

## 尚待确认

### 当前实现进度（2026-11-10）

- Korvo 已连接路由器，HTTP `/api/info` 返回 200；屏幕、触摸和 ESP-NOW 初始化正常，暂无配对节点。
- `audio probe` 实测 ES7210/ES8311 应答正常；`audio mic` 已采到双麦有效数据（四个 TDM 槽中的 0、2 槽），扬声器测试音已由现场确认可听到。按当前需求跳过录音回放验收，优先推进语音上传。
- `gw_audio_capture()` 提供有界时长的 16 kHz、16-bit 单声道 PCM 回调，原诊断命令继续可用。基于 IDF 6.0.1 的音频版本已刷板；网关 Wi-Fi、ESP-NOW 初始化、显示和麦克风诊断通过，旧节点/App/OTA 完整回归尚待验证。
- 已核对上游 WebSocket 协议：上传 Opus、16 kHz、单声道、60 ms 帧；需设备身份、Client UUID、服务端鉴权与 hello 会话协商。尚未实现云端连接、ASR 或 MCP 云端闭环。
- 在 IDF 6.0.1 + `esp_audio_codec` 2.5.0 下，用独立 PSRAM 栈任务完成 16 kHz 单声道、60 ms Opus 编码：每帧输入 1,920 字节、输出缓冲 220 字节；2.04 秒采集得到 34 帧、4,080 字节编码数据，三次编码耗时约 0.73–0.75 秒；编码期间内部堆约 53 KiB。最初在串口 REPL 小栈上直接调用编码器导致崩溃，独立任务后已排除；用 `vTaskDeleteWithCaps()` 回收任务栈，重复运行 PSRAM 不再逐次下降。当前镜像 0x2babf0，3 MiB 槽余 0x45410（约 277 KiB），后续需严格控制新增代码和资源。
- 已加入本地 MCP 工具适配 `gw_voice_tools.[ch]`，只暴露 `gateway.nodes.list` 与 `gateway.node.set`，底层复用 `gw_api_nodes_json()` / `gw_api_exec()`；明确拒绝配对、解绑、网关升级等高权限操作。串口 `mcp list|call` 已在 Korvo 验证：无节点列表正常、未知节点返回 `no_such_node`、非法 MAC 返回 JSON-RPC `-32602`；工具处理函数现在返回 JSON 对象，后续可直接挂 WebSocket/MQTT 传输层。
- 已加入 `gw_voice_ws.[ch]` WebSocket 会话骨架，使用 IDF 6.0.1 的 `esp_websocket_client`：NVS 保存 URL/Token/Client UUID，连接时发送设备身份和 `hello`，收到服务端 `hello` 后才处理 `type=mcp`，MCP 回复包装回 `session_id`/`type=mcp`。串口命令为 `voicews status|start|stop|config <wss-url> <token>`；未配置时 `start` 正确拒绝。真实云端握手尚未执行，不能把骨架当作已接入云端。
- WebSocket/MCP 骨架版已在 Korvo 编译并上板；网关仍能联网、查询 OTA 状态和执行 Opus 诊断，镜像为 0x2bf950，3 MiB 槽余约 263 KiB。真实 URL、Token 和激活信息尚未配置，下一步需要先完成云端账号/设备绑定，再做 hello 握手与服务端 MCP 回包实测。
- 官方设备激活流程返回连接配置，不能简单填一个固定 WebSocket 地址代替激活。仅借鉴接入协议，原固件 OTA 仍由本仓库管理。免费额度/长期收费政策尚未得到可核实的官方说明，不承诺免费不限量。

1. 云端语音服务的设备接入、鉴权和 MCP 支持方式；首选验证可直接连云端的方案，不强制依赖树莓派。
2. “小智小智”模型的可用性与适配方式；它和“你好小智”不是同一个唤醒词。
3. 首版作为实测对象的旧网关节点及其可控通道；已迁移 ESPHome 的彩灯列为桥接阶段。
