# StackChan 配置手册（官方值 vs 本仓库）

本手册只记录**板级/电源/音频总线的配置事实**，以及本仓库与官方实现的差异。
通用开发流程见 [StackChan 开发手册](stackchan-manual.md)；踩坑时间线见
[firmware-findings.md](firmware-findings.md)。

> 原则：配置值优先以 **M5Unified / 官方原理图**为准；本仓库的每个偏差都要有
> 原因、影响、修复和验证状态。不要用“能跑一次”代替配置正确。

---

## 1. 官方依据

- M5Unified CoreS3/StackChan 电源初始化：
  <https://github.com/m5stack/M5Unified/blob/master/src/utility/Power_Class.inl#L411-L424>
- M5Unified CoreS3/StackChan 扬声器使能：
  <https://github.com/m5stack/M5Unified/blob/master/src/M5Unified.inl#L606-L628>
- CoreS3 官方原理图（7 页，音频在第 4 页，AW9523 在第 5 页）：
  <https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/490/Sch_M5_CoreS3_v1.0.pdf>
- StackChan 开源资料（BSP/固件入口）：
  <https://github.com/m5stack/StackChan>

---

## 2. AXP2101 PMIC 初始化

官方 M5Unified 对 `board_M5StackCoreS3` / `board_M5StackCoreS3SE` /
`board_M5StackChan` 使用同一组寄存器：

| 寄存器 | 官方值 | 含义 | 本仓库 |
|---|---|---|---|
| `0x90` | `0xBF` | LDOS ON/OFF control 0 | `0xBF`（一致） |
| `0x92` | `18-5 = 13` | **ALDO1 = 1.8 V，AW88298 I/O** | **曾漏写；`0x3ce400` 补上** |
| `0x93` | `33-5 = 28` | **ALDO2 = 3.3 V，ES7210** | **曾漏写；`0x3ce400` 补上** |
| `0x94` | `33-5 = 28` | ALDO3 = 3.3 V，摄像头 | `28`（一致） |
| `0x95` | `33-5 = 28` | ALDO4 = 3.3 V，TF 卡 | `28`（一致） |
| `0x10` | `0x30` | PMU common config | **曾未写；已补** |
| `0x27` | `0x00` | PowerKey 时序 | `0x00`（一致） |
| `0x69` | `0x11` | CHGLED 设置 | 曾 `0x35`；已对齐 `0x11` |
| `0x30` | `0x0F` | ADC enabled | 曾 `0x3F`；已对齐 `0x0F` |
| `0x97` | 官方数组未写 | BLDO2 | 本仓库写 `28`（保留，待确认用途） |

**2026-10-27 配置错误（已修，待真机确认）**：
`Pmic` 构造函数只写了 ALDO3/ALDO4，**漏了 ALDO1/ALDO2**，并把 `0x69` 写成
`0x35`、`0x30` 写成 `0x3F`、未写 `0x10`。现象是 AW88298（I2C 0x36 / 日志 0x6c）
在播放打开时间歇 NACK，崩溃后像“功放锁死”。这不是接线错，是电源轨配置不完整。

**验证方法**：`0x3ce400` 起，开机日志打印
`PMIC rails: ALDO1=0x0d ALDO2=0x1c ...`；若读回不是 `0x0d/0x1c`，先查 PMIC
写入/总线，不要继续测音频。

---

## 3. AW9523B IO 扩展

I2C 7 位地址 **0x58**。官方 CoreS3/StackChan 用法：

| 位 | 官方用途 | 说明 |
|---|---|---|
| P0.0 | `TOUCH_RST` | 触摸复位 |
| P0.1 | `BUS_OUT_EN` | 外部总线输出使能；M5Unified 有专门的上电/放电时序 |
| **P0.2** | **`AW_RST` / 扬声器 enable** | 官方在使能扬声器时置 1，关闭时先写 AMP 再置 0 |
| P0.3 | `ES_INT` | ES7210 中断 |
| P0.4 | `TF_SW` | TF 卡检测 |
| P0.5 | `USB_OTG_EN` | USB OTG |
| P1.x | LCD/触摸/摄像头等 | 见原理图 |

