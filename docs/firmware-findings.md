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

---

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

### B7. CMake `GLOB_RECURSE` 不重新扫描新增文件

新增 `ogg_opus_muxer.cc` 后链接报 `undefined reference`，因为 `file(GLOB_RECURSE ...)`
在**配置期**展开。需要 `idf.py reconfigure`（或删 `build/`）。
