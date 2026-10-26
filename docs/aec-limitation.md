# 回声消除（AEC）不可用：规格限制与影响

本文说明为什么本方案**无法支持语音打断（barge-in）**，以及这个限制带来的全部影响。

**2026-10-26 更正（重要）**：ES7210 的 TDM-I²S 顺序是 **1,3,2,4**（手册 Figure 2e），
当前 2-slot 采集实际是 **MIC1 + MIC3（扬声器参考）**；AFE 的 `"MR"` 是正确配置。
本文此前“参考未被读 / 第 2 通道是 MIC2”的结论撤回，见
[TDM 参考更正](webrtc-media-tdm-reference-correction-2026-10-26.md)。

**2026-10-24 硬件前提纠正**：历史“无扬声器回采”和“驱动最多2通道”不能继续作为硬件/SDK绝对限制。
官方 C156 原理图第4页确有 `SPK→AEC_P/N→ES7210 MIC3` 参考通路；
但当前 2 通道采集被 `esp_codec_dev` 重配成 2 个 TDM slot，**MIC3 不在读取范围**（按 ES7210 TDM 顺序与 2-slot 重配推断；实板线上 slot 映射仍未实测），
AFE 的 `"MR"` 实际把 MIC2 当参考。4ch 在库层可行，但受共享 I2S 帧宽/功放格式约束，
未经实板验证不能直接启 AEC 或改通道。详见
[硬件/驱动审计](webrtc-media-hardware-driver-audit-2026-10-24.md)。

**2026-10-23 边界更新**：本文已验证的规格限制针对WebSocket/半双工实现。
第2节“WebRTC不可用、只有WebSocket”是历史评估；M0已验证握手/事件，媒体D0设计已复核，
4e53790的near03按既定SPEC §6.2人工复核T1单项通过。duplex04已实际双向收发/提交播放，
但用户一次请求后安静时反复“从前+电流声”，本轮播放质量/行为未通过；
正式T2缺完整控制、AEC未定，不能唯一归因声学回声或服务器缺陷，也不能由“WebRTC内置AEC”推定可用。
T3/T4、M1.5整体及产品语音打断仍未通过，见[重新开发记录](webrtc-media-redevelopment-2026-10-23.md)。
2026-10-24的4d5fa0b候选05准备超时，06同进程显式重开运行30.002秒，三次响应均记录cancelled/turn_detected。
这证实新turn取消机制，不定位声学/电气/噪声根因；用户确认读后安静仍重复“从前”、3次后无声音，
但停止与30秒窗口对齐不明，三次cancelled+30.002秒run_timeout不证明AI正常结束。正式T2未验，已暂停新测试核对硬件/官方实现。
AFE等待错误日志本轮0，不证明声音/回声已修复或长期稳定；两轮均结束Idle，串口已关。
下方强结论仅适用于所述WebSocket/硬件参考路径，不扩大为WebRTC已证不可能。

**结论：这不是代码缺陷，是阿里云 Realtime API 的协议规格加上本硬件驱动的双重限制的必然结果。**
下面每一环都有官方文档或实测日志支撑。

---

## 1. 官方规格：WebSocket 协议不含回声消除

