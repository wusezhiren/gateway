# 集成开发记录

## 2026-11-13 保存当前跨仓版本

目标：将当前全部源码及子仓工作区改动保存为可追溯版本。gateway 包括语音打断缓存交接、数字 PCM 测试、内存优化、I2S 生命周期保护及已有绑定二维码 UI；listen_clouds 保存 transport 内存配置；android 保存已有扫码绑定功能。模块过程分别见 [gateway](../gateway/docs/development-log.md)、[listen_clouds](../listen_clouds/docs/development-log.md)、[android](../android/docs/development-log.md)。总仓以 gitlink 固定各子仓提交组合。

验证：网关 ESP-IDF 6.0.3 构建通过，数字注入实板完成播放期打断、第二轮 STT/MCP、客厅最终关闭；I2S 重复 disable 错误文本未再出现。主机测试上一轮已通过。Android 本轮仅源码审查，未运行 Gradle/手机验收。声学回声、远场与长期压力尚未验收；PCM 数字注入不能代替声学验收。测试工具见 tools/test_wake_barge_in.py；未提交设备凭据、临时音频或构建产物。
