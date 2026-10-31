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

节点固件签名，OTA 时校验签名（`SECURE_SIGNED_APPS_NO_SECURE_BOOT`，RSA-3072，不烧 eFuse）。
网关和 C3 继电器节点（`esp_c3_relay`）关掉了签名，编译不需要密钥，OTA 也不校验签名。

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

节点（按芯片 / 板子选择构建目录）：

```bash
cd node
idf.py -B build_c3 -DIDF_TARGET=esp32c3 -DSDKCONFIG=build_c3/sdkconfig build
idf.py -B build_c3 -DIDF_TARGET=esp32c3 -DSDKCONFIG=build_c3/sdkconfig -p /dev/ttyACM1 flash monitor
# ESP32-S3 核心板把 c3 换成 s3
# Korvo 板当节点（工程名 esp_s3_korvo，见 sdkconfig.defaults.korvo）：
idf.py -B build_korvo -DSDKCONFIG=build_korvo/sdkconfig -DNODE_PROJECT=esp_s3_korvo \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.korvo" set-target esp32s3 build
```

工程名区分固件种类，节点只接受同名固件的升级：硬件配置不同的板子要用不同的 `NODE_PROJECT`（不超过 16 个字符），
并加到 `tools/release_app.py` 的 `NODE_BUILDS`。0.3.0 以前的节点不支持远程升级，需先 USB 烧一次。

节点 GPIO 在 `idf.py menuconfig` → Node 中配置（默认继电器 GPIO4、5，PWM GPIO6；S3 另有 GPIO48 上的 WS2812 彩灯）。
手动编译时 CMake 会缓存上次的 `PROJECT_VER`，改了版本号要加 `-DPROJECT_VER=x.y.z`（发版脚本会自动带上）。

合宙 CORE-ESP32-C3 + 单路低电平触发继电器（USB 灯条）：工程名 `esp_c3_relay`，见 `sdkconfig.defaults.c3relay`，
继电器 IN 接 GPIO4、VCC 接 3V3。这个节点关掉了签名，编译不需要 `keys/signing_key.pem`，OTA 也不校验签名：

```bash
idf.py -B build_c3relay -DSDKCONFIG=build_c3relay/sdkconfig -DNODE_PROJECT=esp_c3_relay \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.c3relay" set-target esp32c3 build
```

ESP32-S3 N16R8 核心板（两个 Type-C，板载 WS2812 在 GPIO48）：用 `build_s3`，接标 USB 的原生口烧录；
GPIO35/36/37 被八线 PSRAM 占用，不要外接。
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
| `ota` / `ota url <url> <sha256> [force]` | 网关升级状态 / 从网上下载升级 |
| `nodeota` / `nodeota <mac> <url> <sha256> [force]` | 节点升级状态 / 下载固件升级节点 |

## 网关 OTA

```bash
# token：网关串口执行 token
tools/ota_upload.py 192.168.x.x gateway/build/esp_s3_gateway.bin --token <token>
```

- 网关不校验签名，默认不允许同版本或降级（`--force` 例外）
- 还在跑旧签名版的网关只接受签名固件，要先 USB 烧一次不签名的版本
- 版本号在构建时指定：`idf.py -DPROJECT_VER=0.2.0 build`
- 新固件确认前崩溃会自动回滚；回滚测试固件可用
  `CONFIG_GW_TEST_ABORT_BEFORE_VALIDATE=y` 单独构建（**不要发布**）
- 接口见 [docs/api.md](docs/api.md)

## 节点 OTA

- App 节点卡片出现“有新固件 vX”时点“升级”，局域网和 4G 都可以；网关从发布仓库下载后经 ESP-NOW 推给节点
- 调试时也可以局域网直接上传：`curl -H "Authorization: Bearer <token>" --data-binary @<bin> "http://<网关IP>/api/ota/node?mac=<节点MAC>"`
- 新固件 60 秒内连不上网关会自动回滚；回滚测试固件用 `CONFIG_NODE_TEST_NO_JOIN_AFTER_OTA=y` 单独构建（**不要发布**）
- 协议和防变砖措施见 [docs/protocol.md](docs/protocol.md#节点升级)

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
- 发版：`tools/release_app.py <版本> --notes "说明"`，自动编译网关固件并内置进 App、编译各种节点固件、
  用发布签名打包、上传到公开发布仓库 https://gitee.com/yu_ji_hai/app_release ，并通过云端通知在线的 App；
  App 启动时检查 `version.json` 自动更新，固件页提示“升级到 vX”把内置固件推给网关（不在局域网时让网关自己下载），
  节点卡片提示“有新固件”让网关下载并升级节点。
  凭据在 `local/`（不上传）：`gitee_token`、`keystore.properties`/`release.keystore`（**务必备份**）、`cloud.json`
- 外网访问：网关和 App 都连云端 MQTT（EMQX Cloud），在 App「网关 → 云端设置」填写，见 [docs/cloud.md](docs/cloud.md)
