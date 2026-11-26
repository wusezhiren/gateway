# 集成开发记录

## 2026-11-26：网关外设自适应 v0.7.1 发布、EMQX ACL 收紧与家里网关离线排查

- **主线整理与推送**：已将私有节点外网控制、管理员隔离与 HLW8032 计量上报变更整理至各子仓 `main` 并推送（`meshproto` `a3a632e`、`node_esp8266` `6825deb`、`android` `cc616e6`、`gateway` `356a37f` → `29da6b5`）。测试台架无屏临时分支保留在 `gateway` 本地 `temp-pure-gateway`，未污染主线。
- **网关外设自适应（`v0.7.1`）**：`gateway` 在 `gw_display_init()` 中增加 Korvo I2C 外设探测（`TCA9554` / `ES8311` / `ES7210` / `CST816S`），未检测到外设时自动进入无头模式，跳过屏幕、SD、语音唤醒（及 UAC）与 GPIO5 ADC 按键，避免无外设核心板悬空误触发配网和资源空转。已用 ESP-IDF 6.0.1 构建 `v0.7.1`，镜像大小 `0x30a6f0`（3,188,464 字节，小于 `0x310000` 与 `0x380000` 两种分区槽），SHA-256 `18869f54c92e4ba84a902c7d5888725c7513538c77a59814b85d9160fd2c54ce`。
- **云端固件发布**：已将 `esp_s3_gateway-0.7.1.bin` 上传至 Gitee `yu_ji_hai/app_release` 的 `v0.3.16` 发行版附件并更新 `version.json`（公网下载 SHA-256 校验通过）。手机关闭 Wi-Fi 走云端模式时，App「固件」页可触发「远程升级到 v0.7.1」。
- **EMQX Cloud ACL（`deployment-n4c113b3`）**：
  - `yjh`（4 条）：覆盖 `44bd8df31b70` 与 `7c4fad21de38` 的 `admin/cmd`（发布）、`admin/resp`（订阅）。
  - `gateway-s3`（12 条）：覆盖两台网关的 `online`/`state`/`resp`/`admin/resp`（发布）及 `cmd`/`admin/cmd`（订阅）。
  - `family`（10 条）：覆盖两台网关普通主题与 `espgw/app/latest`，并新增显式拒绝 `espgw/+/admin/#`（发布 & 订阅 / 不允许）。
  - 「全部用户」暂未加兜底 `#` 拒绝，待确认家里网关 `44bd8df31b70` 已切至 `gateway-s3` 后再收口为严格白名单。
- **当前待排查问题（交接至家里电脑处理）**：
  1. **家里带屏 Korvo 网关 `44bd8df31b70` 离线**：于 `2026-11-26 04:01` 触发遗嘱 `online=0`（最后状态 `03:59`，运行旧 `v0.7.0` / `ota_0`，`ZK520` / `192.168.31.121`），供电正常但未重连 EMQX。优先检查屏幕概览页或串口日志：确认是否因旧共享 MQTT 账号被删除/改密导致 `auth_failed`，或是凌晨 4 点路由器抖动导致 Wi-Fi 断开/设备挂起；连上后将云端账号改为 `gateway-s3`，再升级到 `v0.7.1`。
  2. **测试机核心板 `7c4fad21de38`**：当前在线（`gateway-s3`，`YYY` / `192.168.29.175`，运行旧 `v0.7.0`），已可远程升级到 `v0.7.1`（若已执行 `cloud enable-admin` 则需走串口升级）；其下挂 ESP8266 节点 `78:1c:3c:94:73:38`（`v0.1.1`）的 HLW8032 计量仍无数据，待后续排查硬件接线与 4800 8E1 帧解析。
  3. **App 主云端账号核对**：实测发现手机 App 曾使用 `yjh` 连入主 MQTT；后续开启严格白名单前，App 主「云端设置」应填 `family`，顶部「管理员」入口再填 `yjh`。

## 2026-11-25：管理员私有节点与 HLW8032 计量修复

