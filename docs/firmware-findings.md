# 固件开发中发现的问题记录

开发 StackChan 直连阿里云 V2V 过程中实际踩到并定位的问题。**每条都附证据与当前状态**，
便于回馈上游或日后复查。

分两类：
- **A. 原厂/上游缺陷** —— 与本项目改动无关，原厂固件就存在，值得提 issue/PR
- **B. 我方踩的坑** —— 开发过程中自己引入或误判的，记录以免重复

---

## A. 原厂/上游缺陷

### A1. `USE_DEVICE_AEC` 的板子列表遗漏 StackChan，导致该选项不可见

**现象**：`CONFIG_USE_DEVICE_AEC` 在 StackChan 上无法启用；板子两端都没有回声消除，
设备会被自己的喇叭声打断。

**证据**（`firmware/main/Kconfig.projbuild:824`，StackChan 自带的副本）：

```kconfig
config USE_DEVICE_AEC
    bool "Enable Device-Side AEC"
    default n
    depends on USE_AUDIO_PROCESSOR && (BOARD_TYPE_ESP_BOX_3 || ... || BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_4_3C)
                                                     ↑ 26 个板子，没有 BOARD_TYPE_M5STACK_STACK_CHAN
```

`depends on` 不满足时 Kconfig 符号**不会出现在 sdkconfig 里**，所以：

```
$ grep CONFIG_USE_DEVICE_AEC sdkconfig
（无输出）
$ grep CONFIG_USE_SERVER_AEC sdkconfig
# CONFIG_USE_SERVER_AEC is not set      ← 两个都没开
```

**注意**：真正生效的是 **`firmware/main/Kconfig.projbuild`**，不是
`firmware/xiaozhi-esp32/main/Kconfig.projbuild`。后者虽然存在，但因为 xiaozhi 源码是
被编译进 `main` 组件的，它**从不被读取**——改它不会有任何效果（我在这里浪费了一轮排查）。

**状态**：已把 `BOARD_TYPE_M5STACK_STACK_CHAN` 加入依赖列表（本仓库改动）。
但**开启后反而更糟**，见 A2。

### A2. 开启设备侧 AEC 会消掉用户语音（因为"参考通道"其实是第二只麦克风）

**2026-10-24 纠正**：下述电平下降仍为历史观测，但“硬件没有扬声器回采”的推断撤回。
官方C156页面链接原理图存在MIC3喇叭参考通路；当前所选两通道没有取得有效参考，不等于硬件不存在。
实板revision、slot与参考波形待核实，见B53和[DSH交接](webrtc-media-dsh-handoff-2026-10-24.md)。

**现象**：开启 `CONFIG_USE_DEVICE_AEC=y` 后，AFE 输出电平骤降 20dB，完全听不见人声。

**证据**：

```
AFE 输出峰值：  关闭 AEC 时 1022~4036   →   开启 AEC 后 98~136
```

**根因**：

```c
// cores3_audio_codec.cc
es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
input_channels_ = input_reference_ ? 2 : 1;   // 2 通道
```

官方硬件描述是 **"Dual microphones with ES7210"**——第 2 通道是第二只麦克风。
`AUDIO_INPUT_REFERENCE true` 让 AFE 按 `"MR"`（1 麦 + 1 参考）处理，于是 AEC 拿
"另一只麦克风听到的同一段人声"当回声去抵消。

**结论**：这套硬件**没有扬声器回采通路**，设备侧 AEC 无法工作。
真正要修需要硬件层面把 AW88298 的播放信号接入 ES7210 的某一路输入。
**当前用半双工规避**（设备说话时不上行音频）。

### A3. `input_gain_ = 60` 超出 ES7210 量程，被静默钳位

**现象**：把增益从 60 改到 85，音频电平**毫无变化**。

**证据**（`es7210.c:328`）：

```c
static es7210_gain_value_t get_db(float db)
{
    db += 0.5;
    if (db < 33)   { int idx = db < 3 ? 0 : db / 3; return GAIN_0DB + idx; }
    if (db < 34.5) return GAIN_30DB;
    if (db < 36)   return GAIN_34_5DB;
    if (db < 37)   return GAIN_36DB;
    return GAIN_37_5DB;        // ← 硬件上限 37.5 dB
}
```

| 代码值 | 实际生效 |
|---|---|
| xiaozhi 官方 CoreS3 `30` | 30 dB |
| StackChan 副本 `60` | **37.5 dB（钳位）** |
| 试过的 `85` | **37.5 dB（同样钳位）** |

另外 API 参数单位是 **dB**，不是 0.5dB 步进：
`esp_codec_dev_set_in_channel_gain(dev, mask, float db_value)`。

**状态**：已改为明确写 `37.5f`（本仓库），避免后续误判。

### A4. 通道掩码与 `input_reference` 耦合

**证据**（`cores3_audio_codec.cc` 原代码）：

```c
esp_codec_dev_sample_info_t fs = {
    .channel = 2,
    .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),   // 只启用通道 0
};
if (input_reference_) {
    fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);  // 通道 1 只在"有参考"时启用
}
```

声明 2 通道却只启用 1 个。把 `input_reference` 设为 false 就会**静默关掉第二只麦克风**。

**状态**：已改为按实际通道数循环设置（本仓库）。功能上与原厂 `reference=true` 等价，
但不再与参考标志耦合。

### A5. 冷启动时 AXP2101 I2C 读取失败导致 `abort()` 重启

**现象**：冷启动必崩一次并重启，日志：

```
I (828) M5Stack-StackChan-Board: Init AXP2101
ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
  I2cDevice::ReadReg            i2c_device.cc:29
  Pmic::Pmic                    stackchan.cc:97
  M5StackCoreS3Board::InitializeAxp2101 / create_board
  Hal::init                     hal.cpp:35
  app_main                      main.cpp:24
abort() was called
→ Rebooting...
```

**根因**：PMIC 上电后未就绪，`I2cDevice::ReadReg` 里的 `ESP_ERROR_CHECK` 直接 abort。

**状态**：已在 `i2c_device.cc` 的 `ReadReg`/`ReadRegs` 加 5 次重试（本仓库）。
属于上游 `xiaozhi-esp32` 代码，值得提 PR。

### A6. `firmware/.gitignore` 未忽略 `sdkconfig.defaults.local`

**风险**：该文件用于存放 API Key，但 `.gitignore` 只有 `sdkconfig` 和 `sdkconfig.old`，
**这两个模式都不匹配 `sdkconfig.defaults.local`**——而 `CMakeLists.txt` 的注释却声称
"已 gitignore"（`sdkconfig.defaults.local` 是 git-ignored）。一次 `git add -A`
就会把密钥提交上去。

**状态**：已补 `sdkconfig.defaults.local` 与 `sdkconfig.local`（本仓库）。

### A7. 跳过 OTA 后固件版本号为空

`Ota::current_version_` 只在 `Ota::CheckVersion()` 里赋值。跳过版本检查时该值为空，
而 `HandleActivationDoneEvent()` 无条件读取它显示 "Version"。

**状态**：已改为构造时就填好（本仓库）。属于上游代码。

### A8. esp_peer v1.5.6 拒绝 host 候选对的正常 NAT 映射

**现象**：WebRTC M0 向阿里端点成功交换 SDP 后，重复报
`XOR-MAPPED ... is not local candidate, skip nominate`，最终 CONNECT_FAILED。
即使 Offer 已携带与 XOR-MAPPED 相同的 srflx，也无法完成该路径。

**证据**（2026-10-07 独立复核，详见 [webrtc-m0-review-result.md](webrtc-m0-review-result.md)）：

- 实际 ESP32-S3 `libpeer_default.a` 与官方提交
  `38a697f3b8142d23823eda5209edb89ca76119dc` 中库文件 SHA-256 相同：
  `6b2f3856b9a1639b0480c011688c070c132b6399e9863a74d20d20a271dc7da3`。
- 反汇编及 DWARF 证实 `agent_pair_candidate`（原源 `agent.c:642`）
  跳过类型不同的候选，local srflx 不与 remote host 成对。
  第二轮比较确认该筛选在 **1.5.5 已存在**；且此组合也可能按标准换 base 后
  合法去重，不能单凭未保留 srflx/host 检查对证明本次根因。
- `agent_bind_mapped_matches_local`（原源 `agent.c:1417`）在 controlling、
  非 relay、mapped port 非零时，只比较 XOR-MAPPED 与当前 pair.local 地址，
  不查找已有 srflx。`agent_process_stun_response:1621–1629` 失败时直接返回。
- 本次 host 为 `192.168.1.7:59813`，映射为 `124.126.137.141:13174`，
  上述条件必然拒绝；未执行 RFC 8445 §7.2.5.3.1/2 要求的映射 valid-pair 处理。

**历史版本对照（2026-10-07，同设备/网络/端点，非严格单变量 A/B）**：

| 版本 | 结果 |
|---|---|
| 1.5.6 | `skip nominate` 刷屏 → `CONNECT_FAILED` |
| **1.5.5** | `Select pair` → `Connection OK` → PAIRED → CONNECTED → DATA_CHANNEL_OPENED → **`session.created` + `session.updated` 均收到** |

**v1.5.6 新增了上述错误响应处理。** 两版库比较确认 1.5.5 没有 mapped-address
拒绝分支；当前 1.5.5 全程不出现该警告，且完成信令与传输握手。
历史对照同时修改 M0 并改变另外 21 个依赖版本，不能称为只切组件版本的实验。

**严格单变量 A/B 已补做（2026-10-07，第二轮复核之后）**：

| Arm | 依赖集合 | esp_peer | 库 SHA-256（前 16） | 结果 |
|---|---|---|---|---|
| A | **原始集合** | 1.5.6 | `6b2f3856b9a1639b` | `skip nominate` 刷屏 → `CONNECT_FAILED` |
| B | **同一原始集合** | 1.5.5 | `25a338fc6b05f702` | `Connection OK` → **`session.created` + `session.updated`** |

两臂同一份 M0 代码、同一 ESP-IDF/sdkconfig、同一 STUN 配置与网络；
锁文件**仅 esp_peer 的 `component_hash` 与 `version` 两行不同**，
构建后已复核未被重解析。

**方法教训（重要）**：切版本时**不要删除整个 `dependencies.lock`**——
那会触发整体重解析，连带改动其他 21 个组件（含 Wi-Fi）。
正确做法是直接替换锁文件中目标组件的条目，使清单与锁一致；
构建后必须复核锁文件差异，确认只动了目标组件。

**结论：是 v1.5.6 引入的回归，干扰变量已排除。**

附带实证：服务端事件的通道是 `txt`、stream 为 **1**，
不是客户端创建的 `oai-events` / stream 0。

**状态**：已在 `firmware/main/idf_component.yml` 精确锁定 1.5.5
（非 `^` 范围），并注明解除条件。

