# ESP32-S3 组网项目计划

## 目标

- 以酷世 `KS_S3_KORVO_V4`（ESP32-S3-WROOM-1-N16R8，16MB Flash / 8MB PSRAM）作为网关，
  采购 ESP32-C3 / ESP32-S3 核心板（**4MB Flash 及以上**）作为节点。
- 节点以执行器控制为主（继电器、PWM 调光/调速），常供电。
- 网关**按键长按进入配网**：开热点，手机连接后选择手机当前 Wi-Fi 或输入新 Wi-Fi 完成配网。
- 网关**触摸屏**展示网关与节点信息，左右滑动切换页面，可在屏幕上控制节点、升级节点固件。
- 手机原生 Android App 在**同一局域网**内控制节点、管理配对、升级固件。
- 网关和节点都支持**局域网 OTA**，固件镜像**必须签名**才能被接受。

## 关键决策

| 项目 | 决定 | 说明 |
|---|---|---|
| 组网 | ESP-NOW 星型 | S3 无 802.15.4，不考虑 Zigbee/Thread；单跳，节点直连网关 |
| 网关上行 | Wi-Fi STA 连路由器 | ESP-NOW 与 STA 共用路由器信道，节点扫描信道找网关 |
| 链路加密 | ESP-NOW PMK/LMK | 加密 peer 与热点客户端共用 17 个硬件密钥：节点上限 14，热点 3 |
| 配网 | SoftAP + 强制门户网页 + App 配网接口 | 不装 App 也能用浏览器配网 |
| 屏幕 UI | LVGL v9 + `esp_lvgl_port` + CST816T 触摸 | 中文界面，字体子集 |
| 手机端 | Android 原生（Kotlin + Jetpack Compose） | 局域网 HTTP + WebSocket，mDNS 发现网关 |
| OTA 范围 | 仅局域网 | App/PC 上传固件到网关；外网升级暂不做 |
| 镜像签名 | `SECURE_SIGNED_APPS_NO_SECURE_BOOT`，RSA-3072 | 不烧 eFuse、可撤销；不开硬件 Secure Boot |
| 节点 Flash | 4MB | 双 OTA 槽各 1.5MB |

## 网关硬件资源（KS_S3_KORVO_V4）

| 资源 | 连接 | 备注 |
|---|---|---|
| LCD | ST7789 280×240，SPI：SCLK=GPIO1，MOSI=GPIO0，DC=GPIO2，CS=GPIO46 | 电源/背光经 TCA9554 IO 扩展（P1、P2） |
| 触摸 | CST816T，I2C1：SDA=GPIO17，SCL=GPIO18 | 与 ES8311、TCA9554 共用总线；中断脚 `ESP_TP_INT` 待实物确认 GPIO |
| 按键 | 6 个 ADC 按键，ADC1_CH4（GPIO5），分压中心约 380/820/1180/1570/1980/2410 mV | index 0 = VOL+，1 = VOL-，**5 = REC（2.41V）用作配网键**，其余空闲 |
| BOOT 键 | GPIO0 | **复用为 LCD MOSI，运行时不可用作按键** |
| 串口 | CH343P → UART0 | 调试控制台 |

## 系统架构

```
[Android App] ──局域网 HTTP / WebSocket（token 鉴权）──> [Korvo 网关 ESP32-S3]
   控制 / 配对 / 上传固件 / 发起升级 / 查看进度             │ 触摸屏 UI：状态、节点、控制、升级
                                                            │ 按键长按：配网（SoftAP + 网页）
[手机浏览器] ──网关热点──> 配网网页                           │ Wi-Fi STA ＋ ESP-NOW（同信道）
                                                            ▼ ESP-NOW 加密单播，分片传输 OTA
                                                    [节点1] [节点2] … [节点N]
```

仓库结构：

```
components/meshproto/   网关与节点共用的 ESP-NOW 协议与密钥配置
gateway/                网关工程（ESP32-S3，Korvo）
  main/                 链路、Wi-Fi、配网、OTA、HTTP API
  main/ui/              LVGL 页面
node/                   节点工程（ESP32-C3 / ESP32-S3）
tools/                  PC 端脚本：签名、上传固件、调试 API
app/                    Android App
docs/                   计划、ESP-NOW 协议、App API 文档
```

## 分区表

网关（16MB）：

| 分区 | 类型 | 大小 | 用途 |
|---|---|---|---|
| nvs | data/nvs | 24KB | Wi-Fi 凭据、节点表、API token、屏幕设置 |
| otadata | data/ota | 8KB | |
| phy_init | data/phy | 4KB | |
| ota_0 | app | 3MB | 含 LVGL 和中文字体 |
| ota_1 | app | 3MB | |
| node_fw | data | 4MB | 节点固件暂存，C3 / S3 各 2MB 槽 |
| storage | data | 剩余 | 预留（配网网页资源、日志等） |

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

