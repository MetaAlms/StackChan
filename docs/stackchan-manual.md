# StackChan 开发手册

本手册是 StackChan（M5Stack C156 / CoreS3）固件与实时语音开发的**总入口**：
先读这里，再按链接进入细节。只写**已确认事实**和**明确标注的待验证项**；
证据不足的推测不写成结论。

- **板级/电源/音频配置对照（官方值 vs 本仓库）**：[stackchan-config-manual.md](stackchan-config-manual.md)
- 硬件细节：[stackchan-hardware.md](stackchan-hardware.md)
- 逐条发现与踩坑：[firmware-findings.md](firmware-findings.md)
- 方案与阶段：[aliyun-omni-v2v-plan.md](aliyun-omni-v2v-plan.md)
- 重新开发记录：[webrtc-media-redevelopment-2026-10-23.md](webrtc-media-redevelopment-2026-10-23.md)
- AEC 边界：[aec-limitation.md](aec-limitation.md)
- 设备证据：`docs/device_diag_evidence/`

> 状态（2026-10-27）：M1.5 双向验证中。M1 上行已 PASS；设备 AEC 已启用，
> near 已能实时；duplex 已首次出声约 12 秒，但随后被 CPU 饱和 + I2C 超时 abort。
> 已对照官方 M5Unified，发现 PMIC 漏配 **ALDO1=1.8 V（AW88298）/
> ALDO2=3.3 V（ES7210）**，并把 P0.2 误当复位；`0x3ce400` 已按官方值修正。
> 低复杂度 AEC 的 60s 稳态仍无有效样本（此前 `lowcost02/03/04` 准备超时）。
> 语音打断尚未验收，M2 继续 HOLD；`0x3ce400` 的播放/链路验证待做。
>
> **2026-11-02 更新**：`0x3ce460` 的低复杂度 AEC 仍无有效样本，但卡点已从
> “I2C/功放”换成 **DTLS 握手不再成功**：同一固件下 10:16 那轮还能
> `Client handshake success → CONNECTED (7)`，之后每轮只有 `WANT_READ`、
> 停在 `stage=4`，而 SDP/ICE 与成功轮逐行一致。判为服务端媒体面会话未建立
> （最可能是异常断开后残留会话未释放），见 findings **B66**。
> 固件目前没有任何会话 teardown，只有 `POST` SDP。
>
> **2026-11-03 更新**：DTLS 僵局已自愈（服务端换到 `8.141.115.69`），
> 首次 `self 30` **完整跑通**：`state=Done`、`decoded=1364`、播放 26.8 s、
> 功放 I2C 零失败、`max_abs=19212` 未削波（B68）。剩余唯一卡点：
> **AEC feed 25 ms / 16 ms 块（156% 实时）**，丢 16.5 s 麦克风样本。
> 已加 prepare 阶段有界重试（最多 3 次，新 peer + 新 SDP，退避 2 s）。
>
> **2026-11-04 定版**：下行可用——Wi-Fi 省电关闭、`VOLUME 100` + 2× PCM 增益、
> I2C 写失败只重试（**禁止复总线**）、准备超时 10 s + 命令层重试、RUNNING 需数据通道已开。
> 用户验收“音量足够、无破音”。剩余：**上行 AEC 欠载 25 ms/16 ms**、DTLS 握手 2–34 s 波动。
> 完整可用配置与踩坑清单见 findings **B72**。

---

## 1. 设备与硬件

### 1.1 组成

| 部件 | 型号 | 备注 |
|---|---|---|
| 主控 | ESP32-S3 | 双核 LX7 @240 MHz，16 MB Flash / 8 MB PSRAM |
| 显示 | ILI9342C | 320×240 SPI |
| 触摸 | FT6336U | 与音频器件共用 I2C |
| 摄像头 | GC0308 | DVP |
| 麦克风 ADC | ES7210 | 双麦 + MIC3 回采，I2C 0x40 |
| 扬声器功放 | AW88298 | 1 W，I2S，I2C 7 位地址 0x36（日志写 0x6c）；I/O 由 AXP2101 **ALDO1=1.8 V** 供电，AW9523 **P0.2 为 enable（高有效）** |
| 电源管理 | AXP2101 | I2C 0x34 |
| GPIO 扩展 | AW9523B | I2C 0x58；P0.2 用于 AW88298 复位 |
| 机身 | 2× SCS0009 舵机、PY32L020、Si12T、IR/NFC、550 mAh 电池 | body 本身无麦克风/扬声器 |

