# WebRTC 媒体链路 SPEC

将 StackChan 的实时语音从阿里云 **WebSocket** 协议迁移到 **WebRTC**，
以获得服务端内置的回声消除，从而实现**语音打断**。

- 前置验证：[webrtc-m0-review-result.md](webrtc-m0-review-result.md)、[webrtc-m0-review-result-2.md](webrtc-m0-review-result-2.md)
- 能力边界：[aec-limitation.md](aec-limitation.md)
- 实现计划：[webrtc-media-plan.md](webrtc-media-plan.md)

状态：**待评审**。本文件只定义"做什么"与"验收标准"，不含排期。

---

## 1. 背景与目标

### 1.1 为什么迁移

WebSocket 协议**官方明确不做回声消除**（"无，需客户端自行处理"），
导致设备听到自己的喇叭声并自我打断。设备侧补 AEC 又因
`esp_codec_dev` 最多 2 通道而拿不到第 3 路回采参考。

WebRTC 协议**内置回声消除与降噪**，且弱网对抗为"良好"（WebSocket 为"差"）。
迁移后预期一次性解决：自打断、打断不可用、服务端突发推送（现用背压绕开）三项。

### 1.2 已达成的前置条件

M0 已在真机验证（esp_peer **1.5.5**）：

| 环节 | 状态 |
|---|---|
| SDP 交换（HTTP POST offer → answer） | ✅ |
| ICE | ✅ `Connection OK` |
| DTLS / SRTP | ✅ `CONNECTED` |
| SCTP / DataChannel | ✅ `DATA_CHANNEL_OPENED` |
| `session.created` / `session.updated` | ✅ 均在服务端 `txt` 通道（stream 1）收到 |

**传输层已全部可用。本 SPEC 只覆盖其上的媒体与协议层。**

### 1.3 非目标

- 视频（`video_dir` 保持 `NONE`）
- 替换现有 WebSocket 实现（保留为回退路径，见 §7.3）
- 修复 esp_peer 的 ICE 缺陷（属上游，见 [firmware-findings.md](firmware-findings.md) A8）

---

## 2. 关键架构事实（已核实）

这些事实决定了设计空间，全部有代码或实测依据。

### 2.1 esp_peer 不负责编解码

`esp_peer` 的静态库中**没有** `opus_encode` / `opus_decode` 符号
（`nm libpeer_default.a | grep opus_` 无结果），其唯一依赖是 `esp_libsrtp`。
头文件明确：

```c
/* esp_peer.h:64 */
 *        Send payload still follows `audio_info.codec` / `video_info.codec`.
```

**结论：应用负责 Opus 编解码，esp_peer 负责 RTP 打包/解包 + ICE/DTLS/SRTP/SCTP。**

### 2.2 采样率约束

| 环节 | 采样率 | 是否可改 |
|---|---|---|
| ES7210 采集 / AW88298 播放 | 24 kHz | 板级配置 |
| **esp-sr AFE** | **16 kHz** | ❌ 固定 |
| **WebRTC Opus** | **48 kHz** | ❌ WebRTC 规范固定 |

`esp_audio_codec` 已支持 `ESP_AUDIO_SAMPLE_RATE_48K`。
工程已依赖 `espressif/esp_audio_codec ~2.4.1`（xiaozhi 现用于 16 kHz Opus）。

### 2.3 现有音频管线

```
ES7210 24k 2ch
  └─ AudioService::ReadAudioData 重采样 24k→16k
      └─ AFE（NS/VAD/AEC，16k 单声道，60ms=960 样本）
          └─ Opus 编码 16k → 传输
```

### 2.4 事件通道

服务端在**自建的 `txt` 通道（stream 1）**推送事件，**不是**客户端创建的
`oai-events`（stream 0，实测未打开）。`session.update` 必须在收到
`session.created` 的那条 stream 上回复，否则拿不到 `session.updated`。
（M0 已按此实现并验证。）

---

## 3. 需求

### 3.1 功能需求

| 编号 | 需求 | 优先级 |
|---|---|---|
| F1 | 上行：麦克风音频经 Opus 编码后由 RTP 发送至服务端 | 必须 |
| F2 | 下行：接收 RTP 音频，解码并播放 | 必须 |
| F3 | 事件（`session.update` / `response.*` 等）经 DataChannel 收发 | 必须 |
| F4 | 会话配置改用 WebRTC 语义（`server_vad`，不再用本地 VAD 驱动轮次） | 必须 |
| F5 | 表情联动保留（emoji 解析 → `SetEmotion`） | 必须 |
| F6 | **语音打断可用**（服务端 AEC 生效） | **必须（本项目的根本目的）** |
| F7 | 现有 WebSocket 实现保留为可切换的回退路径 | 必须 |

