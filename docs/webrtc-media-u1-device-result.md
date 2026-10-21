# U1 两臂真机对照结果（2026-10-22）

> 状态：**开发侧实测完成，U1-3 的设备部分已交付**；U1-4 只给建议，未自行宣告
> 1.5.7 采用。本结果只覆盖 U1-S 短诊断，**不替代 M1-1..M1-3，M1 仍未通过**。
> 原始串口日志（已脱敏）在同目录，run 明细见文末。

> 2026-10-22 后续：本页记录的"7 次运行均无 VAD"共同根因是 **DTLS-SRTP profile
> 不匹配**（协商 SHA1_32、esp_peer 实现 SHA1_80），修复后短诊断已出现 VAD 与
> 正确转写。详见 [根因记录](webrtc-media-m1-srtp-profile-rootcause.md)。
> 本页的版本对照结论（1.5.5/1.5.7 无差异）不受影响。

## 环境与口径

| 项 | 值 |
|---|---|
| 设备 | M5Stack StackChan / CoreS3，ESP-IDF v5.5.4 |
| 固件 | 归档两臂，同源指纹 `4c14aaed…6517eb`（见 [U1 结果](webrtc-media-u1-result.md)） |
| arm-a | esp_peer **1.5.5**，app bin `17fe8c19…53c5` |
| arm-b | esp_peer **1.5.7**，app bin `e313ac0e…cb99` |
| 网络 | 同一 Wi-Fi、同一账号/端点；未改 cipher、SSRC、ICE 角色、Opus/48k 参数 |
| 诊断模式 | `CONFIG_STACKCHAN_WEBRTC_M1_SHORT_DIAG=y`，20 ms，2 s 前置静音 + `zh_1` + 800 ms tail，ASR 等待 20 s（U1-S 预检的冻结参数） |
| 运行方式 | `esptool.py run` 复位（**不重刷**，每臂只刷一次），115200 串口全量采集 |

每次运行都是全新会话；`zh_1` fixture、编码器、重采样与 SDP 完全一致。
日志已做脱敏（SSID/BSSID/STA MAC/内网 IP 替换），其余字段原样保留。

## 结果

### 连通运行（两版本共 7 次）

| 运行 | 版本 | DTLS 握手 | DTLS profile | 配置 gate | SRTP / UDP | VAD | ASR | 结果 |
|---|---|---|---|---|---|---|---|---|
| arm-a run1 | 1.5.5 | 2 次失败后成功，总 7992 ms | 2（DATAGRAM） | pass | 1339/1339 全写出 | 0/0 | 0 | asr-timeout |
| arm-a run2 | 1.5.5 | 首握成功 591 ms | 2 | pass | 1339/1339 | 0/0 | 0 | asr-timeout |
| arm-a run3 | 1.5.5 | 首握成功 618 ms | 2 | pass | 1339/1339 | 0/0 | 0 | asr-timeout |
| arm-a run4 | 1.5.5 | 首握成功 597 ms | 2 | pass | 1339/1339 | 0/0 | 0 | asr-timeout |
| arm-b run1 | 1.5.7 | 首握成功 604 ms | 2 | pass | 1339/1339 | 0/0 | 0 | asr-timeout |
| arm-b run2 | 1.5.7 | 首握成功 605 ms | 2 | pass | 1339/1339 | 0/0 | 0 | asr-timeout |
| arm-b run3 | 1.5.7 | 首握成功 582 ms | 2 | pass | 1339/1339 | 0/0 | 0 | asr-timeout |

所有连通运行共同点：

- `session.created on stream 1` → `session.update sent` → **配置回显校验通过**：
  `turn_detection=server_vad thr=0.50 silence=800 transcription=qwen3-asr-flash-realtime`
- 媒体：1339 包 / 29115 B，max payload 325 B（预算 1352 B），媒体时钟 0..26760 ms，
  `pacing late` 4–8，`oversend/encode/resample` 失败均为 0
- SRTP protect 1339/1339、`udp attempts=1339 packets_written=1339`、无短写/失败
- 本地 Opus 解码 1339 ok / 0 failed（peak 0.1904）；重采样 48k RMS peak 0.1922、
  avg 0.0102（含 2 s 静音与 20 s ASR 等待静音，均值不代表语音段）
- **服务端 VAD 恒为 0（started=0 stopped=0），ASR completed=0，`zh_1` 关键词 0/3，
  `transcript=""`，20 s 后 `outcome=asr-timeout`**
