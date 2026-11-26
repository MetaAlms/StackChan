# U1-4 版本取舍结论（2026-10-22）

> **结论：采纳 esp_peer 1.5.7 作为 WebRTC 路径基线**（当前 pin 已是 1.5.7，无需改仓库）。
> 依据：修复 SRTP profile 后，两臂在相同源码/参数/网络下行为完全一致，1.5.7
> 无回归且包含上游 issue208 相关修复；M1 完整验收也是在 1.5.7 上取得的。

## 本次对照条件

| 项 | 值 |
|---|---|
| 源码 | 当前工作树 + [SRTP profile 修复](webrtc-media-m1-srtp-profile-rootcause.md)（wrapper 对两臂同时生效） |
| 配置 | `CONFIG_STACKCHAN_WEBRTC_M1=y`、`SHORT_DIAG=y`、20 ms、2 s 静音 + zh_1 + 800 ms tail、ASR 等待 20 s |
| 构建 | 两臂各自 `idf.py reconfigure && build`；锁差异仅 peer `component_hash`/`version` 与 `manifest_hash` 三行 |
| 运行 | 每臂刷 app 后跑一次短诊断；日志已脱敏 |

## 对照结果

| 观测 | arm-a 1.5.5 | arm-b 1.5.7 |
|---|---|---|
| DTLS 握手 | 首握成功，608 ms | 首握成功，595 ms |
| 协商 SRTP profile | **1**（SHA1_80） | **1**（SHA1_80） |
| VAD | started=1 / stopped=1 | started=1 / stopped=1 |
| ASR | `completed=1`，转写"今天我们测试语音连接。" | 同左 |
| Opus 包 / SRTP | 336 / 336 ok | 336 / 336 ok |
| 短诊断结果 | `outcome=media-completed` | `outcome=media-completed` |

两臂唯一的可观察差异是 DTLS 首握耗时（13 ms），不构成行为差异。
此前"两臂都无 VAD"的对照因 SRTP profile 缺陷而失效，不能用于版本取舍。

原始日志：
`docs/device_diag_evidence/2026-10-22-u1/arm-a-1.5.5-fixed.log`、
`arm-b-1.5.7-fixed.log`。

## 决定与理由

1. **采用 1.5.7**：同拓扑、同源码、同参数下无回归；且它是唯一取得 M1 完整
   `VERDICT: PASS` 的版本（见 [M1 验收结果](webrtc-media-m1-acceptance-result.md)）。
2. **保留 1.5.5 可回退**：仓库仍保留 1.5.5 的锁/归档历史与 findings B30 的
   最小锁恢复方法；若 1.5.7 在 M1.5/M2 出现回归，可切回并用本次日志对照。
3. **不要求重新归档**：本轮两臂构建用于行为对照，构建产物已被后续构建覆盖；
   旧归档（1.5.5/1.5.7）保留为历史证据，标注为"无 SRTP 修复，不可直接用于
  媒体验收"。以后需要可复现归档时，按 U1 任务书重跑 archiver。
4. SRTP 修复与 peer 版本无关，采用 1.5.7 后仍需保留该 wrapper。

## 覆盖边界

- 每臂**各一次**短诊断；不是统计意义上的等价性证明（DTLS 抖动、
  长时稳定性属 M3）。
- 只覆盖 uplink 短诊断；下行、AEC、产品接入未涉及。
- 本轮没有重新生成两臂归档，也没有重跑云端 host 对照。

## 后续

- M1 已 PASS（自测，待评审）→ 按 PLAN 进入 M1.5（AEC 假设验证）前置。
- 把 SRTP profile 修复移植到产品 WebRTC 路径（M2）。
- 给 esp_peer 报上游 issue：offer 的 profile 列表与实际安装的 SRTP policy 不一致。
