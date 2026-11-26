# M1 完整验收结果（2026-10-22）

> **结论：修复 SRTP profile 后，M1 完整模式（三 clip + 5 分钟资源循环）
> 在本机真机上 `VERDICT: PASS`。**
> 这是开发侧自测结果，待评审放行；不宣告 M1.5/产品阶段，也不改变 WebSocket 回退。
> 原始串口日志（脱敏）：`docs/device_diag_evidence/2026-10-22-m1/m1-full-acceptance.log`。

## 构建与运行条件

| 项 | 值 |
|---|---|
| 固件 | 当前工作树构建（esp_peer **1.5.7** 组件）+ [SRTP profile 修复](webrtc-media-m1-srtp-profile-rootcause.md) |
| 模式 | `CONFIG_STACKCHAN_WEBRTC_M1=y`，`SHORT_DIAG=n`（完整三 clip + 资源循环） |
| 帧 | 20 ms，48 kHz mono Opus，RTP 48 kHz |
| 循环 | `CONFIG_STACKCHAN_WEBRTC_M1_LOOP_SECONDS=300` |
| 设备/网络 | CoreS3，同一 Wi-Fi 与端点；未改媒体/协议参数 |
| 夹具 | 内嵌 `zh_1`/`zh_2`/`zh_3`（16 kHz PCM，经 16→48k 重采样） |

## 四层证据

| 层 | 结果 |
|---|---|
| 配置 | `session.created` → `session.update` → 回显校验通过：`server_vad thr=0.50 silence=800`、`qwen3-asr-flash-realtime` |
| 发送 | `opus packets=15901`、1,706,275 B、max payload 325 B（预算 1352 B）、`pacing late=2`、超限/OOM/编码/重采样/发送失败均为 0 |
| SRTP/UDP | `srtp_protect 15901/15901 ok`、`udp 15901/15901 full writes`、无短写/失败、`ssrc_changes=0 pt_changes=0` |
| VAD | `started=69 stopped=69` |
| ASR | `completed=69 failed=0 server_errors=0` |

三 clip 的转写与关键词：

| clip | VAD | 转写 | 关键词 |
|---|---|---|---|
| zh_1 | 1/1 | "今天我们测试语音连接。" | 3/3（测试/语音/连接） |
| zh_2 | 1/1 | "桌上有一本蓝色的书。" | 2/2（蓝色/书） |
| zh_3 | 1/1 | "请回答一加一等于几。" | 2/2（一加一/等于） |

## 验收判据对照

| 判据 | 要求 | 实测 |
|---|---|---|
| **M1-1** | 出现 `input_audio_buffer.speech_started` | 3/3 clip 有 VAD；全程 69 次 start/stop |
| **M1-2** | 转写关键内容与 fixture 一致 | 3/3 完全一致（见上表） |
| **M1-3** | 5 分钟资源检查 | `sustained loop COMPLETE`，wall 318.1 s（预算 300 s） |

固件自己的最终判决：

```
config verified        : yes
3 clips with keywords  : 3/3
3 clips with valid VAD : 3/3
any clip failed        : no
zero codec/oversize    : yes
send path observed     : yes (protect=15901 udp=15901 failed=0)
sustained loop         : COMPLETE
acceptance incomplete  : no
VERDICT: PASS
```

## 资源与延迟

| 指标 | 值 |
|---|---|
| 循环时长 | 318.1 s（66 个 clip 轮次） |
| 内部 DRAM free | 104831 → 104331 B；最小值 101955 → 98183 B |
| PSRAM free | 7,913,256 → 7,902,460 B |
| sender 栈余量 | 10148 B / 32768 B |
| 重采样耗时 | p50 358 µs / p95 455 µs / p99 787 µs |
| Opus 编码耗时 | p50 3296 µs / p95 7637 µs / p99 9315 µs（20 ms 帧预算内） |
| 本地 Opus 解码 | 15901 ok / 0 failed（peak 0.2019） |

## 覆盖边界（如实）

- **单次运行**：一次 PASS，不等于长时稳定性（重连、抖动、丢包留给 M3）。
- **只验上行**：M1 不接麦克风、不做下行播放、不判 AEC/打断；服务端 `response.*`
  事件只做记账，`on_audio` 只计数。
- **`media/wall` 指标仍有口径问题**（findings B34）：本次 `318020 / 260641 ms`，
  wall 不是所有发送帧的墙钟，不能解读为"媒体快于实时"；以 `pacing late=2`
  与 `media clock` 推进为准。
- **版本**：本次跑在 esp_peer 1.5.7 组件上；U1-4 的版本取舍仍需修复后两臂重跑，
  不能把"M1 在 1.5.7 上通过"直接当作采用理由（修复与版本无关）。
- **短模式**仍保留，作为 U1 对照入口；本次构建把 `SHORT_DIAG` 关掉，
  sdkconfig 是本地 gitignore 配置。

## 后续

1. 评审放行本结果 → M1 收口；按 PLAN 进入 M1.5（AEC 假设验证）前置。
2. 把 SRTP profile 修复移植到产品 WebRTC 路径（M2），否则产品链路仍会静默丢包。
3. 修复后重跑 U1 两臂（各至少一次），补齐 U1-4 的版本对照结论。
4. 给 esp_peer 报上游 issue/PR（offer 与实际实现的 SRTP policy 不一致）。
