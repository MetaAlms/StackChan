# REVIEW REQUEST：M1.5 硬件/驱动审计与 AUDIT 候选（2026-10-24）

**Reviewed exact HEAD**：`c524f15fc42fc89830e41cbced80889455693cad`
（本请求文档是其后的 doc-only 提交；实现范围以该 SHA 为准。
此前的 `49fac21`、`5cb1bdd`、`dc429ff` 已被 `c524f15` 取代，后者包含两轮独立评审后的收口，见 §4.1。）
**分支/PR**：`feat/aliyun-omni-v2v` / [Draft PR #1](https://github.com/MetaAlms/StackChan/pull/1)
**本轮性质**：接续任务，非新需求；无虚构 Review ID。无设备操作。

## 1. 本轮做了什么

1. 完成官方设计/固件/库/IDF 四方对照，落盘
   [硬件/驱动审计](webrtc-media-hardware-driver-audit-2026-10-24.md)。
2. 修订 `aec-limitation.md`、`stackchan-hardware.md`、`AGENTS.md`、`spec`、`plan`、
   `findings`（B54/B55）、`redevelopment`，撤回“无回采/最多 2 通道”的绝对结论。
3. 新增**本地、不联网**的 `AUDIT <3..10>` 诊断命令与纯逻辑离线检查，作为下一轮
   区分“声学耦合 vs 其他输入来源”的最小候选。
4. **未修复声音**：没有改 AEC、通道数、gain、swap 或默认媒体路径。

## 2. 代码改动（受 track）

| 文件 | 内容 |
|---|---|
| `firmware/main/hal/webrtc/channel_audit.h`（新） | `ToneGenerator`（1 kHz 连续相位）、`ChannelAuditStats`（RMS/peak/dBFS×10） |
| `firmware/main/hal/webrtc/console_command.h` | `CommandKind::Audit`；只接受 `AUDIT <3..10>`，其余整行拒绝 |
| `firmware/main/hal/webrtc/webrtc_m15.cc` | `RunChannelAudit()`：本地 tone/silence/tone + 现有 2ch 采集；命令分发；banner；`g_audio_initialized` 时拒绝；缓冲堆分配 |
| `firmware/main/hal/board/cores3_audio_codec.h` | `AuditRead/AuditWrite` 转发到原 `Read/Write`（保留 M15 计数，不新增硬件路径） |
| `firmware/tests/channel_audit_check.cc`（新） | 26 断言：音调频率/幅度/相位连续、统计/静音/满幅/通道数 |
| `firmware/tests/console_command_check.cc` | 新增 AUDIT 合法/非法边界（总计 17153 断言） |

`AUDIT` 的三段结构：tone1 → silence → tone2（各段间 200 ms 保护，另有 200 ms 预热丢弃），
输出每通道 `db10/peak` 与 `tone1 - silence` 差；不联网、不开 AFE、不启 AEC，
不改持久化音量（tone 幅度 1024 ≈ -30 dBFS），结束关闭输入/输出；不调用 `feedTheDog()`（其实现为 10 ms 延时），I2S 阻塞调用自身让出 CPU。

## 3. 验证

| 项 | 结果 |
|---|---|
| 离线台账（原路径，epoch 13） | `channel-audit` PASS（26 断言，cache=never）；`console-command` PASS（17153 断言） |
| 预算 | **used 28 / 32**，`replan` 新增检查、未 recover/新建/清预算 |
| 编译/执行边界 | 编译在控制器外（脚本 `--build-only`），执行通过控制器；二进制在工作区外 |
| IDF v5.5.4 M15 构建 | 通过；app `0x3cd830`（`c524f15`），分区余 23% |
| WebSocket 回退构建 | 通过；app `0x3a37a0`，分区余 26%（与历史一致） |
| 生成源码补丁 | 本轮未改 `xiaozhi-esp32`/`managed_components`，无需重生成补丁 |

## 4. 需要评审确认的审计结论

1. **MIC3 参考不在 2-slot 读取范围**（按 ES7210 TDM 顺序与 2-slot 重配推断；实板线上 slot 映射仍未实测）：ES7210 因 3 麦进入 TDM；I2S RX 初始化写 4 slot，
   但 `esp_codec_dev 1.5.11` 打开 2 通道时重写 `total_slot=2 / ws_width=16 /
   slot_mask=0x3`，4-slot 初始化不生效。证据：原理图第4页、`set_drv_fs()` 代码、
   当前 `EnableInput()` 参数。
2. **AFE `"MR"` 实际把 MIC2 当参考**：`AUDIO_INPUT_REFERENCE=true` 生成 `"MR"`，
   而 2-slot 采集的 ch1 按 TDM 顺序应为 MIC2（按 ES7210 TDM 顺序应为 MIC2；实板 slot 映射未测）；这与历史“开 AEC 把用户语音一起消掉”一致。
3. **“最多 2 通道”撤回**：库只拒绝 0/奇数通道；4ch 合法，但 RX 4×16bit 与
   TX 2×16bit 共享 BCLK/WS 不同频，AW88298 也是 16-bit 立体声，故本轮不启用。
4. **#303/#308 不适用**：其 fs/2 镜像音来自 M5Unified 的 32-bit 打包；本项目走
   `esp_codec_dev` 原生 16-bit 写入，不能照搬 swap；#303 仍开，“电流声”未解释。
5. 因此双向循环不能归因“参考串入”；当前最简约解释仍是声学耦合 + 无设备 AEC +
   服务端 AEC 未证。**未定位根因，未修复。**

## 4.1 独立评审后的修正（`dc429ff`）

独立只读评审（2026-10-24）对 `5cb1bdd` 给出 **CHANGES REQUESTED**：1 BLOCKER + 4 MAJOR。
`dc429ff` 逐项修复：

1. **BLOCKER**：`Hal::feedTheDog()` 实际是 `vTaskDelay(1)`（100 Hz 下 10 ms），
   每个 20 ms 时隙被拉长且不喂 TWDT。已从 AUDIT 循环移除；I2S 读写本身阻塞让出 CPU，
   idle-task TWDT 仍被喂。
2. **音量**：`AudioCodec::SetOutputVolume()` 会写 NVS，且 `output_volume()` 的 RAM 值
   不是持久化值。AUDIT 现在**完全不改音量**，tone 幅度降为 1024（约 -30 dBFS）。
3. **DMA/保护段**：保护段 100 ms → **200 ms**（双向 DMA 约 60 ms×2），并在测量前
   预热丢弃 200 ms 源音频。
4. **阻塞上界**：`seconds + 2` 软时限（每次阻塞调用前检查）与 10 次失败中止，
   覆盖预热与测量两段；`aborted` 单独上报，未测得任何相位时不再打印空统计。
5. **文档限定**：所有“MIC3 从未被读”均改为“按 TDM 顺序不在 2-slot 读取范围，
   **实板线上 slot 映射仍未实测**”，消除与“未证”章节的矛盾。

`channel_audit.h` / `console_command.h` 未变，epoch 13 的两项离线检查仍适用；
IDF 重新构建通过（app `0x3cd830`）。复审记录见
[独立评审结果](webrtc-media-audit-review-result.md)。

第二轮复审（无 BLOCKER，2 MAJOR）后的收口（`c524f15`）：

1. **预热段也受时限/失败约束**：`seconds + 2` 的检查与 10 次失败中止覆盖
   drain 预热的 10 个 slot（每次阻塞调用前检查；单次 I2S 可阻塞 1 s，属软上界）。
2. **未测得相位时不打印空统计**：drain 阶段中止时输出
   `AUDIT aborted before measurement`，不再给出 -1200/ frames=0 的误导行。
3. **文档残留清理**：删除“100 ms 保护/结束恢复音量/循环内喂看门狗”等过期描述；
   `ch1=MIC2` 统一加“按 TDM 顺序；实板 slot 映射未测”限定。

## 5. 未证 / 风险

- 实板 revision、ES7210 TDM 实际帧长与 slot 对齐、AEC_P/N 幅度、MIC3 增益：
  仍须示波器/逻辑分析仪，不能只凭原理图。
- `AUDIT` 未经真机运行：它会在 Idle 直接开关 codec 输入/输出；若 AudioService
  保留了某些状态，可能暴露新的顺序问题（命令有 active-run 拒绝与看门狗保护）。
- 服务端 AEC 是否存在/生效未证；M1.5 整体、T2–T4、产品 M2 仍未通过。

## 6. 下一轮需要用户配合的测试（每轮新弹窗 Q，用户点“开始”才执行）

1. `AUDIT 6`：记录 tone/silence 的 ch0/ch1 dB 与峰值，量化声学耦合。
2. `START <id> duplex 30`（同一句请求）：
   a. 正常；
   b. 用户用纸/手遮住 **MIC1/MIC2 出声孔**（不遮扬声器）。
   若遮麦后 `turn_detected` 消失 ⇒ 声学路径；仍出现 ⇒ 非声学输入，再评估
   4ch/电气路径或服务端因素。
3. 4ch 映射/参考实验**不在本轮授权内**：需先对齐 TX/RX 帧宽与功放格式（或仅 RX
   静默采集），并给出代码与数据布局依据。

## 7. 明确未做（边界）

- 不启 AEC、不改通道/gain/swap、不恢复半双工门控、不与 M2/T3/T4 合并。
- 不 merge/auto-merge/发布/提上游 issue；不输出凭据、SDP 或转写正文。
- 设备保持 HOLD：未打开串口、未刷机、未 START。
