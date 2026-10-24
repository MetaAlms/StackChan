# REVIEW REQUEST：M1.5 硬件/驱动审计与 AUDIT 候选（2026-10-24）

**Reviewed exact HEAD**：`5cb1bdd541bce8ea3ab5fe80c3fddac2cb378f42`
（本请求文档是其后的 doc-only 提交；实现范围以该 SHA 为准。
前一个实现提交 `49fac21` 已被 `5cb1bdd` 取代：后者只加“首次媒体运行前才允许 AUDIT”
守卫与堆缓冲，见下。）
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

`AUDIT` 的三段结构：tone1 → silence → tone2（各段间 100 ms 保护），
输出每通道 `db10/peak` 与 `tone1 - silence` 差；不联网、不开 AFE、不启 AEC，
结束恢复输出音量并关闭输入/输出；循环内喂看门狗。

## 3. 验证

| 项 | 结果 |
|---|---|
| 离线台账（原路径，epoch 13） | `channel-audit` PASS（26 断言，cache=never）；`console-command` PASS（17153 断言） |
| 预算 | **used 28 / 32**，`replan` 新增检查、未 recover/新建/清预算 |
| 编译/执行边界 | 编译在控制器外（脚本 `--build-only`），执行通过控制器；二进制在工作区外 |
| IDF v5.5.4 M15 构建 | 通过；app `0x3cd690`（`5cb1bdd`），分区余 23% |
| WebSocket 回退构建 | 通过；app `0x3a37a0`，分区余 26%（与历史一致） |
| 生成源码补丁 | 本轮未改 `xiaozhi-esp32`/`managed_components`，无需重生成补丁 |

## 4. 需要评审确认的审计结论

1. **MIC3 参考从未进入上行**：ES7210 因 3 麦进入 TDM；I2S RX 初始化写 4 slot，
   但 `esp_codec_dev 1.5.11` 打开 2 通道时重写 `total_slot=2 / ws_width=16 /
   slot_mask=0x3`，4-slot 初始化不生效。证据：原理图第4页、`set_drv_fs()` 代码、
   当前 `EnableInput()` 参数。
2. **AFE `"MR"` 实际把 MIC2 当参考**：`AUDIO_INPUT_REFERENCE=true` 生成 `"MR"`，
   而 2-slot 采集的 ch1 是 MIC2；这与历史“开 AEC 把用户语音一起消掉”一致。
3. **“最多 2 通道”撤回**：库只拒绝 0/奇数通道；4ch 合法，但 RX 4×16bit 与
   TX 2×16bit 共享 BCLK/WS 不同频，AW88298 也是 16-bit 立体声，故本轮不启用。
4. **#303/#308 不适用**：其 fs/2 镜像音来自 M5Unified 的 32-bit 打包；本项目走
   `esp_codec_dev` 原生 16-bit 写入，不能照搬 swap；#303 仍开，“电流声”未解释。
5. 因此双向循环不能归因“参考串入”；当前最简约解释仍是声学耦合 + 无设备 AEC +
   服务端 AEC 未证。**未定位根因，未修复。**

## 4.1 `5cb1bdd` 相对 `49fac21` 的修正

- **电源定时器竞态**：`AudioService::Initialize()` 启动 15 s 空闲电源定时器，可能在
  ≤10 s 的本地审计中途关闭 codec。现在只有在首次媒体运行前（`g_audio_initialized`
  为假）才允许 AUDIT，否则拒绝并提示重启。
- **栈占用**：主任务栈 8 KB，AUDIT 的 2 个 480 样本缓冲改为堆分配；循环内继续喂看门狗。
- 这两处不改变 `channel_audit.h` / `console_command.h`，因此 epoch 13 的两项离线检查
  结果仍适用于同一纯逻辑；IDF 重新构建通过（`0x3cd690`）。

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
