# M1 上行阻塞根因：SRTP protection profile 不匹配（2026-10-22）

> **结论：** 设备侧媒体"本地发送成功、服务端毫无反应"的根因是
> **DTLS-SRTP 协商到 SHA1_32，而 esp_peer 固定安装 SHA1_80 的 SRTP 策略**。
> 服务端对每个包认证失败、静默丢弃，所以 VAD/ASR 恒为 0。
> 修复后同一短诊断立刻出现 `speech_started` 与正确转写。
>
> 本页是 M1 的诊断记录，不是 M1 验收：**M1-3（完整三 clip + 5 分钟资源）
> 与 M1.5 尚未执行**。

## 1. 现象（修复前）

2026-10-22 两臂真机短诊断，两版本共 7 次连通运行：

- ICE、DTLS、SCTP/会话、配置回显、SRTP protect、UDP 写出全部成功；
- 本地 Opus 解码 1339/1339 无失败；
- 服务端 `[vad] started=0 stopped=0`、`[asr] completed=0`、`transcript=""`，
  20 s 后 `outcome=asr-timeout`。

详见 [U1 设备结果](webrtc-media-u1-device-result.md)。

## 2. 参考客户端对照（决定性实验）

用 **aiortc** 在 Mac 上对同一端点、同一 `zh_1` 音频做参考客户端
（同一 API key、同一 `session.update`、48k mono Opus、20 ms 帧）：

| 观测 | 结果 |
|---|---|
| SDP exchange | `http=200`，answer 含 `a=rtpmap:96 opus/48000/2` |
| 数据通道 | 服务端自己开 `label=txt`，随即发 `session.created` |
| `session.updated` | `server_vad thr=0.5 silence=800 create_response=true` |
| 服务端 VAD | `input_audio_buffer.speech_started` / `speech_stopped` 各 1 次 |
| 转写 | `conversation.item.input_audio_transcription.completed` =
  **"今天我们测试语音连接。"** |
| 服务端收包 | `remote-inbound-rtp.packetsReceived=589`（本端发出 591） |
| 下行 | `inbound-rtp.packetsReceived=236`，`response.audio.done` |

补充单变量实验：把 aiortc 的 **RTCP 发送完全屏蔽**后，VAD/转写仍然正常
（`packetsReceived=575/585`）。因此 **RTCP 缺失不是原因**——这一点排除了
"设备不发 RTCP SR" 的假设。

结论：**服务端、账号、端点、fixture、session 配置都没有问题**；
问题在设备发出的 RTP 没有被服务端接受。

## 3. 设备侧证据：协商的是 SHA1_32，实现的是 SHA1_80

U1 探针在握手完成后通过公开 API
`mbedtls_ssl_get_dtls_srtp_negotiation_result()` 读取协商结果，日志字段为
`profile=`（SRTP protection profile，不是 DTLS cipher）：

```
DTLS-PROBE: [connected] ... profile reads=1 skipped=0 ->
  TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384 profile=2
```

`profile=2` = `MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32`（IANA 0x0002）。

而 esp_peer 的 SRTP 策略是写死的：

```c
// managed_components/espressif__esp_peer/src/dtls_common.h
srtp_crypto_policy_set_rtp_default(&dtls_srtp->local_policy.rtp);   // = SHA1_80
```

同时它向服务端通告的 profile 列表是
`SHA1_80, SHA1_32, NULL_SHA1_80, NULL_SHA1_32`。Aliyun 网关在列表里优先选
**SHA1_32**，于是：

- 设备按 **80-bit 认证标签**加密 → 每包多 6 字节签名；
- 服务端按 **32-bit 标签**校验 → 全部失败，静默丢弃；
- 本地 `srtp_protect`/`sendto` 都成功，所以探针看不到任何错误。

密钥派生本身没有问题（两种 profile 的 master key 16 B / salt 14 B 相同），
只有认证标签长度和策略不同；这与"本地全成功、服务端全静默"完全吻合。

## 4. 修复

esp_peer 是组件管理器的产物，直接改 `managed_components/` 不可交付（重取会丢）。
修复放在**受 track 的固件源码**里，用 GNU ld 的 `--wrap` 在链接期替换
profile 列表：

- `firmware/main/hal/webrtc/dtls_short_probe.cc`：新增
  `__wrap_mbedtls_ssl_conf_dtls_srtp_protection_profiles()`，
  把传入的 4 个 profile 收窄为 `{SHA1_80, UNSET}`，并打印一次
  `SRTP offer narrowed to SHA1_80 (component offered 4 profile(s) ...)`。
- `firmware/main/CMakeLists.txt`：在 M1 的 wrap 列表加入
  `-Wl,--wrap=mbedtls_ssl_conf_dtls_srtp_protection_profiles`。

临时在 `managed_components` 里做过的验证性改动**已还原**，仓库只包含上述
tracked 源码改动。

## 5. 修复后验证（同一台设备、同一端点、同一短诊断）

```
W DTLS-PROBE: SRTP offer narrowed to SHA1_80 (component offered 4 profile(s) ...)
W DTLS-PROBE: [connected] ... profile=1
W M1: [vad] speech_started (#1)
W M1: [vad] speech_stopped (#1)
W M1: [asr] completed #1 transcript="今天我们测试语音连接。"
W M1: [short] outcome=media-completed
```

原始串口日志（已脱敏）：
`docs/device_diag_evidence/2026-10-22-u1/arm-srtp-profile-fix.log`；
验证性（改组件、未用 wrapper）的中间日志：
`arm-fix-early-run.log`。

## 6. 为什么 U1 两臂结果一样

1.5.5 与 1.5.7 用的是同一份 `dtls_srtp` 代码，SRTP profile 行为不变，
因此两版本都"媒体送达但无 VAD"。这反向印证了 U1 的结论：
**版本差异不是 M1 的阻塞点**，同时也说明之前的 U1 两臂对照必须在
修复 profile 之后重跑才有意义。

## 7. 未完成与后续

- **M1-3 未跑**：完整三 clip + 5 分钟资源检查需要关掉
  `CONFIG_STACKCHAN_WEBRTC_M1_SHORT_DIAG` 重新构建；短模式不能替代。
- **M1-1/M1-2 已可在短模式复现通过**（VAD 与转写都真实出现），但按流程
  仍以完整模式的证据为准。
- **产品路径（M2）也要修**：当前 wrapper 只在 `CONFIG_STACKCHAN_WEBRTC_M1`
  的探针构建里生效；接入产品 WebRTC 时必须一并应用，否则同样静默丢包。
- **上游建议**：给 esp_peer 报 issue / PR —— 要么按协商结果选择
  `srtp_crypto_policy_set_*`，要么只通告实际实现得了的 profile。
  本地 wrapper 是工作区修复，不改变组件本身。
- 修复后应重跑 U1 两臂（至少各一次）确认媒体层面等价，再谈 1.5.7 取舍。
