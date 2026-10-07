# R4-1 / H1–H4：标准栈 host 媒体正对照 —— 最终结果

被复核 HEAD：`3b0844e0b6b3908995ad4a4023b1bf7b84b62f89`（Review 5448053831）。
本文件取代此前 R4-1 的结论小节；早先的"三次尝试 + speech_stopped 未触发"
作为**历史**保留在文末。

## 结论：标准栈三条全部通过

```
[play] zh_1 "今天我们测试语音连接" (195 packets, 3900 ms)
[vad] speech_started item=item_SSPJhK9ceXEhriMXEeHWY
[vad] speech_stopped item=item_SSPJhK9ceXEhriMXEeHWY
[clip] zh_1 vad=1/1 completed=1 kw=3/3 "今天我们测试语音连接。"
[play] zh_2 "桌上有一本蓝色的书" (183 packets, 3660 ms)
[clip] zh_2 vad=1/1 completed=1 kw=2/2 "桌上有一本蓝色的书。"
[play] zh_3 "请回答一加一等于几" (186 packets, 3720 ms)
[clip] zh_3 vad=1/1 completed=1 kw=2/2 "请回答一加一等于几。"
[media] accepted=924 refused=0 exceptions=0 bytes=121325 pkt=3..389 lateFrames=0
[time] mediaMs(18480) vs wallMs(18482) over the same run
VERDICT: PASS
```

三条的 `item_id` **互不相同**，且每条 **VAD 的 item 与 completed 的 item 一致**。
转写文本与冻结原文完全吻合（仅多句末句号）。

**已证**：同一 endpoint / workspace / model / `server_vad 0.5 / 800ms` /
`qwen3-asr-flash-realtime` 配置下，**标准 WebRTC 栈能对同源冻结 fixture
产生完整 VAD start+stop、完整 ASR 与关键词**。

## 根因：**未隔离**（H6-4 订正）

此前本节把"每段重置 pacing 期限"认定为 `speech_stopped` 缺失的根因。
**该认定没有单变量证据**：那一轮同时改变了回复通道、初始 seq/ts、尾静音长度等
多个因素，因此不能把结果归给任何单一改动。

**当前可支持的表述只有**：*本轮组合修复后完整对照通过，具体根因未隔离。*

已排除的负结果（各自试过、都不改变当时的失败）：
数字静音 vs 底噪、`config.timestamp` 与 RTP 头对齐。
**但这也不证明它们永远无关**——它们只是在当轮组合下未改变结果。

## H6-1～H6-4 处置

| # | 处置 |
|---|---|
| **H6-1** | 静音文件改为**从本次生成目录读取**（此前从 `__dirname` 读到旧文件，`run_pass.log` 里 mode/decoded/peak 全是 undefined，而归档 manifest 声称 60 包/1200 ms —— 证明的是另一个文件）。**校验** voice 容器包数与 manifest 一致、`decoded_samples_per_packet==960`、encoder CTL 返回码与回读码率；缺失或不符判 **INVALID**。结果里记录**本次实际消费**的 manifest/静音 hash 与每条 voice 容器 hash |
| **H6-2** | 等待不再按全局 ASR 数组长度释放，改为只由**本条 VAD/committed 公告的 item** 释放；`sameItemVad`、`ownedByClip` 从"告警"变成**判据**，进入终止条件与 PASS；旧/外来/重复/缺字段 completed 一律不释放当前条 |
| **H6-3** | 全部结果字段**在任何失败入口前初始化**（配置失败也可落盘）；改用 **`performance.now()` 单调时钟**贯穿 deadline/边界/wall；`r.window.endMs` 按 **48 ticks/ms 换算**；send 的 `bool`/exception **互斥计数**，非 true 不默认计入 accepted；**迟到帧使本轮无效**而非仅累计 |
| **H6-4** | 根因表述订正为"组合修复后通过，根因未隔离"（见上） |

### 离线检查（H6-2/H6-3 要求）

判定逻辑抽到 `tools/webrtc_probe/control_logic.js`，**runner 与测试共用同一实现**，
因此测试的就是实际判据：