### 3.2 非功能需求

| 编号 | 需求 | 判据 |
|---|---|---|
| N1 | 音频带宽满足 ASR | 服务端能正确转写设备上行语音 |
| N2 | 端到端延迟可用 | 用户停止说话到设备开始应答 ≤ 2 s（局域网） |
| N3 | 运行稳定 | 连续 10 轮对话无断流、无崩溃 |
| N4 | 内存不超限 | 内部 SRAM 最低余量 > 8 KB（当前约 24 KB） |
| N5 | 与服务端 VAD 协同 | `server_vad` 正常触发轮次，无需本地 VAD |

---

## 4. 架构

### 4.1 目标数据流

```
【上行】
ES7210 24k 2ch
  └─ 重采样 24k→16k（沿用现有）
      └─ AFE 16k 单声道
          └─ 重采样 16k→48k          ← 新增
              └─ Opus 编码 48k        ← 新增（复用 esp_audio_codec）
                  └─ esp_peer_send_audio() → RTP/SRTP

【下行】
RTP/SRTP → on_audio_data 回调（Opus 帧）
  └─ Opus 解码 48k                    ← 新增
      └─ 重采样 48k→24k               ← 新增
          └─ AW88298 播放

【控制面】
DataChannel(stream 1) ⇄ JSON 事件（复用现有 emoji 解析与会话逻辑）
```

### 4.2 待决设计项（见 §8 开放问题）

**采样率链路上有一处绕行是"必须"的**：AFE 固定 16 kHz，而 WebRTC Opus 固定
48 kHz，故 16k→48k 的上采样无法避免。这会把上行带宽限制在 8 kHz（AFE 输出的
奈奎斯特频率），**即使 ES7210 采集本身有 12 kHz 带宽**。

是否值得为带宽绕过 AFE，见 §8 问题 Q1。

---

## 5. 接口

### 5.1 esp_peer 的用法（与 M0 的差异）

| 项 | M0（已实现） | 本 SPEC |
|---|---|---|
| `audio_dir` | `SEND_RECV`（未发数据）| `SEND_RECV`（实际收发）|
| `esp_peer_send_audio()` | 未调用 | 按 60 ms 帧节奏调用 |
| `on_audio_data` | 仅计数 | 解码并播放 |
| `manual_ch_create` | `true` + label `oai-events` | 不变 |
| `on_channel_open` | 记录 label/stream_id | 不变（用于确定回复通道）|

`esp_peer_audio_frame_t` 的 `data` 为**已编码的 Opus 帧**，`pts` 为时间戳。

### 5.2 与 xiaozhi 音频服务的边界

**必须复用、不重写**：`AudioService` 的采集/播放任务、`ReadAudioData`
（含 24k→16k 重采样）、AFE 实例、编解码任务的线程与队列。

**需要新增**：16k↔48k 重采样、48k Opus 编解码实例、
`esp_peer` 与编解码队列之间的桥接。

### 5.3 协议层边界

**复用**：emoji 表情解析（[protocol.py](../tools/aliyun_omni/protocol.py) 的 C++ 对应实现）、
会话配置构造、错误处理。

**替换**：传输由 WebSocket 文本帧改为 DataChannel；
音频由 `input_audio_buffer.append`（Ogg 封装）改为 RTP。

**删除**（迁移完成后）：Ogg muxer/demuxer、背压逻辑、排空守卫、
延后提交、本地 VAD 驱动轮次——这些均为绕开 WebSocket 能力缺失而写。

---

## 6. 验收标准

### 6.1 M1：上行通路

| 编号 | 判据 | 验证方式 |
|---|---|---|
| M1-1 | 上行 Opus 帧经 RTP 送达服务端 | 服务端返回 `input_audio_buffer.speech_started` |
| M1-2 | 服务端正确转写上行语音 | `conversation.item.input_audio_transcription.completed` 的 `transcript` 与所说内容一致 |
| M1-3 | 无内存泄漏 | 连续发送 5 分钟后内部 SRAM 余量无明显下降 |

