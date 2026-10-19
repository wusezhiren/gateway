# 网关 HTTP API

网关常驻 HTTP 服务（80 端口），局域网（STA）和配网热点两侧都可访问。

## 鉴权

需要鉴权的接口带请求头 `Authorization: Bearer <token>`，否则返回 `401 {"error":"unauthorized"}`。
token 为 32 位十六进制，配网成功时由 `/api/prov/status` 返回，也可在网关串口执行 `token` 查看。

> 访问局域网设备时不要走系统代理（`curl --noproxy '*'`）。

## 基本信息

### `GET /api/info`（无需鉴权）

```json
{"project": "esp_s3_gateway", "version": "0.2.0", "partition": "ota_1", "idf": "...",
 "mac": "44:bd:8d:f3:1b:70", "uptime_s": 39,
 "wifi": {"ssid": "...", "connected": true, "ip": "192.168.32.222", "rssi": -26, "channel": 11},
 "nodes": {"total": 0, "online": 0},
 "ota": {"state": "idle", "received": 0, "total": 0}}
```

`ota.state`：`idle` / `receiving` / `rebooting` / `failed`（附 `error`），`version` 为目标版本。

## 网关 OTA（阶段 3a）

### `POST /api/ota/gateway[?force=1]`（需鉴权）

请求体为**签名后的**网关固件（`gateway/build/esp_s3_gateway.bin`），`Content-Type: application/octet-stream`，必须带 `Content-Length`。

处理流程：

1. 先收镜像头：不是 app 镜像、工程名不是 `esp_s3_gateway`、版本不高于当前版本（未带 `force=1`）时直接拒绝，不擦写 flash
2. 流式写入空闲 OTA 分区（`ota_0` / `ota_1` 交替）
3. `esp_ota_end` 校验镜像完整性和 RSA-3072 签名
4. 切换启动分区，回复后 1.5 秒重启
5. 新固件首次启动处于待确认状态：Wi-Fi 连上（或未配置路由器、或 120 秒超时）后确认有效；
   确认前崩溃重启则 bootloader 自动回滚到旧固件

成功：`200 {"ok": true, "version": "0.2.0", "reboot_in_ms": 1500}`

| 状态码 | error | 说明 |
|---|---|---|
| 400 | `not_an_app_image` | 不是 ESP app 镜像 |
| 400 | `wrong_project` | 不是网关固件（如节点固件） |
| 400 | `invalid_image_or_signature` | 镜像损坏或签名不对（未签名/别的密钥） |
| 400 | `too_small` / `recv_failed` | 请求体不完整 |
| 409 | `not_newer` | 版本不高于当前，需 `force=1` |
| 409 | `busy` | 已有升级在进行 |
| 413 | `too_large` | 超过 OTA 分区（3MB） |

PC 工具：

```bash
tools/ota_upload.py <网关IP> gateway/build/esp_s3_gateway.bin --token <token> [--force]
```

升级过程中可轮询 `/api/info` 的 `ota` 字段查看进度。

## 配网接口（阶段 2）

仅在配网模式下可用（非配网时返回 `404 {"error":"not_provisioning"}`）：手机连上网关热点 `ESP-GW-XXXX`（默认密码 `a12345678`）后访问 `http://192.168.4.1`。
配网网页和 App 共用这组接口，均无需鉴权（能连上热点即视为在现场）。

### `GET /`

配网网页。热点内的 DNS 把所有域名解析到网关，系统联网检测（`/generate_204`、`/hotspot-detect.html` 等）
被 302 重定向到 `/`，手机会自动弹出该页面；DHCP 同时下发 option 114（Captive Portal URI）。

### `GET /api/prov/scan`

扫描附近 2.4GHz 网络，阻塞约 2~3 秒。同名网络只保留信号最强的一条，按信号降序。

```json
[{"ssid": "Xiaomi_listenai_2.4G", "rssi": -35, "secure": true}]
```

| 状态码 | 说明 |
|---|---|
| 200 | 成功 |
| 503 `{"error":"busy"}` | 正在连接路由器，稍后重试 |

### `POST /api/prov/connect`

```json
{"ssid": "Xiaomi_listenai_2.4G", "password": "********"}
```

- `ssid` 1~32 字节；`password` 为空（开放网络）或 8~63 字节
- 立即返回 `{"ok": true}`，约 0.5 秒后网关才开始连接（先保证响应送达，连接时可能切换信道）
- 结果通过 `/api/prov/status` 轮询

| 状态码 | 说明 |
|---|---|
| 200 | 已受理 |
| 400 `invalid_ssid_or_password` / `bad_body` | 参数不合法 |
| 409 `busy` | 正在连接中或已成功 |

### `GET /api/prov/status`

```json
{"state": "done", "ssid": "Xiaomi_listenai_2.4G", "ip": "192.168.32.222",
 "host": "esp-gw.local", "token": "<32 位十六进制>", "close_in": 28}
```

| 字段 | 说明 |
|---|---|
| state | `waiting` 等待手机 / `client` 手机已连接 / `connecting` 连接中 / `done` 成功 / `failed` 失败 / `off` |
| ssid | 正在或最近一次连接的网络 |
| reason | 仅 `failed`：`bad_password` / `no_ap` / `no_ip` / `failed` |
| ip, host, token | 仅 `done`：网关局域网地址、mDNS 名（阶段 5 启用）、App 访问 API 的 token |
| close_in | 距热点自动关闭的秒数（成功后 30 秒；否则为 10 分钟无操作超时） |

### 客户端注意事项

- 网关连上路由器后，热点会跟随路由器信道，期间请求可能丢失。**所有请求都要设超时（建议 4 秒）**，
  轮询要串行（上一个结束再发下一个），失败后继续重试。
- 成功后 30 秒热点关闭，手机会回到原网络；App 应在 `done` 时保存 `ip` 和 `token`。
- token 只能在配网成功时从热点内拿到，网关后续的 API（阶段 5）都需要带上它。
