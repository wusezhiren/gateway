# 工程目录导航

本仓库是集成总仓，各模块保持独立 Git 子仓，不把源码混合搬入一个工程。

| 目录 | 用途 |
|---|---|
| `gateway_s31/` | S31 新网关、板测配置和测试 |
| `gateway/` | S3 网关及旧网关迁移桥 |
| `node/` | C3 节点 |
| `node_s3/` | S3 节点 |
| `android/` | Android App |
| `components/meshproto/` | 共享无线协议 |
| `components/voice_core/` | 可移植语音核心 |
| `listen_clouds/` | 独立云协议/传输组件 |
| `tools/` | 集成、签名、发布与验证脚本 |
| `docs/` | 跨工程协议和集成记录 |
| `local/`、`keys/` | 本地配置与密钥，不提交 |

## S31 源码

`gateway_s31/main/main.c` 负责启动组合，其余源码按职责落位：

```text
main/
  include/    公共头文件；保留原头文件名以减少迁移影响
  board/      BSP、显示触摸、SD、按键、音频、LED
  core/       消息总线
  services/   Wi-Fi、ESP-NOW、节点服务、HTTP、MQTT、鉴权
  protocol/   命令解析与状态序列化
  ota/        网关/节点 OTA、上传接口和回滚
  ui/         界面与熄屏策略
  voice/      PCM、MCP、会话及语音任务
```

各子仓的 `tests/` 保存测试、`docs/` 保存开发记录。`build*` 和
`managed_components/` 是本地构建/组件管理器产物，可能包含多个实验配置，
不应作为源码浏览入口，也不能直接移动已配置的 CMake 构建目录。
S31 当前板测入口为 `build_s31_board` 和 `sdkconfig.defaults.boardtest`。

目录调整不等于功能验收完成；功能状态以各模块开发记录为准。
