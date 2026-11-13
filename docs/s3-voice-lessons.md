# S3 语音打断迁移记录

本文记录 Korvo/ESP32-S3 网关语音链路的实现坑、内存边界和验证方法，供后续 S31 迁移使用。

## 1. 已验证的链路约束

- 音频采集为 16 kHz、16-bit、mono PCM；ES7210 的 TDM 活跃槽由 `esp_codec_dev_open()` 改成 `0x3`，DMA 每帧实际包含两个样本，不能按四槽数据直接解释，否则采样率会被误算为约 8 kHz。
- 上行 Opus 为 60 ms、16 kHz、单声道；小智 WebSocket v1 要求每帧直接发送裸 Opus binary，不能附加 WebSocket v3 的 4 字节版本头。
- WakeNet 模型 `clean()` 在会话结束后曾触发空指针崩溃；当前做法是清空采集队列、半帧和预滚状态，保留模型实例，不调用该路径。
- 唤醒后的预滚要保留唤醒点前约 0.8 秒，并保留 WebSocket 建连期间采到的语音；否则首个字词会丢失。
- 文件灌音接口 `/api/wake/test` 只适合验证模型检测和文件测试流程。它不会自动把文件剩余 PCM 按真实时间送入播放期的 I2S RX，因此不能单独证明真实声学全双工打断。

## 2. 打断状态机要求

播放 TTS 期间再次唤醒必须按以下顺序执行：

1. WakeNet 保持运行，二次唤醒优先级高于 TTS。
2. 新唤醒创建新的 session generation。
3. 立即停止 TTS 生产和播放，清空旧 TTS 队列。
4. 取消旧 WebSocket/MCP 会话；迟到的 STT、MCP、TTS 结果按 generation 丢弃。
5. 保留二次唤醒后的 PCM 和预滚，交接给新会话。
6. 新会话至少收集 1 秒实时音频后才允许 VAD 静音判停。
7. 旧 I2S/Codec 资源完成关闭后，才由新会话取得 RX/TX。

状态机和音频端口必须分离：核心只处理事件、generation、取消和队列，BSP 端口独占管理 I2S 成对通道及 Codec 生命周期。

## 3. I2S/Codec 生命周期

必须使用统一清理路径，顺序固定为：

```text
停止 PCM 生产
→ 停止/等待编码、播放和网络消费者
→ codec close/delete
→ 删除 codec/data/ctrl 接口
→ disable RX/TX（允许 ESP_ERR_INVALID_STATE）
→ i2s_del_channel
→ 清除全局借用句柄
```

注意事项：

- 不能在旧任务仍使用 TX 时删除或重新初始化 I2S channel。
- `i2s_channel_disable()` 对尚未 enable 的通道会打印错误；清理应将 `ESP_ERR_INVALID_STATE` 视为幂等成功，并记录真实释放失败。
- Duplex TX 借用时，TTS 关闭不能删除该 channel，只能释放 Codec 和本地缓冲；channel 的所有权仍由音频端口持有。
- 每次开始录音、播放或双工前后打印句柄和内存，便于发现重复初始化或交叉释放。

## 4. 内存分配边界

ESP32-S3 有约 512 KiB 片上 SRAM，但 Wi-Fi、LWIP、TLS、AES、I2S DMA、Codec、FreeRTOS 和 WakeNet 会长期/峰值占用其中大部分；8 MiB PSRAM 不能替代需要 `MALLOC_CAP_INTERNAL` 或 `MALLOC_CAP_DMA` 的分配。

### 可放 PSRAM

- HTTP 上传的 PCM、WAV、模型和 OTA 临时数据。
- 预滚环形缓冲、文件测试 PCM、Opus/PCM 非 DMA 工作缓冲。
- WebSocket/MQTT 文本消息的有界副本和 MCP 大响应。
- LVGL 对象内存及非 DMA 的应用缓存，前提是对应组件明确支持 PSRAM。
- 应用任务栈只有在实际调用链确认支持 PSRAM stack 后才能迁移；不要仅凭“任务不重要”迁移。

### 必须谨慎保留内部 SRAM

