# 评审请求：WebRTC 媒体链路 SPEC 与 PLAN

请评审 [webrtc-media-spec.md](webrtc-media-spec.md) 与
[webrtc-media-plan.md](webrtc-media-plan.md)。

**本轮评审的是"计划是否合理"，不是"结论是否成立"。**
M0 的互通性结论已经两轮独立复核并通过真机严格 A/B 验证
（[review-result](webrtc-m0-review-result.md)、
[review-result-2](webrtc-m0-review-result-2.md)），本轮不重复。

---

## 0. 环境与位置

| 项 | 值 |
|---|---|
| worktree | `/Users/amtf/Documents/Git/StackChan` |
| 分支 | `feat/aliyun-omni-v2v` |
| 硬件 | M5Stack StackChan（ESP32-S3，8 MB PSRAM，16 MB Flash）|
| ESP-IDF | v5.5.4 |
| esp_peer | **锁定 1.5.5**（1.5.6 有 ICE 缺陷，见 firmware-findings A8）|
| 评审对象 | `docs/webrtc-media-spec.md`、`docs/webrtc-media-plan.md` |

---

## 1. 背景（一句话）

WebSocket 协议官方不做回声消除，导致设备自打断、无法语音打断。
WebRTC 内置 AEC。M0 已证明传输层可用，现要设计其上的**媒体与协议层**。

---

## 2. SPEC 中依赖的关键事实（均可复核）

若下列任一条有误，SPEC 的设计空间判断即失效。请优先核验。

| # | 主张 | 复核方式 |
|---|---|---|
| F1 | **esp_peer 不做 Opus 编解码**，只做 RTP 打包/解包 | `nm libpeer_default.a \| grep opus_` 无结果；依赖仅 `esp_libsrtp`；`esp_peer.h:64` 注释 "Send payload still follows audio_info.codec" |
| F2 | **WebRTC 的 Opus 为 48 kHz** | Answer SDP 中 `a=rtpmap:111 opus/48000/2` |
| F3 | **esp-sr AFE 固定 16 kHz** | `afe_audio_processor.cc`：`frame_samples_ = frame_duration_ms * 16000 / 1000` |
| F4 | 板级采集/播放为 24 kHz | `main/hal/board/config.h`：`AUDIO_INPUT_SAMPLE_RATE 24000` |
| F5 | `esp_audio_codec` 支持 48 kHz 且工程已依赖 | `esp_audio_types.h`：`ESP_AUDIO_SAMPLE_RATE_48K`；`main/idf_component.yml` 已列 |
| F6 | 服务端事件走自建 `txt` 通道（stream 1）| M0 实测：`[channel open] label='txt' stream_id=1` |
| F7 | 内部 SRAM 余量约 37 KB / 最低 24 KB | 设备日志 `free sram: 37111 minimal sram: 24171` |

**F2 与 F3 的组合是 SPEC 中最重要的约束**：AFE 输出 16 kHz，而 WebRTC 要求 48 kHz，
故 16k→48k 上采样无法避免，上行带宽被限制在 8 kHz——这引出 SPEC 的核心开放问题 Q1。

---

## 3. 计划的核心结构

```
M1    上行 RTP 通路（用预录音频，不接麦克风）
M1.5  AEC 早期判据  ← 新增的止损点
M2    双向 + 接麦克风
M3    打断与稳定性
M4    清理（删除 WebSocket 专用补丁）
```

**设计意图：把"迁移是否值得"（服务端 AEC 是否有效）的答案
从 M3 提前到 M1.5**，代价很小，避免走完 M2/M3 才发现方向错误。

---

## 4. 请重点评审的问题

### Q1（最重要）M1.5 的 AEC 判据设计是否成立？

PLAN 中提出的方案 a：

> 让设备播放一段可辨识音频（如 TTS 念一句话），**同时**把麦克风采集上行。
> 观察服务端转写结果：
> - 转写内容 = 设备播放的内容 → **AEC 未生效**
> - 转写为空或只有环境音 → **AEC 生效**

