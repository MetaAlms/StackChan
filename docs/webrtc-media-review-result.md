# WebRTC 媒体 SPEC / PLAN 评审结果

评审日期：2026-10-08。基线：`feat/aliyun-omni-v2v`，
`02917f425c415a21063bfca15638fda69bf9cc85`。
对象：[SPEC](webrtc-media-spec.md)、[PLAN](webrtc-media-plan.md)、
[评审请求](webrtc-media-review-request.md)。

## 1. 判定与必须修改的设计

**当前版本不通过，先修设计，再开始 M1。**
M0 的互通和后续严格版本对照不在本轮重测范围；问题集中在媒体前提和退出条件。
预录音频探针、保留 AFE、提前验证全双工价值的方向可保留。

| 优先级 | 位置 | 问题 | 必须修改 |
|---|---|---|---|
| P1 | SPEC §1.1、§3.1 F6、§8 Q1 | 将“WebRTC 内置 AEC”推成已证实的服务端能力 | 改为待验证假设；裸 esp_peer 的实际回声控制与打断效果需真机验证 |
| P1 | SPEC §2.2/4.2，PLAN M1/M2 | 混淆 Opus RTP 时钟与 PCM 采样率 | RTP/SDP 保持 48k；编码 PCM 可用 16k，解码可直接输出 24k，取消“必须上采样” |
| P1 | PLAN M1.5 方案 a/退出条件 | 任意本地 TTS + 空转写无法判定 AEC | 用真实下行播放、持续上行检查、近端正对照和双讲；结果分通过/失败/实验无效 |
| P1 | SPEC §5.2，PLAN M2/M3 | 直接复用会话逻辑不能保证全双工与停止旧播放 | 前置连续采集、服务端取消、本地播放停止、队列与处理中旧任务隔离 |
| P1 | SPEC §5.3/7.3，PLAN M4 | 删除回退所需 Ogg/背压/门控与 SPEC §3.1 F7 冲突 | 清理 WebRTC 路径，保留 WebSocket 与公共提示音依赖 |
| P2 | SPEC §5/验收/资源，PLAN M2 | 回调内存、时长缓冲、SRTP 媒体与资源验收不足 | 明确包复制、有界非阻塞桥接、真实包时长、峰值 heap/栈与重连验收 |

本轮只做文档与代码/二进制复核，没有发送媒体、刷机、读取凭据或更改固件。
下文工程阈值是建议，不是已经测得的性能或规范保证。

## 2. 关键事实 F1–F7 的核验

| 主张 | 判定 | 证据与边界 |
|---|---|---|
| F1：esp_peer 不做 Opus 编解码 | 成立于当前默认实现 | send_audio 把已编码 payload 送至 RTP；nm 无 Opus 符号仅为辅助证据，不能单独证明所有处理能力 |
| F2：WebRTC Opus 固定 48k，因此 PCM 必须 48k | 不成立 | 固定的是 RTP clock/SDP rtpmap；Opus 输入/输出 PCM rate 是独立配置 |
| F3：AFE 固定 16k | 当前管线成立 | 当前 processor 按 16000 累积帧；不应扩大成所有未来 AFE 实现的普遍限制 |
| F4：板级采集/播放 24k | 当前配置成立 | 板级配置与现有采样链；12k 是奈奎斯特上限，不是实测有效带宽 |
| F5：已有 48k 编解码 | 成立但无需据此新增 48k 链 | 具体 Opus API 支持 8/12/16/24/48k；现有 16k 编码器可复用，24k 解码有 API/二进制依据 |
| F6：事件在 txt、stream 1 | 本次观测成立 | 新会话继续通过 channel/session 回调发现 stream，不硬编码永久为 1 |
| F7：SRAM 37k/最低 24k | 历史读数成立，媒体预算未证 | M0 分支绕过正常 Application 音频初始化，不能作为两套并行运行的资源基线 |

SPEC §1.2 的 `DTLS / SRTP ✅` 应写为 DTLS 与相关密钥建立已通；
实际 RTP/SRTP 音频收发、播放、持续负载由媒体探针验证。
`CONNECTED` 和 DataChannel 事件不能证明未执行的媒体路径已全部可用。

## 3. Q1：AEC 早期判据

### “服务端 AEC”目前证据不足