### 1.2 音频链路（当前实现）

```
ES7210 采集 24 kHz，TDM 2 slot = MIC1 + MIC3(AEC 参考)
  → I2S RX 24 kHz
  → AudioService 重采样 24k → 16k
  → AFE（AEC 开启，"MR"）输出 16 kHz 单声道
  → Opus 16 kHz / 60 ms 上行（WebRTC，48k RTP 时钟）
下行：
WebRTC Opus 48 kHz → 解码 48k → 重采样 24k → I2S TX 24k → AW88298
```

**确认的 TDM 顺序**：ES7210 TDM-I²S 为 **1,3,2,4**（数据手册 Figure 2e），
所以当前 2-slot 读取是 **MIC1 + MIC3（扬声器回采）**，不是 MIC1+MIC2。
AFE 的 `"MR"` 是正确的；历史上“MIC3 未读”的结论已撤回。
详见 findings B57 / [TDM 更正](webrtc-media-tdm-reference-correction-2026-10-26.md)。

**物理位置**：双麦在正面相机右侧的两个针孔；1 W 扬声器在侧面开槽面板；
机身内部没有声学器件。遮脸会同时改变声场，不能作为单变量声学对照。

### 1.3 引脚

来自 `firmware/main/hal/board/config.h`：

| 信号 | GPIO |
|---|---|
| I2S MCLK | 0 |
| I2S WS | 33 |
| I2S BCLK | 34 |
| I2S DIN（麦克风） | 14 |
| I2S DOUT（扬声器） | 13 |
| I2C SDA | 12 |
| I2C SCL | 11 |

采样率：输入/输出均 24 kHz；ES7210 增益上限 **37.5 dB**，超过会被静默钳位。
AW88298 的复位由 AW9523 P0.2 控制；固件在启动时补了一个复位脉冲
（`ResetAw88298()`），但**不能**把它当作所有 I2C 故障的修复。

---

## 2. 固件、构建与仓库

### 2.1 工具链

- ESP-IDF **v5.5.4**：`~/esp/esp-idf-5.5`，`source ~/esp/esp-idf-5.5/export.sh`
- 串口：`/dev/cu.usbmodem*`，115200
- 分支：`feat/aliyun-omni-v2v`；不直接改 `main`

```bash
source ~/esp/esp-idf-5.5/export.sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

新增源文件后必须 `idf.py reconfigure`（`GLOB_RECURSE` 只在配置期展开）。

### 2.2 目录与补丁（重要）

- `firmware/main/`：本仓库代码，正常提交。
- `firmware/xiaozhi-esp32/`、`firmware/components/`：`fetch_repos.py` 的产物，
  **被 `.gitignore` 忽略**。直接改这里的源码不会被 git 记录，重新 fetch 会丢失。
- 对 xiaozhi 的改动必须同步到 **`firmware/patches/`** 下的补丁。当前相关：
  - `stackchan-webrtc-afe-lifecycle.patch`：AFE 生命周期、异步 feed、AEC 任务模型。
  - `stackchan-webrtc-media.patch`、`stackchan-aliyun.patch`、`xiaozhi-esp32.patch`。
- 修改后重新生成并用 pristine worktree 验证：

```bash
cd firmware/xiaozhi-esp32
git diff -- main/audio/processors/afe_audio_processor.h \
             main/audio/processors/afe_audio_processor.cc \
  > ../patches/stackchan-webrtc-afe-lifecycle.patch
