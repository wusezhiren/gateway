# ESP32-S3 组网项目计划

## 目标

- 以酷世 `KS_S3_KORVO_V4`（ESP32-S3-WROOM-1-N16R8，16MB Flash / 8MB PSRAM）作为网关，
  采购 ESP32-C3 / ESP32-S3 核心板（**4MB Flash 及以上**）作为节点。
- 节点以执行器控制为主（继电器、PWM 调光/调速），常供电。
- 手机原生 Android App 在**同一局域网**内控制节点、管理配对、升级固件。
- 网关和节点都支持**局域网 OTA**，固件镜像**必须签名**才能被接受。

## 关键决策

| 项目 | 决定 | 说明 |
|---|---|---|
| 组网 | ESP-NOW 星型 | S3 无 802.15.4，不考虑 Zigbee/Thread；单跳，节点直连网关 |
| 网关上行 | Wi-Fi STA 连路由器 | ESP-NOW 与 STA 共用路由器信道，节点扫描信道找网关 |
| 链路加密 | ESP-NOW PMK/LMK | 加密 peer 上限 17，节点数上限 16 |
| 手机端 | Android 原生（Kotlin + Jetpack Compose） | 局域网 HTTP + WebSocket，mDNS 发现网关 |
| OTA 范围 | 仅局域网 | App/PC 上传固件到网关；外网升级暂不做 |
| 镜像签名 | `SECURE_SIGNED_APPS_NO_SECURE_BOOT`，RSA-3072 | 不烧 eFuse、可撤销；不开硬件 Secure Boot |
| 节点 Flash | 4MB | 双 OTA 槽各 1.5MB |

## 系统架构

```
[Android App] ──局域网 HTTP / WebSocket（token 鉴权）──> [Korvo 网关 ESP32-S3]
   控制 / 配对 / 上传固件 / 发起升级 / 查看进度             │ Wi-Fi STA ＋ ESP-NOW（同信道）
                                                            │ ① 网关自升级：HTTP 上传写入 OTA 槽
                                                            │ ② 节点固件暂存 node_fw 分区
                                                            ▼ ESP-NOW 加密单播，分片传输
                                                    [节点1] [节点2] … [节点N]
```

仓库结构：

```
components/meshproto/   网关与节点共用的 ESP-NOW 协议与密钥配置
gateway/                网关工程（ESP32-S3，Korvo）
node/                   节点工程（ESP32-C3 / ESP32-S3）
tools/                  PC 端脚本：签名、上传固件、调试 API
app/                    Android App
docs/                   计划、ESP-NOW 协议、App API 文档
```

## 分区表

网关（16MB）：

| 分区 | 类型 | 大小 | 用途 |
|---|---|---|---|
| nvs | data/nvs | 24KB | Wi-Fi 凭据、节点表、API token |
| otadata | data/ota | 8KB | |
| phy_init | data/phy | 4KB | |
| ota_0 | app | 3MB | |
| ota_1 | app | 3MB | |
| node_fw | data | 4MB | 节点固件暂存，C3 / S3 各 2MB 槽 |
| storage | data | 剩余 | 预留（日志、网页资源等） |

节点（4MB）：

| 分区 | 类型 | 大小 |
|---|---|---|
| nvs | data/nvs | 24KB |
| otadata | data/ota | 8KB |
| phy_init | data/phy | 4KB |
| ota_0 | app | 1.5MB |
| ota_1 | app | 1.5MB |
| 剩余 | | 预留 |

## 阶段 1：通信框架 + OTA 底座

已有代码草稿：协议组件、网关（Wi-Fi、节点表、配对、命令重传、串口命令）、节点（扫描入网、心跳、继电器/PWM）。

本阶段完成：

- [ ] 修复编译（`main` 补 `esp_timer` 依赖），网关 S3、节点 S3/C3 三份构建全部通过
- [ ] 按上表建立网关、节点分区表
- [ ] 开启 `BOOTLOADER_APP_ROLLBACK_ENABLE`
  - 节点：新固件入网并收到首个 HB_ACK 后 `esp_ota_mark_app_valid_cancel_rollback()`，60s 内未完成则回滚重启
  - 网关：ESP-NOW 链路启动且 Wi-Fi 连上（或未配置路由器）后标记有效
