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
