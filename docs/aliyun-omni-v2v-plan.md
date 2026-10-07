# StackChan 直连阿里云 V2V 改造计划

> 目标：StackChan 开机后**只连阿里云百炼**，用 `qwen3.8-omni-flash-realtime` 做实时语音对话 + 表情联动。
> 不给小志后端（`api.tenclass.net`）也不给 M5Stack 后端（`47.113.125.164:12800`）付任何费用，也不联系它们。

**分支**：feat/aliyun-omni-v2v（基于 MetaAlms/StackChan fork）

**2026-10-08 状态更新**：本页保留初版 WebSocket 方案。
下文“回采参考已开/AEC 无需自研/WebRTC 不可用”已被后续复核修正，以
[问题台账](firmware-findings.md)、[AEC 边界](aec-limitation.md)为准。
M0 已证 WebRTC 握手/事件互通；媒体 SPEC/PLAN 已按评审完成 D0 修订
（待 Codex 复审）。服务端 AEC 仍为**待验证假设**；Opus 的 48k RTP clock
不强制 PCM 48k，本方案采用的 48k PCM 是用户指定的实现选择。
下一步进入 WebRTC 媒体链路移植。设计（D0 修订版，**待 Codex 复审**）：
[SPEC](webrtc-media-spec.md)、[PLAN](webrtc-media-plan.md)、
[评审请求](webrtc-media-review-request.md)；上一轮
[媒体评审结果](webrtc-media-review-result.md) 保留为历史证据。
调度记录见 [webrtc-media-dispatch.md](webrtc-media-dispatch.md)。

---

## 0. 现状事实（已核实）

| 项 | 结论 | 证据 |
|---|---|---|
| 音频管线 | 已存在，直连可复用 | xiaozhi-esp32/main/audio/audio_service.cc 734 行 |
| AEC / 降噪 | 板载硬件 + AFE，无需自研 | audio/processors/afe_audio_processor.cc 201 行 |
| 麦克风 | ES7210 阵列，回采参考已开 | main/hal/board/config.h:8 `AUDIO_INPUT_REFERENCE true` |
| Opus 编解码 | 现成队列，编码 16 kHz / 60 ms | audio/audio_service.h:39 |
| 协议层 | 抽象接口，协议可插拔 | xiaozhi-esp32/main/protocols/protocol.h |
| WiFi 配网 | **BLE 由手机 App 下发**，热点仅兜底 | main/hal/hal_ble.cpp:456、wifi_board.cc:89 |
| 现有后端 | 走 OTA 换 WebSocket 地址 | application.cc:331,334、ota.cc:167 |
| 阿里端点 | 可达，Bearer 鉴权 | 握手实测 `HTTP 401 InvalidApiKey` |

### 相关文档

| 文档 | 内容 |
|---|---|
| [stackchan-hardware.md](stackchan-hardware.md) | 硬件清单、引脚、采样率链路、硬件陷阱 |
| [firmware-findings.md](firmware-findings.md) | 开发中发现的原厂缺陷与我方踩坑记录 |
| [aec-limitation.md](aec-limitation.md) | **为何不支持语音打断**：官方规格、硬件限制、影响清单与可选路径 |
| [aliyun-credentials.md](aliyun-credentials.md) | 凭据配置与刷机 |

### 阿里协议选型结论

官方 Realtime API 概述的协议矩阵中：

- **AOQ**：端侧仅 Android / iOS / HarmonyOS → 不可用
- **WebRTC**：端侧为浏览器 / 移动端，且 MCU 无可用 ICE/DTLS-SRTP 栈 → 不可用
- **WebSocket**：**全平台（任何支持 WebSocket 的环境）** → 唯一可行

端点：`wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3.8-omni-flash-realtime`

---

## 1. 架构决策

### 1.1 只换协议层，不动音频层

```
┌─────────────────────────────────────────────────┐
│ 复用（0 行改动）                                  │
│   AudioService / AFE(AEC) / Opus / ES7210+AW88298│
│   LVGL 表情 UI / 6 种 Emotion / 伺服动作          │
│   WifiManager / BLE 配网 / SsidManager(NVS)      │
├─────────────────────────────────────────────────┤
│ 新增（约 400-600 行，独立文件）                    │
│   AliyunOmniProtocol : public Protocol           │
├─────────────────────────────────────────────────┤
│ 移除/短路                                        │
│   OTA 检查（api.tenclass.net）                    │
│   M5Stack /stackChan/ws                          │
└─────────────────────────────────────────────────┘
```

### 1.2 协议不兼容是必须新增代码的唯一原因

小志固件说的是自己的方言，不能只改 URL：

- 握手：`{"type":"hello"}` + 死等 `server hello`（websocket_protocol.cc:203）
- 音频帧：自定义二进制 BinaryProtocol3（type + reserved + size(2B BE) + payload）
- 阿里：session.created / session.update / input_audio_buffer.append（base64 JSON），**无二进制帧**

