# 云端协议（MQTT）

网关和手机 App 都主动连同一个 MQTT broker（目前用 EMQX Cloud Serverless，MQTT over TLS 8883），
App 在任何网络下都能控制网关，不需要端口映射或 VPN。

```
节点 ──ESP-NOW── 网关 ──MQTT/TLS──► EMQX Cloud ◄──MQTT/TLS── 手机 App
```

## 连接

| | 网关 | App |
|---|---|---|
| 地址 | `mqtts://<host>:8883` | `ssl://<host>:8883` |
| 证书 | ESP-IDF 证书包（公共 CA） | 系统 CA |
| client id | `espgw-<id>` | `espgw-app-<随机>` |
| 账号 | broker 客户端认证（网关和 App 共用一组） | 同左 |
| 遗嘱 | `espgw/<id>/online` = `0`（保留） | — |

`<id>` 为网关 STA MAC 小写去冒号，如 `44bd8df31b70`（`/api/info` 的 `cloud.id`）。

网关配置存 NVS（`gw_cloud`），写入方式：

- App「网关 → 云端设置」，局域网内保存时调用 `POST /api/cloud` 下发
- 串口 `cloud set <host> <port> <user> <pass>`，`cloud off` 关闭，`cloud` 查看状态

网关连上路由器后才开始连 broker，断线由 esp-mqtt 每 10 秒重连。

## Topic

### 私有节点（ESP32-S3 网关）

管理员模式使用同一 TLS broker 上的独立管理员账号。配置三个账号，并对**指定网关 ID 的精确主题**设置 ACL；下表之外的权限默认拒绝。

| 账号 | 发布 | 订阅 |
|---|---|---|
| 网关 | `online`、`state`、`resp`、`admin/resp` | `cmd`、`admin/cmd` |
| 普通成员 | `cmd` | `online`、`state`、`resp`；如需 App 更新通知，另允许 `espgw/app/latest` |
| 管理员 | `admin/cmd` | `admin/resp` |

表中的相对主题都以 `espgw/<id>/` 开头。管理员账号使用独立密码，按 broker 能力配置登录失败限制。禁止共享账号和通配符覆盖 `admin/#`。网关内部仍验证绑定令牌。管理员通过 `unlock` 取得 5 分钟会话，之后 `get`、`privacy`、`set` 带 `session`，一次只保留一个管理员会话。`privacy` 接受 `mac` 和布尔字段 `private`，可先登记 MAC 再配对。管理员响应不保留。

先配置 ACL 并吊销或轮换已有共享凭据，再从网关串口配置专用网关 MQTT 账号。验证普通账号不能访问管理主题后，执行 `cloud enable-admin` 开启功能。开启后，网关不再向 App 同步 broker 密码，HTTP 云端配置入口和普通网关 OTA 入口也会关闭；网关云端配置通过物理串口维护。每台手机在 App「云端设置」中保存普通成员账号；顶部「管理员」入口另行输入管理员账号和密码。App 锁定、离开前台或会话到期时清除私有列表。

联调先用不接 PC 的插座：确认转为私有后，新订阅者收到的共享保留状态不含该节点；在局域网和外网验证普通成员无权操作；管理员手机关闭 Wi-Fi 后验证解锁、供电、取消断电、确认断电及继电器状态回报。最后验证网关重启后仍私有。曾共享的节点如果在旧版 App 上绑定过桌面小组件或快捷开关，要升级客户端并刷新公共列表，清理旧标签。

| Topic | 方向 | QoS | 保留 | 内容 |
|---|---|---|---|---|
| `espgw/<id>/online` | 网关 → | 1 | 是 | `1` 在线；断线时 broker 发遗嘱 `0` |
| `espgw/<id>/state` | 网关 → | 1 | 是 | `{"info": <GET /api/info>, "nodes": <GET /api/nodes>}` |
| `espgw/<id>/cmd` | App → 网关 | 1 | 否 | 命令 |
| `espgw/<id>/resp` | 网关 → App | 1 | 否 | 命令结果 |

- `state` 在节点上下线、通道变化、配对、OTA、配网状态变化时发布（间隔不小于 300ms），无变化时每 30 秒发布一次。
  保留消息让 App 一订阅就拿到最新状态。
- `pair_left` 是发布时刻的剩余秒数，App 自己倒计时。

## 命令

```json
{"id": "a1b2c3", "token": "<API token>", "op": "set", "mac": "aa:bb:cc:dd:ee:ff", "ch": 0, "value": 1}
```