管理员 MQTT 响应现在使用完整管理员节点列表，私有节点只从普通 `state` 和普通 API 列表隐藏；管理员解锁后仍可查看、设置私有属性并控制继电器。Android App 增加私有节点操作后的管理员列表刷新，并在普通节点卡片及管理员列表显示电压、电流和功率。

ESP8266 节点状态协议增加可选计量尾字段：电压 mV、电流 mA、功率 mW。旧节点状态仍按基础帧兼容；带 HLW8032 的新固件在读到有效帧后上报计量数据。计量系数目前沿用节点固件中的默认系数，正式部署前需用可靠仪表校准，不能把 App 显示值直接作为安全保护依据。

Android App `0.3.14` 已发布并强制更新。ESP32-S3 测试网关已用新协议固件刷入 `/dev/ttyACM2`，写入哈希校验通过，串口确认 MQTT `connected`。ESP8266 新固件已通过 PlatformIO 编译，但因节点已接入 220 V，未在带电状态下烧录；烧录前必须断开市电并确认节点安全，再用 `/dev/ttyUSB0` 刷入 `node_esp8266/.pio/build/nodemcuv2/firmware.bin`，随后重新验证继电器和计量上报。

ESP8266 `0.1.1` 计量固件已上传到 `v0.3.14` 发布附件，并加入在线 `version.json`，工程标识为 `node_esp8266`、芯片标识为 `esp8266`，可由网关节点 OTA 下载和推送。当前测试板节点仍未实际执行 OTA；需在 App 刷新节点状态后点击该节点的「升级」，等待节点以新版本重新入网，再确认计量数据出现。

补测：通过网关 OTA 后，节点 `78:1c:3c:94:73:38` 已确认以 `v0.1.1` 在线重新入网，未发生最终回滚；网关 ID `7c4fad21de38` 当前状态正常。通过普通 MQTT 状态订阅确认节点 `online=true`、`version=0.1.1`，但 `meter` 仍为空，说明 HLW8032 有效帧尚未被 ESP8266 解析到，计量链路还未通过。下一步应在不改变继电器控制的前提下检查 HLW8032 串口接线、电平、4800 8E1 配置和校准帧，暂不把 App 无计量显示归因于网关或 OTA。

## 2026-11-24：私有插座外网控制测试台架

| 设备 | 当前串口 | 识别信息 |
| --- | --- | --- |
| ESP32-S3 测试核心板 | `/dev/ttyACM2`（USB 序列号 `5D2F031995`） | `esptool flash_id` 确认 ESP32-S3 rev v0.2、8 MB PSRAM、16 MB Flash；Wi-Fi MAC `7c:4f:ad:21:de:38`，网关 ID `7c4fad21de38` |
| ESP8266 测试节点 | `/dev/ttyUSB0` | 台架标注，尚未刷写或联调 |

`/dev/ttyACM0` 和 `/dev/ttyACM1` 未探测成功；串口编号可能变化，烧录前以 USB 序列号和芯片 MAC 为准。已用 ESP-IDF 5.4 和 `build_temp_pure_gateway2` 对 `/dev/ttyACM2` 执行整套烧录，bootloader、分区表、OTA 初始数据和应用均通过写入哈希校验。复位后串口 `gw>` 可用，`cloud` 返回 `not configured`。App `0.3.11` 已发布。MQTT 管理员 `yjh` 和普通成员 `family` 的现有规则对应旧网关 ID `44bd8df31b70`；网关专用账号 `gateway-s3` 已建，测试板 ID 对应的授权及网关云端配置待做。测试先用空载插座，确认移动数据下管理员能解锁和控制、普通成员看不到也操作不了私有节点，再接实际负载。

**家中正式部署须重新烧录正式使用的 S3 网关。** 烧录前确认实际板的网关 ID，按该 ID 单独配置 MQTT 主题权限、Wi-Fi 和网关凭据，再验证普通控制及管理员外网解锁。节点配对和私有标记属于对应网关的配置；换板后检查是否需要重新配对或重新设为私有。已有兼容 ESP8266 节点不因这项权限功能而要求重刷。不要照搬测试板的网关 ID 或测试配置。