- [ ] 开启签名校验：`SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`
- [ ] 入网消息上报固件版本（`esp_app_desc_t.version`）、芯片型号、工程名
- [ ] 节点通道值存 NVS，上电立即恢复；网关记录期望状态，节点重新入网后补发
- [ ] 协议文档 `docs/protocol.md`
- [ ] 验证：Korvo 烧网关固件，串口命令 `pair / nodes / set / unpair / wifi` 可用；节点到货后联调配对、控制、掉线重连

## 阶段 2：局域网 OTA

### 2a 网关自升级

- `POST /api/ota/gateway`：流式写入空闲 OTA 槽，`esp_ota_end()` 校验签名，成功后切换启动分区并重启
- 网关重启期间节点掉线数秒，之后自动重新入网
- PC 脚本 `tools/ota_upload.py` 先行验证，App 复用同一接口

### 2b 节点升级（ESP-NOW 分片）

- `POST /api/ota/node?target=esp32c3|esp32s3`：存入 `node_fw` 对应槽
  - 网关预检：镜像头芯片型号、工程名、版本、签名块存在，元数据（版本、大小、SHA256）记入 NVS
- `POST /api/ota/node/start`：指定节点或全部同型号节点，网关逐个升级
- 新增协议消息（从第一版起保持兼容，这是今后唯一的升级通道）：

| 消息 | 方向 | 内容 |
|---|---|---|
| `OTA_BEGIN` | 网关 → 节点 | 大小、SHA256、版本、芯片型号、工程名、分片大小 |
| `OTA_DATA` | 网关 → 节点 | offset + 数据 |
| `OTA_ACK` | 节点 → 网关 | 累计确认 offset / 错误码 |
| `OTA_END` | 网关 → 节点 | 结束，节点校验后重启 |
| `OTA_ABORT` | 网关 → 节点 | 取消 |

- 传输：滑动窗口（8 片）+ 累计 ACK，超时从已确认 offset 续传；分片 200B 兼容 ESP-NOW v1，
  两端支持 v2 时可增大分片（最大 1470B）提速
- 升级期间正常处理 SET 和心跳，不影响控制
- 节点侧：`esp_ota_begin/write/end` 校验 SHA256 与签名，切换启动分区，重启
- 结果判定：节点以新版本重新入网 = 成功；超时未回来 = 失败（节点自行回滚）
- 拒绝降级（除非显式 `force`），拒绝芯片型号或工程名不符的镜像

### 签名密钥管理

- `espsecure.py generate_signing_key --version 2 --scheme rsa3072` 生成私钥
- 私钥**不进仓库**（`.gitignore` 排除 `*.pem`），需单独备份；丢失后已部署设备只能 USB 重刷
- 网关和节点共用一把签名密钥，构建时通过 `SECURE_BOOT_SIGNING_KEY` 指向本地路径
- 首次 USB 烧录的固件也必须是签名镜像

## 阶段 3：网关 App API

- [ ] SoftAP 配网：未配置路由器时开热点，App 下发 Wi-Fi 信息并生成 API token
- [ ] mDNS：`esp-gw.local`
- [ ] HTTP + WebSocket，全部接口 token 鉴权
  - 节点列表、状态推送、SET、配对窗口、解绑
  - OTA 上传、启动升级、进度推送
- [ ] API 文档 `docs/api.md`；`tools/` 下提供 Python 测试客户端

## 阶段 4：Android App

- [ ] 安装 JDK，搭建 Kotlin + Jetpack Compose 工程（`app/`）
- [ ] 配网、发现网关、保存 token
- [ ] 节点列表与控制：按节点上报的通道类型自动生成开关 / 滑条
- [ ] 配对与解绑
- [ ] 固件页：显示网关和各节点版本，从手机选择签名后的 `.bin` 上传，升级单个或全部节点，显示进度与结果

## 阶段 5：业务扩展

- [ ] 更多执行器类型（电机、舵机等）与输入类型（按键、传感器上报）

## 暂不做

- 外网远程控制与外网 OTA（云 broker、服务器拉取固件）
- 硬件 Secure Boot / Flash 加密（涉及不可逆 eFuse）
- 节点电池供电与深睡
- 多跳组网