| op | 参数 | 同 HTTP |
|---|---|---|
| `set` | `mac`、`ch`、`value` | `POST /api/nodes/set` |
| `unpair` | `mac` | `POST /api/nodes/unpair` |
| `rename` | `mac`, `name` | `POST /api/nodes/rename` |
| `pair` | `seconds`（0~300） | `POST /api/pair` |
| `get` | — | 只触发一次 `state` 发布 |
| `ota` | `url`（https）、`sha256`（64 位十六进制）、`version`（可选，仅显示）、`force`（可选） | 无（远程升级专用，见下） |
| `node_ota` | `mac`、`url`（https）、`sha256`、`force`（可选） | `POST /api/nodes/ota` |

回复：`{"id": "a1b2c3", "ok": true}` 或 `{"id": "a1b2c3", "ok": false, "error": "offline"}`，错误码与 HTTP 接口相同，
另有 `unauthorized`（token 不对）、`bad_op`。执行后网关会发布新的 `state`。

## 远程升级网关（ota）

不在局域网时，App 从发布仓库的 `version.json` 取 `gatewayUrl` / `gatewaySha256`，发 `ota` 命令，
**网关自己用 HTTPS 下载**（跟随 gitee 的跨域跳转，证书用 IDF 证书包校验），边下边写空闲 OTA 分区。

- 回复 `ok: true` 只表示已开始；进度和结果看 `state.info.ota`：
  `state` = `receiving`（`received` / `total`）→ `rebooting` → 网关以新版本重新上线；或 `failed` + `error`
- 立即拒绝：`bad_url`（非 https）、`bad_sha256`、`busy`、`no_mem`
- 下载后拒绝（当前固件照常运行）：`not_newer` / `wrong_project` / `not_an_app_image`（读到镜像头就拒绝，不擦写）、
  `connect_failed` / `download_http_error` / `download_failed` / `incomplete`、`sha256_mismatch`、`invalid_image_or_signature`

防变砖：

1. 只写空闲 OTA 分区；bootloader、分区表、正在运行的分区永不改写，断电或断网只会让这次升级作废
2. 切换启动分区前：SHA-256 与命令一致 + `esp_ota_end` 校验镜像完整性和 RSA 签名
3. 新固件首次启动必须在 180 秒内**连上 Wi-Fi 和云端**（配了云端时）才确认有效，否则自动回滚；
   确认前崩溃或看门狗复位，bootloader 也会回滚。回滚后 `info.ota.rolled_back` 给出被回滚的版本
4. App 只在 `gatewayVersion` 高于网关当前版本时提供远程升级，不做降级

## 升级节点（node_ota）

App 从 `version.json` 的 `nodes` 里按**工程名 + 芯片**找到适合该节点的固件，版本更高时提供“升级”，发 `node_ota` 命令
（局域网时走 `POST /api/nodes/ota`，效果相同）。网关自己下载到 `node_fw` 分区，核对后经 ESP-NOW 推给节点
（协议与防变砖见 [protocol.md](protocol.md#节点升级)）。同一份固件已在暂存区（SHA-256 相同）时不重复下载。

- 回复 `ok: true` 只表示已开始；进度和结果看 `state.info.node_ota`：
  `downloading` → `sending` → `rebooting` → `done`；或 `failed` + `error`
- 立即拒绝：`bad_mac`、`bad_url`、`bad_sha256`、`no_such_node`、`offline`、`busy`（同一时间只升级一个节点，且不和网关升级同时进行）
- 下载阶段拒绝（节点不受影响）：`wrong_chip` / `wrong_project` / `not_newer` / `not_an_app_image`（读到镜像头就拒绝）、
  `connect_failed` / `download_failed` 等、`sha256_mismatch`
- 深睡节点先等它醒来（最多 `sleep_s + 10` 秒），等不到报 `node_asleep`
- 推送阶段：`node_no_response`（节点固件太旧不支持升级，或不在线）、`node_timeout`（传输中断）、
  `node_no_session`（节点中途重启）、`invalid_image_or_signature`、`node_sha256_mismatch` 等，节点继续运行旧固件
- 重启后：`rolled_back`（新固件 60 秒内没连上网关，节点已回滚）、`no_rejoin`（150 秒没回来）

`version.json` 中的节点固件：

```json
"nodes": [{"project": "esp_s3_korvo", "chip": "esp32s3", "version": "0.3.0",
           "url": "https://gitee.com/.../esp_s3_korvo-esp32s3-0.3.0.bin", "sha256": "...", "size": 856064}]
```

## 安全

- 传输全程 TLS；broker 账号只能连上 broker，**控制网关还要带 API token**，token 只在配网或屏幕确认绑定时下发给手机。
- 网关和 App 共用一组 broker 账号，能订阅所有 `espgw/#`。只供自己使用时足够；
  多用户时应给每个网关单独账号，并在 broker 上按 `espgw/<id>/#` 设置 ACL。
- 远程升级只接受 https 地址，固件还要过 SHA-256 和签名校验；拿到 broker 账号但没有 token 的人发不了升级命令。
