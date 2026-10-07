# StackChan 开发约定

M5Stack StackChan（ESP32-S3 / CoreS3）开源固件仓库。
本文件是给 AI 助手与协作者的硬性约定，先读再动手。

## 分支与远端

```
origin  git@github.com:MetaAlms/StackChan.git     ← 我们的 fork
上游    m5stack/StackChan                          ← 只读参照
```

- 不在 `main` 上直接改，工作分支 `feat/aliyun-omni-v2v`
- **不要用 `git clone` 覆盖本目录**：`firmware/xiaozhi-esp32/`、`firmware/components/`
  是 `fetch_repos.py` 的产物，不在 git 里，覆盖后要重跑（约几分钟）

## 工具链

| 项 | 值 |
|---|---|
| ESP-IDF | **v5.5.4**，在 `~/esp/esp-idf-5.5`（本机另有 5.2.3 供旧项目用，勿动）|
| 激活 | `source ~/esp/esp-idf-5.5/export.sh` |
| 串口 | `/dev/cu.usbmodem*`（设备休眠会失联，按电源键唤醒）|

## 记录规范（重要）

**开发中发现的东西必须落盘，不要只留在对话里。**

| 发现类型 | 记到哪 |
|---|---|
| 原厂/上游缺陷、硬件陷阱、自己踩的坑 | [docs/firmware-findings.md](docs/firmware-findings.md) |
| 硬件型号、引脚、采样率链路等稳定事实 | [docs/stackchan-hardware.md](docs/stackchan-hardware.md) |
| 方案与阶段计划 | [docs/aliyun-omni-v2v-plan.md](docs/aliyun-omni-v2v-plan.md) |
| 能力边界与已知不可为 | [docs/aec-limitation.md](docs/aec-limitation.md) |

每条发现要带**证据**（日志、代码位置、实测数据）与**当前状态**。

理由：像"增益改了没用"这种结论，背后是 ES7210 量程钳位，花了很多轮才定位；
不记下来下次会重新踩，而部分还属于值得提 PR 的上游缺陷。

## 凭据

- 本机密钥走 macOS Keychain，见 `blue-keychain-*` skill
- **固件凭据文件是 `firmware/sdkconfig.defaults.local`**，已 gitignore
- **绝不把密钥写进代码、日志、提交或文档**

## 已知的上游坑（动手前先看）

完整列表见 [docs/firmware-findings.md](docs/firmware-findings.md)，最常踩的三个：

1. **`firmware/main/Kconfig.projbuild` 才是生效的 Kconfig**，
   `firmware/xiaozhi-esp32/main/Kconfig.projbuild` **从不被读取**（xiaozhi 源码是编进 main 组件的）
2. **ES7210 增益上限 37.5dB**，`input_gain_` 写超过的值会被静默钳位
3. **本方案无法支持语音打断**：阿里 WebSocket 协议官方规格为
   「回声消除/降噪：无，需客户端自行处理」，而 ESP32 只能用 WebSocket
   （AOQ 仅限 Android/iOS/HarmonyOS）。设备侧补 AEC 又因
   `esp_codec_dev` 最多 2 通道而拿不到第 3 路回采参考。
   **这是规格限制，不要再当作 bug 去修。** 详见
   [docs/aec-limitation.md](docs/aec-limitation.md)

## 构建

```bash
source ~/esp/esp-idf-5.5/export.sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

新增源文件后必须 `idf.py reconfigure`（`GLOB_RECURSE` 只在配置期展开）。