补测：纯网关启动分支提前返回，导致 MQTT 服务未初始化。已将 MQTT 初始化移入该分支，重新编译并通过 `/dev/ttyACM2` 执行 `app-flash`，写入哈希校验通过。测试板随后以独立账号 `gateway-s3` 通过 MQTT/TLS 上线，串口确认 `id: 7c4fad21de38`、`connected`。管理员通道尚未启用，待配置测试板主题权限并完成普通账号隔离后再联调。

## 2026-11-21：第三个 C3 节点恢复原始基线

识别第三个节点 MAC `90:64:9b:47:10:c8`，串口 `/dev/ttyACM0`。已执行整片 Flash 擦除并刷回原始 `esp_c3_relay` 固件，写入校验通过；通过 esptool 执行启动并多次打开网关配对窗口后，网关仍只看到前两个节点 `90:64:9b:45:7d:4c`、`90:64:9b:47:00:84`，第三节点未出现在 `/api/nodes`，其串口当前无应用层启动日志。第三节点尚未完成入网和控制验证，需继续检查 USB/复位/供电或板级启动状态。

## 2026-11-21：第二个 C3 节点恢复原始基线并验证控制

识别第二个节点 MAC `90:64:9b:47:00:84`，串口 `/dev/ttyACM0`。已执行整片 Flash 擦除并刷回原始 `node/build_c3relay/esp_c3_relay.bin`，随后网关配对窗口内重新入网。当前两个节点均在线：`90:64:9b:45:7d:4c`、`90:64:9b:47:00:84`。对第二节点通道 0 执行 SET `0→1`，网关状态立即回报 `value=1/desired=1`；再执行 `1→0`，回报 `value=0/desired=0`。第二节点原始基线控制闭环通过。第一个节点已同样恢复原始固件但此前未重新入网，需在其重新接入后单独复测。

## 2026-11-21：恢复原始固件基线并复测

已将 S3 网关通过 `/dev/ttyACM1` 刷回原始 `gateway/build_wake` 完整镜像（含 bootloader、16MiB 分区表、ota_data、app，保留 NVS），并将第一个 C3 relay 通过 `/dev/ttyACM0` 执行 `erase_flash` 后刷回原始 `node/build_c3relay` 固件。主机测试、两端原始固件构建均通过。清 NVS 后打开网关配对窗口，节点仍未重新入网，网关 `/api/nodes` 为 `online=false`，因此尚未执行 SET/STATE 控制验证；当前问题已排除本轮试验性代码和旧 NVS 残留，需继续检查节点供电、USB/复位状态、ESP-NOW 信道与板级硬件链路，暂不再改协议代码。

## 2026-11-21：撤回控制链路试验性修改

前几轮为处理现场 SET 失败，曾在 S3 网关和 C3 节点加入低功耗时序、JOIN_RSP 重发/单播、peer 清理和校验放宽等试验改动；现场仍未形成稳定闭环，且用户确认原节点控制代码此前正常。现已撤回这些试验代码与 C3 relay 低功耗配置变更，恢复到各子仓原始提交实现。原始 C3 relay 和 S3 网关固件重新构建成功，`bash tools/test_host.sh` 全部通过。现场设备尚未刷回原始版本；下一步应同时刷回原始网关/节点固件、清除节点 NVS 后重新配对，再以干净基线复测，避免混合不同试验版本。

## 2026-11-21：验证未加密单播 JOIN_RSP 与旧 peer 清理

新增诊断确认网关向节点建立未加密 peer 成功，三次 JOIN_RSP 的 `esp_now_send` 均返回 `ESP_OK`；节点端仍未出现 `joined gateway`，网关最终将节点标为 offline。已在节点重新扫描前删除旧加密网关 peer，并在网关改用临时未加密单播 JOIN_RSP，随后再恢复加密 peer。节点修复版已成功刷入 `/dev/ttyACM0`，网关诊断版已刷入 `/dev/ttyACM1`；当前 API 仍显示节点 offline，SET 未执行，最终闭环尚未通过。需要节点重新上电并抓取其启动/接收诊断日志，当前串口无应用输出。