**疑虑：**

1. 这个判据是否可靠？设备自身播放的声音经 AEC 后应被抑制到什么程度才算"生效"？
   若只抑制 10 dB，转写可能仍能识别出片段——**阈值怎么定？**
2. 服务端 VAD 可能把设备播放的声音当作语音输入并触发回应，
   这本身是否就是"未生效"的证据？还是 AEC 生效也会出现？
3. **有没有更快/更可靠的判据？** 例如：
   - 直接观察某类事件或字段？
   - 对比"播放时"与"静音时"的上行转写差异？
4. 若 M1.5 判定为"生效"但 M3 实测仍自打断，是否说明这个判据设计有缺陷？

### Q2 采样率路径：先保留 AFE（带宽 8 kHz）是否可接受？

SPEC Q1 提出是否应绕过 AFE 以获得 12 kHz 带宽。

**我的倾向是"先保留 AFE"**，理由：
- 绕过 AFE 要重接 xiaozhi 的音频服务（与状态机、唤醒词、编解码队列耦合深）
- WebRTC 下 AEC/VAD 由服务端负责，AFE 的价值只剩降噪
- 8 kHz 带宽对中文 ASR 通常够用

**请判断：**
1. 8 kHz 带宽对阿里 ASR 是否够用？有没有已知的失败案例？
2. 上采样 16k→48k 是否会给 ASR 引入伪影？还是纯属浪费带宽但无害？
3. 是否应在 M1 就**并行验证**绕过 AFE 的可行性，以免 M2 才发现带宽不够？

### Q3 `pts` 的语义

`esp_peer_audio_frame_t.pts` 在头文件中无说明。示例中直接用了递增序号。

**疑问：** 取值错误是否影响**服务端 AEC 的时间对齐**？
WebRTC 的 AEC 依赖参考信号与麦克风信号的时间对齐——若 RTP 时间戳
（由 esp_peer 从 pts 推导）不准，AEC 是否就失效了？
**若是，这会直接击穿 M1.5 的判据。** 请评估。

### Q4 M1 用预录音频是否合适？

**优点**：不碰音频服务，失败时归因清晰，工作量约 1/3。
**疑虑**：
1. 预录音频的节奏、时长、静音间隔若与真实对话不同，
   服务端 VAD 可能不按预期触发——会不会导致 M1 假阴性？
2. 是否应改为"接麦克风但只发上行"，以更接近真实？
3. 有没有比"嵌入或从 SD 卡读"更合适的音频注入方式？

### Q5 里程碑划分

1. **M1.5 的位置**是否最优？还能更早吗？
2. **M4 的删除清单**（Ogg muxer、背压、排空守卫、延后提交、本地 VAD、半双工门控）
   是否有不该删的项？特别是：**服务端 AEC 生效后，半双工门控真的可以删吗？**
3. 是否缺少必要的里程碑？例如"长时间稳定性"或"弱网"？

### Q6 未被识别的风险

SPEC §7.2 列了 5 项（AEC 无效、48k 编码 CPU、SRAM 不足、上采样伪影、上游不修）。
**还有哪些？** 特别希望了解：
- WebRTC 下 DTLS/SRTP 的加密开销对 ESP32-S3 双核的影响
- RTP 抖动缓冲与播放队列的交互（现有 `MAX_PLAYBACK_TASKS_IN_QUEUE = 2` 是否够）
- 与服务端 20 ms 打包（SDP 中 `a=fmtp:111 minptime=20`）的匹配

---

## 5. 期望的输出

1. 对 Q1–Q6 的逐条回答
2. SPEC 中依赖的关键事实（§2 表）若有错误，请指出
3. **计划是否存在"走完了才发现不可行"的路径**——即止损点是否足够靠前
4. 任何我在本轮引入的新错误或过度推断