M5Stack 自己也是这个模式：main/hal/hal_ws_avatar.cpp 另起一套 WebSocket 和帧格式，
没有重写音频管线。本次改造遵循同一惯例。

### 1.3 复用 Protocol 抽象不产生任何授权费用

xiaozhi-esp32 是 MIT（Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.），
README 明确 "allowing anyone to use it for free, including for commercial purposes"，
且该库**已被 StackChan 官方固件依赖**（firmware/repos.json 第 4 项）。
付费只发生在使用其托管后端时，本方案不使用。

---

## 2. 分阶段执行

### 阶段 1 — 环境准备（用户负责）

- [ ] 安装 **ESP-IDF v5.5.4**（本机现为 5.2.3，**不满足**；三个锁定组件要求 >= 5.5.2）
- [x] fork 仓库就绪，origin → git@github.com:MetaAlms/StackChan.git
- [x] 分支 feat/aliyun-omni-v2v
- [ ] cd firmware && python3 ./fetch_repos.py（重 clone 后需重跑）
- [ ] 提供阿里凭据（见第 5 节）

> 注：IDF 只是构建期要求，与运行时方案无关；不升级无法编译。

### 阶段 2 — Mac 端链路验证器（我负责，不依赖凭据）

零第三方依赖（/api-ws/v1/realtime 事件均为 JSON 文本帧，标准库即可实现）。

交付 tools/aliyun_omni_probe.py，能力：

1. 建连 + session.update（semantic_vad）
2. 上行：16 kHz / 16-bit / 单声道 PCM 分片 → input_audio_buffer.append
3. 下行：接收 response.audio.delta，落盘 24 kHz PCM 供试听
4. 验证 raw-opus2 下行与 opus / raw-opus 上行是否可用（决定固件走哪条编码路径）
5. 统计**首包延迟**、端到端延迟
6. 打印完整事件流，作为固件状态机的对照基准
7. 内置表情 tag 解析器（与固件共用同一套规则表）

**通过标准**：401 → 101，会话跑通一轮完整问答，音频可播放。

### 阶段 3 — 固件协议实现（我负责）

新增 firmware/main/hal/aliyun/aliyun_omni_protocol.{h,cc}，实现 Protocol 接口：

| 接口 | 实现要点 |
|---|---|
| Start() | 等 WifiBoard 异步就绪（wifi_board.h:52 明确 StartNetwork 立即返回），再连 wss |
| OpenAudioChannel() | 握手带 Authorization: Bearer；等 session.created |
| 会话配置 | 发 session.update，turn_detection: semantic_vad，音频格式按阶段 2 结论 |
| 上行 | PopPacketFromSendQueue() → input_audio_buffer.append（base64 / 或 raw-opus） |
| 下行 | response.audio.delta → PushPacketToDecodeQueue()（复用 Opus 解码） |
| 打断 | 服务端 input_audio_buffer.speech_started → AbortSpeaking() |
| 重连 | 指数退避，参考 hal_ws_avatar.cpp:153 的 5 s 重连模式 |

**边界处理**：
- 服务端 VAD 与本地 VAD 二选一，避免抢轮次（本地 VAD 保留给唤醒词）
- 单会话上限 120 分钟，到期主动 session.finish 后重连
- 同一会话不关闭会导致上下文累积，需周期性重建

### 阶段 4 — 表情联动（我负责）

模型不输出 M5 的 ControlAvatar 帧，只输出文本 + 音频，因此需要自建桥接：

1. 在 instructions 中约定模型输出 [happy] 这类标记
2. 从 response.audio_transcript.delta 文本流解析标记
3. 映射到 avatar::Emotion（Neutral / Happy / Angry / Sad / Doubt / Sleepy）
4. 用现成的 TimedEmotionModifier 落地（main/stackchan/modifiers/timed.h:66）
5. 文字显示时剥离标记，**语音保留**（保证音画同步，无需额外对齐）

规则表先在阶段 2 的模拟器上调试定稿，再写入固件，避免反复烧录。

### 阶段 5 — 摘除外部后端（我负责）

1. Application::CheckNewVersion() 短路：本机已配阿里凭据时直接跳过，不联网
2. 确认 AliyunOmniProtocol 不从 OTA 返回的 websocket.url / token 取配置
3. 保留 OTA 代码路径但默认不激活（便于随时回退）

**副作用（已知并接受）**：失去远程固件升级，后续刷机走 USB。
如需保留 OTA 仅用于升级、但不影响音频链路，可改为开关控制。

### 阶段 6 — 凭据注入（我负责设计）

不用明文硬编码 API Key，两种模式：

