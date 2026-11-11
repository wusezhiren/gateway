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
 "nodes": {"total": 0, "online": 0, "pair_left": 0},
 "ota": {"state": "idle", "received": 0, "total": 0},
 "node_ota": {"state": "sending", "mac": "98:88:e0:14:75:e0", "received": 400000, "total": 856064, "version": "0.3.0"},
 "cloud": {"configured": true, "connected": true, "id": "44bd8df31b70"}}
```

`ota.state`：`idle` / `receiving` / `rebooting` / `failed`（附 `error`），`version` 为目标版本。

`node_ota`：最近一次节点升级。`state` = `idle` / `downloading`（下载或上传到网关）/ `sending`（传给节点）/
`rebooting`（节点校验通过已重启，等它回来）/ `done` / `failed`（附 `error`）；`received` / `total` 是当前阶段的进度。

## 发现

网关注册 mDNS：主机名 `esp-gw.local`，服务 `_espgw._tcp`（端口 80，实例名 `ESP-GW-XXXX`，
TXT `mac`=STA MAC、`version`=固件版本）。App 用 TXT 里的 MAC 识别同一台网关。

## 绑定（获取 token）

已联网的网关通过屏幕确认把 token 交给 App，均无需鉴权：

### `POST /api/bind`

```json
{"name": "Xiaomi 23013RK75C"}
```

返回 `{"id": "1a2b3c4d", "timeout_s": 60}`，网关屏幕弹出“绑定手机”确认框。已有请求在等确认时返回 `409 busy`。
`name` 最多 24 字节，建议只用 ASCII（屏幕字库只有常用汉字）。

### `GET /api/bind?id=<id>`

| state | 说明 |
|---|---|
| `pending` | 等待屏幕确认 |
| `allowed` | 已允许，同时返回 `token`（只返回一次） |
| `denied` | 屏幕上点了取消，或 60 秒超时 |
| `unknown` | id 不对或结果已过期（30 秒） |

## 节点（需鉴权）

### `GET /api/nodes`

```json
{"pair_left": 0,
 "nodes": [{"mac": "aa:bb:cc:dd:ee:ff", "name": "relay-1", "online": true, "rssi": -48,
            "uptime_s": 3600, "last_seen_s": 2, "version": "0.1.0", "project": "esp_s3_node",
            "chip": "esp32c3", "sleep_s": 0, "rx_interval_ms": 0, "rx_window_ms": 0,
            "ch": [{"type": "relay", "max": 1, "value": 1},
                   {"type": "pwm", "max": 1023, "value": 512, "desired": 600}]}]}