- [x] 修复编译（`main` 补 `esp_timer` 依赖），网关 S3、节点 S3/C3 三份构建全部通过
- [x] 按上表建立网关、节点分区表
- [x] 开启 `BOOTLOADER_APP_ROLLBACK_ENABLE`
  - 节点：新固件入网并收到首个 HB_ACK 后 `esp_ota_mark_app_valid_cancel_rollback()`，60s 内未完成则回滚重启
  - 网关：ESP-NOW 链路启动且 Wi-Fi 连上（或未配置路由器）后标记有效
- [x] 开启签名校验：`SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`
- [x] 入网消息上报固件版本（`esp_app_desc_t.version`）、芯片型号、工程名
- [x] 节点通道值存 NVS，上电立即恢复；网关记录期望状态，节点重新入网后补发
- [x] 协议文档 `docs/protocol.md`
- [x] 验证：Korvo 烧网关固件，启动、分区表、签名、串口命令 `pair / nodes / set / wifi` 正常（控制台走原生 USB）
- [ ] 节点到货后联调：配对、控制、掉线重连、状态恢复、解绑
- [ ] 回滚验证留到阶段 3（需要 OTA 才能产生待确认镜像）

## 阶段 2：配网

### 进入配网

- 从未配置过路由器：开机自动进入配网模式
- 未联网（已配置但连不上）：**长按 REC 键 3 秒**进入
- 已联网时长按：屏幕弹窗确认后进入，避免误触断网
- 屏幕设置页也可进入；10 分钟无操作自动退出，恢复原配置

### 热点与屏幕提示

- 热点名 `ESP-GW-XXXX`（MAC 后 4 位），WPA2，固定密码 `a12345678`（menuconfig `GW_PROV_AP_PASSWORD`，留空则每次随机 8 位）
- 屏幕显示热点名、密码和**连接热点的二维码**（`WIFI:T:WPA;S:...;P:...;;`），手机扫码即可连上
- 屏幕实时显示配网进度：等待手机连接 → 已连接 → 正在连接路由器 → 成功（显示 IP）/ 失败原因

### 配网网页（无需 App）

- 强制门户：DNS 劫持 + HTTP 重定向，手机连上热点后自动弹出配网页；也可手动访问 `http://192.168.4.1`
- 页面内容：
  - 网关扫描到的 **2.4GHz** 网络列表，按信号强度排序，点选后输入密码
  - 手动输入 SSID / 密码（支持隐藏网络）
- 提交后网关以 APSTA 模式尝试连接，页面轮询结果：成功 / 密码错误 / 找不到网络 / 获取 IP 超时
- 成功后保存凭据，热点再保留 30 秒让手机拿到结果后关闭

### App 配网（阶段 6 实现，接口在本阶段定好）

- App 先读取手机**当前连接的 Wi-Fi 名称**预填（需定位权限），用户输入密码
- App 用 `WifiNetworkSpecifier` 自动连接网关热点，调用配网接口下发凭据并回读结果，同时拿到 API token
- **限制**：Android / iOS 都不允许 App 或网页读取系统保存的 Wi-Fi 密码，
  “选择手机已连接的 Wi-Fi”只能自动填好 SSID，**密码仍需用户输入一次**
- **限制**：ESP32-S3 只支持 2.4GHz；手机当前连的是 5GHz 网络时提示用户换成同一路由器的 2.4GHz 网络

### 其他

- 配网期间 ESP-NOW 照常工作；连上路由器后信道变化，节点会自动重新扫描入网
- 串口 `wifi set` 命令保留作调试用，`prov start` 可在已联网时强制进入配网（屏幕确认框做好之前的替代）
- ~~已知问题（阶段 1 实测）~~ 已修复：自动重连改为 5s→5min 指数退避，带信道提示优先扫上次信道，重试间隙回到上次信道

### 阶段 2 进度

- [x] REC（ADC index 5，实测 2407mV）长按 3 秒：未联网进入配网；已联网仅提示需屏幕确认；配网中长按取消
- [x] SoftAP + DHCP option 114 + DNS 劫持 + 404 重定向，Android 自动弹出配网页（实测）
- [x] 配网网页：扫描列表、手动输入、密码显示切换、结果提示；请求带超时、串行轮询
- [x] 先回响应再连路由器，并用扫描到的信道做提示，切信道期间页面仍能拿到成功结果（实测）
- [x] 成功后保存凭据与信道、生成 API token，30 秒后关热点；10 分钟无操作退出（实测）
- [x] 重启后自动重连（0.2s 关联，1.5s 拿到 IP）
- [x] ESP-NOW 加密 peer 数降为 14，给热点留 3 个连接（否则热点连接数为 0）
- [x] 配网接口文档 `docs/api.md`
- [ ] 密码错误、找不到网络的页面提示（逻辑已实现，待实测）
- [ ] 自动重连指数退避（需断开路由器实测）
- [ ] 已联网时的屏幕确认框（阶段 4）

## 阶段 3：局域网 OTA

### 3a 网关自升级