- sender 栈余量 9540–9824 B / 32768；编码 p50 2.66–3.07 ms、p99 6.87–7.41 ms

### 一次传输层失败（arm-a 刷机后的第一次启动）

- `hs calls=19 dtls=18 ok=0 fail=18 total=23324 ms`，profile=-1，`session_created=0`
- 结束 `VERDICT: FAIL - transport layer. stage=server answer received(4) peer_state=6`
- 探针明确记录：**DTLS 记录双向都有（19 TX / 41 RX），但握手始终没有成功**
  （`last_ret=-30848`，`agent_recv timeout`）
- 同一臂随后 4 次运行（含重刷前的两次启动）均连通，**该失败未复现**

## 结论

1. **版本不改变媒体验收结果。** 两个 peer 版本共 7 次连通运行全部媒体送达
   SRTP/UDP 边界、无本地编码/发送错误、会话配置校验通过，但服务端 VAD/ASR 都是 0。
   M1 的阻塞点**不在 esp_peer 1.5.5/1.5.7 的差异上**。
2. **失败面已收窄。** ICE（PAIRED/Candidate responded）、DTLS 握手、SCTP/DataChannel、
   会话配置、SRTP 保护、UDP 写出、本地 Opus 解码均已有成功证据；剩下未解释的
   只有"服务端是否解码/接受了这些 RTP"或"服务端 VAD 的触发条件"。
3. **DTLS 抖动存在，但不能归因版本。** arm-a 出现 1 次 18 连败、1 次 2 重试；
   其余首握。arm-b 3/3 首握。样本量不足以说 1.5.7 改善；且失败形态是
   DTLS 握手超时，不是 issue208 的 ICE 提名拒绝（ICE 已 PAIRED、Candidate 有响应）。
4. **诊断口径有一个缺陷（非媒体缺陷）。** `media/wall elapsed 26780 / 3865 ms` 的
   wall 只累计 `StreamClip` 段（约 `zh_1` 的 3.887 s），而 media 含前置静音与
   ASR 等待静音（1339 帧 × 20 ms）。该行不能被解读为"媒体比实时快 7 倍"，
   也不能作为实时性/丢包判据。见 findings B34。

## 对 U1-4 的建议

- **不把 1.5.7 设为默认基线**：本次没有任何"媒体/VAD 改善"证据，1.5.7 也带入了
  锁文件重求解的维护成本；保持 1.5.5 基线、1.5.7 归档保留为候选。
- 若后续需要 issue208 的 XOR-MAPPED/prflx 修复，再按本归档切换，并重新做本页对照。
- 这是**建议**，最终采用与否由评审确认；本文件不宣告采用。

## 下一步（M1 阻塞定位，按性价比排序）

1. **host/browser 参考客户端**：用同一账号/端点、同一 `zh_1` Opus 走 WebRTC，
   观察服务端是否产生 VAD/ASR。
   - 同样为 0 → 问题在服务端/账号/端点配置面，先把这一层排除；
   - 有 VAD → 问题在设备 RTP 是否被接受（SRTP protection profile、payload、时序）。
2. **补齐两个观测**：下行 RTP/RTCP 计数（当前只计数不输出）、DTLS-SRTP
   **protection profile**（当前只记录 DTLS cipher，B23 类混淆的根源）。
   如果服务端对我们的 SRTP 完全无反馈，优先怀疑 profile/keys 而不是 VAD 阈值。
3. **帧参数对照**：answer 声明 `opus/48000/2`，当前设备发单声道；用 stereo（双声道
   复制）和 `fmtp` 差异做单变量对照，不改全局 48k 要求。
4. 视结果再决定是否需要 RTCP 或更长的连续语音段。

## 未下的结论

- **不宣告 M1 通过**：短模式有界，不覆盖 M1-1..M1-3，也没有 5 分钟资源检查。
- **不宣告 1.5.7 采用**：无改善证据，且样本量小。
- 不主张"服务端无回声控制"或任何 AEC 结论；本轮没有下行播放与双讲实验。
- 不把"本地 SRTP/UDP 写出成功"当作"服务端收到并识别语音"的证据。

## 附件（同目录）

| 文件 | 内容 |
|---|---|
| `arm-a-1.5.5-transport-fail.log` | DTLS 18 连败 → transport FAIL 的完整启动段 |
| `arm-a-1.5.5-run1.log` … `run4.log` | 1.5.5 四次连通运行 |
| `arm-b-1.5.7-run1.log` … `run3.log` | 1.5.7 三次连通运行 |
