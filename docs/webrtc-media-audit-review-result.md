# 独立评审结果：M1.5 硬件/驱动审计与 AUDIT 候选（2026-10-24）

> **最终结论：APPROVE**（实现 HEAD `97864efefd0c8627da1c1d93e39b5f393c23f01e`；
> 其后 `ab036f9` 仅更新本评审请求文档）。
> 评审范围仅限本轮“审计 + 本地 AUDIT 候选”；**不表示声音/回声已修复**，
> 也不放行 M1.5/T2–T4/M2。设备侧仍须新的刷写/测试授权与弹窗 Q。

## 评审方式

由独立的只读评审会话完成（不修改工作区、不刷机、不开串口、不启动设备）。
每轮给出 `[BLOCKER]/[MAJOR]/[MINOR]` 分级与明确 verdict；修复后复审。

| 轮次 | 目标 HEAD | 结论 | 主要问题 |
|---|---|---|---|
| R1 | `5cb1bdd` | CHANGES REQUESTED | 1 BLOCKER + 4 MAJOR |
| R2 | `dc429ff` | CHANGES REQUESTED | 无 BLOCKER；drain 未纳入时限、文档残留 |
| R3 | `c524f15` | CHANGES REQUESTED | 无 code BLOCKER；100ms 残留、超时口径、空统计 |
| Final | `97864ef` | **APPROVE** | 仅余一条注释口径（见“残余”） |

## R1 问题与修复

| # | 问题 | 修复 |
|---|---|---|
| B1 | `Hal::feedTheDog()` 实为 `vTaskDelay(1)`（100 Hz 下 10 ms），会把每个 20 ms 时隙拉长且不喂 TWDT | 从 AUDIT 循环移除；I2S 阻塞调用自身让出 CPU，idle-task TWDT 仍被喂 |
| M1 | `SetOutputVolume()` 会写 NVS，且 RAM 中的 `output_volume()` 不是持久化值 | AUDIT **完全不动音量**；tone 幅度降为 1024（≈ -30 dBFS） |
| M2 | 100 ms 保护段短于约 120 ms 的 DMA 往返 | 保护段 200 ms + 测量前预热丢弃 200 ms |
| M3 | I2S 超时下失败仅计数，可能长时间占用控制台 | `seconds + 2` 软时限 + 10 次失败中止 + `aborted` 上报（软上界见下） |
| M4 | 文档“MIC3 从未被读”与“slot 映射未测”自相矛盾 | 所有相关文档统一加“按 TDM 顺序；实板线上 slot 映射未测”限定 |

## R2/R3 追加修复

- drain 预热的 10 个 slot 也纳入时限与失败计数（否则 `seconds + 2` 不是墙钟上界）。
- 未测得任何相位时输出 `AUDIT aborted before measurement`，不打印 `-1200 / frames=0` 的误导统计。
- 逐相位按样本门控；`tone1`/`silence` 不完整时不输出 `echo_delta`。
- 清理过期文档：“100 ms 保护”“结束恢复音量”“循环内喂看门狗”“-18 dBFS/音量 40”；
  更正超时口径：`DEFAULT_WAIT_TIMEOUT = 1000` ticks，100 Hz 下约 10 s
  （`esp_codec_dev/platform/audio_codec_data_i2s.c:25,713,739`）。
- `ch1=MIC2` 全部改为 TDM 推断并注明实板 slot 未测。

## 验证证据

| 项 | 结果 |
|---|---|
| `channel_audit_check` | PASS，26 断言（cache=never，控制器执行） |
| `console_command_check` | PASS，17153 断言（含 AUDIT 合法/非法边界） |
| 离线台账 | epoch 13，**used 28/32**，held 0；无 recover/新建/清预算 |
| IDF v5.5.4 M15 构建 | 通过，app `0x3cd8f0`，分区余 23% |
| WebSocket 回退构建 | 通过，app `0x3a37a0`，余 26%（本轮前构建，M15-only 代码不影响） |
| 工作树/边界 | 干净；无设备操作；无 AEC/通道/gain/swap 改动；无凭据/SDP/转写泄漏 |

## 残余（不阻塞本轮）

- `firmware/main/hal/webrtc/webrtc_m15.cc:671-672` 注释仍写 “up to 1 s”/“before every
  blocking call”，与已更正的 10 s 口径不一致；**仅注释**，文档已正确。下一次代码改动
  顺手修正，不单独为此改变已评审的 exact HEAD。
- AUDIT 的实测 DMA 延迟、实板 TDM slot 映射、AEC_P/N 幅度与 MIC3 增益仍未验证；
  `AUDIT` 本身尚未上真机。

## 结论与下一步

- **APPROVE** 本轮审计与本地 AUDIT 候选；不改变 M1.5/T2–T4/M2 的未通过状态。
- 下一轮需用户明确授权：
  1. 刷入该候选（刷写授权独立于测试授权）；
  2. 新弹窗 Q 后运行 `AUDIT <3..10>`；
  3. 同一请求做 `duplex 30` 正常 vs 遮住 MIC1/MIC2 出声孔的对照。
- 未授权前不打开串口、不刷机、不 START。
