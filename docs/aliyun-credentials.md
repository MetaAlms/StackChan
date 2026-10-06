# 配置阿里云凭据并刷机

本文是拿到 API Key 之后的完整操作步骤。**改动只涉及一个 gitignored 的本地文件。**

## 1. 前置条件

| 项 | 要求 | 检查方式 |
|---|---|---|
| ESP-IDF | v5.5.4（项目要求 >= 5.5.2） | `source ~/esp/esp-idf-5.5/export.sh && idf.py --version` |
| 依赖 | 已拉取 | `ls firmware/xiaozhi-esp32 firmware/components` |
| 百炼 | 已开通 Omni-Realtime 模型 | 百炼控制台 |
| 凭据 | API Key（`sk-` 开头）+ 业务空间 ID | 百炼控制台 |

依赖没拉过的话：`cd firmware && python3 ./fetch_repos.py`

## 2. 写凭据文件

创建 `firmware/sdkconfig.defaults.local`（**已在 .gitignore 里，不会进版本库**）：

```ini
# 启用阿里云协议，替代 xiaozhi 后端
CONFIG_STACKCHAN_ALIYUN_ENABLE=y

# 百炼 API Key
CONFIG_STACKCHAN_ALIYUN_API_KEY="sk-替换成你的"

# 业务空间 ID（qwen3.x-omni-*-realtime 系列需要专属域名）
# 留空则使用公共域名 dashscope.aliyuncs.com
CONFIG_STACKCHAN_ALIYUN_WORKSPACE_ID="替换成你的"

# 可选：模型与音色
CONFIG_STACKCHAN_ALIYUN_MODEL="qwen3.8-omni-flash-realtime"
CONFIG_STACKCHAN_ALIYUN_VOICE="Tina"
```

> 项目根 `firmware/CMakeLists.txt` 会自动检测到这个文件并叠加到 `sdkconfig.defaults`
> 之上，不需要手工指定 `SDKCONFIG_DEFAULTS`。

## 3. 编译并烧录

```bash
source ~/esp/esp-idf-5.5/export.sh
cd firmware

# 改了 sdkconfig.defaults.local 之后必须让配置重新生成
rm -f sdkconfig

idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

串口名用 `ls /dev/cu.*` 找。退出 monitor 是 `Ctrl+]`。

## 4. 验证启动日志

成功的话应该看到这些关键行：

```
I (xxx) AliyunOmni: url=wss://{workspace}.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime model=qwen3.8-omni-flash-realtime voice=Tina configured=1
I (xxx) AliyunOmni: seeded 'api_key' from build config
I (xxx) Application: Aliyun mode: skipping OTA version check
I (xxx) Application: Using Aliyun Qwen-Omni-Realtime protocol
I (xxx) AliyunOmni: connecting to wss://...?model=qwen3.8-omni-flash-realtime
I (xxx) AliyunOmni: session.created id=...
I (xxx) AliyunOmni: session.updated
```

**关键验证点**：**不应该**出现任何 `api.tenclass.net` 或 `47.113.125.164` 相关日志。
出现就说明 OTA 短路没生效。

## 5. 对话测试

连上后直接对设备说话：

| 日志 | 含义 |
|---|---|
| `>> 你好` | 识别到你的话 |
| `<< [happy] 你好呀！` | 模型回复（原始，含标记） |
| `emotion -> happy` | 表情标记命中，屏幕应同时变表情 |
| `speech_started (barge-in)` | 你打断它了，播放应停止 |

**表情验证**：注意屏幕上的眼睛/嘴巴是否随回复变化。若一直是中性脸，
说明模型没按约定输出标记 —— 检查系统提示词，或在 Mac 上用
`tools/aliyun_omni_probe.py` 先验一遍。

## 6. 常见问题

| 现象 | 原因与处理 |
|---|---|
| `no API key in NVS or build config` | `sdkconfig.defaults.local` 没生效。确认删了 `sdkconfig` 后重新 build |
| `Aliyun mode` 日志不出现 | `CONFIG_STACKCHAN_ALIYUN_ENABLE=y` 或 API Key 为空 |
| 一直 `network not connected yet, will retry` | WiFi 没连上。用手机 App 走蓝牙重新配网 |
| `Aliyun connect failed` | 检查 Key、WorkspaceId、地域；先用 Mac 探针排掉网络层问题 |
| `server error` | 看打印的完整错误 JSON，通常是模型未开通或格式不支持 |
| 有回复文字但没声音 | 下行编码问题。把 `raw-opus2` 换成 `pcm` 试试 |
| 表情不动 | 模型没输出标记，或标记不在八选一词表内 |

## 7. 安全提醒

Key 编进固件意味着**拿到设备就能提取出来**。开发阶段无所谓，
但如果设备要交给别人，应该：

1. `sdkconfig.defaults.local` 里 **留空** `CONFIG_STACKCHAN_ALIYUN_API_KEY`
2. 改为在运行时写入 NVS（当前需自行通过 BLE 或串口实现）

运行时写入的值**优先于**编译期配置，播种逻辑不会覆盖它，
所以两种方式可以共存、平滑迁移。

## 8. 回退到 xiaozhi 后端

把 `sdkconfig.defaults.local` 里的 `CONFIG_STACKCHAN_ALIYUN_ENABLE` 改成 `n`
（或删掉整个文件），`rm -f sdkconfig` 后重新 build。

`Application::InitializeProtocol()` 在 Aliyun 未配置时会回落到原有的
`MqttProtocol` / `WebsocketProtocol` 逻辑，OTA 检查也会恢复。