## 2026-11-21：定位并修复 JOIN_RSP 入网失败

双串口实板日志显示：S3 网关持续收到 C3 节点 JOIN_REQ 并打印 `rejoined`，但节点始终 `no gateway found`，未接受 JOIN_RSP。网关原先只广播一次 JOIN_RSP，节点等待窗口仅 150 ms；已将网关广播改为 3 次、间隔 10 ms，并将节点 JOIN_RSP 校验从“必须匹配本次请求 seq”放宽为目标 MAC 和 `MP_JOIN_OK` 匹配，兼容重发响应。网关修复版已通过构建并刷入 `/dev/ttyACM1`；节点修复版构建通过（`0xcb640`），但当前节点未进入下载模式，刷写报 USB `Write timeout`，尚未写入。待按 BOOT+EN/RST 进入下载模式后刷入，再做双端 JOIN/SET/STATE 闭环验证。

## 2026-11-21：尝试关闭 C3 relay 低功耗模式

为绕过低功耗 ESP-NOW 接收窗口导致的掉链路，将 `node/sdkconfig.defaults.c3relay` 的 `CONFIG_NODE_LOW_POWER` 关闭，并同步修改本地 `build_c3relay/sdkconfig`。重新构建成功，`esp_c3_relay.bin` 大小 `0xcb650`，配置确认未启用低功耗。尝试通过 `/dev/ttyACM0` 刷入时两次均在进入下载模式阶段报 USB `Write timeout`，因此固件尚未写入节点；需要按住 BOOT、按一下 EN/RST 进入下载模式后重试。网关仍保留在线运行版本，尚未因本轮失败改变。

## 2026-11-21：继续定位 C3 节点无法入网

第一个节点 `90:64:9b:45:7d:4c` 刷入 0.4.2 修复版后，通过串口确认持续输出 `node_link: no gateway found, retry`；网关 API 显示的 `online=true` 是网关保留的旧节点表状态，不代表当前节点已连接。打开网关 60 秒配对窗口后节点仍未入网，故 SET 无法发送到节点。节点固件提供硬件复位键长按 3 秒清除旧网关绑定的路径；当前需对该节点执行物理清绑定，再在网关配对窗口内重新入网，之后才能继续验证 SET/STATE。

## 2026-11-21：修复低功耗节点控制链路并刷入网关

现场复测确认 S3 网关收到 SET 后节点持续重入网，实际值不变且网关日志报 `SET ... not acked`。先后修复网关侧心跳后 SET 延迟、待确认期间 `HB_ACK(STAY_AWAKE)`，最终定位到节点侧缺陷：收到 `STAY_AWAKE` 后只更新时间戳，没有立即调用 `lp_update(true)` 打开持续接收模式。已在 `node/main/services/node_link.c` 补上立即切换，C3 relay 固件构建通过（`0xcc8a0`，4MB 分区余 `0xb3760`）。S3 网关修复版已通过 USB `/dev/ttyACM0` 刷入，保留 NVS 和 16MiB 分区；主机全量测试与 `git diff --check` 通过。

当前两个 C3 节点未接 USB，尚未刷入节点修复版，因此现场 API 控制仍可能失败。待分别接入两个节点后执行 `idf.py -p <节点串口> -B build_c3relay flash`，再复测 `90:64:9b:45:7d:4c` 和 `90:64:9b:47:00:84` 的 SET/STATE 闭环。

## 2026-11-21：修复低功耗 C3 节点 SET 时序