| 模式 | 用途 | 方式 |
|---|---|---|
| 快速迭代 | 开发期 | sdkconfig.defaults.local（已 gitignore，CMakeLists.txt 会自动加载叠加） |
| 交付形态 | 长期 | 存 NVS，经配网流程写入（复用 Settings 机制） |

---

## 3. 分工

| 谁 | 做什么 |
|---|---|
| **我** | 阶段 2-6 全部代码；文档；验证脚本；不碰 WiFi 配网与音频管线 |
| **你** | 提供阿里凭据；跑 Mac 模拟器确认 101；升级 IDF 并编译；烧录并在真机验证；回报日志 |

**我无法替代的部分**：真机烧录与听感验证、实际延迟测量。这些必须由你完成，我据此迭代。

---

## 4. 最终交付物

1. **可用的固件**：StackChan 开机 → 有 WiFi 直连阿里 → 语音对话 + 表情联动
2. **新协议实现**：aliyun_omni_protocol.{h,cc}（独立文件，不改现有文件）
3. **Mac 端验证器**：tools/aliyun_omni_probe.py（零依赖，可独立复现协议）
4. **表情 tag 规则表**：模型 prompt 约定 + 解析映射表
5. **文档**：docs/ 下的接线说明、凭据配置、故障排查
6. **Git 记录**：干净的分阶段 commit，推送到 fork

---

## 5. 需要你提供

1. ~~fork 仓库地址~~ 已完成：MetaAlms/StackChan
2. **百炼 API Key**（sk-...）
3. **WorkspaceId**（业务空间 ID，部分模型需专属域名）
4. 确认模型与地域已在控制台开通

---

## 6. Git 工作流（已就绪）

```
origin  git@github.com:MetaAlms/StackChan.git   ← 你的 fork，可推送
分支    feat/aliyun-omni-v2v
上游    m5stack/StackChan（只读，未配置 remote）
```

- 开发在 feat/aliyun-omni-v2v，分阶段 commit
- 推送到 origin（fork），随时备份，**重 clone 不会丢**
- 需要同步上游时再手工加 upstream remote

### 重 clone 会丢的东西

以下内容**不在 git 里**，重 clone 后必须重跑：

| 内容 | 恢复方式 |
|---|---|
| firmware/xiaozhi-esp32/、firmware/components/ | cd firmware && python3 ./fetch_repos.py |
| firmware/sdkconfig.defaults.local | 手工重建（本方案会用它存凭据） |
| 已构建的 build/ | idf.py build |

> 教训：本计划文件首次创建时未提交，已被一次重 clone 清掉。现已纳入 git。
> **本方案的任何产出都必须及时 commit。**

### 记录规范

开发过程中发现的**原厂缺陷、硬件陷阱、以及自己踩过的坑**，必须随手记进
[firmware-findings.md](firmware-findings.md)，不要只留在对话里。理由：

- 这类结论往往花了很多轮排查才得到（例如"增益改了没用"背后是量程钳位）
- 不记下来，下次会重新踩
- 部分属于上游缺陷，记录完整才好提 issue/PR

硬件相关的稳定事实（型号、引脚、采样率链路）记进
[stackchan-hardware.md](stackchan-hardware.md)。

---

## 7. 风险与应对

| 风险 | 等级 | 应对 |
|---|---|---|
| 本机 IDF 5.2.3 无法编译（需 5.5.4） | **高** | 阶段 1 先解决；无法编译则一切无从验证 |
| 无 API Key 前无法验真协议 | **高** | 阶段 2 用 sentinel key 验证到 401，其余参数待 Key 到位后确认 |
| Opus 帧长 / 格式未被账号支持 | 中 | 阶段 2 实测 opus / raw-opus / pcm 三种上行；备选退到 PCM |
| 无法真机测试 | 中 | 协议逻辑在阶段 2 的模拟器上先跑通，降低烧录次数 |
| 首次接通的延迟 / 打断体验不达预期 | 中 | semantic_vad 参数（threshold / silence_duration_ms）可调，模拟器上先调 |
| 摘掉 OTA 后失去远程升级 | 低 | 已知代价；可改成开关 |
| API Key 明文风险 | 低 | 阶段 6 的 NVS 方案；生产建议上临时凭证 |

---

## 8. 里程碑

| # | 里程碑 | 判定标准 | 依赖 |
|---|---|---|---|
| M1 | 端点与鉴权路径确认 | 401 响应（已完成） | — |
| M2 | Mac 端全链路跑通 | 一问一答 + 音频落盘 + 延迟数据 | API Key |
| M3 | 表情规则表定稿 | 模拟器上标记解析全部命中 | M2 |
| M4 | 固件编译通过 | idf.py build 无错误 | IDF 5.5.4 |
| M5 | 真机连通 | 设备连上阿里并回话 | M4 + 烧录 |
| M6 | 表情联动 + 打断体验达标 | 真机观感确认 | M5 |