**已向既有 issue 补充证据，未另建同症状 issue**：
[issue #208](https://github.com/espressif/esp-webrtc-solution/issues/208)
（2026-09-30，ESP32-S3 + LiveKit Cloud，仍 open）
→ [issuecomment-6041819735](https://github.com/espressif/esp-webrtc-solution/issues/208#issuecomment-6041819735)
（2026-10-07，账号 MetaAlms，评论数 3 → 4）。

补充内容为本项目独有、其余报告未提供的三项：

1. **二进制层根因**——两版发布库反汇编对比，确认
   `agent_bind_mapped_matches_local` 为 1.5.6 新增
   （1.5.5 库中该符号与 `skip nominate` 字符串均为 0 次），
   而 `agent_pair_candidate` 的类型筛选 1.5.5 已存在。
   现有报告均为行为层推断，无代码路径。
2. **严格单变量 A/B**——并写明"删除整个 `dependencies.lock` 会重解析
   约 21 个无关组件而使对照失效"这一方法陷阱。
3. **第三个独立端点**——阿里云 Realtime WebRTC，非 LiveKit、无 TURN，
   且为**端口改写型 NAT**（59813 → 13174），区别于原报告的端口保持型。

另提出与已有评论的**冲突点**并请其澄清：zigit 称 1.5.2 亦失败故非回归，
而我们受控实测 1.5.5 成功，回归窗口应为 1.5.5 → 1.5.6
（其 1.5.2 或为 `third_party` vendored 版本）。

并给出修复方向（依 RFC 8445 §7.2.5.3.1/2：拒绝前先查本地候选表复用，
或按 `mapped_addr` 建 prflx 并提名，须保留既有 STUN 校验），
说明无法提 PR 的原因（`agent.c`/`ice.c` 不在公开仓库，
仅存在于 7 个预编译 `libpeer_default.a` 中），
以及愿意在 ESP32-S3 上实测修复库。

草稿与发布记录见 [upstream-issue-208-comment.md](upstream-issue-208-comment.md)。

未获得的证据：媒体链路（AEC / 双向语音 / 打断）在 M0 中未验证。

**⚠️ 锁定 1.5.5 的已知代价**（回退时一并放弃了 v1.5.6 的 6 项修复）：

| v1.5.6 修复项 | 对我们的影响 |
|---|---|
| TCP/UDP 同 IP 端口候选映射错误 | 当前 Offer/Answer 使用 UDP，未见触发条件 |
| **DTLS HELLO 早到被丢弃（增加缓存）** | 保留建连时序缺陷；当前设备为 DTLS client，与已报告的 DTLS server 场景不同，发生率未测 |
| ICE-lite 属性处理 | 当前阿里 Answer 无 `a=ice-lite`，节点行为改变后需重评 |
| **ICE 提名条件（本条即导致失败者）** | 🔴 回退的理由 |
| SDP/候选字符串注释整理 | 🟢 无 |
| H264 profile | 🟢 无（不用视频） |

HELLO 缓存的二进制位置为 `agent_recv_one_packet`（原源 `agent.c:1872–1885`），
在 ICE pairing 期间保存一个最多 1400 字节的 non-STUN 数据报，随后交给 DTLS。
[上游 #205](https://github.com/espressif/esp-webrtc-solution/issues/205)
报告早到 ClientHello 丢失后重传恢复，表现为握手延迟。
当前 Answer `setup:passive`，设备为 DTLS client，典型触发条件适用性较低。
应验证重复建连/重连；没有证据表明已连接的语音会话仅因持续更久而触发此窗口。
不能断言“只有此项真实风险”或直接评为中风险；偶发失败先按 ICE/DTLS/应用阶段定位。
1.5.5 作为临时基线合理，持续媒体和重连稳定性未验证。

**准确触发条件**：响应通过检查并找到 pair 后，controlling、pair 非 relay、
mapped port 非零、mapped 地址与原 pair.local 不同，会被新增分支拒绝。
是否最终连接失败还取决于其他路径；不是“所有 NAT”或“阿里公网 host 特有”。
公网 `typ host` 符合 RFC 8445 §5.1.1.1/Appendix A，不能据云 IP、3478 端口、
地址轮换推定其为 TURN relay。既有的类型筛选还影响其他跨类型组合；
本次确定的回归应聚焦错误拒绝 NAT 响应，而不是声称必须额外保留 srflx/host 检查对。
详见 [第二轮复核结果](webrtc-m0-review-result-2.md)。

---

### A9. esp_peer 1.5.5 的 Opus RTP 发送路径未见 payload 长度校验

**证据**（2026-10-08 媒体设计复核）：ESP32-S3 官方库 SHA-256
`25a338fc6b05f702947c2af0e924f2dd3006503908e889dcdc512782d0ea3a58`。
DWARF 的 `rtp_encoder_t.buf` 长度 1428；Opus 使用
`rtp_encoder_encode_generic`（原源 `rtp.c:315`），函数偏移 0x77–0x85
直接 memcpy 完整 size 到 `buf + 12 + ext_reserve`，此前未见大小检查或分片。
`peer_send_audio` 将应用的 frame.data/size 直接交给 RTP encoder。

**状态**：静态复核并交叉确认；未发送越界包、未实机验证此风险、未发布上游 issue。
媒体桥接必须限制单个 raw Opus packet 的长度与边界，不能传大块 PCM、Ogg 或多包拼接。
是否其他版本修复尚未核验。详见 [媒体评审结果](webrtc-media-review-result.md) Q6。

## B. 我方踩的坑

### B1. `session.created` 走主任务队列 → 自锁死 40 秒

**现象**：连上后 40 秒收不到 `session.created`，超时断开；而服务端其实 30ms 就回了。

**证据**：

```
W (166859) WebSocket: [timing] 101 received
W (166869) [tls] conn_read returned 502 bytes          ← 数据 10ms 就到了
W (166869) [frame] delivered 498 bytes: {"type":"session.created",...}
            ↓ 然后 40 秒空白
E no session.created within 40000 ms
I session.created                                       ← 超时后才"出现"
```

**根因**（我写的）：

```cpp
// OpenAudioChannel() 在主任务上：
xEventGroupWaitBits(..., kBitSessionReady, ...);   // 主任务阻塞等这个位
// 网络任务收到数据后：
Application::GetInstance().Schedule([...]{ HandleServerEvent(...); });  // 投递到主任务队列
// → 主任务正阻塞等它，永远处理不到这个排队的回调 = 自己等自己
```

**修复**：握手关键事件（`session.created`、`error`）在回调内**当场处理**，
其余事件仍走主任务队列。40 秒 → **28 毫秒**。

### B2. 误判"上行音频卡 40 秒"

同一批日志里我一度以为是 TLS 层的 record 卡住，还去查了 `esp_tls_conn_read` 的超时、
省电模式、专属域名 vs 公共域名。**实际数据一直正常到达，是 B1 的投递问题。**

教训：看到"超时后数据才到"这种模式，先怀疑等待方与投递方的关系，而不是链路。

### B3. `raw-opus` / `raw-opus2` 实测不可用

文档列了这些格式，实测（Mac 直连服务端）：

| 方向 | `pcm` | `opus`(Ogg) | `raw-opus` | `raw-opus2` |
|---|---|---|---|---|
| 上行 | ✅ | ✅ | ❌ 服务端直接断连 | — |
| 下行 | ✅ | ✅ | ❌ 报错 | ❌ 报错 |

报错原文：`{"code":"CLIENT_ERROR","message":"Unsupported encode format: raw-opus2"}`

**结论**：只能用 `opus`（Ogg 封装）或 `pcm`。选择 `opus` 以保留板端 Opus 编码
（约 4kB/s，比 16kHz PCM 的 32kB/s 省 8 倍）。

### B4. 服务端 `semantic_vad` 对 Ogg 封装的 Opus 不触发

**证据**（同样的音频，唯一差别是 turn_detection）：

```
semantic_vad（自动轮次）    heard=False reply=False
手动 commit（无 VAD）      heard=True  reply=True   "你好，请用一句话介绍你自己。"
```

**结论**：必须用本地 VAD 驱动轮次——设备侧 AFE 的 VAD 可靠，检测到说完就发
`input_audio_buffer.commit` + `response.create`。已加 300ms 最短时长保护。

### B5. 音频文件结尾戛然而止 → 永远收不到回复

Mac 验证脚本上踩的：没有尾部静音时服务端 VAD 观察不到 end-of-speech，
永远不 commit 轮次，表现为"服务端无响应"但日志里 ASR 已经识别对了。
已加 `--pad-silence-ms`（默认 1200ms）。

**设备上不存在这个问题**（麦克风是连续流）。

### B6. `%lld` 在 ESP-IDF 日志里输出成字面量 `ld`

```
E no session.created within 40000 ms (tls+upgrade took ld ms, waited ld ms)
                                            ↑ 两个数字没打出来
```

`int64_t` 要用 `(int)` 强转后配 `%d`。占用了一轮排查。

### B8. 音频包被整批丢弃 → 播放一顿一顿

**现象**：让 AI 讲长故事时，播放明显卡顿、断断续续。

**证据**：

```
服务端 audio.delta 到达间隔：最小 70ms / 中位 300ms / 最大 1850ms
每个 delta 约携带 900ms 音频，间隔却有 1.8 秒 —— 缓冲垫不住就断音
```

**根因**（调度顺序，我引入的）：

```cpp
// application.cc:553 —— 只在"正在说话"状态下才喂解码器
protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
    if (GetDeviceState() == kDeviceStateSpeaking) {
        audio_service_.PushPacketToDecodeQueue(std::move(packet));
    }
});
```

而事件调度顺序让音频包**全部落在状态切换之前**：

```
网络任务 → Schedule(response.created)      队列: [created]
网络任务 → Schedule(delta#1..#N)           队列: [created, d1..dN]
主任务执行 created → 发 tts:start
                   → Application 又 Schedule(setState)
                                           队列: [d1..dN, setState]   ← 状态切换排到最后
主任务执行 d1..dN  → 状态仍是 listening → 全部丢弃 ❌
主任务执行 setState → 此时音频已丢完
```

`response.created` 走主任务队列时排在音频包**前面被处理**，但它引发的状态切换又通过
一次嵌套 `Schedule` 排到了**最后**。

**修复**：把 `response.created` / `response.done` 与握手事件一样**当场处理**
（`must_precede_audio()`），使状态切换先于音频包入队。

**验证**：

```
修复前：丢包告警持续出现，最大间隔 1850ms
修复后：丢包告警 0 条，最大间隔 1110ms
```

**教训**：多层 `Schedule` 嵌套时，事件的实际处理顺序与直觉相反。凡"生产者按状态门控
消费者"的设计，都要确认状态变更事件排在被消费的数据之前。

### B9. 情绪标记被朗读出来（TTS 念出 "Happy"）

**现象**：每次回答的开头都会听到英文单词 "Happy" / "Neutral"。

**根因**：我用"在文本里插 `[happy]` 标记"驱动表情，而**这个标记同时位于语音合成的
文本中**，于是 TTS 把它念了出来。阿里的 API 只有文本与音频两个通道，没有表情侧信道。

**定位方法**（可复用）：把生成的回复音频**回灌给服务端自己的 ASR**，看它实际"听到"什么：

| 标记格式 | 服务端实际听到 |
|---|---|
| `[happy]` 方括号 | `'Happy，我是Stack Chen，一个能陪你聊天、帮你解决问题的桌面机器人。'` |
| `😊` emoji | `'我是Stack Chen，你的桌面机器人伙伴。'` |

**结论**：方括号标记会被朗读，**emoji 不会**。改用 emoji 作为情绪标记。

**影响面**：`tools/aliyun_omni/protocol.py`、探针自测、固件的扫描器与系统提示词全部
改为 emoji。C++ 侧扫描器要按**字节**匹配 4 字节 UTF-8，并处理跨 delta 的半个 emoji
（Python 侧字符串按码点索引，emoji 是原子的，无此问题）。

### B10. 长回答播放一顿一顿——服务端推送速率远高于播放速率

**现象**：让 AI 讲长故事时播放明显跳字、断断续续。

**证据**（固件实测到达速率）：

```
[rate]  88 packets =   5280 ms audio arrived over  1378 ms wall (3.83x realtime)
[rate] 292 packets =  17520 ms audio arrived over  3251 ms wall (5.39x realtime)
[rate] 3556 packets = 213360 ms audio over        63213 ms wall (3.38x realtime)
```

**服务端以 2.9~5.4 倍速推送音频**，而解码队列上限只有 `2400ms / 60ms = 40` 包。

**根因**：队列满时 `PushPacketToDecodeQueue(packet, wait=false)` **直接丢弃**。
按 3.4 倍速计算，每播放 1 包就丢弃约 2.4 包——**约 70% 音频被丢掉**，
所以听起来是跳字而非卡住。

**为什么不能简单加大缓冲**：

```
free sram: 21971 minimal sram: 19271      ← 内部 RAM 只剩约 20KB
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512   ← 小于 512B 的分配强制进内部 RAM
```

音频包约 150 字节，全部落在内部 RAM。要装下 213 秒需要约 3000 包 ≈ 375KB，
必然 OOM。**8MB PSRAM 帮不上忙**，因为分配粒度太小。

**解法：背压**。让服务端等设备播完，而不是设备丢音频：

1. `response.audio.delta` 改为**在网络任务内联处理**
   （`is_audio_payload()`）——阻塞主任务没用，网络任务会继续读并堆积；
   阻塞读 socket 的那个任务才会让 TCP 窗口关闭，服务端自然被限速
2. `PushPacketToDecodeQueue(packet, /*wait=*/true)` —— 改为阻塞式推入
3. 队列上限降到 8 秒（`8000 / OPUS_FRAME_DURATION_MS`），
   因为只需吸收抖动，不再需要装下整个回答
4. 溢出时打**错误级**日志（原先静默丢弃）

**验证**：溢出丢包 0 条，队列深度稳定在 8~51。

**教训**：面对"生产者比消费者快"的流，正确做法是背压而非扩缓冲；
但背压必须施加在**真正读取数据源的那个任务**上，否则只是把数据挪到另一个队列。

### B11. 服务端 VAD 的参数名是 `server_vad`，不是 `semantic_vad`

派生于 OpenAI API 的命名习惯，我写成了 `semantic_vad`。阿里文档明确：

> 将 `session.turn_detection.type` 设为 **`"server_vad"`** 以启用 VAD 模式

**但改成正确名称后实测仍不触发**（同样的音频，手动 commit 能正确识别）：

```
server_vad + Ogg 输入  → heard=None spoken=None
手动 commit + 同音频   → heard='你好，请用一句话介绍你自己。'
```

**结论**：服务端 VAD 对本项目的 Ogg 封装 Opus 输入不工作，**必须用本地 VAD 驱动轮次**。
两件事都要做：参数名写对（避免被服务端判为非法配置），同时不依赖服务端 VAD。

### B12. 打断（barge-in）需要 AEC，而阿里不提供——这是架构限制

> 完整分析（官方规格引用、影响清单、可选路径）见
> [aec-limitation.md](aec-limitation.md)，本节仅记结论。

**现象**：想让设备"讲个故事，中途打断让它数数"，设备不理，且反应很慢。

**参考实现怎么做的**（`xiaozhi-esp32/main/application.cc`）：

```cpp
ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

case kDeviceStateSpeaking:
    if (listening_mode_ != kListeningModeRealtime) {
        audio_service_.EnableVoiceProcessing(false);   // 说话时不上行
    }
    break;
```

**结论：`kListeningModeRealtime`（可打断）的前提是 AEC 可用。**
AEC 关闭时，xiaozhi 自己也退化成半双工 `kListeningModeAutoStop`。

**所以原厂固件能打断，是因为小志的服务端做 AEC**（这正是 `CONFIG_USE_SERVER_AEC`
与握手消息里 `aec: true` 的用途）。而阿里 Realtime 的 WebSocket 协议
**明确不做回声消除**。因此：

| 组合 | AEC 来源 | 能否打断 |
|---|---|---|
| 原厂固件 + 小志服务端 | 服务端 | ✅ |
| 本方案 + 阿里 Realtime | 无 | ❌ 只能半双工 |

**这不是本项目的代码缺陷，是所选后端的接口能力差异。**

### B13. 设备侧 AEC 的参考通道存在但读不出来

**推断依据**：

```c
es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
```

官方硬件描述是"双麦克风"，但 ES7210 配了 **3 路输入**。多出来的这一路很可能是
扬声器回采参考——这正是 2 麦 + 1 参考的经典 AEC 布局，也是启用第三路的原因。

**实测**：设为 3 通道后 AFE **确实按 `MMR` 建起了处理链**：

```
AFE Pipeline: [input] -> |AEC(SR_HIGH_PERF)| -> |SE(BSS)| -> |VAD(WebRTC)| -> ...
```

**但 I2S 驱动直接拒绝**：

```
I2S_IF: channel mode 0 bits:16/16 channel:2 mask:1
E (12088) I2S_IF: Not support channel 3
```

`esp_codec_dev` 的 I2S 接口**最多 2 通道**，于是麦克风完全没有数据
（AFE 输出 `peak=0`），设备彻底不响应。

**反证第 2 通道不是参考**：开启设备侧 AEC 且 `input_channels_=2` 时，
AFE 输出峰值从 4036 塌到 120——说明它拿"第二只麦克风"当真值，
把用户语音一起抵消了。**第 1 通道确实是麦克风，不是回采。**

**要真正用上第三路**，需要绕开 `esp_codec_dev`，直接驱动 I2S TDM 接收路径。
ES7210 侧已经支持（TDM 槽位掩码为 `SLOT0|1|2|3`）。

**当前状态**：维持 2 通道 + 关闭设备侧 AEC，用半双工规避自打断。

### B7. CMake `GLOB_RECURSE` 不重新扫描新增文件

新增 `ogg_opus_muxer.cc` 后链接报 `undefined reference`，因为 `file(GLOB_RECURSE ...)`
在**配置期**展开。需要 `idf.py reconfigure`（或删 `build/`）。

### B14. WebRTC M0 的状态与 DataChannel 判据可能误报

**证据**（2026-10-07 静态复核，以下为 `2f5df7b` 的修复前实现）：

- `firmware/main/hal/webrtc/webrtc_m0.cc:308` 用 `peer_state >= CONNECTED` 判定
  传输建立；组件 API 的 CONNECTED=7、CONNECT_FAILED=8，故失败会误报 PARTIAL。
- 同文件 `174–182` 在首个 DATA_CHANNEL_OPENED 时固定向 stream 0 发送 update，
  发送前即设 update_sent=true；未使用 on_channel_open 给出的 label/stream_id，
  未按发送返回值判断成功。服务端 txt 先打开时，可能导致 session.updated 假阴性。
- `/tmp/wrtc-test/test.js` 实际在 txt 上回复 session.created，且对照实验远端 IP
  与设备不同，不能将其判词“成功即设备库有错”作为独立证明。

**状态**：已在 `4ba415f` 修复状态判据、记录服务端 channel/stream 并按发送结果
更新标志；`60796e0` 记录 stream 1 收到 session.created/session.updated。
这些问题不解释 A8 的 ICE 拒绝，但会影响成功判定。
详见 [webrtc-m0-review-result.md](webrtc-m0-review-result.md)。

### B15. 删除锁文件后切组件版本，改变了其他 21 个依赖

**证据**（2026-10-07 第二轮复核）：
`git diff 2f5df7b 4ba415f -- firmware/dependencies.lock` 显示 22 个组件版本变化，
除 esp_peer 外还有 21 个，包括 Wi-Fi 3.1.2 → 3.1.5、esp_codec_dev 1.5.11 → 1.5.4、
esp_mmap_assets 1.4.0 → 2.0.1。M0 改动也发生在同一对照中。
`main.cpp:40` 的 StartNetwork 实际调用该 Wi-Fi 组件；M0 前还运行 HAL 初始化，
不能把其余组件全部视为对网络、内存、任务时序无影响。

**状态**：已纠正文档的“严格单变量 A/B”主张，未改依赖或补做设备测试。
后续固定 M0、SDK/sdkconfig 及其他依赖版本/hash，仅切 esp_peer，检查最终锁文件
差异并保留固件/库 hash 和 SDP。删除整个锁文件不适合作为严格 A/B 的步骤。
历史对照的混淆不否定当前 1.5.5 成功或 A8 中确定的二进制错误分支。
详见 [webrtc-m0-review-result-2.md](webrtc-m0-review-result-2.md)。

### B16. 把 Opus 的 48k RTP 时钟误当成 PCM 必须 48k

**证据**（2026-10-08）：RFC 7587 §4.1/7 要求 Opus 的 RTP clock/SDP 为 48k，
不要求编码输入 PCM 为 48k。具体 `esp_opus_enc.h:70` 允许 8/12/16/24/48k；
实际 decoder 也可独立输出 24k，已从 API 与二进制验证。
M0 `webrtc_m0.cc:406` 原注释本就区分 capture rate 与 Opus clock。

实际 esp_peer 1.5.5 二进制：`peer_send_audio` 直接传 frame.pts，
`calc_timestamp` 的 Opus 分支执行 `pts * 48`，因此 **pts 单位为毫秒**。
20ms 包：16k PCM 320 样本，pts 增加 20，RTP timestamp 增加 960；
60ms 包相应为 960/60/2880。下行 pts 是 raw 32 位 RTP timestamp 换算，
不归零、不扩展回绕，不能直接当本地播放时钟。

**状态**：媒体 SPEC/PLAN 中“16k→48k 上采样不可避免”已判不成立，设计待修改。
推荐复用 AFE16k/Opus16k 编码，下行直接解码为板级 24k；esp_peer 的
audio_info.sample_rate 仍为 48000。协议/API 可行，阿里媒体效果尚未实测。
详见 [媒体评审结果](webrtc-media-review-result.md) Q2/Q3。

### B17. WebRTC 的“内置 AEC”不能直接推出裸 RTP 的服务端 AEC

**证据**（2026-10-08）：[RFC 7874 §5](https://www.rfc-editor.org/rfc/rfc7874.html#section-5)
建议端点设备实现 AEC；[W3C echoCancellation](https://www.w3.org/TR/mediacapture-streams/#dom-mediatrackconstraintset-echocancellation)
是采集侧配置。阿里浏览器示例使用 getUserMedia，AOQ 的 3A 配置也在客户端 SDK。
这些资料没有证明裸 esp_peer 会自动获得服务端回声处理，也不能证明服务端必然没有。

原 M1.5 任意本地 TTS 未必有服务端参考；“空转写”也可能来自播放时上行关闭、
转录配置/失败或 VAD。`speech_started` 是 VAD 事件，不是 AEC 指示器；
没有 AEC 后 PCM，不能从转写反推 ERLE/dB 抑制量。

**状态**：迁移收益须作为待验证假设；原方案 a 的二元退出条件不通过。
改用同一会话真实下行播放、连续上行计数与信号检查、近端正对照和双讲，
结果分目标通过/目标失败/实验无效。未执行媒体实验。
详见 [媒体评审结果](webrtc-media-review-result.md) Q1。

### B18. 服务端取消不能代替停止本地播放，媒体桥接需隔离旧任务

**证据**（2026-10-08 静态复核）：

- `application.cc:1007–1012` 的 AbortSpeaking 仅写 aborted_ 并发送 cancel，
  aborted_ 没有读取方，未清本地播放队列。
- `audio_service.cc:702–713` 的 ResetDecoder 清队列，但解码任务在 359 解锁处理后
  于 395–397 重新入队，播放任务在 313–324 出队后写设备；清队列不撤销处理中任务。
- `EnableVoiceProcessing:613` 每次 enable 重置 decoder/输入重采样并 warmup；
  speaking 关闭上行的现有状态机也不能直接复用为全双工。
- esp_peer 默认 on_audio 回调借用 RTP/jitter payload，frame 为栈对象；异步消费
  必须先复制。阻塞 PushPacketToDecodeQueue(wait=true) 会堵住 peer 主循环，
  不会提供 WebSocket/TCP 那种服务端背压。
- PlaySound（audio_service.cc:667）使用 OggDemuxer；M4 删除它会影响本地提示音。
  其余 Ogg/背压/门控也仍是 WebSocket 回退所需。

**状态**：仅记录，未修改固件。媒体计划需加入本地输出停止/有界尾音、
会话/响应代际与晚到 RTP 边界、有界非阻塞队列、全程采集和完整回退。
N4 的 8KiB free 总和不足以证明资源预算，需结合能力 heap、largest block、栈与
完整媒体/AFE/UI/重连实测。详见 [媒体评审结果](webrtc-media-review-result.md) Q5/Q6。

### B19. 编码 API 的多帧输出不是一个可直接发送的 Opus packet

**证据**（2026-10-08，M1 预检）：实际 `esp_audio_codec` 2.4.1，组件提交
`4d5cbe02f59fb45e40112d63317a8ddd00019cd4`；ESP32-S3 库
`firmware/managed_components/espressif__esp_audio_codec/lib/esp32s3/libesp_audio_codec.a`
SHA256 为 `faf4c0d76030b58f22df8258ec1b9c436b7445a7d875de3b26a8c888f4d9657a`。

`esp_opus_enc_get_frame_size` 的输入长度为字节，现有
`audio_service.cc:82–83` 调用后除以 `sizeof(int16_t)`。48k mono s16、20ms
对应 1920 字节／960 样本。编码结构中的 `out.len` 是调用方提供的缓冲容量，
`out.encoded_bytes` 才是有效输出长度（`include/encoder/esp_audio_enc.h:59`）。

对上述库中的 `esp_opus_enc.c.obj` 做 `objdump -dr --disassemble=esp_opus_enc_process`：
函数偏移 `+0xb3` 用输入长度除以帧字节数，`+0x103` 调用 `opus_encode`，
`+0x131–0x14e` 累加编码字节并移动输出指针继续编码。因此一次交给 API 多帧 PCM
会把多个 raw Opus packet **直接拼接**，没有逐包长度前缀，不能把整块结果当单个 RTP payload。

**状态**：API 行为已从头文件、现有调用与二进制独立核验；未声称 SDK 本身有缺陷。
M1 任务要求每次 `in.len == in_size`，逐包发送 `encoded_bytes`，并检查缓冲容量和
RTP payload 上限；实际实现及真机证据待复核。见 [M1 任务](webrtc-media-m1-task.md)。

### B20. 硬件第三路参考的限制不等同于 SDK 禁止软件播放参考

**证据**（2026-10-08，只读条件预研，HEAD `3ab11e9823866fa0979298caa71184c15bb6c478`）：
本机 ESP-SR 2.3.1 `esp_afe_sr_iface.h:89–100` 接受16k/int16交错内存PCM；
`esp_afe_config.h:152–167` 定义 R 为 playback reference；
`esp_aec.h:62–73` 的 refdata 明确定义为送扬声器的采样数组。
[Espressif FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/audio-development-framework.html#what-is-the-difference-between-software-aec-acoustic-echo-cancellation-and-hardware-aec-in-the-esp-adf-audio-application-development-framework)
区分 ADC 回采与主控制器复制的软件参考。

当前 `config.h:22` 设置 reference=true；codec 固定两路物理麦采样，
AFE 因此组成 MR，但设备 AEC 关闭，第二路仍是麦克风，未混入播放PCM。
旧 AudioService `audio_service.cc:309` 有最终提交播放的24kPCM，
现有应用没有把它与采集对齐并转换/组成软件参考。
播放队列、I2S DMA、增益/失真和重采样时序的效果证据均缺失。

**状态**：仅证实 SDK 的输入接口支持软件参考；当前工程未实现/未验证。
历史“未取得第三路有效硬件参考”及当前半双工能力限制保留；
不能直接打开 AEC，把第二只麦当参考，也不能据接口可用宣称打断可用。
不改变已冻结 SPEC 或 M1.5 HOLD，后续需要具体设计复核与真实同步/双讲实验。
完整路径与边界见 [条件预研](webrtc-software-reference-feasibility.md)。

### B20. 【已订正｜原结论错误】本机 Mbed TLS 公开 role/profile 接口

**订正（2026-10-08，Review 5448554040）**：本条原称"本机没有公开的 role/profile
getter、`chosen_dtls_srtp_profile` 是 MBEDTLS_PRIVATE 故不可读"，**该结论错误**。
实测本机 Mbed TLS 3.6.5 存在 `mbedtls_ssl_conf_get_endpoint`（ssl.h:2124）、
`mbedtls_ssl_context_get_config`（:2298）、
`mbedtls_ssl_get_dtls_srtp_negotiation_result`（:4383，公开 void，
填充公开类型 `mbedtls_dtls_srtp_info`）、`mbedtls_ssl_is_handshake_over`（:5114）。
读取该公开返回类型的 `chosen_dtls_srtp_profile` 字段是**使用公开契约**，
不是猜内存偏移。

**教训**：把"字段带 `MBEDTLS_PRIVATE` 宏"等同于"不可用"是错的 ——
判据应是**该类型的获取函数是否为公开 API**。

### B20b. 【历史】原（错误）结论

**证据**（2026-10-08，D1-1 实现）：本机 Mbed TLS 3.6.5
（`~/esp/esp-idf-5.5/components/mbedtls/mbedtls/include/mbedtls/ssl.h`）中
`mbedtls_ssl_get_ciphersuite(ssl)` 存在（:4860），但**没有**公开的
endpoint role 或 selected SRTP profile getter；`chosen_dtls_srtp_profile`
是 `MBEDTLS_PRIVATE` 字段（:1247）。

**影响**：短诊断**不能**通过公开接口报告 role 或 SRTP profile。
读取该私有字段即等于猜结构体布局，是必须避免的做法。
报告中应记 `n/a`，而不是编造或从私有内存推断。

**状态**：已在 `dtls_short_probe.cc` 中按此实现；cipher **名称**可获取。

### B21. `--wrap` 同一符号只能有一个定义；跨探针共享需导出内部入口

**证据**：同时为 RTP 与 DTLS 观测包装 `lwip_sendto` 会产生
`multiple definition of __wrap_lwip_sendto`。

**处置**：`__wrap_lwip_sendto` 只保留一处（`dtls_short_probe.cc`），
RTP 侧改为导出 `rtp_probe::ObserveSendto()`，由该唯一包装转发。

### B22. 设备侧固定 SSRC 与 SDP 的关联（待验证）

**证据**（2026-10-08，D1-2 短诊断）：`first: pt=111 seq=0 ts=0 ssrc=0x00000006`。

**订正（Review 5448554040）**：该 SSRC **已在同次 Offer 中宣告为 `a=ssrc:6`**，
与探针观测一致。RFC 3550 §5.1/§8 的随机性要求用于避免**会话内碰撞**，
**小数值本身不是非法条件**，不能当作"设备未在 SDP 声明 SSRC"或根因。
保留为"固定 SSRC / SDP 关联待验证"。

### B23. 【已撤回｜归属错误的历史日志】设备 DTLS 握手完成但 SRTP profile 为 UNSET

> **本条结论已撤回（2026-10-13，[Review5498100335](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5498100335) D3-1）。**
> 下面的日志**归属错误**：它把 STREAM(HTTPS) 的握手算成了 DTLS，把真正的 DATAGRAM
> 握手记成 `stream_excluded`。因此「DTLS 完成」「SRTP 未协商」「失败在 DTLS 之后」
> 三条件结论**全部不成立**，不能作为设备侧证据，更不能据此报上游。
> 原始日志**保留**为历史证据（不删除），正确归属见 B24。
> 摘要行原文是 `dtls=1 ... profile=0 UNSET`，其中那次 `dtls=1` 实际是 HTTPS 的 TLS。

**证据**（2026-10-08，D2-1 短诊断，`docs/device_diag_evidence/run_d2_profile_unset.log`，
**归属错误的历史日志**）：

```
setups=2 (datagram=1 stream=1) | hs calls=7 dtls=1 ok=1 fail=0 stream_excluded=6
last_ret=0 last=288ms over=1 role=0
cipher=TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256
profile=0 UNSET
```

**当时的要点（均已撤回，留作对照）**：
1. ~~DTLS 握手成功完成（`is_handshake_over=1`，ret 0）~~ —— 该次握手是 STREAM。
2. ~~SRTP 保护 profile 未协商~~ —— 这个 profile0 读自被误判为 DTLS 的 HTTPS context。
3. ~~失败点位于 DTLS 之后~~ —— 由 1、2 推出，前提不成立。

**状态**：**历史日志，归属错误，结论撤回**。
`run_d2_profile_unset.log` 保留，但只作为「分类写反」的现场记录；
设备媒体根因**仍未确定**。正确的诊断契约见 B24 与
[D3 记录](webrtc-media-m1-diagnostic-review-3.md)。

### B23b. D1 短诊断结论修正：公开 getter 存在，100 包仅覆盖前置静音

**证据**（2026-10-08，复核 exact HEAD
`a3a7b9f4ccf7aeb7a25f75cc39c1010262edf570`）：本机 Mbed TLS 3.6.5
`ssl.h:2124/2298/4383/5114` 分别提供公开 endpoint/config/profile/completion
接口。用 `firmware/build/compile_commands.json` 中 `webrtc_transport.cc` 的
同一编译器、选项及工作目录编译最小调用片段，退出码0；目标对象引用真实
`mbedtls_ssl_get_dtls_srtp_negotiation_result`。材料保留在
`/tmp/stackchan-public-dtls-getters.cc` 与 `.o`。公开 getter 输出的
`info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile)` 可以按官方声明读取枚举，
不等于猜测 SSL 内部布局；不读取或记录 MKI。
本页第二个 B20 的“公开 getter 不存在”及其不可行结论**已被此证据推翻**。

`/tmp/m1-dtls.log:289` 的 protect100/UDP100 汇总发生在`:292` 的 zh_1
开始之前，首包 RTP15/SRTP25 对应3字节静音 payload。因此仅证明前置静音的
本地保护和整长写入；不能证明语音 fixture 的同等路径，更不证明远端解密。
实际 `dtls_common.h:179–206` 的 client/server key/salt 分配正确；反向 wildcard
标签在本机 libsrtp3首次 protect 克隆时会设 sender，不足以单独定位此次失败。
profile 列表支持多个值而 policy 固定默认80位认证仍是条件风险，需先量实际协商值。
只读证据：`/tmp/stackchan-srtp-role-policy-review-c865084/findings.md`。

**状态**：M1 未通过，继续修同一 D1 诊断。不能据前置静音100包排除设备编码或
SRTP互通，不能把 SSRC6 的数值小当根因；
[RFC3550 §5.1/§8](https://www.rfc-editor.org/rfc/rfc3550.html#section-8)
要求随机选择以避免会话内碰撞，没有“小值非法”的门槛。
同次日志`:216–217`的Offer已宣告`a=ssrc:6`，与探针值一致；
不能直接认定设备缺少SSRC声明。
保留设备参数，先测同一 DTLS context 的 role/profile 和实际语音 fixture 写出。

### B24. D2 的 profile0 来自 HTTPS 误归类，不能认定 SRTP 未协商

**证据**（2026-10-13复核 full HEAD `b45aaa70ee324e2f9997412c1d573dd31b1269be`）：
本机 Mbed TLS `ssl.h:301–302` 定义 STREAM=0、DATAGRAM=1；
`dtls_short_probe.cc:467–470` 将两者写反。摘要所谓dtls1/stream_excluded6
实际上把STREAM计为DTLS、DATAGRAM排除。因此上一条B23（归属错误的历史日志）的
“DTLS成功/profile0实测、失败位于DTLS之后”结论**撤回**，该日志保留为历史。
只有正确识别同一DATAGRAM context后，才能报告真实DTLS完成与选中SRTP profile。
此外conf_transport真实返回void，释放符号为config_free（不存在conf_free）；
这些探针契约必须先修，不应以错误UNSET结果提新上游issue。

**上游状态**：维护者在
[2026-10-08发布回复](https://github.com/espressif/esp-webrtc-solution/issues/208#issuecomment-6059327698)
确认组件1.5.7已发布；
[官方changelog](https://components.espressif.com/components/espressif/esp_peer/versions/1.5.7/changelog?language=en)
列出XOR-MAPPED提名拒绝修复及prflx支持。独立LiveKit/NAT验证是上游修复库的
外部证据，不代替本设备阿里端验证。Issue仍OPEN。

**状态**：用户已授权按PLAN U1受控升级。先修诊断、取1.5.5有效基线，再同代码
对照1.5.7，评审后采用精确pin；不删整个锁、不并调媒体/角色/cipher。
48k媒体与M1.5验收独立保持。复核及派修见新D3记录。

### B25. 1.5.7 静态兼容性与真实组件指纹

**证据**（2026-10-13，官方组件只读下载/二进制比较）：
1.5.7源提交 `33a863f1a6c57bdcb7821f6f7a8e159035a0acf6`；
注册表component_hash为 `92bc1b80772acf8a0e2dc36193ec24278e0793140eca2a4060d8ce7b45c47845`；
S3库SHA256为 `e6fa4ccb3256007105bf7eff1e313dc5c06669f7d3b6df192571ebdba4496285`。
二者不是同一种hash，更新锁条目不得用库/zip的SHA冒充component_hash。
来源为[官方注册表元数据](https://components-file.espressif.com/components/espressif/esp_peer.json)。

与本机1.5.5相比：DTLS/UDP公开源码、结构/枚举与CMake保持一致，
依赖仍private esp_libsrtp ^1.0.0。S3 RTP/jitter/rolling-buffer对象相同，
七个相关音频函数指令/常量/重定位相同；pts毫秒→48*ticks与1428B整包缓冲
及本应用1352B保守payload预算继续有效。新增pthread mutexattr符号本机IDF5.5.4已实现。

**状态**：仅未发现所查表面的新ABI/依赖阻塞，尚未安装、构建、真机运行或采用新版。
最终link/wrapper路径与实际两臂媒体仍需U1验证；升级不会修复本应用的诊断分类错误。
完整指纹、来源和证据边界见[静态核查](esp-peer-157-preflight.md)。

### B26. U1-0：探针身份分类与真实 API 签名/生命周期修复

**证据**（2026-10-13，[Review5498100335](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5498100335)
D3-1/D3-2，修自 `b45aaa70ee324e2f9997412c1d573dd31b1269be`）：

判据全部取自**本机 Mbed TLS 3.6.5 头文件**，不是推测：

| 事实 | 位置 |
|---|---|
| `MBEDTLS_SSL_TRANSPORT_STREAM == 0`、`DATAGRAM == 1` | `ssl.h:301–302` |
| `mbedtls_ssl_conf_transport` 返回 **void** | `ssl.h:2143` |
| 释放符号是 `mbedtls_ssl_config_free`（**无** `mbedtls_ssl_conf_free`）| `ssl.h:5741` |
| `mbedtls_ssl_free`（清 context 身份也要 wrap）| `ssl.h:5569` |
| `mbedtls_ssl_conf_get_endpoint`/`context_get_config`/`is_handshake_over` 为 static inline | `ssl.h:2124/2298/5114` |

**旧 wrapper 是死代码，不是"未被观测到"**：`firmware/build/stack-chan.elf`
（b45aaa7 构建，重建前采集）中 `nm` 只有 `__wrap_mbedtls_ssl_handshake/setup/conf_transport/
config_defaults`，**没有任何 `__wrap_mbedtls_ssl_conf_free`**，
真实符号是 `mbedtls_ssl_config_free`。因为该符号不存在，`__real_*` 引用随
`--gc-sections` 一起被丢弃。这正是"wrapper 有定义 ≠ 有调用"的现场。

**修复后的最终 ELF 实际 callsite**（`stack-chan.map` 的 Cross Reference Table）：

| wrapper | 实际调用者 |
|---|---|
| `__wrap_mbedtls_ssl_handshake` | libmbedtls `ssl_msg.c.obj`、esp-tls `esp_tls_mbedtls.c.obj`、esp_peer `dtls_srtp.c.obj` |
| `__wrap_mbedtls_ssl_setup` | esp-tls `esp_tls_mbedtls.c.obj`、esp_peer `dtls_srtp.c.obj` |
| `__wrap_mbedtls_ssl_conf_transport` | esp_peer `dtls_srtp.c.obj` |
| `__wrap_mbedtls_ssl_config_defaults` | esp-tls、esp_peer `dtls_srtp.c.obj` |
| **`__wrap_mbedtls_ssl_config_free`** | esp-tls、esp_peer `dtls_srtp.c.obj` |
| **`__wrap_mbedtls_ssl_free`** | esp-tls、esp_peer `dtls_srtp.c.obj` |

反向核对：未 wrap 的 `mbedtls_ssl_*` 真实符号**只**被 `dtls_short_probe.cc.obj`
（即 wrapper 里的 `__real_*`）引用，说明真实释放确实经过观测点。
两个 HTTPS(esp-tls) 与 DTLS(esp_peer) 都会命中同一组 wrapper，
所以"只有同一次成功 setup 登记的 (ctx,conf) 才算 DATAGRAM"是必需的，不是保守。

真实 callsite 在**源码**里而非预编译库：`libpeer_default.a` 不含任何
`mbedtls_ssl_*` 未定义符号（只用 `mbedtls_md_*`）；DTLS 在
`espressif__esp_peer/src/dtls_srtp.c`（IDF<6.0 分支）与 `src/dtls_common.h`，
二者与 mbedtls 不同目标文件，`--wrap` 有效。
`dtls_srtp_reset_session()` 会在**同一个 context 指针**上
`mbedtls_ssl_free → init → setup`，并可能换角色，所以 free 必须终止身份、
已登记配置的 transport 必须**更新**而不是 early return。

**修复形态**：新增 ESP 无关的 `firmware/main/hal/webrtc/dtls_probe_core.h`，
把身份表/分类/profile 门控/errno 纪律集中一处，设备 `.cc` 与离线检查**共用同一份**
代码；transport 一律走公开宏；只有**同一次成功 setup 登记的 (ctx, conf)** 才能判为
DATAGRAM，配置单独存在不足以定身份（未知保持未知），Arm/新代际清空身份，
每个 wrapper 保留真实返回并把 errno 恢复为真实调用产生的值。

**离线实测**（`firmware/tests/`）：
- `run_dtls_probe_offline_check.sh`：用本机真实头文件编译并运行，
  8 组场景 **120 项断言全通过**（STREAM 成功被排除、DATAGRAM 成功读 profile=1、
  未完成不读 profile、未知 setup 不被当成 DTLS、conf_transport 由 STREAM 切
  DATAGRAM 生效、free/Arm 终止身份、返回值与 errno 保持、固定表溢出显式计数）。
  属 C++ 编译/运行检查，**不在**单进程离线控制器覆盖内，单独报告。
- `check_dtls_probe_actual_source.py`：经控制器运行的实际源码检查，
  另见台账 `dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`。

**状态**：诊断代码已修，**设备两臂实测未做**（本轮未发现 `/dev/cu.usbmodem*`）。
不得据此宣告 M1 通过，也不得编造 profile 或升级结果。
以下 M1 未完成项按 Review 保留、不并入本轮：
`my_id` 声明未赋值/使用、protect 仍找同 tuple 最新 pending、旧调用跨 Arm 污染
`g_rep`、`SettleWindow()` 只有声明/定义无调用、`NotePoll/NoteDtlsSocket/
NoteRtpSocket` 尚未接线（恒零不能当观测）、DTLS record 样本 ring 未做。

### B27. U1-0 第二次复核：跨 Arm 握手、前置 errno、真实签名断言与受控行为执行

**证据**（2026-10-13，[Review5498323405](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5498323405)
复核 exact HEAD `2403b287faeefe06942ab008513472cadb787c89`）。三项 F1–F3 均已修：

**F1 —— 跨 Arm 的在途握手不得写入新报告。** 原 `WrapHandshake` 不保存入口代际，
真实调用返回后才分类，分类与写报告之间还有 getter：Arm 若发生在 real 调用内，
旧调用计入新报告的 `hs_calls`/`unknown_ctx`；若发生在分类后、写报告前，
旧 completion/profile 会进入新报告。现改为：**入口下锁固定代际与身份**，real 调用不持锁，
并在三处核对同一代际 —— real 返回后、**读取 profile 前**、以及最终提交时再持锁核对。
过期调用**一个字段都不写**（因此也不设"已丢弃"计数器，否则仍是在改新报告），
真实返回与 errno 照常透传。

**F2 —— 前置观察不得改变 real 的入口 errno。** 原代码保存入口 errno 后先执行计时函数
再进入 real，没有恢复；计时若污染 errno，real 与调用方都会收到观察器的值。
现改为所有前置观察（锁/分类/计时）结束后、**紧邻 real 调用**恢复入口 errno。

**F3 —— 编译期签名证明与受控行为执行。** 六项断言原为"与手写函数类型比较"，
现直接比较 `decltype(&__real_mbedtls_ssl_X)` 与 `decltype(&mbedtls_ssl_X)`，
上游改签名即编译失败。行为检查拆成两步：编译（控制器覆盖外，单独报告）与
**执行已编译二进制**（不 fork/不联网），后者经原台账注册为
`u1-d3-probe-behaviour`（cache=never），构建产物与输入源码共同进入 fingerprint。
产物置于工作区**之外**（`~/.codex/dev-harness/artifacts/stackchan-webrtc-media/`），
因为控制器 sandbox 拒绝读取工作区，工作区内的二进制无法执行。

**实测**：行为检查 **177 项断言通过**（含新增场景 9「跨 Arm」与场景 10「errno 透传」），
经控制器运行输出与本地一致。**负对照**证明测试有效：分别去掉 F1 的代际核对、
F2 的入口恢复后，场景 9 出现旧 completion/profile/计数泄漏（`hs_calls`/`profile_reads`/
`have_profile` 等 12+ 项失败），场景 10 出现入口与出口 errno 失败（5 项）。
IDF v5.5.4 build 通过（`stack-chan.bin` 0x3104d0）。

**状态**：F1–F3 已修并验证；**本机无 `/dev/cu.usbmodem*`，设备两臂实测仍未做**，
不宣告 M1 通过、不编造设备结果。peer 保持 1.5.5，未改媒体参数，未进入 U1-1～3。
本 Review 明确**不**要求"活跃 session 修改/释放 config 后继续真实握手"这类
Mbed TLS 禁止的调用序列（`ssl.h:2065–2072`），也不把它当本设备阻塞。

### B28. U1-S 开发中预检：短入口与诊断声明须独立核对

**证据**：2026-10-13，full bcd4e35 之后的 WIP 短模式，
`RunShortDiag` 缺少媒体时钟初始化；完整三 clip 的 final verdict 未排除短模式，
未运行的长循环会显示 COMPLETE；clip 前500ms为静音，却仅凭 clip 写出计数称为
speech evidence；区间起始边界缺失被误报成没有新包；ASR超时不拷已有turn VAD/item。
具体静态路径及最小修正见 [U1-S预检](webrtc-media-u1-preflight.md)。

**状态**：**五处均已修**（媒体时钟初始化、短/完整验收分离、clip 写出证据正名、
区间边界与"有无新包"分离、ASR 无结果保留 partial VAD/item），并从修正后的**共同源码**
重建两臂；旧 WIP 1.5.5 归档更名 `superseded-arm-a-wip-1.5.5/`，不作基线。
每处纠正都有负对照触发失败（`u1-short-wiring`）。
仍不是真机故障结论：**设备离线，无实测**。详见 [U1结果](webrtc-media-u1-result.md)。

### B29. 真实媒体集成的静态接线边界（准备资料）

**证据**：当前AudioService/AFE源码的60ms PCM、单帧编码carry、队列与生命周期，
以及Transport Start/Stop契约，见 [只读接线准备](webrtc-media-m2-preparation.md)。
当前decode/send均40包（2400/60），历史133包说明已在SPEC/准备记录纠正。

**状态**：只读调查，未派媒体实施、未改生成目录、未实测。不把模型/静态接线当作
全双工或打断通过；后续须满足真实退出、旧PCM隔离及实际硬件停音的阶段出口。

### B30. 只改一个 peer pin 就会重求解整图：一次改动 20 个无关组件

**证据**（2026-10-13，U1-2，ESP-IDF v5.5.4 组件管理器）：

`main/idf_component.yml` 只把 `espressif/esp_peer` 从 `1.5.5` 改为 `1.5.7`，
`idf.py reconfigure` 后 `dependencies.lock` 有 **21 个条目变化**，其中 **20 个与 peer 无关**：

```
78/esp-wifi-connect 3.1.2->3.1.5        78/esp_lcd_nv3023 1.0.0->1.0.1
78/uart-eth-modem 0.3.4->0.3.5          espressif/adc_battery_estimation 0.2.1->0.2.2
espressif/adc_mic 0.2.2->0.2.3          espressif/bmi270_sensor 0.1.1->0.1.2
espressif/button 4.1.6->4.1.7           espressif/esp32-camera 2.1.5->2.1.8
espressif/esp_codec_dev 1.5.11->1.5.4   espressif/esp_image_effects 1.0.1->1.2.0
espressif/esp_lcd_co5300 2.0.3->2.2.0   espressif/esp_lcd_panel_io_additions 1.0.1->1.0.1~1
espressif/esp_lcd_touch_cst816s 1.1.1->1.1.2   espressif/esp_lcd_touch_gt1151 1.1.0->1.1.1
espressif/esp_lcd_touch_gt911 1.2.0~1->1.2.1   espressif/esp_lcd_touch_st7123 1.0.1->1.0.2
espressif/esp_mmap_assets 1.4.0->2.0.1  espressif/knob 1.0.2->1.1.0
lvgl/lvgl（条目录入变化）                txp666/otto-emoji-gif-component 1.1.1->1.3.0
waveshare/custom_io_expander_ch32v003 1.0.1->1.0.2
```

机制：`idf_component.yml` 一变，锁里的 `manifest_hash` 失配 → 管理器**重求解整图**并按
"最新满足版本"落盘，而不是只动被改的那个依赖。其中 `esp_codec_dev` 是**降级**、
`esp_mmap_assets` 是跨大版本升级——对"单变量 A/B"是直接污染。

**可用处置（已实测）**：把无关条目逐条**恢复为基线锁的已知值**（含其已知
`component_hash`，不猜新 hash），只保留目标依赖条目与 `manifest_hash` 的变化。
之后 `idf.py reconfigure` **接受并沿用该锁**，把 `managed_components/` 中无关组件恢复为
基线版本（实测 `78/esp-wifi-connect` 3.1.2、`esp_mmap_assets` 1.4.0、`knob` 1.0.2、
`otto-emoji` 1.1.1、`esp_codec_dev` 1.5.11、`lvgl` 9.4.0 全部回到基线）。
最终锁差异仅 3 行：peer 的 `component_hash`、peer 的 `version`、`manifest_hash`。

**状态**：已用于 U1 两臂对照，并在结果文档明示该人工处置交 U1-4 复核。
不是"手改锁绕过校验"：恢复的是**本仓库自己刚归档的基线值**，
且管理器接受后仍按锁校验下载物。

### B31. U1归档必须保留flash相对路径及私有权限

**证据**：c173d19归档将bootloader/partition镜像扁平存储、app镜像只放臂根，
但bin/flash_args与JSON引用嵌套相对路径，三条引用缺文件；归档自身权限755/644。
Review5498605495后的归档修复只用各臂原镜像补齐路径，10/10引用均存在，
app镜像仍为原臂hash，既存镜像/ELF/map/库字节未改变；root/臂目录700、敏感配置600。
本机esptool4.12的flash选项属于write_flash子命令；结果文档的参数顺序已由Codex纠正。

**状态**：归档准备已通过Codex/独立只读复核，具体记录见
[U1复核](webrtc-media-u1-review-result.md)。未刷机、未输出敏感配置内容；
设备实测与版本采用仍待接入，不以归档检查替代M1媒体验收。

### B32. 两版本媒体都送达边界，但服务端 VAD/ASR 恒为 0（U1 真机，7/7 运行）

**证据**：2026-10-22 两臂真机短诊断（1.5.5 四次、1.5.7 三次）全部：
`session.created`→`session.update`→配置回显校验通过
（`server_vad thr=0.50 silence=800`、`qwen3-asr-flash-realtime`）；
SRTP protect 1339/1339、UDP 1339/1339 全写出、无短写/失败；
本地 Opus 解码 1339 ok/0 failed；但 `[vad] started=0 stopped=0`、
`[asr] completed=0`、`zh_1 kw=0/3`、`transcript=""`、20 s 后 `asr-timeout`。
同一日志里 `media/wall elapsed : 26780 / 3865 ms` 的 wall 只统计 clip 段（见 B34）。
原始日志：`docs/device_diag_evidence/2026-10-22-u1/`。

**解读**：本地 SRTP/UDP 写出成功只证明"包进了 socket"，不证明服务端解密/解码成功。
由于两版本结果完全一致，M1 的阻塞点不是 esp_peer 版本，也不是 ICE/DTLS/SCTP/
会话配置；应优先排查服务端是否接受这些 RTP（SRTP protection profile、payload/声道、
时序）或服务端 VAD 触发条件（账号/端点/配置面）。
**未排除**：SRTP 密钥或 profile 与设备侧不一致导致服务端静默丢包。

**状态**：已用 7 次运行复现；下一步用 host/browser 参考客户端与下行 RTP/RTCP、
SRTP profile 观测区分"设备包不被接受"与"服务端 VAD 不触发"。
M1 仍未通过，不得把本发现当作 M1 通过或 AEC 结论。

### B33. DTLS 握手偶发超时：记录双向流动但仍 18 连败，不能归因 peer 版本

**证据**：arm-a（1.5.5）刷机后第一次启动：
`hs calls=19 dtls=18 ok=0 fail=18 total=23324 ms`、`profile=-1`、`session_created=0`，
结尾 `VERDICT: FAIL - transport layer. stage=server answer received(4) peer_state=6`；
探针同一行显示 `DTLS TX records=19 in_dgrams=19`、`RX records=41`，
并明确 `DTLS records flowed both ways ... but the handshake never returned success`
（`last_ret=-30848`，`agent_recv timeout`）。同臂后续运行有 1 次两重试后成功
（`hs calls=4 dtls=3 ok=1 fail=2 total=7992 ms`），其余首握 591–618 ms；
arm-b（1.5.7）三次首握 582–605 ms。

**解读**：失败形态是 DTLS 握手阶段的超时/重试，而 ICE 已 PAIRED、Candidate 有响应，
**不是 issue208 的 ICE 提名拒绝**。单次失败出现在刷机后首启，随后的同臂运行未复现，
不能排除服务端会话/TURN 状态或网络瞬态；样本量不足以宣称 1.5.7 改善握手。

**状态**：已记录为 U1 实测的抖动项；U1-4 建议保持 1.5.5 基线（待评审）。
后续若再出现，按同一 `hs calls/fail/total`、profile 与 RX/TX records 记录，不猜独立根因。

### B34. U1-S 的 `media/wall` 统计口径混用，不能当实时性判据

**证据**：`firmware/main/hal/webrtc/webrtc_m1.cc` 的 `StreamClip()` 只在 clip 发送段
累加 `g_metrics.wall_us`（本机约 3.865 s，接近 `zh_1` 的 3.887 s），
而 summary 的 media 用 `g_metrics.frames_media * kFrameMs`，包含前置 2 s 静音与
ASR 等待期间持续发送的静音（1339 帧 = 26.78 s）。因此固定出现
`media/wall elapsed : 26780 / 3865 ms`，**不是**"媒体比实时快 7 倍"，
也不能据此判断 pacing、丢包或服务端接收节奏。

**状态**：仅诊断口径问题，不影响实际发送（日志中 `pacing late` 4–8、
`media t` 与 wall 同步推进）。修复时要么让 `wall_us` 覆盖所有发送帧，
要么把该行改名为 clip 段耗时；在修复前不得引用该比值下结论。

### B35. 【根因】SRTP 协商 SHA1_32、实现 SHA1_80：服务端静默丢弃全部媒体

**证据**：
1. U1 短诊断（1.5.5/1.5.7 共 7 次连通运行）本地 SRTP/UDP 全成功，但服务端
   `[vad]/[asr]` 恒为 0。
2. 同端点用 **aiortc** 参考客户端发同一 `zh_1`：VAD 触发、转写
   "今天我们测试语音连接。"、服务端回报收到 589/591 包 → 服务端无问题。
3. 探针经 `mbedtls_ssl_get_dtls_srtp_negotiation_result()` 读到
   **`profile=2` = `MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32`**；
   但 `managed_components/espressif__esp_peer/src/dtls_common.h` 对所有 profile
   都调用 `srtp_crypto_policy_set_rtp_default()`（= SHA1_80，80-bit 认证标签）。
   服务端按 32-bit 标签校验 → 全部认证失败、静默丢包；本地 `srtp_protect`
   与 `sendto` 当然成功，因此既有诊断看不到错误。
4. 密钥派生不变（AES128_CM 的 key 16 B / salt 14 B 与 profile 无关）。

**旁证**：屏蔽 aiortc 的 RTCP 发送后 VAD/转写仍正常（收到 575/585），
所以"设备不发 RTCP SR"不是本问题的原因。

**修复（已实测）**：在 tracked 源码里用 `--wrap` 收窄
`mbedtls_ssl_conf_dtls_srtp_protection_profiles()` 的 offer 到
`{SHA1_80}`（`dtls_short_probe.cc` + `main/CMakeLists.txt`），
`managed_components` 的临时改动已还原。修复后同一短诊断：
`profile=1`、`speech_started/stopped`、ASR 转写正确、
`outcome=media-completed`。

**状态**：根因与修复已验证 [根因记录](webrtc-media-m1-srtp-profile-rootcause.md)；
M1-3（完整三 clip + 5 分钟资源）与 M1.5 未跑；产品路径（M2）需同样应用此修复；
建议给 esp_peer 报上游 issue（offer 与实现不一致，应二选一修）。

### B36. 参考客户端是对齐"服务端行为"与"设备实现"的最短路径

**证据**：aiortc（Python，pylibsrtp/PyAV）对同一端点、同一 `zh_1`、同一
`session.update` 建立了完整链路并通过 VAD/ASR；关键点：

- 服务端会**自己开一个 `label=txt` 的 DataChannel** 并立即发 `session.created`；
  客户端自建 `oai-events` 通道会与之抢同一 stream id 而卡在 connecting；
- `session.update` 必须发在服务端 `txt` 通道上，回显
  `turn_detection` 才完整（`prefix_padding_ms`、`create_response` 等）；
- 发送前必须等 `session.updated`：参考脚本最初在 `recv()` 里睡到未来时刻，
  导致 `start_at` 更新后仍不发送，表现为"完全没声音"的假阴性——
  这类错误会污染对照结论。

**用途**：以后再出现"设备发得出去、服务端没反应"，先花十分钟跑参考客户端，
能把问题三分为服务端/账号、设备媒体实现、或诊断口径，再决定查哪一侧。
**状态**：脚本在本机 `/tmp/aliyun_rtc_ref2.py`（未入库；含读取本地配置的逻辑，
不应提交带凭据的运行产物）。日志要点已记入 B35 与
[根因记录](webrtc-media-m1-srtp-profile-rootcause.md)。

### B37. 修复 SRTP profile 后 M1 完整验收通过（单次运行基线）

**证据**：2026-10-22 关闭短模式、完整 M1 模式真机运行（日志
`docs/device_diag_evidence/2026-10-22-m1/m1-full-acceptance.log`）：

- 3 个 clip：VAD 1/1，转写与关键词 `3/3`、`2/2`、`2/2` 完全一致；
- 全程 `vad started=69 stopped=69`、`asr completed=69 failed=0`；
- `srtp_protect 15901/15901`、`udp 15901/15901`、`pacing late=2`；
  编码/重采样/超限/OOM/发送失败均为 0；
- 300 s 资源循环 `COMPLETE`（wall 318.1 s）：DRAM free 104831→104331
  （min 98183）、PSRAM free 7913256→7902460、sender 栈余量 10148/32768；
- 重采样 p95 455 µs、编码 p95 7637 µs / p99 9315 µs；
- 固件判决 `VERDICT: PASS`。

**解读**：根因修复（B35）之后，M1 的四层验收在本机单次运行内成立。
这同时确认此前"1.5.5/1.5.7 都无 VAD"不是 peer 版本问题，而是 SRTP profile/策略
不一致；当时版本取舍（U1-4）仍需修复后重跑两臂，后续结果见 B38。

**状态**：2026-10-23 接替复核已放行 M1 上行范围；M1.5 尚未验收。单次运行不代表长时稳定性
（重连/抖动/丢包属 M3）；只验上行，不涉及 AEC/打断。

### B38. U1-4：修复 SRTP profile 后两臂等价，采纳 1.5.7

**证据**：2026-10-22 两臂用同一源码（含 SRTP wrapper）与参数各跑一次短诊断：
1.5.5 首握 608 ms，1.5.7 首握 595 ms；两臂均协商 `profile=1`（SHA1_80）、
`vad started=1 stopped=1`、ASR 转写"今天我们测试语音连接。"、
`opus packets=336`、`srtp_protect 336/336`、`outcome=media-completed`。
锁差异仍只有 peer `component_hash`/`version` 与 `manifest_hash` 三行。

**决定**：采纳 1.5.7（无回归；M1 完整 `VERDICT: PASS` 在其上取得），
保留 1.5.5 回退路径。此前"两版本都无 VAD"的对照因 SRTP profile 缺陷失效，
不得再用于版本取舍。

**状态**：[U1-4 结论](webrtc-media-u1-4-decision.md)；每臂仅一次运行，
未重新归档两臂构建，旧归档标注为无 SRTP 修复、不可直接用于媒体验收。

### B39. M1.5 相对等待累积编码耗时，20ms帧只能发送约40fps

- **发现日期**：2026-10-23；状态：已改绝对deadline；第三轮同固件复位实测恢复约50fps。
- 首轮真实AFE上行：`t=12800ms sent=115 mic_drop=28`，
  `t=62890ms sent=2093 mic_drop=554`，硬件read与编码错误均为0。
  每次`next_send=now+20000`把编码与任务调度耗时计入下个周期，持续低于50fps。
- 修复：`next_send+=20000`；仅严重落后超过60ms时重建基准，
  不靠无界排队或连续突发发送掩盖。保留捕获pts，丢帧也推进时钟。
- 证据：私有原始日志位于原task artifact的`m15-takeover/device-1.log`；
  对外仅保存不含SDP/凭据的计数摘录，见接替记录。

### B40. 重采样最大输出容量不能用本次输入样本数作上界

- **发现日期**：2026-10-23；状态：修正M15保护条件；第三轮转换失败计数为0。
- 新增24→16转换保护时误要求`max_out <= in_samples`，导致
  API返回OK仍全部被丢弃：第二轮`read_ok=1247 capture_cvt_failed=1247`。
- API的`get_max_out_sample_num`返回最坏容量，包含流状态/块边界余量，
  不能直接当作本次实际输出，也不能仅因降采样就假定它少于输入。
- 当前保护改为检查查询返回码、非零/有界容量（8192每声道样本）、process返回码、
  `actual_out <= capacity`，只把实际输出交AFE。硬件读取和转换错误分别计数。
- 代码：`patches/stackchan-webrtc-media.patch`中的ReadAudioData保护；
  依据：已安装`esp_ae_rate_cvt.h`的容量与actual in/out参数契约，第二轮真机计数。


### B41. 真实双向探针播放后出现多轮VAD/ASR，回声假设仍需控制验证

- **日期**：2026-10-23；状态：双向链路历史观测，回声归因未证，M1.5 尚未验收。
- 用户确认本轮设备旁无人说话、环境基本安静；初始fixture后已切真实mic。
  日志记录 `decoded=980 write_ok=980`，期间多次speech/ASR/response事件。
  `write_ok` 只表示硬件写入 API 成功调用次数，不是 RTP 包数、实际听见或播放完成证据。
- 关键限制：该轮控制事件队列丢118项，未记录转写与先前回答的匹配，
  不能把事件重复直接等同于AEC缺陷，更不能归因到某个提供方。
- 后续候选过滤非关键增量事件、加入仅计数的规范化精确匹配；文本重合阳性也不能
  独立证明声学回声，不得据此止损或判定 AEC 失败。阴性不能单凭“无转写”放行。
  后续应先补公共媒体生命周期与受控验收入口，再按明确“开始”口令对时运行有效对照。
- 证据：`docs/device_diag_evidence/2026-10-23-m15/device-3-repeat-counts.log`、
  `docs/webrtc-media-takeover-2026-10-23.md`以及本轮用户的现场确认。

**B41后续反证（2026-10-23）**：关喇叭近端控制实测`write_ok=0`时，
在约定读句子之前仍有其他ASR/response；当时未重新确认安静窗口，不能归因，
但说明不能仅按重复事件判声学回声。用户三句随后达到关键词3/3覆盖；
该轮未按“开始”口令对时，且旧统计未按独立 input item 去重，
只保留为采集/ASR 链路观测，**不登记正式 T1 通过**。
此前回声嫌疑仍待按明确“开始”口令同步的控制实验验证，不能宣告AEC或迁移目标失败。
见`docs/webrtc-media-takeover-2026-10-23.md`后续启动约定。

### B42. 限时媒体探针不能代替产品生命周期与停止验收

- **日期/证据基线**：2026-10-23，`2b69e06`；状态：已确认交付缺口，本轮重新开发，未验收。
- `main.cpp` 的 M1.5 分支绕开正常 `Application`；产品 `application.cc` 的 speaking
  分支仍会关闭非 Realtime 采集并重置 decoder。连续上行仅在探针观察，不代表产品完成。
- `webrtc_m15.cc` 的 `speech_started` 仅计数，`StopWebRtcPlayback` 只在整轮结束调用；
  `Codec::ResetDownlink` 尚无调用者。下行回调丢弃 pts，队列仅保存 payload，
  未交付响应内迟到 RTP 处理或重连，不能宣称自动插话可用。
- AudioService 的 epoch/提交锁只约束软件后续写入；20ms 拆块不清除已提交的 DMA 尾音，
  底层写入还可能阻塞。API 成功次数不能证明实际停止延迟，未测必须标为未验证。
- **处置**：按 [SPEC §4.8](webrtc-media-spec.md#48-本轮重新开发的执行与观测契约2026-10-23)
  与 [PLAN](webrtc-media-plan.md) 的 R0–R3，优先公共媒体会话生命周期、停止隔离、
  默认 Idle 的显式 START/STOP 入口；报告 t0、采集/下行 pts、分原因丢帧、
  独立 input item 的 ASR 去重。保持真实48k PCM、1.5.7 与 WebSocket 回退。
  本轮只离线开发验证；真机每轮等待用户“开始”，不自动复位、采集或重跑。


### B43. 原生 AFE Stop 不能作为可重用会话的 join 边界（2026-10-23）

**证据基线**：`e956b4b` 的 `xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc` Stop只清运行标志、
reset AFE/input buffer；fetch task 独立运行，输出缓冲区没有在 Stop 时同步清空或确认退出。
`AudioService::Stop()` 同样没有提供三类任务与 AFE 的完整 join。
直接销毁/重建服务或清空全局队列再开新会话，不能证明旧回调和残留音频已隔离。

**状态**：`e956b4b`先以一轮/上电保留owner规避悬空回调；后续已补M15专用AFE迭代join、
输入CaptureLifecycle锁/generation、稳定activeRun指针及固定worker。Stop返回后无当前Feed/fetch/回调在途，
清理确认Transport join与reset成功后才能删除旧Run，允许新ID显式START；失败仍保留owner并要求重启。
这是持久任务的迭代静止边界，不是销毁所有AudioService任务的完整join，也不宣称自动重连或真机多轮已通过。
见 [重新开发记录](webrtc-media-redevelopment-2026-10-23.md)。

### B44. 采集预热等待不等于排空DMA；RX延期关闭论据已收窄（2026-10-23）

**证据**：`audio_codec.h`的DMA为6×240帧，CoreS3输入24k（约60ms）；
旧AudioInputTask预热仅`vTaskDelay(120)`，这段时间并未读取/丢弃RX缓存；AFEStop单独重置也不能排掉ADC缓存。
**论据纠正**：平台源码“TX启用时延期关闭IN”受`SOC_I2S_HW_VERSION_1`条件限制，
本机ESP32-S3为VERSION_2，不能援引该分支断言本板关闭IN会延期。
这不改变睡眠不等于主动读取排空的事实，也不据此修改现有实际排空方案。

**状态**：M15新轮实际读出并丢弃12次10ms原始源采样（24k共2880帧，大于当前DMA容量），
随后校验转换器reset及AFE ringbuf reset（接口返回1成功/-1失败），首AFE回调后才开放发送和计时。
读取、reset、Feed和输入电源timer使用统一同步边界；转换/排空/reset失败单列并结束准备。
已做代码复核、共享同步组件60项检查和两配置构建。694b695的near_20261023_02单轮已报告
drained_source_frames=2880、drain_failed=0、afe_reset_failed=0，RUNNING建立于首个预热回调之后；
只证明该轮执行了源采样排空/reset路径，不证明多轮无残留、DSP内部历史已清空或实际尾音边界。
证据见[nano-near-02-counts.log](device_diag_evidence/2026-10-23-m15/nano-near-02-counts.log)。


### B45. StackChan手动下载模式与写后ROM停留（2026-10-23）

**证据**：本轮USB自动复位未收ROM响应；用户长按底部microSD旁RST约3秒进入下载模式后，
`--before no_reset`刷入80df789成功并校验hash。`--after hard_reset`后仍见boot0x23 DOWNLOAD，
改`--before no_reset --after watchdog_reset chip_id`后见boot0x2b SPI_FAST_FLASH_BOOT，启动0x20000应用。
官方步骤：[M5Stack StackChan下载模式](https://docs.m5stack.com/en/uiflow2/stackchan/program)。

**状态**：本轮已恢复应用启动，非固件写入失败；后续不能把有串口节点等同ROM已响应，
也不能把写后仍在ROM的STATUS无响应当作应用Idle失败。M15跳过正常产品UI，Starting up屏幕文字本身不是进程状态证据。

### B46. Nano不支持64位格式，M15日志后续字段错位（2026-10-23）

**证据**：80df789启用CONFIG_LIBC_NEWLIB_NANO_FORMAT；ESP-IDF components/newlib/Kconfig说明不支持64位整型格式。
本轮实际`elapsed_ms=ld`、`submitted_samples=lu`，且后续config/transport_joined等字段出现无效值。
不能根据这类字段推断配置、时间或播放计数。见[脱敏日志](device_diag_evidence/2026-10-23-m15/restart-near-01-counts.log)。

**状态**：M15改固定栈缓冲十进制转换和%s，补read_ok/write_ok增量；离线41项边界/相邻参数检查及独立复核通过。
固件重新构建通过，交付694b695。随后获新“开始”授权的near_20261023_02中，
`t0_us=52914616`、`elapsed_ms=60000`、播放样本/后续参数均正常十进制；read_ok=6020、write_ok=0。
只修观测，不把该轮DTLS建连成功归因于日志修复，也不外推多轮或声学验证。
证据见[nano-near-02-counts.log](device_diag_evidence/2026-10-23-m15/nano-near-02-counts.log)。

### B47. 首轮受控启动DTLS阶段未完成，媒体未运行（2026-10-23）

**证据/状态**：80df789默认Idle约48秒无网络/AFE启动，显式START后HTTP200、ICE配对，停CONNECTING。
25次-0x6900是WANT_READ；准备超时安全结束，sent/decoded/ASR/drain均0，LIFECYCLE三项为1，STATUS回Idle。
旧echo-fallback日志已有同形态，不能单轮归因为此次生命周期重构，也不将WANT_READ单独当作致命错误。
**本轮只支持启动门/超时清理观察；没有近端/T1或AEC结论。** 后续设备诊断须新“开始”，不自动重跑。


### B48. 同步完整统计日志扰动20ms媒体发送（2026-10-23）

**证据**：694b695的near_20261023_02在同一发送worker每5秒同步输出完整Report；
Running每组5行、约1KB，PROGRESS到HARDWARE日志跨度约50–70ms，
最大实际SendAudio间隔90471us；mic_full在大多数5秒间隔增加4帧。
60秒共sent2948、mic_full46、停止排空6，合计3000帧，与AFE输出960000样本一致。
同worker日志阻塞与积压/丢帧的节奏吻合，不能把所有丢帧唯一归因于日志；
随后短报告候选的较短近端观测见下方补充，并非等时长/等刺激的严格A/B。
见[nano-near-02-counts.log](device_diag_evidence/2026-10-23-m15/nano-near-02-counts.log)。

**处置/状态**：离线最小修复把Running的每5秒完整Report改为一条LIVE短计数，FINAL保留完整统计；
不改队列/调度/编解码/期限，既有“迟于计划超过60ms则重置发送计划”策略也未改。
按115200 baud、8N1和短行字节数估算线速约14ms，只是字节传输估算，不含调度/锁/格式化/驱动等待，
**不是执行上界，也不是丢帧已消除的证据**。ESP-IDF5.5.4构建通过（应用0x3cbba0，余23%），
交付为4e53790，之后另获授权刷写/实测；原台账epoch12/used26/limit32不变，任何新一轮仍等用户“开始”。
本轮ASR/speech/responses均0；用户随后明确当时正在开发别的、没有看到消息。
先START倒计时再提示读句，没有同步到有效近端刺激，不构成近端正对照，不能据此判采音/识别/AEC失败。
后续须把设备准备/刷写与说话测试分开对表；准备后Idle，真正START之前先弹窗Q，
等用户看到并明确点“开始”后才START采集/倒计时，再于RUNNING后提示读句。本轮结束无追加设备动作。

**短报告候选设备补充**：4e53790的near_20261023_03先弹窗确认后START，用户确认三句已说完；
3独立ASR item均status0、关键词mask1/3/7，reason=completed，elapsed23010ms。
sent1146、mic_full/rx_full/event_full及invalid均0，停止排空mic3/events1；
AFE367680样本=1149帧=1146发送+3停止排空，最大SendAudio间隔30317us、队列年龄90634us。
read_ok2321/write_ok0，源排空2880帧，错误0，清理三项1后STATUS Idle。
本轮同步近端刺激与关键词覆盖成立；23.01秒不能对比旧60秒宣称长期零丢帧，ASR短日志仍同步输出。
短日志改善仅是迹象。结合本轮先Q后START、用户确认三句完成、3独立Accepted item关键内容覆盖、
config1及采集/编码/发送正常、喇叭write0/无错误，独立人工复核按SPEC §6.2判**T1单项PASS**，限本配置/本轮。
日志硬编码T1_T4=NOT_ACCEPTED表示程序不自动验收，保留原始字段；不妨碍人工单项复核。
T1规范没有满60秒或额外声学/分句指标要求；T2–T4、M1.5整体、AEC、插话/多轮/产品仍未通过。
见[live-near-03-counts.log](device_diag_evidence/2026-10-23-m15/live-near-03-counts.log)。

### B49. 写后分区校验异常，读回一致后复位恢复，原因未定（2026-10-23）

**证据**：694b695的app写入/hash校验成功，随后启动曾报分区校验错误。
esptool读回0x8000起4096字节，前3072字节与本地partition bin逐字节一致，其余均FF；
第二次watchdog_reset后正常从应用启动到Idle，并完成单轮60秒near及清理。

**状态**：启动已恢复，原因未定；没有改分区表。app写入校验与分区读回一致是不同检查，
不能据一次一致读回断言所有启动链路正确，也不能将短暂校验异常定性为已定位的分区损坏。
本轮结束串口已关闭，无自动复位或后续START。该单轮不代表连续重启/多轮稳定性通过。


**后续单轮观测**：4e53790本轮自动刷写/hash校验成功，RTS硬复位直接正常Idle，未观察到分区校验异常。
这仅是后续一轮正常启动，不定位或消除694b695写后异常根因；未改分区表，也未进行连续重启验收。


### B50. 安静环境双向出现反复短答与电流声，根因未定（2026-10-23）

**证据**：4e53790的duplex_20261023_04先弹窗确认再START，RUNNING后用户只说一次故事请求，
随后确认一直安静、无其他音源。设备反复“从前+电流声”；root立即STOP，用户确认声音已停。
运行58180ms，reason=requested；ASR17、speech18、response17，计数包含初始合法请求，不能直接等同回声触发次数。
sent2886、mic_full17、停止排空mic4，AFE930240样本=2907帧，三项闭合；
decoded/accepted/write_ok均780，submitted_samples354000，即24k下累计API音频14.75秒，
不证明连续≥30秒声学播放。read_ok5835、排空2880帧，硬件/转换/drain/reset错误0。
LIFECYCLE三项1，STATUS回Idle；用户确认STOP后声音停，无声学延迟测量，无再START。
见[duplex-04-counts.log](device_diag_evidence/2026-10-23-m15/duplex-04-counts.log)。

**状态**：本轮可听播放质量与重复行为未通过，正式T2证据不足、AEC未定。
缺正常/最高预期音量、多次连续≥30秒、实际声学回声对照等完整控制；
不能将循环唯一归因为声学回声、服务器缺陷或本地格式错误。串口打开实际触发USB reset，不能当同进程多轮证据。
near03的T1单轮通过保持，T3/T4、M1.5整体、重连/产品尚未通过。
**2026-10-24离线补充**：已增加TURN事件元数据与Codec样本计数，AFE等待/CodecStats/最终TURN独立复核通过。
TURN初稿共享essential队列的问题已修为独立8项诊断队列，满仅记drop；原essential8项保持。
最多64项记录在结束后打印，接收与快照时间分离，缺失/非法、截断/溢出、停止丢弃分别记录；
FNV32仅供诊断，不作RTP归属。结束后报告48k/24k样本、直方图、accepted/submitted样本与峰值/端点。
只观察`response.done.response.status`；`status_details`实际返回才记录，两个会话bool为三态观测不改配置。
媒体/WS构建与四源码补丁重建通过，详见重新开发记录；实现4d5fa0b已推送并刷入，
随后完成05准备超时与06短诊断，观测到turn_detected取消，声音/输入根因仍未定，不声称声音已修复。

### B51. M15的20ms AFE等待短于32ms音频块，产生高频失败日志（2026-10-23）

**证据**：AFE日志feed/fetch均512样本、16k，即32ms块。M15使用20ms有限fetch等待，
near03打印731次、duplex04打印1845次`AfeAudioProcessor: Error code:-1`；
正常近端T1同样出现，不能把该日志总数当成实际采样错误或双向循环根因。
`esp_afe_sr_iface.h`将ticks_to_wait定义为等待结果的超时，结果提前到达可返回，并非每次固定睡眠。
20ms窗口短于音频块，预期等待与真实ESP_FAIL在现有日志中没有区分，高频同步输出也可能扰动调度。

**处置/状态**：最小改动仅M15等待20→100ms（至少1tick），保留所有错误日志/处理、
running检查、input/fetch锁、Stop迭代静止边界；普通模式仍portMAX_DELAY，不改音频数据/模型/AGC/AEC。
100ms覆盖块周期及调度余量，但不是Stop硬上界，锁等待/回调仍有额外耗时。
**2026-10-24验证**：跟踪AFE lifecycle patch已同步，独立复核通过。
root从xiaozhi HEAD e77dedb1309153bb63fed285772962c920c97dd4依次应用vendor/Aliyun/media/AFE四补丁，
AudioService h/cc与AFE h/cc均和live逐字节一致。IDF5.5.4媒体构建通过（0x3ccc30，余23%），
独立WebSocket回退构建通过（0x3a37a0，余26%）；两构建/重建在控制器外，原台账epoch12/26of32不变。
实现4d5fa0b已推送并刷入，05/06分别获新弹窗确认后执行；05准备超时，06运行30.002秒。
06实际AFE `Error code:-1`为0，100ms等待改善已有本轮证据，不承诺所有条件均不超时，亦不证明声音已修复。

**本次刷写准备记录**：首次default_reset无串口数据；用户弹窗确认手动下载后，no_reset刷写/hash通过。
RTS硬复位停ROM下载，单独watchdog_reset chip_id后正常Idle；观察20秒无START/POST/peer/AFE。
该操作只验证候选启动/Idle，不改变分区，也不定位历史启动或声音异常根因。


### B52. 三个回答被turn_detected取消，输入来源仍未定（2026-10-24）

**证据**：4d5fa0b的duplex05获新弹窗开始后stage4准备超时，无RUNNING/媒体，回Idle。
再获新弹窗开始后，同串口/同进程、无reset/重刷的duplex06运行30002ms并回Idle，串口已关。
首条ASR在22.075秒；全轮4个独立ASR、4speech、3response，TURN存22项、丢弃/溢出0。
3个不同response标签均done.status=cancelled、detail_reason=turn_detected，与相邻新speech_started接收相差0/5/1ms；
create_response/interrupt_response回显均true，客户端无主动cancel发送代码。
本轮可确定新turn检测取消了回答，不能据此唯一定位声学回声、电气串扰、噪声或服务端缺陷。
用户已确认本轮读完后一直安静，无其他说话/音源；仍听到“从前从前”，说3次后没声音。
停止时间与30秒窗口对齐不清楚；日志为三个cancelled/turn_detected及30.002秒run_timeout，不能当AI正常结束。
未打印正文的item不推定为已验证故事文本，触发输入来源仍未定。用户要求禁止新测试，先核对真实StackChan硬件/官方实现。

下行10×480+115×960=115200个48k样本，转换/准入/提交24k均57600（2.4秒累计API音频），
write125/错误0、max_abs11161/端点0；这不是无噪声或连续播放证明。
上行AFE479040/320=1497帧，而1490发送+3满+3停止排空=1496，1帧未单列；
可能涉及转换预热空包或Pop后Continue超时，源码允许但本轮未实证，不伪闭合。
硬件read3018/write125、源排空2880，硬件/转换/reset错误0，LIFECYCLE三项1、STATUS Idle。

**状态**：新turn取消机制有事件实据，根因未定；正式T2仍未验收，不改声音、VAD或cancel策略掩盖现象。
05准备失败→06有效运行仅为一次同进程重开观测，不代表长期多轮稳定。代码不变，台账26/32。
见[05日志](device_diag_evidence/2026-10-23-m15/duplex-05-counts.log)与[06日志](device_diag_evidence/2026-10-23-m15/duplex-06-counts.log)。

### B53. 旧“无喇叭回采 / 最多2通道”前提应撤回，先交DSH核实映射（2026-10-24）

**证据**：[StackChan Core C156官方页面](https://docs.m5stack.com/en/core/StackChan_Core)当前链接的
[原理图第4页](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/490/Sch_M5_CoreS3_v1.0.pdf)显示
AW88298 SPK_VOP/VON经电阻/电容网络接AEC_P/N，再接ES7210 MIC3P/N；MIC1/2接物理麦克风。
因此A2、“硬件清单”及config.h中无喇叭回采的表述过强，不能再作硬件不存在的证明。
该PDF仍名CoreS3_v1.0，用户实板revision、有效参考波形和当前slot映射未实证，不宣称设备AEC已可用。

当前managed esp_codec_dev的`audio_codec_data_i2s.c::_i2s_valid_fmt`拒绝0/奇数通道，
历史3ch报错不证明最多2ch或4ch TDM不可能。此处只撤回绝对上限，不现场改通道或启用AEC。
本地音频引脚/初始化与官方StackChan同路也不证明实际音质正确；M5Unified #303的样本对顺序修复，
须对照实际IDF+esp_codec_dev路径才能判断适用性，不能盲加swap。

**状态**：用户授权整理文档交DSH接续开发；本次无代码/构建/设备操作。
下一步先完成硬件/驱动对照与单变量修复，实际测试仍须新Q点击开始；M1.5/T2及产品未验收。
完整坐标、证据、退出条件与边界见[DSH交接](webrtc-media-dsh-handoff-2026-10-24.md)。

### B54. （2026-10-26 更正：见 B57）原判“MIC3 未被读”有误

> 本文的“MIC3 不在读取范围 / 4-slot 死配置导致参考缺失”结论已撤回。
> ES7210 TDM-I²S 顺序为 `1,3,2,4`，2-slot 实际是 MIC1+MIC3 参考；见 B57。

### B54. MIC3 参考通路存在，但 2 通道采集从未读取它；4-slot 初始化是死配置

**证据**（详见[硬件/驱动审计](webrtc-media-hardware-driver-audit-2026-10-24.md)）：
1. 官方 C156 页面所链[原理图第4页](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/490/Sch_M5_CoreS3_v1.0.pdf)
   显示 `SPK_VOP/VON` 经 `150K/1% + 22pF + 1uF` 成为 `AEC_P/AEC_N`，接
   ES7210 `MIC3P(31)/MIC3N(32)`；MIC1/MIC2 才是物理麦克风，MIC4 短接 GND。
2. 官方固件与本项目都把 ES7210 配成 `MIC1|MIC2|MIC3`；驱动在 `mic_num>=3` 时写
   `REG12=0x02` 进入 TDM 模式（`device/es7210/es7210.c`）。
3. I2S RX 初始化写 `slot_mask=SLOT0..3`、`total_slot=AUTO`，但
   `esp_codec_dev 1.5.11` 的 `platform/audio_codec_data_i2s.c::set_drv_fs()`
   在 `channel=2` 打开时重写 `total_slot=fs->channel(2)`、`ws_width=16`、
   `slot_mask=0x3`。**初始化里的 4 个 slot 从未生效**。
4. 因此当前上行只含 MIC1/MIC2；按 TDM 顺序 MIC3 参考不在数据里（按 ES7210 TDM 顺序与 2-slot 重配推断；实板线上 slot 映射仍未实测）；而 `AUDIO_INPUT_REFERENCE=true`
   让 AFE 用 `"MR"`，把 ch1 当参考——按 TDM 顺序 ch1 应为 MIC2（按 ES7210 TDM 顺序应为 MIC2；实板 slot 映射未测）。开启设备 AEC 时会把
   用户语音一起消掉（历史实测峰值 4036→120）。

**边界**：`esp_codec_dev` 只拒绝 0/奇数通道；4ch 在库层合法（`total_slot=fs->channel`），
历史 `Not support channel 3` 不是“最多 2 通道”。但 4ch+播放受共享 I2S 帧宽
（RX 4×16bit vs TX 2×16bit）与 AW88298 16-bit 立体声格式约束，本轮未启用。

**状态**：设计/驱动事实已核实；实板 revision、TDM 实际帧长/slot 对齐、AEC_P/N 幅度、
MIC3 增益仍未示波器验证。**不要据此直接启 AEC、改通道或加 swap。**
M1.5 的双向循环不能归因于“参考串入”，因为参考根本没有被读；最简约解释仍是
声学耦合 + 无设备 AEC + 服务端 AEC 未证。

### B55. `AUDIT` 本地命令：量化扬声器到当前上行的耦合，不替代 AEC 验收

**实现**：M15 诊断固件新增 USB 命令 `AUDIT <3..10>`：本地播放 1 kHz 正弦
（幅度 1024 ≈ -30 dBFS，**不改持久化音量**），用现有 2 通道路径采集，三段
tone/silence/tone（各段间 200 ms 保护，另有 200 ms 预热丢弃），输出每通道 RMS（dBFS×10）与 peak，
以及 `tone1 - silence` 的 dB 差。不联网、不开 AFE、不启 AEC；不改持久化音量，
结束关闭输入输出。
保护段 200 ms、预热丢弃 200 ms、总时限 seconds+2、失败 10 次中止。
纯逻辑在 `firmware/main/hal/webrtc/channel_audit.h`，离线检查 `channel-audit`
（26 断言，cache=never）；命令解析在 `console_command.h`（只接受 3..10）。
**约束**：只在首次媒体运行前可用（`g_audio_initialized` 为真时拒绝；AudioService 的
15 s 空闲电源定时器可能在审计中途关闭 codec，需重启获得干净状态）；缓冲堆分配
（主任务栈 8 KB）；不调用 `feedTheDog()`（实现为 10 ms 延时），I2S 阻塞调用自身让出 CPU；
`seconds + 2` 软时限与 10 次失败中止。

**边界**：只测当前 2 通道里的声学+电气总耦合，不能单独区分两者；参考通道不在流内。
M5Unified #303/#308 的 fs/2 镜像音来自其 32-bit 打包，本项目走 esp_codec_dev 原生
16-bit 写入，**不能照搬 swap**；#303 仍开着，用户报告的“电流声”来源未定。

**状态**：代码/离线检查/构建完成，**未做设备运行**（HOLD）。下一轮须新弹窗 Q：
先 `AUDIT 6`，再做正常双向与“遮住 MIC1/MIC2 出声孔”的对照；若遮麦后 turn_detected
消失则指向声学路径，否则再查电气/噪声/服务端。

### B56. （2026-10-26 更正：见 B57）AUDIT 实测与“MIC3 未读”解释

> 本文的耦合数据有效，但“MIC3 不在 2ch 里”的解释已撤回：
> ch1 实际是 MIC3 参考（TDM-I²S 1,3,2,4）；封麦孔后 ch1 不变正是电气参考的证据。
> 见 B57。

### B56. AUDIT 6 实测：扬声器输出显著进入当前 2ch 上行（+24~27 dB over silence）

**证据**：2026-10-24 刷入 `97864ef` 候选（M15 + AUDIT，app `0x3cd8f0`），
Idle 下执行 `AUDIT 6`（1 kHz，幅度 1024，不改音量），脱敏日志
`docs/device_diag_evidence/2026-10-24-m15/audit-06-idle.log`：

```
AUDIT seconds=6 slots=300 write_fail=0 read_fail=0 aborted=0 volume=unchanged tone_amp=1024
AUDIT tone1 frames=43200 ch0_db10=-228 ch0_peak=4314 ch1_db10=-198 ch1_peak=5425
AUDIT silence frames=43200 ch0_db10=-465 ch0_peak=528 ch1_db10=-471 ch1_peak=522
AUDIT tone2 frames=43200 ch0_db10=-228 ch0_peak=4337 ch1_db10=-197 ch1_peak=5411
AUDIT echo_delta_db10 ch0=237 ch1=273 aec=undetermined local_only=1
```

结束回 Idle，未联网、无 POST/peer 会话；输入/输出已关闭。

**解读**：扬声器输出确实显著进入了当前 2 通路上行（tone 比静音底噪高约 24–27 dB），
足以在助手播放 TTS 时被服务端 VAD 当成新语音并取消回答；这与 B50/B52 的
“turn_detected 取消”现象一致。**但不能区分声学耦合与电气/串扰**：
- 本轮只测到 MIC1/MIC2 的合计耦合，MIC3 参考仍不在 2-slot 读取范围；
- 遮住麦克风出声孔后重跑 AUDIT 是最小区分实验：差值骤降 ⇒ 声学为主；
  基本不变 ⇒ 电气/其他路径。

**状态**：设备已刷入候选并回 Idle；未做 duplex/云端测试，M1.5/T2 仍未通过。

**2026-10-24 遮脸复测（反射遮挡，非最终对照）**：用纸/手轻遮 CoreS3 正面后再跑
`AUDIT 6`，耦合**变大**：tone1 ch0 -22.8→**-16.0 dBFS**、ch1 -19.8→**-14.6 dBFS**
（峰值 4314/5425→8075/9199），静音底噪基本不变（-44.9/-47.4）。
`echo_delta` 从 +23.7/+27.3 变为 **+28.9/+32.8 dB**。
硬质反射面贴近麦克风会产生约 **+6 dB 压力加倍**，量级与此吻合；
这更像“麦克风在脸后、听到声学路径”，而不是电气串扰（电气路径不应被纸/手改变）。
**吸声复测（厚布/毛巾）**：tone 为 -25.1/-22.7 dBFS，silence 为 -53.5/-55.4 dBFS，
`echo_delta` +28.4/+32.7 dB。与无遮挡相比，**tone 只降 2–3 dB，而环境底噪降 7–8 dB**。
若 1 kHz 是经脸部/麦克风孔进入的空气声，厚布应把 tone 衰减远大于 2–3 dB；
因此该 tone 很可能**不是（或不只是）脸部空气路径**：可能是结构传导，或 MIC3 参考/电气
路径已以某种方式混入当前 2 通道（slot 映射未实测，不能排除）。
证据：`audit-06-face-covered-reflective.log`、`audit-06-face-covered-absorbent.log`。
**官方布局纠正**：双麦在 CoreS3 正面下方黑边、相机右侧两针孔；1W 扬声器在侧面开槽面板；
body 无喇叭/麦克风（Zephyr 板级文档）。
**遮侧面扬声器复测**：tone 反而 -17.7/-18.1 dBFS（比无遮挡高 2~5 dB），
说明布与设备接触引入了结构传导/反射，未形成有效声学隔离；该轮不能当“声源被遮”的对照。
**下一步判据**：只封住正面两个麦克风针孔（扬声器不动）——tone 大幅下降 ⇒ 声学经麦孔；
基本不变 ⇒ 电气/参考路径（说明 MIC3 参考可能以某种方式混入 2 通道，slot 映射假设需修正）。

### B57. 【更正】TDM-I²S 顺序 1,3,2,4：MIC3 参考一直在 2 通道上行里

**证据**：
1. ES7210 datasheet Rev 18.0（2021-10）**第 6 页 Figure 2e（TDM I²S）** 的 LRCK
   通道顺序是 **`Channel 1 → Channel 3 → Channel 2 → Channel 4`**（渲染页面核对，
   非文本抽取假象）；Figure 2f（TDM Left Justified）才是 1→2→3→4。
2. 本项目驱动走 `ES_I2S_FMT_I2S` 且 `mic_num>=3` 时写 `REG12=0x02` 进入 TDM
   （`esp_codec_dev/device/es7210/es7210.c`），即 Figure 2e 模式。
3. `esp_codec_dev` 以 `channel=2` 打开时把 RX 重配为 2 slot；因此两个 slot 是
   **MIC1 + MIC3（AEC_P/N 参考）**，不是 MIC1+MIC2。
4. 设备实测：`AUDIT 6` 在只封住正面麦克风针孔后，ch1 的 1 kHz 耦合几乎不变
   （-19.8 → -18.4 dBFS），底噪下降；ch1 表现与“电气参考”一致
   （`docs/device_diag_evidence/2026-10-24-m15/audit-06-mic-taped.log`，
   该文件在下一笔提交补入）。

**更正范围**：B54、B56、[硬件/驱动审计](webrtc-media-hardware-driver-audit-2026-10-24.md)
F1/F2、`AGENTS.md`、`aec-limitation.md`、`stackchan-hardware.md`、spec/plan/redevelopment
中所有“MIC3 未被读 / ch1=MIC2 / AFE MR 错误”的表述全部撤回。
`input_reference=true` 与 AFE `"MR"` 是**正确**配置。

**含义**：
- 设备 AEC 的参考数据一直在上行里，缺的是 `CONFIG_USE_DEVICE_AEC`（当前未启用）；
- 历史“开 AEC 吞语音（4036→120）”需要重评（双讲/延迟/参考增益/模式），
  不能用它证明“无参考”或“AEC 不可行”；
- 不需要为 AEC 改 4ch/帧宽；只有要读 MIC2/MIC4 时才需要另设计。

**状态**：文档已更正；未改代码/AEC/通道/swap，未刷机。
下一步需新授权：有条件启用 `CONFIG_USE_DEVICE_AEC`，先本地验证回声下降，
再做双讲验证不吞用户语音；仍按新弹窗 Q 执行。

### B58. 开启设备 AEC 后实时性不足：发送仅 ~25% 实时、麦克风队列溢出（本轮不判 AEC 有效性）

**证据**：2026-10-26 刷入 M15 + `CONFIG_USE_DEVICE_AEC=y`（其余不变，app `0x3cd8c0`）；
单句柄执行 `START aec06 duplex 90`，RUNNING 正常（session_tag、config=1），90s 后
`FINAL state=Done reason=run_timeout`，脱敏日志
`docs/device_diag_evidence/2026-10-24-m15/aec-duplex-06.log`：

- `sent=1128`（约 12.5 包/s；20ms 帧应为 50/s，即约 25% 实时）；
- `mic_full=3365`（麦克风队列持续满），`max_afe_gap_us=70502`、`max_send_gap_us=97125`；
- `afe_samples=1440000`（90s×16k，AFE 侧实时产出）；
- `decoded=0`、`speech=0`、`responses=0`、`items=0`、`T1_T4=NOT_ACCEPTED`；
- 无 panic/重启、`failures=0`、`LIFECYCLE capture_quiescent=1 transport_joined=1`。

**解读**：AEC 能初始化并跑完 90s，但开启后 AFE 回调出现约 70ms 间隔，
发送端被拖慢到 1/4 实时、麦克风队列溢出；服务端未获得可识别语音/未产生 turn。
因此**本轮不能用来判断 AEC 是否有效，也不能归因回声**。
这不是“AEC 无效”的结论，而是 **AEC 开启后的实时预算/调度问题**。

**下一步**：先做 AEC 性能与管线核验（AFE 任务核/优先级、feed/fetch 尺寸、
AEC 模式/内存、发送 pacing 与背压），确保 near 模式能稳定 50 包/s 且 ASR 正常，
再做 duplex 双讲；仍须新弹窗 Q，不以此轮结果判定 AEC。

**2026-10-26 near 对照（AEC 开启、无播放）**：`START perf02 near 30`：
`sent=1453 / 30s ≈ 48.4 包/秒`、`mic_full=0`、`afe_samples=465600`（29.1s×16k）、
`max_afe_gap=113ms`，RUNNING 在 START 后 **19.4s**。说明 AEC 本身能实时工作；
duplex 的 25% 速率是 **TX/播放路径（全双工）** 引起的，不是 AEC 普遍性变慢。
本轮用户按 5 秒说话，整段落在 RUNNING 之前，`speech/items=0` 不能用于判断 AEC 吞音。
日志：`docs/device_diag_evidence/2026-10-24-m15/aec-near-02.log`。

**2026-10-26 第二次 run 对照（关键）**：同一 boot 内紧接着跑 `START perf03 near 60`：
`sent=753 / 60s ≈ 12.55 包/秒`、`mic_full=2240`（约 37/s 持续增长）、AFE 仍实时
（`afe_samples=960000`），RUNNING 17.3s。与 perf02（复位后第一次 run，48.4/s、
`mic_full=0`）对比，**退化出现在第二次及以后的 run**，不是 near/duplex 或 TX 本身。
此前 aec06 也是 aec05 之后的第二次 run（12.5/s），同样吻合。
非 AEC 固件的历史 06 是第二次 run 却约 50/s，因此更像 **AEC 开启后的重复 run 复用问题**，
而非 AEC 计算量本身。
日志：`audit-06-*.log`、`aec-near-02.log`、`aec-near-03-second-run.log`、`aec-duplex-06.log`。
**下一实验**：每次测试前物理复位，只跑“第一次 run”，先验证 AEC 首轮行为；
复用问题单独立项，不混入 AEC 效果判断。

**2026-10-26 AEC 首轮 duplex 准备超时**：复位后第一次 run 执行
`START perf04 duplex 60`，`FINAL state=Failed reason=prepare_timeout stage=4 config=0
session_tag=00000000`，无 RUNNING、无媒体、AFE 0 样本；用户按 30 秒说话时尚未开放准入。
这是连接准备层失败（同类于此前的 05），**不是 AEC 结果**，不能据此判断回声/吞音。
日志：`docs/device_diag_evidence/2026-10-24-m15/aec-duplex-04-prepare-timeout.log`。

**2026-10-26 AEC 首轮 duplex 仍非实时（更正“第二次 run”假设）**：复位后第一次 run
`START perf06 duplex 120`，RUNNING 在 START 后约 7.4s；用户按“现在说”提示正常说话。
但 `sent=1514 / 120s ≈ 12.6 包/秒`，`mic_full=4479` 且从 RUNNING 后 3 秒就开始增长
（33.8s 时 `sent=35 mic_full=100`）；`decoded/speech/items/responses` 全 0，
`afe_samples=1920000`（120s×16k，AFE 实时）。用户语音在队列中严重滞后，
服务端在窗口结束前未获得可识别语音，因此仍**不能判 AEC**。
对比：perf02 near 第一次 run 48.4/s、mic_full=0；perf05 duplex 第一次 run 前 8s 约 48/s；
perf03 near 第二次、aec06 duplex 第二次均 12.5/s。
**结论**：慢速不是单纯“第二次 run”，而是 **AEC + duplex/输出路径（speaker=1）不稳定**，
可出现在第一次 run；near 首轮是唯一稳定实时的组合。需要调度/背压插桩与修复，
不能再靠盲测。日志：`aec-duplex-06-first-run.log`。

### B59. 发送耗时插桩：SendAudio 不是瓶颈；无语音 duplex 稳定 ~49 包/秒

**证据**：2026-10-26 插桩固件（`2d3cbd0`，AEC=y，app `0x3cd9e0`）：
- 无语音 duplex 60 连跑 3 次（同一 boot/句柄）：base1 `sent=2959`（49.3/s）、
  base3 `sent=2973`（49.6/s）；`send_calls` 与 sent 一致，
  **`send_avg_us=2527`、`send_max_us=5976`（约 2.5/6 ms）**，`loop_max_us=98149`，
  `mic_full=35/19`；base2 FINAL 正常（60.009s，报告行被批量采集提前截断，未取到终值）。
- 对照：此前慢速 run（12.5/s、`mic_full` 持续增长）都发生在用户说话之后；
  无语音批次 3/3 未复现慢速。
- 插桩本身经独立只读评审 APPROVE（行为中性），评审附注：`loop_max` 在
  `vTaskDelay(1)` 前采样，看不到唤醒延迟；`mic_empty` 计的是循环次数。

**解读**：`Transport.SendAudio` 单次 2.5ms、最大 6ms，不是 80ms 阻塞；
慢速时 worker 的停顿来自别处（说话后服务端事件/网络活动、日志/调度、WiFi PS 等），
尚不能归因 AEC 或 duplex。**没有一次“慢速 run”的 TIMING 数据**，根因未定。

**状态**：设备 Idle，未继续测试。下一轮：在无语音条件下批量复现慢速并取 TIMING；
同时评估 `WIFI_PS_NONE` 与事件处理路径（cJSON/日志）作为低风险候选，先离线复核。
日志：`docs/device_diag_evidence/2026-10-24-m15/aec-duplex-batch-base1..3.log`。