现场验证发现两个 C3 节点均在线，S3 网关 API 返回 `{"ok":true}`，但节点实际 `value` 不变而 `desired` 改变。根因定位为网关收到节点心跳后立即连续发送 `HB_ACK` 和 `SET`；低功耗节点只有约 50 ms 接收窗口，SET 可能落在窗口关闭之后。修改 `gateway/main/services/gw_link.c`：收到心跳时先发送 `HB_ACK`，将待处理 SET 延迟 10 ms，交由链路任务在窗口内发送，避免两帧同时排队。`bash tools/test_host.sh` 全部通过，S3 `build_wake` 构建通过，镜像 `0x3367b0`、应用分区余 `0x49850`。尝试 OTA 到现场网关时返回 `413 too_large`，因为现场旧分区槽小于新镜像，尚未部署；需通过 USB 刷入包含新分区表的完整网关固件后再做实板复测。

## 2026-11-21：现场 API 控制节点回归

通过当前网关 `192.168.31.121` 的 API 读取到两个在线 C3 节点：`90:64:9b:45:7d:4c`（固件 0.4.1，RSSI -45~-51）和 `90:64:9b:47:00:84`（固件 0.4.2，RSSI -65~-67）。对第一个节点执行 `ch=0` 的 `value=1` 后网关返回 `200 {"ok":true}`，但等待 2 秒后节点实际 `value` 仍为 0、`desired` 为 1；随后发送 `value=0` 恢复期望值，返回仍为 200，最终 `value=0/desired=0`。结论：App/HTTP 鉴权和网关接收路径正常，节点在线但本次 SET 未得到实际状态确认；需要串口日志或现场观察继电器，继续定位 ESP-NOW SET/STATE 回执或继电器输出。未改变最终节点期望状态。

## 2026-11-21：继续查找 S3 TTS 灌音实板条件

检查本机网络为 `192.168.137.128/24`，扫描 `192.168.137.0/24` 未发现响应 `/api/info` 的 S3 网关；本机也没有 `/dev/serial/by-id`、`/dev/ttyACM*` 或 `/dev/ttyUSB*`。找到的相关音频中，`/home/yjh/code/esp-sr/esp-tts/samples/S3_xiaole_speed0.wav` 是 16 kHz/16-bit/mono，可满足接口格式，但不是已确认包含两轮唤醒/指令的测试文件；`nihaoxiaozhi_16k_stereo.wav` 是双声道，不符合 S3 `/api/wake/test` 的 mono 要求。本轮未发送灌音请求，避免向未知设备或错误音频执行测试。

## 2026-11-21：确认 S3 TTS/PCM 灌音验证入口

确认 `tools/test_wake_barge_in.py` 通过 `POST /api/wake/test` 向 S3 网关灌入 16 kHz、16-bit、mono WAV/裸 PCM，支持一次文件测试覆盖首次唤醒、第一轮指令、TTS 播放期间二次唤醒和第二轮指令。脚本已通过 `python3 -m py_compile` 和 `--help` 验证；当前主机没有可用网关串口、IP/token 或待灌音频文件，因此本轮未执行实板 HTTP 灌音，不能据此宣称 TTS 声学播放或唤醒闭环通过。

## 2026-11-21：验证 S3 网关唤醒构建

目标：确认当前 S3 网关 WakeNet/语音唤醒相关代码可编译，并运行已有主机侧语音测试。`bash tools/test_host.sh` 全部通过；加载 `/home/yjh/esp/esp-idf-6.0-git` 后在 `gateway/` 执行 `ninja -C build_wake` 成功，生成 `build_wake/esp_s3_gateway.bin`，镜像大小 `0x3367a0`，小于最小应用分区 `0x380000`，剩余 `0x49860`。本次未烧录实板，当前未发现 `/dev/serial/by-id` 或串口设备，因此尚未验证真实麦克风唤醒、WakeNet 日志和后续语音会话。

## 2026-11-19：推送总仓并切换默认分支