[RFC 7874 §5](https://www.rfc-editor.org/rfc/rfc7874.html#section-5)
建议端点设备实现 AEC；补偿远端无 AEC 的远端回声消除能力是可选项。
[W3C 的 echoCancellation](https://www.w3.org/TR/mediacapture-streams/#dom-mediatrackconstraintset-echocancellation)
是采集侧约束。阿里[WebRTC 接入](https://help.aliyun.com/zh/model-studio/realtime-webrtc-access)
示例使用浏览器 getUserMedia 和远端播放器；
[AOQ 自定义采集](https://help.aliyun.com/zh/model-studio/aoq-custom-audio-capture)
把 3A 处理配置放在客户端 SDK。

因此，协议选型表里的“内置”不能证明裸 esp_peer/RTP 会自动得到服务端 AEC。
这些证据也不能反过来证明阿里服务端没有远端回声处理。
SPEC 应把迁移价值写成假设，把验收写成可观测行为：**播放时持续上行，既不自打断，
又能识别人声插话并停止旧回答。** 服务端 VAD 与 AEC 是不同能力。

### 原方案会同时产生假阳性与假阴性

| 观察 | 可能原因 | 可得结论 |
|---|---|---|
| 任意本地 TTS 被转写 | 服务端没有该音频的参考；或有可识别的残余回声 | 不能直接判 AEC 不存在/无效 |
| 没有转写 | 上行关闭/丢弃、转录未开启、VAD 阈值、初始化/转录失败、声音太轻、实际回声抑制 | 不能直接判 AEC 生效 |
| speech_started | 服务端 VAD 检测到语音活动 | 不是 AEC 状态或抑制量指示器 |
| 转写到少量自身声音 | 残余进入 ASR；ASR 与模型理解也可能不同 | 不等于必然自打断，仍需关联轮次/取消与真实播放 |

本项目已经存在播放时关闭采集/丢上行的逻辑，必须排除这一直接混淆。
正确 RTP 时间戳不能代替 AEC 的播放参考与实际播放/采集延迟。
本地任意音频可作为参考覆盖范围的附加实验，不能作为主闸门的止损刺激。

### 保留 M1.5，默认采用最小真实双向回路

M1.5 提前接入麦克风和服务端实际音频解码播放，只做媒体、事件和测量，
暂不接完整 UI/表情。用同一 RTC 会话的真实回复作远端刺激；记录实际播放 PCM
及时间，不把“服务端生成完成”视为“设备播放完成”。

| 测试 | 必要控制 | 建议早期退出条件 |
|---|---|---|
| 喇叭关闭，近端人声 | ASR 配置确认，采集/编码/发送正常 | 三个不同短句识别关键内容，建立正对照 |
| 只有真实远端播放 | 正常及最高预期音量；播放期间发送计数持续增加，麦克风确实有声学回声 | 多次连续至少 30 秒播放，不产生自身声音造成的新轮次/取消 |
| 远端播放 + 人声插话 | 近端短句与远端内容不同，音量/距离/阈值保持一致 | 新问题可识别，旧播放实际停止，并改答新问题 |
| 插话之后再次说话 | 检查取消后的采集与会话状态 | 继续正常识别和回应 |

结果写为：**目标通过 / 目标失败 / 实验无效**。
正对照失败、媒体时间轴错误或播放时上行中断属于实验无效，先修实验；
有效测试下远端独说/双讲不达标，才判当前配置不满足目标，评估止损。
M1.5 通过只允许继续投入，不能取代 M3 在更多音量、距离、噪声与重连条件下验收。

[服务端事件](https://help.aliyun.com/zh/model-studio/server-events)
定义的是 VAD/转录结果，没有提供 AEC 后 PCM 的测量接口。
没有这种观测或其他可靠仪器，不能从“空转写”计算 ERLE 或给出 10 dB 等抑制量。
优先按误触发率、插话识别率、旧音频停止延迟验收。
可提议 `speech_started → 旧音频停止` 的 p95 ≤ 200ms，真实插话起点至停止另计；
此为待测产品目标，不是现有实现保证。

浏览器 echoCancellation 开/关且核对实际 getSettings 的对照，可低成本探测
客户端处理依赖；它不能替代 ESP32 真机，或独立证明服务器是否有 AEC。

## 4. Q2：采样率与 AFE

[RFC 7587 §4.1/7](https://www.rfc-editor.org/rfc/rfc7587.html#section-4.1)
要求所有 Opus 模式使用 48k RTP 时钟，SDP 使用 `opus/48000/2`。
实际 PCM 可以为 16k 单声道；`/2` 也不要求每个 payload 都是双声道。

具体 API 证据：

- [esp_opus_enc.h:70](/Users/amtf/Documents/Git/StackChan/firmware/managed_components/espressif__esp_audio_codec/include/encoder/impl/esp_opus_enc.h:70)
  明确允许 8/12/16/24/48k。
- 实际 esp_opus_dec_open 把 decoder 的 sample_rate/channel 直接传给
  opus_decoder_create；[Opus decoder API](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html)
  支持 24k 输出。
- [M0:406](/Users/amtf/Documents/Git/StackChan/firmware/main/hal/webrtc/webrtc_m0.cc:406)
  本来就注明 Opus clock 与采集率不同。

建议最小路径：

```text
上行：24k 采集 → 现有重采样/AFE 16k → Opus 16k 单声道编码 → RTP(48k 时钟)
下行：raw Opus packet → Opus 解码为 24k 单声道 PCM → 24k 播放
```

**esp_peer 的 audio_info.sample_rate 保持 48000；encoder PCM 配置 16000，
decoder 输出配置 24000。** 不需要为了避免下行重采样改协商 clock 为 24000。
此路径的规范与 API 可行性已核验，阿里端实效仍由 M1/最小双向测试证明。

先保留 AFE 合理；16k 对应理论最高 8k 音频带宽，是否满足 ASR 应用仍需语料实测。
无需在 M1 并行重接整个音频服务。若后续有可重复的音质/识别问题，再对照
原始 24k 编码；绕过 AFE 的 24k PCM 同样可以直接编码，无需先升为 48k。

[AFE processor:155](/Users/amtf/Documents/Git/StackChan/firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc:155)
只用 VAD 更新状态，随后无条件累积处理后的音频；**不是按本地 VAD 只输出有声帧**。
本地 VAD 可保留 UI/检测用途，停止驱动 WebRTC 的 commit/response 即可。
设备侧 AEC 的现有假参考问题仍需保持关闭；不能把“AFE”整体等同于已有效 AEC。

## 5. Q3：pts 已确认是毫秒

实际 esp_peer **1.5.5**，源码坐标 `c8650846b512e6e1375e5f78c1c41619b8d645eb`，
ESP32-S3 库 SHA-256：
`25a338fc6b05f702947c2af0e924f2dd3006503908e889dcdc512782d0ea3a58`。
反汇编/DWARF 对应原源 peer_default.c:1471、rtp.c:403/416、peer_default.c:618/627：

```c
// 二进制等价逻辑，非官方公开源码。
peer_send_audio(frame) {
    rtp_encoder_encode(enc, frame->pts, frame->data, frame->size);
}
// Opus 分支
rtp_timestamp = (uint32_t)(pts_ms * 48);
// 接收侧
rate = recv_audio_sample_rate ?: cfg.audio_info.sample_rate ?: 8000;
frame_pts_ms = (uint64_t)raw_rtp_timestamp * 1000 / rate;
```

关键汇编：peer_send_audio +0x48 从 frame 偏移 0 读取 pts，直接传 encoder；
calc_timestamp Opus 分支 +0x13 `addx2` 乘 3，+0x16 `slli ... 4` 再乘 16。

| 包时长 | 16k PCM 样本/声道 | pts 增量（ms） | RTP timestamp 增量 |
|---|---:|---:|---:|
| 20ms | 320 | 20 | 960 |
| 60ms | 960 | 60 | 2880 |

按采集/媒体时间累计；缺帧、主动丢弃或 DTX 时保留时间轴，不能把“已成功发送包数”
当媒体时间。发送 pacing 用实时期限，预录音频不能一次性倾倒。
示例的合成 G711 序号不是正确 Opus 时间戳依据。

接收 pts 未扣除首包原点，也未扩展 32 位 RTP 回绕，不能直接当本地播放截止时间。
48k 完整回绕周期约 24 小时 51 分，随机起点可使首次回绕更早。
桥接层需定义新会话/SSRC 重置、相对时间与回绕处理；若使用 raw RTP 边界数据，
确认接口是否提供足够信息。正确时间轴保障媒体调度，**不证明服务端 AEC 已有参考或正确对齐**。

## 6. Q4：预录音频适合 M1，补齐配置和尾部静音

认可预录探针，能排除麦克风/状态机改造。但“无转写就证明传输/编解码失败”过强。
建议固定模型、fixture hash、PCM 参数、Opus 参数与预期关键词：

1. 等 session.updated；明确启用当前模型支持的 input_audio_transcription，
   检查配置回显，不把 SDK 参数直接写成 JSON 字段。
2. 一次编码并发送一个完整 raw Opus packet，保持实际节奏；不传 Ogg、PCM、
   多包拼接或带自定义长度前缀的 payload。
3. 先用 20ms 包建立明确基线；可再测现有 60ms 以评估复用成本。
   这是工程选择，**minptime=20 不证明远端固定 20ms**。
4. fixture 含前后静音，尾部至少覆盖配置的 silence_duration_ms 并留余量，
   避免发完语音就停发导致 VAD 没有观察到静音。
5. 记录发送返回值、包数/字节/媒体时长、VAD start/stop、transcription.failed、
   转写与超时；将配置、发送、VAD、ASR 失败分开，恢复 M1 的 5 分钟资源检查。

依据：[WebRTC 初始化后再挂载媒体](https://help.aliyun.com/zh/model-studio/realtime-webrtc-access)、
[客户端事件的转录配置](https://help.aliyun.com/zh/model-studio/client-events)。
少量 PCM 或预编码 raw Opus fixture 可放 flash/文件，按单包边界读取；
不必为了 M1 接入完整存储/UI，也不必重复读取密钥生成新的验证脚本。

## 7. Q5：调整里程碑与删除边界

建议顺序：

| 阶段 | 退出条件 |
|---|---|
| 前提核验 | 将服务端 AEC 标为假设；定下 PCM/clock/pts、包边界与回调契约 |
| M1 | 16k 预录 Opus 上行、正确时间轴/VAD/ASR，基本资源读数 |
| M1.5 | 最小真实双向、全程上行、近端正对照/远端独说/双讲；有效实验才允许止损 |
| M2 | 接产品音频服务、事件/UI；包含本地打断/旧任务隔离和状态机改造 |
| M3 | 不自打断与真实插话同时通过，补抖动/丢包/长时/断网重连，量化停止延迟 |
| M4 | 清理 WebRTC 分支、保留 WebSocket 与公共功能，实测两种固件配置回归 |

M1.5 实际需要提前做原 M2 的一部分媒体接入，不能再称为完全不投入双向链路。
它仍比完整产品移植更小，但需要按依赖重新估算，不能从方法描述证明“工作量 1/3”。
SPEC 与 PLAN 的 M2/M3 验收编号要统一；不要在 SPEC 写 M2 判 AEC，PLAN 却等 M3。

M4 **不能删除**仍被 WebSocket 使用的 Ogg muxer、背压、轮次/半双工逻辑。
[AudioService::PlaySound:667](/Users/amtf/Documents/Git/StackChan/firmware/xiaozhi-esp32/main/audio/audio_service.cc:667)
还用 OggDemuxer 播放本地提示音，它不是 WebSocket 专属。
只让 WebRTC 不经过这些逻辑，必要时按协议分支收紧作用域。
真正删除回退实现必须另行修改 SPEC §3.1 F7；不能一边要求随时回退，一边删除其功能依赖。

## 8. Q6：必须纳入的媒体/资源风险

### 全双工和本地打断不是改传输即可获得

[Application:979](/Users/amtf/Documents/Git/StackChan/firmware/xiaozhi-esp32/main/application.cc:979)
说话时会关闭非 Realtime 模式的采集；现有 Aliyun SendAudio 也有上行门控。
[EnableVoiceProcessing:613](/Users/amtf/Documents/Git/StackChan/firmware/xiaozhi-esp32/main/audio/audio_service.cc:613)
每次 enable 都重置 decoder/输入 resampler，并进入 warmup。
WebRTC 会话期间应连续采集，不能频繁开关它制造“空转写”。
接收媒体也不应只受异步 UI speaking 状态控制，避免 RTP 先于控制事件到达而丢首帧。

[AbortSpeaking:1007](/Users/amtf/Documents/Git/StackChan/firmware/xiaozhi-esp32/main/application.cc:1007)
目前仅设 aborted_ 和发送 cancel，aborted_ 没有读取方，不会停止本地队列。
[ResetDecoder:702](/Users/amtf/Documents/Git/StackChan/firmware/xiaozhi-esp32/main/audio/audio_service.cc:702)
也不能撤回已出队、正在解码/播放的任务；解码在锁外完成后还能重新入队。
需要设计会话/响应代际检查、丢弃过期解码结果、当前输出的停止/有界尾音和
晚到旧 RTP 包处理。控制事件有 response_id，RTP payload 未必有对应关联，
不能假定只加 generation 就自动解决两个通道的乱序边界，最小回路要专门验证。
打断不要清掉用户正插话的上行；换会话/断线才清理旧上行与未完成任务。
服务端取消按模型支持处理，不要照搬其他厂商的 truncate/output-buffer 事件。

### 回调数据借用、网络循环不可阻塞

实际 on_audio 在栈上构造 frame，payload 指向 RTP/jitter 接收内存，
回调后被释放/复用。**异步消费必须在回调返回前复制 payload 与所需元数据**。
默认实现由 peer 主循环驱动收包/状态；回调内阻塞解码、播放或 `wait=true`
等待队列，会同时阻塞 ICE、RTCP、SCTP/取消事件。
必须用拥有内存的有界队列快速返回，并定义满队列/欠载/丢包恢复。
UDP 没有现有 WebSocket/TCP 阻塞读取的等价背压。

Opus generic RTP 编码器有 1428 字节内部包缓冲，直接 memcpy payload 到
`buf + 12 + ext_reserve`，此路径未见长度检查/分片（rtp.c:315）。
应用需按实现容量和 MTU 限制单包长度；不能把大块 PCM 或多包编码结果整体送入。

### jitter、播放与帧时长

esp_peer **已有** audio_recv_jitter；默认 100ms timeout/100kB。
本机 M0 已设 **100ms、32KiB jitter cache、32KiB send pool**，并不是正在使用全部默认值。
jitter 按序号整理、等缺包，连续包可立即回调；不是按 PCM 时钟定时播放的设备缓冲。
应用不应重复堆一个长抖动缓冲，而应测总排队时长、欠载/溢出和迟到率。

| 当前 AudioService 队列 | 帧数 | 每包 60ms | 每包 20ms |
|---|---:|---:|---:|
| decode | 133 | 7.98s | 2.66s |
| send | 40 | 2.4s | 0.8s |
| encode/playback，各自 | 2 | 120ms | 40ms |

另有处理中任务与 I2S 缓冲，playback=2 不能单独证明是否足够。
按时长、字节和目标延迟设计容量，不直接沿用 WebSocket 的 8 秒缓存。
按实际 Opus 包时长/解码样本数分配 PCM；不假定 remote 固定 20ms。
SDK decoder 的默认 INVALID 配置仅按 60ms 计算建议空间，而合法 Opus packet 可到 120ms。
不要因每次帧时长变化就重建同一流的 decoder、丢掉编解码状态；需改缓冲策略。
FEC/PLC 不会因 SDP 写 useinbandfec 就自动替应用恢复丢包，应验证实际恢复路径。

### CPU、heap 与验收

DTLS 是建连开销，SRTP 是持续媒体开销；分别测握手峰值和双向 encode/decode/
SRTP/AFE 的 p95/p99 耗时、deadline miss、loop 阻塞与播放欠载。
不能由双核/硬件加密支持推定实时预算足够。

当前 M0 绕过正常音频服务初始化，且 free sram 只测 MALLOC_CAP_INTERNAL。
**8KiB 总余量不是资源可行性的充分判据**。补测 internal+8bit、DMA、PSRAM 的
free/minimum/largest block、分配失败和各 task stack 高水位；实际检查大缓冲
allocator，而不是仅在设计里要求“放 PSRAM”。覆盖双向/AFE/UI、首次建连/重连、
长响应、队列接近满和取消后的资源回落。

10 轮是功能冒烟，不能代表长时稳定；增加持续双向、抖动/丢包和重复重连。
重建连接需重新发现事件 stream、完成 SDP/会话初始化并隔离旧回调/数据。
阿里端是云端，N2 的“局域网”改为明确 Wi-Fi/地域/网络条件下的测量目标与分位数。
不自打断失败要先定位采集门控、时间轴、排队、VAD、回声参考及残余，不能一律归因 AEC。

## 9. 交付与状态

相关发现同步至 firmware-findings A9/B16–B18，并给历史 AEC 边界/总计划加上本轮状态说明。
原 SPEC/PLAN 保持待修改，供作者按上述结论修订；没有替它们宣告通过。
设备恢复 WebSocket 是日常使用的选择，不是评审或后续 M1 的技术前置条件；
本轮未刷写设备。

二进制复查对象为 peer_default.c.obj、rtp.c.obj、sdp.c.obj、rtp_jitter.c.obj
及 Opus decoder；提取/反汇编方法沿用 M0 复核文档。
临时输出在 `/tmp/stackchan-media-review-v155/` 和
`/tmp/stackchan-esp-peer-review-v155/`；长期证据以库 SHA、原源坐标、公式和现有代码位置为准。