- `POST /api/ota/gateway`：流式写入空闲 OTA 槽，`esp_ota_end()` 校验签名，成功后切换启动分区并重启
- 屏幕显示接收与校验进度，重启前提示（阶段 4，进度回调 `gw_ota_set_cb` 已就绪）
- 网关重启期间节点掉线数秒，之后自动重新入网
- PC 脚本 `tools/ota_upload.py` 先行验证，App 复用同一接口

进度：

- [x] 常驻 HTTP 服务 `gw_http`，配网接口改挂其上（非配网时 404）；`GET /api/info`；Bearer token 鉴权
- [x] `POST /api/ota/gateway`：先验镜像头（app 镜像 / 工程名 / 版本不降级，`force=1` 例外）再写 flash
- [x] PC 工具 `tools/ota_upload.py`（绕过系统代理）；串口 `token`、`ota` 命令
- [x] 实测（Korvo，局域网，约 8 秒/900KB）：
  - 拒绝：无 token 401、同版本 409、节点固件 400、随机数据 400、**未签名固件签名校验失败 400**
  - 0.1.0 → 0.2.0 升级成功，`ota_1` 启动，2.4 秒确认有效
  - **回滚**：OTA 一个确认前 abort 的 0.3.0（`GW_TEST_ABORT_BEFORE_VALIDATE`），bootloader 自动回到 0.2.0
  - `force=1` 降级回 0.1.0，回滚后的分区可再次写入

### 3b 节点升级（ESP-NOW 分片）

- `POST /api/ota/node?target=esp32c3|esp32s3`：存入 `node_fw` 对应槽
  - 网关预检：镜像头芯片型号、工程名、版本、签名块存在，元数据（版本、大小、SHA256）记入 NVS
- 发起方式：App / API（`POST /api/ota/node/start`）或**网关屏幕**（单个节点或全部同型号节点），网关逐个升级
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

## 阶段 4：网关屏幕 UI

### 交互

- 左右滑动切换主页面，底部页码圆点；上下滑动滚动列表；点击进入详情，右滑或返回键返回
- 顶部状态栏：Wi-Fi 状态图标、在线节点数 / 总数、时间（联网后 SNTP 对时）
- 危险操作（解绑、升级、进入配网）弹窗二次确认
- 无操作 60 秒熄屏，触摸或按键唤醒；配网、升级进行中不熄屏
- 事件驱动刷新：节点上下线、状态变化、OTA 进度通过 `gw_link` 事件推给 UI

### 页面

| 页面 | 内容 | 操作 |
|---|---|---|
| 概览 | 网关名、Wi-Fi（SSID / IP / 信号）、ESP-NOW 信道、在线节点数、固件版本、运行时间 | — |
| 节点列表 | 每个节点一张卡片：名称、在线状态、RSSI、版本、“可升级”标记 | 点击进入详情 |
| 节点详情 | MAC、版本、运行时间、信号；按通道生成开关 / 滑条 | 控制通道、**升级固件**、解绑 |
| 固件 | 网关版本、暂存的节点固件版本（C3 / S3）、可升级节点数、升级队列与进度条 | **全部升级**、取消 |
| 设置 | 配对窗口（60 秒倒计时）、进入配网、亮度、熄屏时间、关于、重启 | — |
| 配网（弹出） | 热点名、密码、二维码、配网进度 | 取消配网 |

### 实现要点

- `esp_lvgl_port` 管理 LVGL 任务与刷新，`esp_lcd_touch_cst816s` 驱动触摸（兼容 CST816T）
- 绘制缓冲放内部 RAM（DMA），LVGL 对象内存可放 PSRAM
- 中文字体用 LVGL 字体工具生成常用字子集（16px / 20px），控制固件体积
- UI 只调用 `gw_link` / `gw_wifi` / OTA 模块的公开接口，不直接操作 ESP-NOW
- 先用串口命令验证业务逻辑，UI 最后接入

## 阶段 5：网关 App API

- [ ] mDNS：`esp-gw.local`
- [ ] HTTP + WebSocket，全部接口 token 鉴权（token 在配网时生成）
  - 节点列表、状态推送、SET、配对窗口、解绑
  - OTA 上传、启动升级、进度推送
- [ ] API 文档 `docs/api.md`；`tools/` 下提供 Python 测试客户端

## 阶段 6：Android App

- [ ] 安装 JDK，搭建 Kotlin + Jetpack Compose 工程（`app/`）
- [ ] 配网：读取当前 Wi-Fi 名称预填、自动连接网关热点、下发凭据、保存 token
- [ ] 发现网关（mDNS）
- [ ] 节点列表与控制：按节点上报的通道类型自动生成开关 / 滑条
- [ ] 配对与解绑
- [ ] 固件页：显示网关和各节点版本，从手机选择签名后的 `.bin` 上传，升级单个或全部节点，显示进度与结果

## 阶段 7：业务扩展

- [ ] 更多执行器类型（电机、舵机等）与输入类型（按键、传感器上报）

## 暂不做

- 外网远程控制与外网 OTA（云 broker、服务器拉取固件）
- 硬件 Secure Boot / Flash 加密（涉及不可逆 eFuse）
- 节点电池供电与深睡
- 多跳组网
