# 网关 HTTP API

## 配网接口（阶段 2）

仅在配网模式下可用：手机连上网关热点 `ESP-GW-XXXX`（默认密码 `a12345678`）后访问 `http://192.168.4.1`。
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
