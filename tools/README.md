# tools/ — Mac 端协议验证工具

在动 ESP32 固件之前，先用这里的东西把阿里云链路和表情规则验真。
目的是把**协议调试成本从"反复烧录"降到"跑一条命令"**。

## 为什么自己实现 WebSocket 客户端

`aliyun_omni/ws_client.py` 是手写的 RFC 6455 客户端，没有用 `websockets`
或 `websocket-client`。三个原因：

1. **零安装**：任何有 Python 3.10+ 的机器直接跑，不需要 pip
2. **可见性**：报文的握手、掩码、分帧逻辑全部可见可断点，这正好是
   ESP32 上还要再实现一遍的同一层
3. **对照实现**：`ws_client.py` 里客户端帧必须掩码（RFC 6455 §5.3）这类细节，
   是固件实现的参考

## 快速开始

### 1. 离线自测（不需要 Key，不需要网络）

```bash
python3 tools/aliyun_omni_probe.py --self-test
```

覆盖表情标记解析、流式累积器（含"delta 从标记中间切断"的回归用例）、
session 载荷结构、重采样。

### 2. 实时链路验证

```bash
export DASHSCOPE_API_KEY=sk-xxx
# 没有 Key 时也会验到握手层，用哨兵 Key 会得到 401，这已经能证明
# 端点可达 + TLS 正常 + URL 构造正确

afconvert -f WAVE -d LEI16@16000 question.m4a question.wav   # 录一句问话
python3 tools/aliyun_omni_probe.py --wav question.wav
afplay reply.wav
```

### 3. 可选参数

| 参数 | 用途 |
|---|---|
| `--manual` | 关掉服务端 VAD，由客户端显式 commit + response.create |
| `--input-format` | `pcm` / `opus` / `raw-opus`，用来测账号实际接受哪种上行 |
| `--output-format` | `pcm` / `raw-opus2` / …，用来测下行编码 |
| `--voice` | 音色，默认 `Tina` |
| `--workspace-id` | 业务空间 ID，会作为 `X-DashScope-WorkSpace` 头发送 |
| `--instructions` | 覆盖系统提示词（默认是表情标记提示词） |
| `--silence-ms` | VAD 判停静音时长，默认 800 |
| `--vad-threshold` | VAD 阈值，默认 0.5 |

## 输出会告诉你什么

跑完会打印一份报告，里面有几项直接决定固件怎么写：

- **`first audio lag`** — 首包延迟。这是用户体验的核心指标，也是决定要不要
  在固件里加缓冲的依据。
- **`bandwidth`** — 上下行字节数。用来判断 base64(JSON) 封装的 PCM 是否
  值得换成 `raw-opus2`。
- **`reply (visible)`** — 剥掉情绪标记后的可见文本，和真机上屏幕要显示的一致。
- **`EMOTION -> xxx`** — 每次识别到情绪标记就打一行，这是表情联动的调试输出。

## 表情标记规则（Mac 与固件共用）

`aliyun_omni/protocol.py` 是这套规则的**唯一真源**，固件实现必须与它一致。

模型只会输出文本和音频，不会输出 M5Stack 的 `ControlAvatar` 帧。所以表情靠约定：
在系统提示词里要求模型在句首插入 `[happy]` 这类标记。

解析契约：

```
advance_emotion_events(state, delta) -> (new_emotion_events, new_visible_text)
```

- **返回的是增量**，不是全量。调用方可以直接打印，不需要自己去重。
- **可以安全地在标记中间切断**：`"[hap"` 会被扣住，等下一个 delta 补全。
  因为 `response.audio_transcript.delta` 本来就按任意字节边界切分。
- **每个标记只触发一次**，无论被切成多少片。
- **字面量 `[` 有歧义**：单独一个 `[` 会扣住一格再决定。代价是字面方括号晚
  几十毫秒显示，收益是真标记永远不会在屏幕上闪一下。

三处容易踩的坑，都在自测里有回归用例：

1. **段落下标不稳定** —— 新 delta 会把末尾文本段落变长（`" Hel"` → `" Hello!"`），
   所以不能用下标锚定已发内容，必须比对内容前缀
2. **已发内容会缩回** —— 单独一个 `[` 先被当文本发出，下一帧被吸收进标记。
   所以只发共识前缀之后的部分
3. **延迟判定不能有状态泄漏** —— 无状态的 `clean_text()` 绝不能延迟任何东西，
   否则 `"[oops hi"` 这种永远不可能是标记的文本会被永久吞掉。
   延迟只存在于有状态的流式累积器里

## 相关文档

- 总体计划：[docs/aliyun-omni-v2v-plan.md](../docs/aliyun-omni-v2v-plan.md)
- 协议依据：阿里云百炼 Realtime API 概述（AOQ / WebRTC / WebSocket 选型矩阵）
