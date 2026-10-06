# StackChan 硬件清单与配置对照

后续开发的事实底稿。**结论优先来自官方文档**，代码取值来自本仓库，两者不一致的地方单独标注。

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
ES7210 采集 24kHz 2ch
  └─ I2S 传输 24kHz（日志：STD Mode 1 bits:16/16 channel:2 sample_rate:24000）
      └─ AudioService::ReadAudioData 重采样 24k → 16k
          └─ AFE 处理，输出 16kHz 单声道，60ms = 960 样本
              └─ Opus 编码 16kHz / 60ms
                  └─ 上行
```

下行反向：Opus 解码 16k → 重采样 16k → 24k → I2S → AW88298。

---

## 4. ⚠️ 硬件陷阱（开发前必读）

### 4.1 "双麦克风"被当成"麦克风 + 回声参考"

```c
// config.h
#define AUDIO_INPUT_REFERENCE true
```

这个命名会让人以为 ES7210 上有一路扬声器回采。**实际上官方文档写明是"双麦克风"**，
第 2 个通道是**第二只麦克风**，不是参考信号。

影响：
- 输入格式被 AFE 推成 `"MR"`（1 麦 + 1 假参考），第二只麦克风被浪费
- **一旦开启设备侧 AEC，它会拿"另一只麦克风听到的同一段人声"当回声去抵消**，
  把用户的语音一起消掉（实测 AFE 输出峰值 4036 → 120）

详见 [firmware-findings.md](firmware-findings.md) 第 1 节。

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
