# esp-s3 组网

ESP-NOW 星型组网：酷世 KS_S3_KORVO_V4 作网关，ESP32-C3 / ESP32-S3 核心板作节点。
计划与进度见 [docs/PLAN.md](docs/PLAN.md)，协议见 [docs/protocol.md](docs/protocol.md)。

## 目录

```
components/meshproto/   网关与节点共用的协议定义
gateway/                网关固件（ESP32-S3）
node/                   节点固件（ESP32-C3 / ESP32-S3）
keys/                   签名私钥（不入库）
docs/                   文档
```

## 环境

ESP-IDF 5.4：

```bash
. ~/esp/esp-idf/export.sh
```

## 签名密钥

固件必须签名，OTA 时校验签名（`SECURE_SIGNED_APPS_NO_SECURE_BOOT`，RSA-3072，不烧 eFuse）。

```bash
mkdir -p keys
espsecure.py generate_signing_key --version 2 --scheme rsa3072 keys/signing_key.pem
```

- 私钥被 `.gitignore` 排除，**务必另行备份**；丢失后已部署设备只能 USB 重刷。
- 所有设备使用同一把密钥；换密钥后旧设备无法 OTA 到新固件。

## 编译与烧录

网关：

```bash
cd gateway
idf.py build
idf.py -p /dev/ttyACM1 flash monitor   # Korvo 原生 USB 口（303a:1001），以 ls /dev/serial/by-id 为准
```

节点（按芯片选择构建目录）：

```bash
cd node
idf.py -B build_c3 -DIDF_TARGET=esp32c3 -DSDKCONFIG=build_c3/sdkconfig build
idf.py -B build_c3 -DIDF_TARGET=esp32c3 -DSDKCONFIG=build_c3/sdkconfig -p /dev/ttyACM1 flash monitor
# ESP32-S3 节点把 c3 换成 s3
```

节点 GPIO 在 `idf.py menuconfig` → Node 中配置（默认继电器 GPIO4、5，PWM GPIO6）。
ESP-NOW 密钥在 → Mesh protocol 中配置，网关和节点必须一致。

## 网关串口命令

| 命令 | 说明 |
|---|---|
| `pair [秒]` | 打开配对窗口（默认 60s，0 关闭） |
| `nodes` | 列出节点、版本、在线状态、通道值 |
| `set <idx> <ch> <value>` | 设置节点通道 |
| `unpair <idx>` | 解除配对 |
| `wifi [status]` / `wifi set <ssid> [pass]` / `wifi clear` | 路由器配置 |
| `prov [start\|stop\|status]` | 配网模式（热点 `ESP-GW-XXXX` / `a12345678`） |
| `keys [on\|off]` | 打印按键 ADC 读数 |

## 配网

- 从未配网：开机自动进入配网模式
- 未联网时长按 **REC** 3 秒进入；配网中再长按取消
- 手机连热点 `ESP-GW-XXXX`（密码 `a12345678`），自动弹出配网页（或访问 `http://192.168.4.1`）
- 接口见 [docs/api.md](docs/api.md)
