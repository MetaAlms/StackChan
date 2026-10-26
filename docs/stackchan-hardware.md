# StackChan 硬件清单与配置对照

后续开发的事实底稿。**结论优先来自官方文档**，代码取值来自本仓库，两者不一致的地方单独标注。

**2026-10-24 审计纠正**：下表和§4.1的“没有扬声器回采参考”是历史过强断言，不再作为硬件事实使用。
[StackChan Core C156官方页面](https://docs.m5stack.com/en/core/StackChan_Core)所链接的
[原理图第4页](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/490/Sch_M5_CoreS3_v1.0.pdf)显示喇叭信号接ES7210 MIC3。
官方设计存在通路；用户实板revision、有效参考波形及当前驱动slot映射仍待确认，不等于AEC已经可用。
历史3通道失败也不证明驱动最多2通道；接续审计见[DSH交接](webrtc-media-dsh-handoff-2026-10-24.md)。

来源：
- [M5Stack StackChan 产品文档](https://docs.m5stack.com/en/StackChan)
- [Zephyr 板级文档 m5stack_stackchan](https://docs.zephyrproject.org/latest/boards/m5stack/m5stack_stackchan/doc)（硬件描述最完整）
- [StackChan ESPHome 集成](https://docs.m5stack.com/en/homeassistant/devices/stackchan)

---

## 1. StackChan Core（主控，即 M5Stack CoreS3）

| 部件 | 型号 | 说明 |
|---|---|---|
| SoC | **ESP32-S3** | 双核 LX7 @240MHz，Wi-Fi + BLE |
| 内存 | 16MB Flash / **8MB PSRAM** | PSRAM 用于 LVGL 图像缓存（代码里申请 2MB） |
| 显示 | **ILI9342C** | 2.0" IPS，320×240，SPI |
| 触摸 | **FT6336U** | 电容多点触摸 |
| 扬声器 | **AW88298** | 1W，I2S 功放 |
| 麦克风 | **ES7210** ADC + **双麦克风** | ⚠️ 只有麦克风，**没有扬声器回采参考通道** |
| 摄像头 | **GC0308** | 0.3MP，DVP |
| 六轴 IMU | **BMI270** | |
| 磁力计 | **BMM150** | |
| 接近/环境光 | **LTR-553ALS-WA** | |
| RTC | **BM8563** | |
| 电源管理 | **AXP2101** | |
| GPIO 扩展 | **AW9523B** | |
| 存储 | microSD | |

## 2. 机器人身体

| 部件 | 型号 | 说明 |
|---|---|---|
| 电池 | 550mAh + **INA226** 电量计 | |
| 舵机 | **2× SCS0009** 串行总线舵机 | UART1，**GPIO6/7**，**1Mbps 半双工** |
| IO 扩展 | **PY32L020** | 舵机电源控制 + **12 颗 RGB LED** |
| 触摸面板 | **Si12T** | 三区（头顶等） |
| 红外发射 | GPIO5 | |
| 红外接收 | **IRM56384** | GPIO10 |
| NFC | **ST25R3916** | |
| 扩展口 | 3× Grove | |

---

## 3. 代码中的引脚与音频参数

来自 [main/hal/board/config.h](../firmware/main/hal/board/config.h)：

```c
#define AUDIO_INPUT_REFERENCE    true      // ⚠️ 见下方"陷阱"
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_0
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_33
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_34
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_14     // 麦克风数据入
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_13     // 喇叭数据出

#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_12
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_11
#define AUDIO_CODEC_AW88298_ADDR AW88298_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR
```

**采样率链路**（容易踩坑，记清楚）：

```
ES7210 采集 24kHz；当前只读 2 个 TDM slot = MIC1/MIC2（MIC3 参考未读）
  └─ I2S 传输 24kHz（日志：STD Mode 1 bits:16/16 channel:2 sample_rate:24000）
      └─ AudioService::ReadAudioData 重采样 24k → 16k
          └─ AFE 处理，输出 16kHz 单声道，60ms = 960 样本
              └─ Opus 编码 16kHz / 60ms
                  └─ 上行
```

下行反向：Opus 解码 16k → 重采样 16k → 24k → I2S → AW88298。

---

## 4. ⚠️ 硬件陷阱（开发前必读）

### 4.1a 物理开孔位置（官方图片，2026-10-24）

- **双麦克风**：CoreS3 **正面下方黑边、相机镜头右侧的两个小圆孔**（旁边是 LTR-553 光线/接近传感器）。
- **1W 扬声器**：CoreS3 **侧面带横向开槽的白色面板**（官方标注 `1W SPEAKER`）。
- 机器人 body 只有电池、两个舵机、LED、触摸、IR/NFC，**没有扬声器也没有麦克风**
  （[Zephyr 板级文档](https://docs.zephyrproject.org/latest/boards/m5stack/m5stack_stackchan/doc/index.html) 的 StackChan Core / Robot body 清单）。
- 参考图：[C156 官方图 01](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1254/C156_StackChan_Core_main_pictures_01.webp)。

**影响**：之前“遮住整个脸”的对照同时盖住了麦克风孔与部分声场，不能单独证明声学/电气；
正确对照是**只封麦克风针孔**（麦克风暴露 vs 封孔），扬声器保持不被触碰。

### 4.1 参考通路存在，但当前 2 通道采集读不到它

```c
// config.h
#define AUDIO_INPUT_REFERENCE true
```

官方 C156 原理图第 4 页确认：`SPK_VOP/VON` 经 `150K/1% + 22pF + 1uF` 形成
`AEC_P/AEC_N`，接到 **ES7210 MIC3P/N**；MIC1/MIC2 才是物理麦克风。
所以“硬件没有回采”是错的。

但当前驱动把 ES7210 配成 `MIC1|MIC2|MIC3`（3 麦 ⇒ TDM 模式），
I2S RX 初始化虽写 `SLOT0..3`，`esp_codec_dev` 打开 2 通道时会重写
`total_slot=2 / ws_width=16`，**4-slot 初始化不生效**。结果：
- 上行只有 MIC1/MIC2，**按 TDM 顺序 MIC3（参考）不在读取范围**（按 ES7210 TDM 顺序与 2-slot 重配推断；实板线上 slot 映射仍未实测）；
- AFE 按 `AUDIO_INPUT_REFERENCE=true` 生成 `"MR"`，把 ch1 当参考，
  按 TDM 顺序 ch1 应为 MIC2（按 ES7210 TDM 顺序应为 MIC2；实板 slot 映射未测）；
- 开启设备侧 AEC 时会拿 MIC2 当参考，把用户语音一起消掉
  （实测 AFE 输出峰值 4036 → 120）。

4 通道 TDM 在库层可行，但 RX 4×16bit 与 TX 2×16bit 共享 BCLK/WS 不同频，
AW88298 当前也是 16-bit 立体声；需先对齐帧宽或做仅 RX 的静默采集。
详见 [硬件/驱动审计](webrtc-media-hardware-driver-audit-2026-10-24.md)。

### 4.2 ES7210 增益的量程上限是 37.5 dB

```c
// es7210 驱动 get_db()：37 以上一律返回 GAIN_37_5DB
input_gain_ = 60;   // 看起来像 60dB，实际被钳到 37.5dB
```

API 参数单位是 **dB（float）**，不是 0.5dB 步进。**写 60 和写 85 效果完全相同**，
调试增益时容易误判"改了没用"。

### 4.3 舵机是 1Mbps 半双工串行总线

UART1 + GPIO6/7，**半双工**。直接接普通串口工具会失败。

### 4.4 省电策略会导致 USB 断连

```
M5Stack-StackChan-Board: Init power save timer: sleep=300 s, shutdown=600 s
```

电池供电且 5 分钟无操作会休眠、10 分钟关机。**调试串口时如果设备突然"失联"，
先按电源键唤醒**，不是固件崩溃。


### 4.5 CoreS3采集DMA与跨轮缓存边界

当前`AudioCodec`配置6个DMA描述符、每描述符240帧；24kHz输入对应约60ms源音频容量
（`audio/audio_codec.h`、`main/hal/board/cores3_audio_codec.cc`）。
`esp_codec_dev/platform/audio_codec_data_i2s.c`在S3配对TX仍工作时可能延期关闭IN，因此不能把
`EnableInput(false/true)`当作RX缓存已刷新；单纯等待也不会读空缓存。
M15在新轮读取丢弃120ms源采样后重置转换器/AFE缓存，具体实现和验证边界见
[findings B44](firmware-findings.md#b44-采集预热等待不会排空dma关闭in也未必立即关rx2026-10-23)。


### 4.6 手动下载模式

M5Stack官方步骤为长按底部microSD卡槽旁RST约3秒，进入下载模式后再刷写，
见[官方说明](https://docs.m5stack.com/en/uiflow2/stackchan/program)。
本机esptool USB自动复位未连接时，此步骤已使刷写成功；若写后ROM仍显示DOWNLOAD，
本轮`--after watchdog_reset`可启动应用。具体观测见[findings B45](firmware-findings.md#b45-stackchan手动下载模式与写后rom停留2026-10-23)。