- I2S RX/TX DMA 缓冲。
- 中断、flash cache disabled 路径和底层 Codec 回调使用的栈/缓冲。
- TLS/mbedTLS/AES 需要的内部连续块。
- WakeNet 推理路径中由组件要求的内部工作区。
- LVGL/LCD 绘制缓冲如果配置为 DMA buffer，必须满足 DMA 能力；不要同时把 `buff_dma=true` 和普通 PSRAM 混用。

### 内存诊断

至少在以下阶段记录：

```c
heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
heap_caps_get_free_size(MALLOC_CAP_DMA)
heap_caps_get_largest_free_block(MALLOC_CAP_DMA)
heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
```

阶段包括 WebSocket 建连前后、开始录音、MCP `tools/call`、TTS 打开、开始双工和打断取消。关注最大连续块，不只看总空闲量。

### 已发现的具体坑

- 曾把 TTS 任务栈从 16 KiB 降到 8 KiB，设备明确报：`A stack overflow in task tts_play`，随后复位。Opus 解码、重采样和 Codec 调用需要保留经过实测的 16 KiB 栈。
- TLS 失败时曾出现 `internal=30403`、`largest=8704`、`psram=7712896`，说明瓶颈是内部 SRAM 连续块，不是 PSRAM 总容量。
- WebSocket/MQTT 缓冲和任务栈可以适度缩小，但每次调整必须配合 TTS、MCP、TLS 和双工全链路实测。
- LVGL 任务栈、LCD DMA buffer、网络任务栈不能盲目迁移到 PSRAM；底层回调和 cache-disabled 路径可能不支持 PSRAM stack/buffer。迁移失败时优先恢复已知安全配置。

## 5. 网络和 TLS

- 语音会话开始前暂停并确认 MQTT 已释放，避免 MQTT TLS 与 WebSocket TLS 同时占用内部 SRAM。
- WebSocket、MQTT 使用有界 buffer 和有界队列，队列满必须计数并按策略取消会话，不能无限增长。
- MCP/JSON 响应放 PSRAM，但发送前仍可能触发 TLS/AES 内部临时分配；发送前应记录最大连续内部块。
- WebSocket 任务回调不做 Opus 解码、I2S 写入或节点控制，只复制有界消息并入队。
- 网络错误、TLS 失败、断线和取消都必须进入同一个会话清理路径。

## 6. 自动 TTS 灌音测试

仓库脚本：

```text
tools/test_wake_barge_in.py
```

它支持：

- 16 kHz/16-bit/mono WAV 或裸 little-endian PCM；
- 禁用系统代理访问局域网网关；
- `Authorization: Bearer` 鉴权；
- 网关扫描或指定 IP；
- 通过 `/api/wake/test` 提交有界 PCM。

示例：

```bash
python3 tools/test_wake_barge_in.py \
  192.168.x.x test.pcm --token "$GW_TOKEN"
```

脚本本身不保存 token，不应把 token、录音或 TTS 临时文件提交到仓库。真实验证必须区分：

1. 文件 WakeNet 检测次数；
2. 第一轮 STT/MCP/TTS；
3. 播放期实时 RX 的二次 WakeNet 检测；
4. 旧 TTS/WS 取消日志；
5. 新会话 STT/MCP；
6. 目标节点状态闭环。

只看到 HTTP 200 或 `file_test_detections=2`，不能单独宣称打断通过。

## 7. S31 迁移顺序

1. 先实现并单测 `voice_core`、VAD、generation、取消和 MCP，不接 BSP。
2. S31 音频端口先做录音、播放、RX/TX 成对通道和反复 open/close 测试。
3. 再做 Opus 编解码和内存峰值测试，记录内部 SRAM、DMA 和 PSRAM 三类指标。
4. 先启用 SIMPLEX 完成单轮云端闭环。
5. 明确证明 S31 同时录放稳定后，才启用 FULL_DUPLEX 和播放期二次唤醒。
6. 设计 fake audio/transport 测试，让第二次唤醒在 TTS 消费期间进入，而不是把两段音频简单串接后当作一次文件测试。
7. 板上验收使用客厅灯这类确定节点，验证“打开→第二次唤醒→关闭”的状态闭环；测试日志不能包含凭据。
