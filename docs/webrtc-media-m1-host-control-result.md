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

## 根因：每段重置 pacing 期限

此前 `sendFrames` 每次调用都重新 `deadline = Date.now()`，在媒体流中引入间隙，
服务端的 VAD 因此只触发 `speech_started`、**始终不触发 `speech_stopped`**。
改成**全流程单一单调绝对期限**（H1）后，三条立即全部通过。

**这同时撤回了先前两个猜测**：数字静音 vs 底噪、以及 `config.timestamp`
与 RTP 头对齐，**都不是** stop 缺失的原因（两者都试过、都不改变结果）。
本文件不再把 SR 时钟列为条件风险。

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
| `encode_manifest.json` | 源 hash、pcm48 hash、包数/包长、尾部补齐、静音参数与解码证据 |

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
