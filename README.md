# esp-s3 组网

ESP-NOW 星型组网：酷世 KS_S3_KORVO_V4 作网关，ESP32-C3 / ESP32-S3 核心板作节点。
计划与进度见 [docs/PLAN.md](docs/PLAN.md)，协议见 [docs/protocol.md](docs/protocol.md)。

## 目录

```
components/meshproto/   网关与节点共用的协议定义
gateway/                网关固件（ESP32-S3）
node/                   节点固件（ESP32-C3 / ESP32-S3）
android/                手机 App（Kotlin + Compose）
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

## 网关 OTA

```bash
# token：网关串口执行 token
tools/ota_upload.py 192.168.x.x gateway/build/esp_s3_gateway.bin --token <token>
```

- 只接受用 `keys/signing_key.pem` 签名的网关固件，默认不允许同版本或降级（`--force` 例外）
- 版本号在构建时指定：`idf.py -DPROJECT_VER=0.2.0 build`
- 新固件确认前崩溃会自动回滚；回滚测试固件可用
  `CONFIG_GW_TEST_ABORT_BEFORE_VALIDATE=y` 单独构建（**不要发布**）
- 接口见 [docs/api.md](docs/api.md)

## 配网

- 从未配网：开机自动进入配网模式
- 未联网时长按 **REC** 3 秒进入；配网中再长按取消
- 手机连热点 `ESP-GW-XXXX`（密码 `a12345678`），自动弹出配网页（或访问 `http://192.168.4.1`）
- 接口见 [docs/api.md](docs/api.md)

## 手机 App

```bash
cd android
JAVA_HOME=~/.local/jdk ./gradlew :app:assembleDebug     # 需要 JDK 17、Android SDK 35
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

- 新网关：App 内连接热点配网，成功后自动保存地址和 token
- 已联网网关：App 通过 mDNS 查找（或手动输入 IP），在网关屏幕上点“允许”完成绑定
- 外网访问：家里树莓派做 Tailscale 子网路由，手机装 Tailscale 即可，见 [docs/PLAN.md](docs/PLAN.md)
