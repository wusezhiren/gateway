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
| `pair` | `seconds`（0~300） | `POST /api/pair` |
| `get` | — | 只触发一次 `state` 发布 |

回复：`{"id": "a1b2c3", "ok": true}` 或 `{"id": "a1b2c3", "ok": false, "error": "offline"}`，错误码与 HTTP 接口相同，
另有 `unauthorized`（token 不对）、`bad_op`。执行后网关会发布新的 `state`。

## 安全

- 传输全程 TLS；broker 账号只能连上 broker，**控制网关还要带 API token**，token 只在配网或屏幕确认绑定时下发给手机。
- 网关和 App 共用一组 broker 账号，能订阅所有 `espgw/#`。只供自己使用时足够；
  多用户时应给每个网关单独账号，并在 broker 上按 `espgw/<id>/#` 设置 ACL。
- 网关固件升级仍只走局域网（`POST /api/ota/gateway`），远程升级以后再做。
