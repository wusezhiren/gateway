# 集成开发记录

## 2026-11-13：工程导航与S31目录整理

先提交推送各子仓基线并固定总仓32177ea，再按S31职责拆分源码及公共头文件，保持子仓位置和构建目录不变。导航见[repository-layout.md](repository-layout.md)。S31主机测试、UI、分区检查和16MiB板测配置构建通过；未部署目录整理固件，不扩大为新增实板功能验证。

## 2026-11-13：S31 网关迁移基线

固定已上板开发的S31、S3迁移桥、Android0.3.5兼容及voice_core VAD组合。各子仓记录详见：

- [S31](../gateway_s31/docs/development-log.md)
- [S3](../gateway/docs/development-log.md)
- [Android](../android/docs/development-log.md)
- [语音核心](../components/voice_core/docs/development-log.md)

S31实物16MiB Flash/PSRAM，双OTA板测配置；卧室/客厅迁移后用户确认可控。SD待插卡，走廊灯不在本轮验收范围，完整语音尚未集成。配置凭据仅设备NVS和忽略的本地目录，不随本次提交上传。