### 6.2 M2：双向与打断

| 编号 | 判据 | 验证方式 |
|---|---|---|
| M2-1 | 下行音频可听且清晰 | 主观听感 + 无断流 |
| M2-2 | 服务端 VAD 正常结束轮次 | 无需本地 VAD，停顿后自动回应 |
| M2-3 | **打断可用** | 设备说话期间用户插话，设备**停止当前回答并改答新问题** |
| M2-4 | **不自打断** | 设备连续说话 ≥ 30 s，期间不因自身声音重启回答 |

**M2-3 与 M2-4 是本次迁移的成败判据。**
若 M2-3 通过但 M2-4 失败，说明服务端 AEC 对设备自身声音的抑制不足，
需回到 [aec-limitation.md](aec-limitation.md) 重新评估路径（见 §7.2 风险 R1）。

### 6.3 回归

| 编号 | 判据 |
|---|---|
| R-1 | 表情联动正常（emoji → `SetEmotion`） |
| R-2 | 多轮对话正常，无上下文错乱 |
| R-3 | 切换回 WebSocket 实现仍可用（F7） |

---

## 7. 约束与风险

### 7.1 约束

| 项 | 值 |
|---|---|
| 内部 SRAM 余量 | 约 37 KB 常态 / 24 KB 最低（**紧张**）|
| PSRAM | 8 MB（Opus 与 RTP 的大缓冲应落在此处）|
| ESP-IDF | v5.5.4 |
| esp_peer | **锁定 1.5.5**，不可升 1.5.6 |

### 7.2 风险

| 编号 | 风险 | 影响 | 缓解 |
|---|---|---|---|
| **R1** | **服务端 AEC 实测无效** | **迁移目的落空** | 见 §8 Q2；M2 尽早验证，失败则止损 |
| R2 | 48 kHz Opus 编码 CPU 超预算 | 音频断续、AFE 掉帧 | 先实测编码耗时；必要时降 Opus 复杂度 |
| R3 | 内部 SRAM 不足 | 崩溃或功能裁剪 | 大缓冲显式走 PSRAM；实测峰值 |
| R4 | 16k→48k 上采样引入伪影 | ASR 准确率下降 | M1-2 直接检验转写正确性 |
| R5 | 上游修复不可得 | 长期锁 1.5.5 | 已接受；代价见 [firmware-findings.md](firmware-findings.md) A8 |

### 7.3 回退

现有 WebSocket 实现**完整保留**，通过 Kconfig 开关切换。
M2 结束若 R1 成立（AEC 无效），回退到 WebSocket 并保留全部既有功能
（对话、识别、表情、背压），仅放弃打断。

---

## 8. 开放问题（请评审重点回答）

**Q1：上行是否应绕过 AFE？**
AFE 固定 16 kHz，导致上行带宽被限在 8 kHz，而 ES7210 采集本身有 12 kHz 带宽。
绕过 AFE 可直接 24k→48k，带宽更好，且 WebRTC 下 AEC/VAD 均由服务端负责，
AFE 的价值只剩降噪。代价是需重接 xiaozhi 的音频服务（AFE 与状态机、
唤醒词、编解码队列耦合较深）。
**是否值得？还是先按"保留 AFE"实现，把带宽留作后续优化？**

**Q2：如何尽早、低成本地验证服务端 AEC 是否有效？**
这是决定整个迁移价值的问题，但按 §6.2 要到 M2 才有答案。
M1 只发上行，**能否设计一个更早的判据**？
例如：让设备播放一段音频的同时上行采集，观察服务端是否把自身播放内容
也转写进 `transcript`——若是，说明 AEC 未生效。

**Q3：下行是否也需要重采样链？**
48k 解码 → 24k 播放。可否让 esp_peer 直接协商较低采样率
（如 `audio_info.sample_rate = 24000`）以避免下行重采样？
需确认阿里端是否接受非 48k 的 Opus 协商。

**Q4：`pts` 应如何取值？**
`esp_peer_audio_frame_t.pts` 的语义与单位（采样数？毫秒？）在头文件中未说明。
取值错误是否影响服务端 AEC 的时间对齐？

**Q5：M1 的最小实现范围能否再压缩？**
能否不接真实麦克风，先用一段预录音频走通 RTP 上行？
这样 M1 可完全脱离音频服务的改造，独立验证传输与编解码。
