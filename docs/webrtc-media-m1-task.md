# M1 开发任务：48k PCM 上行探针

用户已授权 Codex 接管完整移植，DSH `stackchan` 为实现者。此任务启动第一实现阶段；完成后 Codex 复核并继续调度 M1.5／产品接入。

工作目录 `/Users/amtf/Documents/Git/StackChan`；分支 `feat/aliyun-omni-v2v`；PR <https://github.com/MetaAlms/StackChan/pull/1>。

设计基线：`57ab76eba38b9b8c9bacc95c704698b6316f0ac2`。
放行评审：[Review 5445711841](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5445711841)。
先核对 worktree、branch、HEAD 并读取该评审及 SPEC/PLAN。当前三份 Codex 记录修改及本任务文件是已授权的文档记录，随 M1 一并提交；不要覆盖其他改动。

## 目标与边界

实现、构建并在现有设备验证：**预录 16k mono s16 PCM → 设备上的独立 16→48k 重采样 → 48k mono Opus 编码 → raw Opus RTP 上行 → 服务端 VAD／完整 ASR 结果**。

M1 的“raw Opus”指发送的 packet 格式；fixture 的源是 PCM。只回放预编码 Opus，不能验证用户要求的重采样与编码链路。

- 新增默认关闭、与 M0 互斥的 M1 探针入口；保留 M0 不发媒体的语义。正常 WebSocket 固件入口、AudioService、AFE、UI 与公共提示音不改。
- 复用已互通的 SDP／ICE／DTLS／DataChannel 逻辑；不要复制整套传输栈。必要的局部复用应保持简单。
- 新源码、fixture、生成脚本／参数均在跟踪范围内；默认正常固件不嵌入探针大音频资产。生成目录改动必须由 patch／桥接代码恢复。
- 48k PCM 不降级；esp_peer 继续锁 1.5.5。M1 不宣称媒体下行、AEC、双讲、产品集成已通过。

## 必须实现的契约

1. **单一媒体 sender／编码器 owner**。重采样器连续复用，检查容量与返回值，只用每声道 `actual_out`，积累跨帧余量。`get_frame_size` 返回字节；编码输入字节与样本换算正确。先 20ms（48k mono s16：1920B／960 样本），后用同一源 fixture 测 60ms（5760B／2880 样本）。不改全局 `OPUS_FRAME_DURATION_MS`。初始关闭 DTX，记录实际编码参数与码率。
2. **实时 pacing 与媒体时间**。`esp_peer_audio_frame_t.pts` 按已核验的毫秒语义，由媒体样本时长推进（RTP clock48k）；使用独立 sender 的绝对媒体期限，不能贴在会阻塞的 peer poll 上，也不能一口气灌完 fixture。丢帧／发送失败仍保留正确时间轴，记录媒体时间和实际墙钟时间。断线先停并等待 sender，再销毁其 peer／codec；不能发送与 close 并发。
3. **单包长度检查**。一次发送一个 raw Opus packet，不含 Ogg／OpusHead／容器头。每次 `esp_opus_enc_process` 的 `in.len` 必须恰好为 getter 的 `in_size`，输出使用推荐容量，只发送 `out.encoded_bytes`；当前 SDK 会把多帧输入的多个 packet 直接拼接，不能一次喂入累积的多帧 PCM 再当单包发送。编码输出容量、actual length、1428 整包缓冲减去实际 RTP 头／扩展及网络 MTU 预算均明确检查，超限丢弃且计数。检查空指针／负长度／异常长度，不能在库内越界后才检测。
4. **事件与配置**。使用 cJSON 的精确 `type`，沿本会话发现的 stream 回复 `session.update`；显式 `server_vad`，保留阈值0.5、静音800ms作为第一基线。启用 `input_audio_transcription` 对象并核对 `session.updated` 的回显，然后释放 sender。配置不成立要报告配置失败，不混称 transport失败。
5. **ASR 观测**。单独记录 `input_audio_buffer.speech_started/stopped`、`conversation.item.input_audio_transcription.completed` 的 transcript／item_id，以及 `.failed` 的错误；普通 `error` 也单独记录。不能用 substring 命中、delta 文本或发送返回成功代替完整 ASR 验收。下行本阶段只计数／丢弃，不阻塞 peer 回调。
6. **任务共享状态同步与生命周期**。使用实际可核查的同步，避免跨任务无锁读写 flags；初始化失败／超时／断线有明确退出路径，不永久等待，不泄露任务、编码器或转换器。