```
$ node tools/webrtc_probe/test_control_logic.js
16 passed, 0 failed
```

覆盖：正常归属、旧/外来 item、重复、缺 `item_id`/`transcript`、
start/stop 属不同 item、关键词缺失、无 VAD stop、未归属 completed、
failed、拒发/异常/迟到使本轮无效、输入证据无效、条数不足。

## H1–H4 处置

| # | 处置 |
|---|---|
| **H1** | `sendMessageBinary` 的 **bool 返回值**现在被检查，分别统计 `accepted / refused / exceptions`；任一 refused 或 exception 使本轮判 `INVALID`。**单一单调绝对期限**贯穿全部 clip、静音与 ASR 等待；记录 `lateFrames`。媒体时长由**被接受的帧数**推导，墙钟同覆盖（`mediaMs 18480` vs `wallMs 18482`） |
| **H2** | 生成器 `encode_fixture.py` **自身冻结** `text` 与 `keywords` 并写入 manifest；runner 遇到缺失/空的 keywords **拒绝评分**（不再回退 `[]` 把 0/0 当全中）。VAD 记录实际 **item_id**；每条只取**本条的 VAD/ASR 窗口**；completed 的 item 若不在本条 VAD 公告的 item 中会告警；未达标即**终止序列** |
| **H3** | 事件回调接收**实际 channel 对象**并据其回复 `session.update`；记录真实 label（本次为 `txt`）；检查其返回值。**全部结果状态在函数开头初始化**，配置失败分支不再落入 temporal dead zone，失败也能落盘。结束时释放 track/channel/peer，**退出码与 verdict 一致**（PASS=0 / FAIL=2） |
| **H4** | 静音包改为**由生成器可重跑产生**并附**逐包本地解码样本数与峰值**证据（`mode=dither`、960 样本/帧、peak \|x\|=49）。**未知事件类型按 type 记录**——正是这一条暴露了此前被静默丢弃的 `conversation.item.input_audio_transcription.delta`、`input_audio_buffer.committed` 与整个 `response.*` 生命周期。结论订正为：host 通过**只**证明该标准栈/模型/配置能处理同源音频 |

## 保留的原始证据

`tools/webrtc_probe/host_control_evidence/`：

| 文件 | 内容 |
|---|---|
| `run_pass.json` | 本次通过运行的完整结构化结果 |
| `run_pass.log` | 同次运行的脱敏原始输出 |
| `encode_manifest.json` | 源 hash、pcm48 hash、包数/包长、尾部补齐、CTL 返回码与回读、逐包解码样本数 |
| `silence_packets.json` | 本次生成目录中**实际被消费**的静音文件（hash 记在 `run_pass.json`） |

`run_pass.json` 的 `inputEvidence` 记录本次运行**实际消费**的 manifest/静音 hash
与每条 voice 容器 hash，使结果自带输入证据。

## 重跑命令

```bash
python3 tools/webrtc_probe/encode_fixture.py /tmp/host_media
NDC_PATH=/tmp/wrtc-test/node_modules/node-datachannel \
  node tools/webrtc_probe/standard_media_control.js /tmp/host_media /tmp/host_media/result.json
```

依赖实测：node-datachannel **0.33.4**（libdatachannel **0.24.5**）、libopus **1.5.2**。
凭据仅从 gitignore 的 `firmware/sdkconfig` 读取；输出不含 key/Authorization/完整 SDP/ICE 密码。

## 边界

host 通过**不证明**设备发出相同字节，**不证明**设备媒体、AEC、全双工或产品移植通过。
**M1 仍未通过**，本实验不代替 M1 验收。设备侧 SRTP/UDP 计数、DTLS 元数据、
item_id 绑定、wall/loop 范围、sender 信号量、60 ms、回归与隔离重建**仍未完成**。

---

## 历史（已被上文取代）

早先三次运行停在 `speech_started` 有、`speech_stopped` 无，当时结论曾写为
"服务端/模型/配置无法处理该音频已排除，问题收窄到设备侧"——**该表述过强**，
当时证据只支持"某些输入到达并触发 VAD 起始"。现已按 H4 订正。