发现此前其他电脑直接克隆总仓时仍检出旧默认分支 `master`，因此只包含旧的 `listen_clouds` 子仓。已将 README 的分步克隆说明提交为 `e3a4036` 并推送到 `main`，再通过 GitHub API 将 `wusezhiren/gateway` 默认分支从 `master` 切换为 `main`。用临时目录执行 `git clone https://github.com/wusezhiren/gateway.git` 和 `git submodule update --init --recursive` 验证通过，S3/S31 网关、C3/S3 节点、Android、meshproto、voice_core、listen_clouds 共 8 个子仓均成功检出。

## 2026-11-19：调整总仓拉取说明

按一总仓多子仓的使用方式，README 改为先执行 `git clone https://github.com/wusezhiren/gateway.git`，再进入总仓目录执行 `git submodule update --init --recursive`，避免把子仓递归克隆和总仓初始化流程混在一条命令中。已有总仓仍使用 `git pull --ff-only` 后执行子仓初始化/更新。仅修改文档，未改变 Git 子模块配置或指针。

## 2026-11-19：确认一总仓多子仓结构

核对总仓 `.gitmodules` 和各子模块状态：本仓库是集成总仓，子仓包括 S3 网关 `gateway`、S31 网关 `gateway_s31`、C3 节点 `node`、S3 节点 `node_s3`，以及 `meshproto`、`voice_core`、`listen_clouds` 和 Android。总仓远程为 `https://github.com/wusezhiren/gateway.git`；S3 网关子仓远程为 `https://github.com/gateway-esp/gateway-s3.git`。本次仅核对配置，未修改代码或子模块指针。

## 2026-11-13：其余子仓统一整理

S3网关实现按board/services/ota/voice分层，C3/S3节点按board/services/ota分层；meshproto与voice_core实现集中src。Android及listen_clouds已有标准分包/目录，仅补导航。总仓测试脚本与S31跨组件测试同步新路径。全量tools/test_host.sh通过，S3网关IDF6构建、C3与S3节点IDF5.4构建通过。误用节点IDF6时暴露既有SHA头不兼容，恢复项目IDF5.4后通过，未为目录整理引入SDK迁移。没有烧录、发布或改变运行设备。

## 2026-11-13：工程导航与S31目录整理

先提交推送各子仓基线并固定总仓32177ea，再按S31职责拆分源码及公共头文件，保持子仓位置和构建目录不变。导航见[repository-layout.md](repository-layout.md)。S31主机测试、UI、分区检查和16MiB板测配置构建通过；未部署目录整理固件，不扩大为新增实板功能验证。

## 2026-11-13：S31 网关迁移基线

固定已上板开发的S31、S3迁移桥、Android0.3.5兼容及voice_core VAD组合。各子仓记录详见：

- [S31](../gateway_s31/docs/development-log.md)
- [S3](../gateway/docs/development-log.md)
- [Android](../android/docs/development-log.md)
- [语音核心](../components/voice_core/docs/development-log.md)

S31实物16MiB Flash/PSRAM，双OTA板测配置；卧室/客厅迁移后用户确认可控。SD待插卡，走廊灯不在本轮验收范围，完整语音尚未集成。配置凭据仅设备NVS和忽略的本地目录，不随本次提交上传。
## 2026-11-16：S3 语音节点接入 S31 网关

目标：S3 设备保留唤醒与控制入口，节点管理、配对、重试、状态和 OTA 统一由 S31 网关负责。

本次将跨设备控制定义为 `MP_NODE_CMD`（类型 13）。S31 ESP-NOW 接收层只允许已登记且在线的加密 peer 发起，并把请求交给 `s31_nodes_set()`，避免另起一套控制状态机。目标节点仍由 S31 的现有 SET 重试和 STATE 确认机制处理。

验证：总仓 `bash tools/test_host.sh` 全部通过，`git diff --check` 通过。S3 构建已进入工程配置但因本机 ESP-IDF 6.0 使用的 xtensa 工具链为 14.2、工程要求 15.2 而停止；S31 构建因当前 IDF 安装没有 `esp32s31` toolchain 而停止。待 S3 语音节点固件完成 MAC/目标节点选择及实板联调后验证实际语音控制。当前未烧录实板。