**本仓库偏差**：曾把 P0.2 当“复位”，启动时拉低 10 ms 再拉高
（`ResetAw88298()`）。官方没有这个低脉冲；P0.2 是 enable，高有效。
`0x3ce400` 已移除启动低脉冲，保持 P0.2 高。

> 注意：原理图网络名是 `AW_RST`，但 M5Unified 的扬声器回调把它当 enable 使用。
> 以官方代码的实际时序为准；不要凭网络名再猜“复位极性”。

---

## 4. 音频总线与器件

### 4.1 I2C（内部总线，GPIO12/11）

| 器件 | 7 位地址 | 日志写地址 | 备注 |
|---|---|---|---|
| AXP2101 | 0x34 | 0x68 | PMIC |
| AW88298 | 0x36 | 0x6c | 1 W 功放；ALDO1 1.8 V |
| ES7210 | 0x40 | 0x80 | 麦克风 ADC；ALDO2 3.3 V |
| AW9523B | 0x58 | 0xb0 | IO 扩展 |
| FT6336U | 0x38? / 0x68? | — | 触摸；启动扫描可见 0x68/0x69 |

### 4.2 I2S（当前固件）

| 信号 | GPIO |
|---|---|
| MCLK | 0 |
| BCLK | 34 |
| WS | 33 |
| DIN（麦克风） | 14 |
| DOUT（扬声器） | 13 |

当前是一个 I2S duplex（`I2S_NUM_0`），TX/RX 共用 BCLK/WS；ES7210 配 TDM，
2 slot = **MIC1 + MIC3（AEC 参考）**，顺序 `1,3,2,4`。
官方 M5Unified 的麦克风示例用 `I2S_NUM_1`，这是 BSP 的独立初始化方式；
本仓库沿用 xiaozhi 的 duplex 方案，near 已实测可用，暂不改。

### 4.3 采样率链路

```
ES7210 24 kHz TDM 2ch → I2S RX 24k → 重采样 16k → AFE 16k 单声道
  → 重采样 48k → Opus 48k/20ms → WebRTC 上行
下行：Opus 48k → 解码 48k → 重采样 24k → I2S TX 24k → AW88298
```

---

## 5. 配置错误记录表

| 日期 | 配置项 | 错误值/缺失 | 官方值 | 影响 | 状态 |
|---|---|---|---|---|---|
| 2026-10-27 | AXP2101 `0x92` ALDO1 | 未写 | `13`（1.8 V） | AW88298 I2C 边缘/NACK | `0x3ce400` 修复，待验证 |
| 2026-10-27 | AXP2101 `0x93` ALDO2 | 未写 | `28`（3.3 V） | ES7210 供电未显式配置 | `0x3ce400` 修复，待验证 |
| 2026-10-27 | AXP2101 `0x69` | `0x35` | `0x11` | CHGLED 配置偏差 | 已对齐 |
| 2026-10-27 | AXP2101 `0x30` | `0x3F` | `0x0F` | ADC 通道使能偏差 | 已对齐 |
| 2026-10-27 | AXP2101 `0x10` | 未写 | `0x30` | PMU 公共配置缺失 | 已补 |
| 2026-10-27 | AW9523 P0.2 | 当复位拉低 | enable 高有效 | 功放使能时序错误 | 已移除低脉冲 |
| 2026-10-26 | ES7210 TDM 通道 | 误以为 MIC1+MIC2 | `1,3,2,4`，2slot=MIC1+MIC3 | AEC 参考判断错误 | 文档/ AFE `MR` 已更正 |

---

## 6. 变更配置的检查清单

1. 先查 M5Unified 对应 board 的 `Power_Class.inl` / `M5Unified.inl`，不要只看旧
   xiaozhi 代码。
2. 电源轨改动必须能读回（新增/保留开机日志）；不能只写不验。
3. 一次只改一个配置组；改 PMIC 不同时改 AEC/采样率。
4. 改完记录：官方依据链接、原值、新值、影响、验证轮次与日志路径。
5. 涉及 AW9523 时，先确认 P0.1 `BUS_OUT_EN` 的时序，避免把外部总线切坏。
6. 新增 `I2cDevice::ReadReg` 的调用要考虑失败：**不要用 `ESP_ERROR_CHECK`
   把一次总线超时升级成重启**；用 `TryReadRegs` 并降级。