来源：[Realtime API 概述 - 阿里云百炼](https://help.aliyun.com/zh/model-studio/realtime-api-overview)

### 协议选型对比表（原文）

| 维度 | AOQ | WebRTC | WebSocket |
|---|---|---|---|
| 适用场景 | AI 多模态实时交互、弱网场景、混合数据传输 | 浏览器端互动、传统音视频通话 | 服务端集成、快速原型验证 |
| 浏览器兼容性 | 不支持 | 原生支持 | 原生支持 |
| 接入难度 | 低 | 中等 | 极低 |
| **弱网对抗** | 极致 | 良好 | **差** |
| 数据类型 | 音视频 + 文本 | 音视频 + 文本 | 文本/音频/图像 |
| 建连速度 | 快 | 慢 | 慢 |
| **回声消除/降噪** | **内置** | **内置** | **无，需客户端自行处理** |
| AI 场景适配 | 原生为 AI 多模态数据特征深度定制 | 传统设计，AI 场景需额外适配 | 基础，适合纯文本或低实时性场景 |
| **端侧平台支持** | **Android / iOS / HarmonyOS** | 浏览器、移动端 | **全平台**（任何支持 WebSocket 的环境） |
| Token Plan | 支持 | 支持 | 支持 |

### 原文补充说明

> - **AOQ 方案**：适合对延迟、弱网对抗、多模态数据传输有极致要求的 AI 实时交互场景，
>   同时**内置回声消除和降噪能力**，尤其是移动端原生应用。
> - **WebRTC 方案**：适合需要浏览器原生支持、已有 WebRTC 基础设施的传统音视频通话场景，
>   **内置回声消除和降噪能力**。
> - **WebSocket 方案**：适合服务端集成、快速原型验证、对接入门槛要求极低的场景。

**关键一行：WebSocket 的"回声消除/降噪"为「无，需客户端自行处理」。**

---

## 2. 为什么不能用带 AEC 的那两种协议

```
ESP32-S3 能用哪种？
├─ AOQ        端侧平台 = Android / iOS / HarmonyOS   → ❌ ESP32 不在支持列表
├─ WebRTC     端侧平台 = 浏览器、移动端               → ❌ 需要浏览器/媒体栈
└─ WebSocket  端侧平台 = 全平台                       → ✅ 唯一可用
```

**WebSocket 是 ESP32-S3 唯一可用的协议，而它恰好是三种里唯一不含 AEC 的。**

替代路径的代价：

| 方案 | 可行性 |
|---|---|
| 在 ESP32 上跑 WebRTC | 需移植完整媒体栈（ICE/DTLS/SRTP/RTP + 抖动缓冲），工程量远超本方案本身，且 MCU 资源紧张 |
| 用 AOQ | 仅移动端原生 SDK，ESP32 无法接入 |
| 手机 App 做 AOQ/WebRTC 中继 | 技术上可行，但引入中间层，与"设备直连阿里、不经中间服务器"的初衷冲突 |

---

## 3. 设备侧自己做 AEC 为什么也不行

设备侧 AEC 需要**扬声器回采参考通道**——把功放的输出信号送回 ADC，供算法对齐消除。

### 3.1 参考通道很可能存在

```c
// firmware/main/hal/board/cores3_audio_codec.cc
es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
```

官方硬件描述是"双麦克风 + ES7210 ADC"，但 ES7210 配了 **3 路输入**。
多出的第 3 路很可能就是回采参考——这正是"2 麦 + 1 参考"的经典 AEC 布局，
也是启用第三路的原因。

设为 3 通道后，**AFE 确实按 `MMR` 建起了带 AEC 的处理链**：

```
AFE Pipeline: [input] -> |AEC(SR_HIGH_PERF)| -> |SE(BSS)| -> |VAD(WebRTC)| -> |WakeNet| -> [output]
```

说明 **AFE 侧完全支持"双麦 + 参考"**。

### 3.2 参考其实就在 2 通道里（2026-10-26 更正）

ES7210 在 TDM-I²S 模式下的通道顺序是 `1,3,2,4`（手册第 6 页 Figure 2e），
而当前 2-slot 读取取的是前两个 slot，也就是 **MIC1 + MIC3（AEC_P/N 参考）**。
`AUDIO_INPUT_REFERENCE=true` 生成的 `"MR"` 因此是**正确**的：ch0 麦克风、ch1 参考。

设备实测也一致：封住正面两个麦克风针孔后，ch1 的 1 kHz 耦合几乎不变
（-19.8 → -18.4 dBFS），而底噪下降；这正是电气参考的特征（见 B56/B57）。

### 3.3 历史“开 AEC 吞语音”需要重评

旧实验记录“AEC 开启后 AFE 峰值 4036→120”曾被解释为“第二通道是麦克风”。
在确认 ch1 是参考之后，这个解释不成立；更可能是双讲/延迟/参考增益或 AEC 模式问题。
不能用旧结论证明“硬件没有参考”或“设备 AEC 不可行”。

### 3.4 要突破需要做什么

参考已在 2 通道里，**不需要 4ch 才能做设备 AEC**；需要的是正确启用和验证 AEC：

- 保持 `"MR"` 与 2-slot 采集（MIC1+MIC3），启用 `CONFIG_USE_DEVICE_AEC`；
- 先用本地已知音验证 AEC 输出中的回声下降；
- 再做双讲（同时说话+播放）确认不吞用户语音、不产生半双工门控；
- 若双讲异常，调 AEC 模式/延迟/参考增益，而不是回退半双工；
- 4ch/帧宽改动只在需要读 MIC2/MIC4 时才考虑，不再作为 AEC 前置。

---

## 4. 影响清单

### 4.1 直接失去的能力

| 能力 | 状态 | 说明 |
|---|---|---|
| **语音打断（barge-in）** | ❌ **不可用** | 设备说话时无法接受新的语音指令 |
| 全双工对话 | ❌ 不可用 | 只能半双工轮流说 |
| 设备侧回声消除 | ❌ 不可用 | 参考通道读不出来 |

### 4.2 受影响的交互行为

**为什么打断必然失效**——xiaozhi 的状态机在说话时会关闭整个上行采集：

```cpp
case kDeviceStateSpeaking:
    if (listening_mode_ != kListeningModeRealtime) {
        audio_service_.EnableVoiceProcessing(false);   // 上行处理整体关闭
    }
```

而监听模式由 AEC 是否可用来决定：

```cpp
ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}
```

**AEC 不可用 ⟹ `kListeningModeAutoStop` ⟹ 说话时 AFE 停止采集 ⟹
用户在设备说话期间说的话根本没进缓冲区，也就无从"延后提交"。**

> 对比：原厂固件配小志后端时，**AEC 由小志服务端完成**，因此走的是
> `kListeningModeRealtime`，打断正常。这是本方案与"原厂体验"最本质的差异。

### 4.3 不受影响的能力（已实测验证）

| 能力 | 状态 |
|---|---|
| 设备直连阿里 Realtime | ✅ |
| 语音识别（ASR） | ✅ 实测准确转写用户原话 |
| 语音合成播放 | ✅ |
| 表情联动（emoji 驱动） | ✅ |
| 长回答连续播放 | ✅ 已用背压解决服务端 3~5 倍速推送 |

### 4.4 附带的相关限制

**WebSocket 的"弱网对抗 = 差"**（同一张官方表格）。实测后果是服务端以
**2.9 ~ 5.4 倍速**突发推送音频：

```
[rate] 3556 packets = 213360 ms audio arrived over 63213 ms wall (3.38x realtime)
```

已通过**背压**解决（队列满时阻塞读取任务，让 TCP 窗口关闭以限速服务端），
细节见 [firmware-findings.md](firmware-findings.md) B10。

---

## 4.5 补充：WebRTC 路径的 AEC 是**待验证假设**

上文结论针对 **WebSocket 协议**（官方规格明确"无，需客户端自行处理"）。

换用 WebRTC 后，官方选型表把回声消除/降噪标为"内置"，但
**这不能推出裸 esp_peer/RTP 会自动获得服务端回声控制**：

- [RFC 7874 §5](https://www.rfc-editor.org/rfc/rfc7874.html#section-5) 建议**端点设备**实现 AEC
- [W3C echoCancellation](https://www.w3.org/TR/mediacapture-streams/#dom-mediatrackconstraintset-echocancellation)
  是**采集侧**约束
- 阿里 WebRTC 示例使用浏览器 `getUserMedia`（自带采集侧 3A）；
  AOQ 自定义采集把 3A 配置放在**客户端 SDK**

这些证据也**不能反证**服务端没有远端回声处理。
**故 WebRTC 路径的迁移收益按待验证假设处理**，
验收写成可观测行为，由 M1.5 判定（见
[webrtc-media-spec.md](webrtc-media-spec.md) §1.3 / §6.2）。

## 5. 可选路径

| 方案 | 效果 | 代价 | 风险 |
|---|---|---|---|
| **A. 接受现状（半双工）** | 对话、识别、表情、连续播放均正常；无打断 | 无 | 无 |
| **B. 打开 `kListeningModeRealtime`** | 说话时仍采集，打断的话能进缓冲区 | 改动小 | 会把设备自身声音一并录入，模型可能误把自己的话当用户输入 |
| **C. 绕开 `esp_codec_dev` 直驱 I2S TDM** | 拿到第 3 路参考 → 真 AEC → 真打断 | 需自写 I2S 接收/通道拆分/重采样 | 工作量大，不保证一次成功 |
| **D. 换用 AOQ/WebRTC 中继** | 官方内置 AEC，体验对齐原厂 | 需手机 App 或服务端做中继 | 引入中间层，违背"直连"初衷 |

**当前实现采用方案 A。**

---

## 6. 复现与验证方法

| 验证项 | 方法 | 预期 |
|---|---|---|
| 官方规格 | 打开 [Realtime API 概述](https://help.aliyun.com/zh/model-studio/realtime-api-overview)，看协议对比表 | WebSocket 行：回声消除"无，需客户端自行处理" |
| 3 通道被拒 | 把 `input_channels_` 设为 3 编译烧录 | 日志出现 `E I2S_IF: Not support channel 3`，`peak=0` |
| AEC 消掉语音 | 2 通道 + `CONFIG_USE_DEVICE_AEC=y` | AFE 输出峰值从 ~4036 塌到 ~120 |
| 半双工行为 | 设备说话时对它说话 | 无响应（音频未进缓冲区，非识别失败） |

---

## 7. 相关文档

| 文档 | 内容 |
|---|---|
| [aliyun-omni-v2v-plan.md](aliyun-omni-v2v-plan.md) | 总体方案与阶段计划 |
| [stackchan-hardware.md](stackchan-hardware.md) | 硬件清单、引脚、采样率链路 |
| [firmware-findings.md](firmware-findings.md) | 全部问题记录（B12/B13 为本文详述项） |
