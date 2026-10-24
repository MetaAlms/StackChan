# 硬件/驱动对照审计与最小诊断候选（2026-10-24）

> **状态：审计完成，根因未定位，本轮不修复声音/回声。**
> 已确认一个真实的驱动/设计不一致：**ES7210 的 MIC3（扬声器参考）不在当前 2-slot 读取范围**
> （按 TDM 顺序与 2-slot 重配推断；实板线上 slot 映射仍未实测）；
> AFE 被标成 "MR"，但第二通道实际是 MIC2。4 通道 TDM 在库层可行，但本轮未启用。
> 本轮只新增一个**本地、离线可验证的通道审计命令** `AUDIT`，不联网、不启 AEC；
> 设备侧保持 HOLD，实际测试须下一轮新弹窗 Q 授权。

## 1. 审计对象与坐标

| 项 | 值 |
|---|---|
| 官方页面 | [StackChan Core C156](https://docs.m5stack.com/en/core/StackChan_Core)（SKU C156） |
| 页面所链原理图 | [Sch_M5_CoreS3_v1.0.pdf](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/490/Sch_M5_CoreS3_v1.0.pdf)，7 页，**第 4 页 = 音频页** |
| 官方固件 | [m5stack/StackChan @ eff0d6b5](https://github.com/m5stack/StackChan/commit/eff0d6b5109663faec8f1d98848b4b0e5246bbfc) |
| 本项目实现 | `firmware/main/hal/board/cores3_audio_codec.cc`，交接前设备固件 `4d5fa0b` |
| SDK | 本机 ESP-IDF **v5.5.4**；`esp_codec_dev` 解析为 **1.5.11**（`firmware/dependencies.lock`） |
| 方法 | `pdftotext -layout/-raw` 逐页提取 + 页面渲染核对；官方源码 raw 下载逐行对照 |

## 2. 原理图事实（第 4 页）

- U8 **AW88298**（I2C 0x36）：`BCK15=I2S_BCK`、`DATAI16=I2S_DATO`、`WCK17=I2S_WCK`，
  **`DATAO19` 为 NC（红叉）**；`VON1→FB1→SPK_VON`、`VOP2→FB2→SPK_VOP`。
- U9 **ES7210**（I2C 0x40）：`SCLK9=I2S_BCK`、`LRCK10=I2S_WCK`、
  `SDOUT1/TDMOUT11=I2S_DATI`、`SDOUT2/TDMIN12=NC`、`DMIC_CLK14=NC`。
- **参考通路**：`SPK_VOP/SPK_VON` 分别经 `150K/1% + 22pF + 1uF` 网络形成
  `AEC_P`/`AEC_N`，接到 **ES7210 `MIC3P`(31) / `MIC3N`(32)**；`MIC4P/N`(27/28) 短接到 GND。
- 全 7 页**没有** MAX98357 或其他功放，也没有扬声器连接器；`SPK_VOP/VON` 以网络标签离开本页。
- 第 3 页：`GPIO0` 同时是 `ESP_BOOT` 与 `I2S_MCLK`（经 R34 51Ω 到 ES7210 MCLK5）。
  这是“下载模式/复位异常”时需要记住的共享引脚事实，不是音频结论。

## 3. 官方固件 vs 本项目驱动

官方 `core3_audio_codec.cc` 与本项目同源；逐项对照：

| 项 | 官方 eff0d6b5 | 本项目当前 | 结论 |
|---|---|---|---|
| ES7210 选择 | `MIC1|MIC2|MIC3` | 同 | 3 麦 ⇒ [es7210.c](https://github.com/espressif/esp-adf-libs) 判 TDM 模式（`mic_num>=3`），写 `REG12=0x02` |
| I2S RX 初始化 | TDM，`slot_mask=SLOT0..3`，`total_slot=AUTO` | 同 | **初始化是死配置**，见 §4 |
| 输入 channel | `2`，mask 0 +（reference 时）1 | 强制 `2`，mask 0+1 | 等效；本项目另修了 60→37.5 dB 钳位、两通道增益 |
| AFE 输入格式 | `input_reference=true` ⇒ **"MR"** | 同 | **标签与数据不一致**，见 §4 |
| 输出 | 16-bit，24k，mono（`EnableOutput` channel=1） | 同 | lib 把 mono 归一成 2ch、只选 ch0 |
| 功放 | AW88298，16-bit 路径不写位宽寄存器 | 同 | 未证实际格式/失真 |
| 独立麦克风测试 | `hal/audio.cpp` 读 ch1 当 "reference" | — | 与接线（MIC3）不一致的同一假设 |

## 4. 两个硬发现（可直接复核）

### F1. 4-slot TDM 初始化从未生效；按 TDM 顺序 MIC3 不在 2-slot 读取范围（按 ES7210 TDM 顺序与 2-slot 重配推断；实板线上 slot 映射仍未实测）

`esp_codec_dev 1.5.11` 的 `platform/audio_codec_data_i2s.c`：

- `set_drv_fs()` TDM 分支：`slot_cfg.total_slot = fs->channel`，
  `slot_cfg.ws_width = slot_bits * fs->channel / 2`，`slot_mask = fs->channel_mask`；
- `_i2s_data_set_fmt()` 会把 `channel==1` 归一为 2 并设 mask=ch0；
- 因此 `EnableInput()` 以 `channel=2, mask=0x3` 打开后，RX 被**重配成 2 slot**，
  初始化里的 `SLOT0..3` 不生效。

结果：**当前上行只有 MIC1/MIC2 两个 slot；按 ES7210 的 TDM 顺序，MIC3（AEC_P/N 参考）不在读取范围**（按 ES7210 TDM 顺序与 2-slot 重配推断；实板线上 slot 映射仍未实测）；
而 AFE 仍按 `input_reference=true` 生成 "MR"，把 **ch1 当作回声参考**——实际 ch1 是 MIC2。
这也解释了历史现象：开启设备 AEC 时“把用户声音一起消掉”（peak 4036→120），
因为 AFE 拿 MIC2 当参考，而不是扬声器参考。

### F2. “最多 2 通道”应撤回；4 通道 TDM 在库层可行

`_i2s_valid_fmt()` 只拒绝 **0 和奇数**通道（`(fs->channel >> 1 << 1) != fs->channel`），
且 mono 在到达它之前已被归一成 2ch；**4ch 是合法的**，TDM 会按 `total_slot=4` 配置。
历史上 `Not support channel 3` 是库对 **3ch** 的拒绝，不是硬件或 4ch 的上限。

但 4ch 不能直接开：I2S0 是共享 BCLK/WS 的全双工口，RX 4 slot ×16bit（64 bit/帧）与
TX 2 slot ×16bit（32 bit/帧）不同频；AW88298 当前也是 16-bit 立体声格式。
**“4ch + 同时播放”需要先解决帧宽/功放格式，属于下一轮的有依据改造，本轮不做。**

## 5. 与 #303/#308 的关系

[M5Unified #303](https://github.com/m5stack/M5Unified/issues/303) 的“HW v2 样本对顺序”
是 M5Unified `Speaker_Class.cpp` 把两个 16-bit 样本打包进一个 32-bit DMA 字、
且旧样本在高半字造成的 **fs/2 镜像音**（24k 下 440Hz → ~11.6kHz），
由 [#308](https://github.com/m5stack/M5Unified/pull/308) 在 0.2.20 修复；
它是 **I2S 外设代际**问题，不是板卡 revision。

本项目走 `esp_codec_dev` 原生 16-bit 写入，没有该 32-bit 打包路径，
**不能照搬 swap**；#303 本身仍开着（即使用 raw IDF I2S + 最小音量仍有失真报告），
所以“电流声”仍不能归因于该已知修复。

## 6. 对双向循环的含义（不夸大）

- 已证：三个响应均被新 turn 取消、`create_response/interrupt_response=true`、
  客户端无主动 cancel（[B52](firmware-findings.md)）。
- 现证：**电气参考（MIC3）不在上行数据里**，所以不能把循环归因于“参考通道串入”；
  最简约解释仍是**扬声器 → MIC1/MIC2 的声学耦合**，叠加“设备无 AEC、服务端 AEC 未证”。
- 未证：实板 revision/走线、ES7210 TDM 实际帧长与 slot 对齐（需示波器/逻辑分析仪）、
  AEC_P/N 幅度、MIC3 增益、“电流声”来源、服务端 AEC。
- 因此本轮**不启 AEC、不改 gain/swap/通道**。

## 7. 本轮最小诊断候选：`AUDIT`

新增 USB 命令（仅 M15 诊断固件，默认 Idle 不自动执行）：

```
AUDIT <3..10>
```

行为：本地播放 1 kHz 正弦（幅度 1024 ≈ -30 dBFS，**不改持久化音量**），
同时用**现有 2 通道路径**采集；三段：tone1 → silence → tone2（各段间 100ms 保护），
逐通道输出 RMS（dBFS×10）与 peak，并给出 `tone1 - silence` 的 dB 差。

- 不联网、不开 AFE、不启 AEC、不改默认媒体路径；结束关闭输入/输出。
- 保护段 200 ms、预热丢弃 200 ms 源音频；总时限 `seconds + 2`，失败超过 10 次即中止。
- **只在首次媒体运行前可用**：`AudioService::Initialize()` 会启动 15 s 空闲电源定时器，
  它可能在审计中途关闭 codec；因此 `g_audio_initialized` 为真时直接拒绝并提示重启。
- 缓冲区堆分配（主任务栈 8 KB，不把约 2 KB 局部数组压在栈上），循环内喂看门狗。
- 意义：量化“扬声器输出有多少进了当前上行”。若差值很大，说明当前无 AEC 的声学回路
  足以解释 turn_detected；若很小，则循环输入来自别处（噪声/环境/服务端）。
- 它**不能**单独区分声学与电气：参考通道不在 2ch 流里。区分需要下一轮的
  “遮挡麦克风物理对照”或 4ch 映射实验（设计见 §8）。

实现与验证：

- 纯逻辑 `firmware/main/hal/webrtc/channel_audit.h`（ToneGenerator / 统计 / dBFS）。
- 命令解析 `console_command.h`（`AUDIT` 仅接受 3..10，其它整行拒绝）。
- 采集/播放复用 `CoreS3AudioCodec` 的 `Read/Write` 与 M15 计数，不新增硬件路径。
- 离线台账：新增 `channel-audit`（26 断言，cache=never），
  `console-command` 复跑（17153 断言）；epoch 13，used 28/32，无 recover/新库。
- IDF 构建见 [审计评审请求](webrtc-media-audit-review-request.md)。

## 8. 下一轮设备实验设计（须新 Q 点击开始）

1. `AUDIT 6`：记录 tone/silence 的 ch0/ch1 dB 与峰值——量化声学耦合。
2. `START <id> duplex 30`，同一句请求，**分别**做两次：正常；用户用纸/手**遮住
   MIC1/MIC2 出声孔**（不遮挡扬声器）。若遮麦后 turn_detected 消失 ⇒ 声学；
   若仍出现 ⇒ 非声学输入（电气/噪声/服务端），再进 4ch 或示波器。
3. 如需确认参考通路：4ch TDM 实验必须先把 TX 帧宽与功放格式对齐（或仅 RX、
   禁 TX 的静默采集），并明确不是本轮授权范围。

## 9. 明确未做 / 未下结论

- 未改声音、未启 AEC、未改通道数、未加 swap；不声称“电流声”已修复。
- 未读实板 revision，未做波形/scope；官方 PDF 名为 CoreS3_v1.0，不等于实板版本。
- 未做设备侧运行；设备保持 HOLD（不打开串口、不刷机、不 START）。
- M1.5 整体、T2–T4、产品 M2 仍未通过；本审计只缩小了输入来源的可疑范围。