git apply --check --reverse ../patches/stackchan-webrtc-afe-lifecycle.patch
```

- 生效的 Kconfig 是 **`firmware/main/Kconfig.projbuild`**；
  `firmware/xiaozhi-esp32/main/Kconfig.projbuild` 从不被读取。
- 凭据只放在 gitignored 的 `firmware/sdkconfig.defaults.local`，禁止打印/提交。

---

## 3. M15 受控诊断固件

### 3.1 行为

- 开机默认 **Idle**，不采集、不联网、不播放；屏幕可能仍显示旧文案。
- 通过 **USB Serial/JTAG** 控制台接收命令，115200。
- 每次 `START` 使用**新的 run ID**；结束后回 Idle，通常可重开（除非 cleanup 失败）。

### 3.2 命令

| 命令 | 说明 |
|---|---|
| `START <id> near <5..180>` | 只采集/上行，不开扬声器 |
| `START <id> duplex <5..180>` | 采集 + 播放；需要用户说话 |
| `START <id> self <5..180>` | 自播：RUNNING 后固件注入一条文本用户轮并请求回答；用户不用说话 |
| `STOP <id>` | 停止匹配的 run |
| `STATUS` | 查看 state / run / 是否需要重启 |
| `AUDIT <3..10>` | 本地扬声器 tone + 2 通道采集，不联网；需 boot 后未初始化 AudioService |
| `VOLUME <0..100>` | 设置并持久化输出音量，立即生效（`effective=100` 表示已拉满） |

`self` 的协议细节：`conversation.item.create`（`input_text`）→ `response.create`。
Qwen 的 `response.create` 只定义 `type`/`event_id`，逐条 `instructions` 不在字段表内。
详见 M15 自播设计。

### 3.3 关键日志字段

- `RUNNING ... mode=near|duplex|self ... t0_us=...`
- `SELFPLAY ... item_sent=1 request_sent=1 stream=...`
- `LIVE`：每 5 秒的轻量计数
- `FINAL ... state=Done|Failed reason=run_timeout|prepare_timeout|...`
- `MEDIA`：sent / decoded / mic_full / rx_full / failures
- `TIMING`：send_calls / send_avg_us / send_max_us / loop_max_us / mic_empty
- `PLAYBACK`：accepted / full / submitted_samples / submit_us
- `PCM`：decoded48 / converted24 / accepted24 / submitted24 / **max_abs**
- `EVIDENCE`：ASR items、speech、responses、self_play、AEC=UNDETERMINED
- `AFE feed`：chunks / avg_us / max_us / failures / dropped_samples / **max_backlog_ms**
- `LIFECYCLE`：capture_quiescent / transport_joined / reusable

### 3.4 测试纪律

1. 每个实际测试先发 Q，用户明确“开始”后才 `START`；旧授权不复用。
2. 准备阶段**不要**让用户说话；看到真实 `RUNNING` 后再提示。
3. 一次只改一个变量；异常循环立即 `STOP`，确认 Idle/停音。
4. 不用关 AEC / 降 VAD / 半双工门控来掩盖双向失败。
5. 每轮记录 exact HEAD、构建配置、app 大小、日志路径与结论。
6. 原始串口日志先脱敏（SSID/BSSID/MAC/IP）再入 `docs/device_diag_evidence/`。

---

## 4. 已确认结论（按证据）

### 4.1 链路与协议

- M1 上行探针 **PASS**：3 个 fixture 片段 + 318.1 s 循环，真实 48k PCM 上行；
  `esp_peer` 已采用 **1.5.7**。
- SRTP 服务端只接受 SHA1_80；固件通过 `--wrap` 收窄 offer，修复已保留。
- WebSocket 回退保留；当前实时语音走 WebRTC。
- 自播文本轮可用：`responses=1`、`self_play=1`、`speech=0`（单轮，不证明 AEC 有效）。

### 4.2 AEC 与实时性

- `CONFIG_USE_DEVICE_AEC=y` 时，AEC 的 `afe_feed → aec_process → ipnlms/FFT`
  原本同步跑在 `audio_input`（prio 8 / core0），会饿死媒体发送（prio 5）和
  播放输出（prio 4）。证据：WDT IDLE0 + backtrace + 31 包/秒（B60）。
- 把 feed 移到 **core1 / prio 3 / 8 KB 栈** 的 `audio_communication`，每轮喂满
  一个 fetch 块（fetch/feed = 512/256 = 2 个 chunk）后再 fetch：
  - near 30 s：**49.2 包/秒**、`mic_full=17`、无 WDT；
  - `AFE feed avg=14.4 ms`（16 ms 音频）、`max=17.2 ms`、`max_backlog=50 ms`、
    `dropped=0`（B61）。
- 这说明 **capture/发送调度修复有效**；但 AEC 单块耗时已接近实时上限，
  播放时仍可能占满 core1（B63）。
- **不能**据此说 AEC 已有效：双讲、回声抑制、打断都未验收。

### 4.3 播放 / “滋啦”

- 故事确实播出过：`decoded=614`（约 12.3 s）、用户听到“阿明”的故事（B63）。
- 早期的“滋啦”最可能是 **AEC 占满 CPU 导致输出/I2S 供数断续**；
  `PCM max_abs=32643` 接近满幅是次要叠加因素（可能削波），但**未做降音量对照**。
- 目前不能把“滋啦”归因于喇叭损坏、网络丢包或编解码器。

### 4.4 I2C / abort 链

- 播放期间出现 `I2C dev 6c`（AW88298）与 PMIC 0x34 / FT6336 超时；
  `ESP_ERROR_CHECK` 把一次 I2C 超时放大成 abort/reboot（B63）。
- 这不是“找不到喇叭”：同一轮故事已经播放；`write_ok` 也曾为 2766/0 失败。
- 已改：PMIC 电源状态读失败只记录、`EnableOutput` 打开失败不 abort
  （`0x3ce460`，待 60 s 验证）。
- **已确认的配置差异**：官方 M5Unified 把 **ALDO1=1.8 V（AW88298）**、
  **ALDO2=3.3 V（ES7210）** 写进 PMIC 初始化；我们的 `Pmic` 漏了 0x92/0x93，
  且把 P0.2 当复位脉冲。`0x3ce400` 已按官方值修正，待验证；这比“接线错”
  更符合间歇 NACK 的表现。

---

## 5. 当前阻塞与未决项

1. **低复杂度 AEC 验证**：`0x3ce460` 首测因功放 I2C 锁死未播放
   （`AFE feed avg=19.8ms` 仅 1.1s 启动样本，不能公平比较）；冷启动后两次
   `prepare_timeout`，未 RUNNING。需等链路稳定后重跑 60s，再看 `sent`、
   `AFE feed avg`、WDT/I2C；不能据这两轮否定 low-cost。
2. **可打断（barge-in）**：用户明确要求；尚未做双讲。需先确认 AEC 有效
   （TTS 播放时用户插话，旧播放停止、上行不被自己回声打断）。
3. **回声是否触发 VAD**：自播一轮 `speech=0` 是好信号，但单轮不能证明。
4. **音量/削波**：输出音量未记录；`max_abs` 近满幅，需一次降音量对照。
5. **M2 产品化**：连续采集、本地 stop/epoch、UI/表情、重连均未开始。

---

## 6. 故障排查

| 现象 | 优先检查 | 处理 |
|---|---|---|
| 串口无输出 / `Device not configured` | USB CDC 休眠/单次打开限制 | 物理复位/重插；一次只开一个 host |
| `prepare_timeout stage=4 config=0` | DTLS connected 但 ICE/DC 未完成 | 视为链路偶发；重试一次，不判 AEC |
| `I2C dev 6c` / `0x34` 超时 | CPU 饱和、总线锁死、供电 | 先看 WDT/backtrace；不要直接判喇叭坏 |
| 播放中“滋啦” | CPU 饥饿/underrun、PCM 近满幅 | 先看 `AFE feed`/WDT；再降音量对照 |
| 故事播放后无声 | run 窗口结束、abort、输出未开 | 看 `FINAL`/`PLAYBACK`/`LIFECYCLE`；区分正常停止与崩溃 |
| WDT IDLE0/IDLE1 | 高优先级任务占满核 | 找 backtrace；不要只加延时掩盖 |

| 串口静默但 `ioreg` 仍能看到设备 | 一次 boot 只允许一次 host 打开（B70） | 不要再 open/close；一个句柄跑到底，或物理重插 |
| `ESP_ERROR_CHECK ... I2cDevice::WriteReg` | 有人调过 `i2c_master_bus_reset()`（B72） | 禁止复位总线；功放打开失败只重试 |
| `RUNNING` 出现但 `sent=0` | START 在启动过程中发出，或传输未连上（B71） | 等 `IDLE` 横幅再发命令；RUNNING 已加传输门禁 |

**USB CDC 已知坑**：设备一次 boot 往往只接受第一个 host 打开；后续可能读写无响应，
需物理 RST/重插。测试脚本应先等 `IDLE; USB commands` 横幅，再发 `STATUS`，并容忍首条命令丢失。
**正确做法**：一次测试会话只开一个串口句柄；要刷机时用同一句柄脉冲 RTS 复位，让“关→开”
发生在一次全新 boot 之后（B70/B72）。

---

## 7. 证据索引（2026-10-24 M15）

`docs/device_diag_evidence/2026-10-24-m15/`：

- `audit-06-*.log`：通道审计（tone/遮孔）
- `aec-near-02.log`、`aec-near-03-second-run.log`：AEC near 对照
- `aec-duplex-04-prepare-timeout.log`、`aec-duplex-06*.log`：duplex 慢速/取消
- `aec-duplex-batch-base1..3.log`：无语音批量
- `selfplay-01.log`、`selfplay-02.log`：自播首测（31 包/秒）
- `selfplay-05..08-*.log`：AEC 任务模型后的 I2C abort 链
- `selfplay-09/10-*.log`：冷启动/重测；10 首次出声 12 s
- `near-01-aecfix.log`：AEC 任务修复后 49.2 包/秒
- `selfplay-11-lowcost.log`：低复杂度 AEC 60 s（**待生成/待验证**）

逐条编号见 [firmware-findings.md](firmware-findings.md)：B57（TDM 更正）、
B58（AEC 首测无效）、B59（SendAudio 插桩）、B60（AEC 饿死 CPU0）、
B61（core1 修复 near 实时）、B62（早期 I2C abort 记录）、B63（首次出声与 PMIC abort）。

---

## 8. 开发边界

- M1.5 未验收前不进入 M2；不把 near 的 49/s 当成双向通过。
- 不盲目开/关 AEC、改通道、加 swap、降 VAD；每次只改一个有证据的变量。
- 不 merge、不 auto-merge、不发布、不提上游 issue（除非用户明确要求）。
- 不泄露凭据、SDP、转写正文；日志先脱敏。
- 每个设备测试单独 Q；准备/刷写授权与测试授权分开。
- 发现必须落盘：硬件/驱动进 `stackchan-hardware.md`，坑与缺陷进
  `firmware-findings.md`，方案/阶段进 `aliyun-omni-v2v-plan.md`，本手册只做总览。

---

## 9. 下一步清单

- [ ] 跑 `0x3ce460` 的 60 s self：确认 `sent≈50/s`、`AFE feed avg` 下降、无 WDT/I2C abort。
- [ ] 若仍 I2C 超时：查总线/供电；保留非 abort 日志，不盲目换硬件。
- [ ] 一次降音量对照，排除 PCM 近满幅削波。
- [ ] AEC 双讲：用户正常说话，确认旧 TTS 停止、回声不触发 VAD。
- [ ] 通过后冻结 M1.5 前置，再开 M2（连续采集/本地 stop/UI/重连）。