协议参考：[客户端事件](https://help.aliyun.com/zh/model-studio/client-events)、[服务端事件](https://help.aliyun.com/zh/model-studio/server-events)、[WebRTC 接入](https://help.aliyun.com/zh/model-studio/realtime-webrtc-access)。当前客户端 JSON 的 ASR 对象模型为 `qwen3-asr-flash-realtime`；SDK 的 `enable_input_audio_transcription` 不是 JSON 字段。实际模型兼容性仍以服务端回显／错误及实测为准。

## 冻结的 fixture 与验收口径

使用三条普通短句，优先通过本机 `/usr/bin/say` 与 ffmpeg 生成，避免新增云端 TTS 依赖；保留脚本、声音选择、文本、文件格式、样本数、SHA256、每条关键词。可用句子为“今天我们测试语音连接”“桌上有一本蓝色的书”“请回答一加一等于几”；本机中文声音不可用时，可改成同等的英文短句并记录，不需要用户录音。

- 源 PCM 固定16k／mono／s16；每条至少500ms前静音、1200ms后静音（覆盖800ms VAD阈值并留余量）。记录末帧补齐及转换余量处理，不伪造持续采集。
- 20ms与60ms测试使用**相同源 fixture**。每组3条均出现正确关键词，且配置、媒体发送、VAD和completed ASR四层日志能按 item／轮次对齐；禁止仅看到“有转写”即通过。
- 记录原始 Opus packet数量、总字节、payload最大值、pts起止、媒体时长、墙钟时长、pacing迟到数、发送／编码／转换失败数。正常基线预期零超限和零codec错误；异常情况定位修复后重测。
- 跑至少5分钟持续媒体循环；预热后按固定间隔记录内部／DMA／PSRAM的剩余heap和最小heap、任务栈余量、编码及转换的耗时分位、欠载／丢包／发送失败。报告具体数值及走势；无WDT、任务重启或持续内存下降，不能把探针读数当完整产品预算。
- 测试时间有上限：SDP／会话初始化超时、ASR超时及失败分别报告；发生故障应修复本阶段必要逻辑，不用无限重试掩盖。

## 构建与设备验证

使用 ESP-IDF **5.5.4**（`source ~/esp/esp-idf-5.5/export.sh`），新增源文件后 `idf.py reconfigure`。现有凭据只走 ignored 本地配置／Keychain，不输出完整 sdkconfig、API key、Authorization 或 SDP 的 ICE 密码；M1 日志采用必要的非敏感参数摘要。

用户已授权媒体移植所需的本机构建与设备测试，可将现有 M0 探针设备刷到 M1 后获取串口证据；本轮不需要先刷回 WebSocket。若串口失联／设备休眠，应先完成代码、编译和可运行测试准备，再报告具体硬件阻塞，不虚构真机结果。

编译两种配置：M1探针，以及 M0/M1均关闭的正常 WebSocket。用私有SDKCONFIG／独立build目录确认实际flags；注意顶层CMake会强制加入本地defaults，修改defaults并不自动覆盖现有sdkconfig。只打印所需bool与非敏感测试参数。

干净重建在独立临时 checkout 验证候选提交的跟踪源码／patch／组件锁，运行 `fetch_repos.py` 并由IDF配置恢复managed components；当前生成目录和本地配置保留。副本使用无密钥占位配置即可做编译验证。

## 交付与停止条件

1. 将实测命令、配置摘要、fixture参数／hash、脱敏日志与结果落盘（建议 `docs/webrtc-media-m1-result.md`）；新增陷阱／缺陷按 AGENTS 落到 findings，证据不足要明确标注。Codex 已把多帧拼接预检记为 B19，随本轮保留并据实际实现更新状态。
2. 更新 SPEC/PLAN／调度状态：D0通过、M1待复核；未实现的后续阶段继续保留待验证状态。清理本轮记录的行尾空白。运行相关验证和覆盖本轮提交范围的 `git diff --check`，确认无凭据／冲突标记。
3. 提交、推送并更新当前draft PR为实际M1实现范围。在 PR 发针对**实际完整新 HEAD**的 `REVIEW_REQUEST`，给出本轮基线、改动、验证证据及未完成项，报告URL／full SHA，然后 **STOP**。
4. Codex 将独立审查代码、时间轴／容量／生命周期和实测证据，修复循环完成后继续派发 M1.5。不要自行 merge、auto-merge、发布、改上游或提前宣称完整WebRTC已完成。