```

- `chip`：`esp32c3` / `esp32s3` / `esp32c6` / `unknown`
- `ch[].type`：`relay`（0/1）/ `pwm`（0..max）/ `rgb`（彩灯，`max` 65535，取值按位打包开关、亮度、颜色，见 [protocol.md](protocol.md#通道描述-mp_ch_desc_t)）；`desired` 存在且与 `value` 不同表示正在同步
- `last_seen_s`：距最后一次收到该节点消息的秒数，从未收到为 -1
- `sleep_s`：节点通道全关时深睡、每隔这么多秒醒来一次，0 为常醒（老节点、网关重启后节点还没入网时也是 0）。
  深睡节点的 `set` 同样返回 `ok`，命令在节点下次醒来时生效，最长延迟 `sleep_s` 秒
- `rx_interval_ms` / `rx_window_ms`：省电节点每隔多少毫秒开一次多长的接收窗口，0 为一直收；命令最长延迟 `rx_interval_ms`

### `POST /api/nodes/set`

`{"mac": "aa:bb:cc:dd:ee:ff", "ch": 0, "value": 1}` → `{"ok": true}`（已发出，节点确认后 `value` 更新）

| 状态码 | error |
|---|---|
| 400 | `bad_mac` / `bad_channel_or_value` / `bad_body` |
| 404 | `no_such_node` |
| 409 | `offline` |

### `POST /api/nodes/unpair`

`{"mac": "..."}` → `{"ok": true}`；错误同上。

### `POST /api/nodes/rename`

`{"mac": "...", "name": "客厅灯条"}` → `{"ok": true}`。名字存在网关上，1~16 字节 UTF-8（约 5 个汉字），
超长或含控制字符返回 `400 bad_name`；改名后节点重新入网也不会被它上报的默认名覆盖，解绑后失效。
网关屏幕字库只有常用汉字，生僻字会显示不出来。

### `POST /api/pair`

`{"seconds": 60}` 打开配对窗口（0~300，0 立即关闭）。剩余秒数见 `/api/nodes` 的 `pair_left` 或 `/api/info` 的 `nodes.pair_left`。

### `POST /api/nodes/ota`

`{"mac": "...", "url": "https://...", "sha256": "<64 位十六进制>", "force": false}` → `{"ok": true}`

网关从 `url` 下载节点固件（跟随跳转，证书用 IDF 证书包校验）暂存到 `node_fw` 分区，核对后推给节点，
进度看 `/api/info` 的 `node_ota`。与云端 `node_ota` 命令相同，错误码见 [cloud.md](cloud.md)。

| 状态码 | error |
|---|---|
| 400 | `bad_mac` / `bad_url`（非 https）/ `bad_sha256` |
| 404 | `no_such_node` |
| 409 | `offline` / `busy`（已有节点或网关升级在进行） |

## 云端配置（需鉴权）

协议见 [cloud.md](cloud.md)。

### `GET /api/cloud`

```json
{"configured": true, "connected": false, "id": "44bd8df31b70",
 "host": "xxxx.emqxsl.cn", "port": 8883, "username": "espgw", "password": "***", "error": "auth_failed"}
```

`password` 只返回给带 token 的请求，App 用它自动同步云端设置。`error`：最近一次连接失败原因（`auth_failed` / `tls_failed` / `connect_failed`），连上后清空。

### `POST /api/cloud`

`{"host": "xxxx.emqxsl.cn", "port": 8883, "username": "espgw", "password": "***"}` → `{"ok": true}`，
保存后立即重连。`host` 为空表示关闭并清除配置。

## 网关 OTA（阶段 3a）

### `POST /api/ota/gateway[?force=1]`（需鉴权）

请求体为网关固件（`gateway/build_wake/esp_s3_gateway.bin`，当前网关不校验签名），`Content-Type: application/octet-stream`，必须带 `Content-Length`。

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
tools/ota_upload.py <网关IP> gateway/build_wake/esp_s3_gateway.bin --token <token> [--force]
```

升级过程中可轮询 `/api/info` 的 `ota` 字段查看进度。

## 节点 OTA

### `POST /api/ota/node?mac=aa:bb:cc:dd:ee:ff[&force=1]`（需鉴权）

请求体为签名后的节点固件（开发调试用；正式升级用 `POST /api/nodes/ota` 让网关自己下载）。
网关读到镜像头就核对：芯片型号、工程名与该节点一致，版本更新（`force=1` 例外），不合格不写 flash；
收完存入 `node_fw` 分区后立即回复 `{"ok": true}`，后台推给节点，结果看 `/api/info` 的 `node_ota`。

| 状态码 | error |
|---|---|
| 400 | `bad_mac` / `not_an_app_image` / `wrong_chip` / `wrong_project` / `too_small` / `too_large` / `recv_failed` |
| 404 | `no_such_node` |
| 409 | `offline` / `busy` / `not_newer` |

```bash
curl -H "Authorization: Bearer <token>" --data-binary @node/build_korvo/esp_s3_korvo.bin \
     "http://<网关IP>/api/ota/node?mac=98:88:e0:14:75:e0"
```

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
- token 在配网成功时从热点内拿到，或之后通过「绑定」在网关屏幕确认后获取。
