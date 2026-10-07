# R4-1：标准栈 host 媒体正对照 —— 实际执行结果

Review 5447928154（reviewed exact HEAD `3ab11e9`）要求**只做**这一件事，
并要求交付**真实执行结果**而非设计。本文件是那次执行的结果。

## 执行

| 项 | 值 |
|---|---|
| 脚本 | `tools/webrtc_probe/standard_media_control.js`（新增） |
| 编码 | `tools/webrtc_probe/encode_fixture.py`（新增） |
| 栈 | node-datachannel **0.33.4**（libdatachannel **0.24.5**），`getLibraryVersion()` 实测 |
| 编码器 | 本机 **libopus 1.5.2**（`opus_get_version_string()` 实测） |
| 重采样 | ffmpeg `aresample=48000`，16k mono s16 → 48k mono s16 |
| 源 | 冻结的 `firmware/main/hal/webrtc/fixture/zh_*.pcm`（未重新生成） |
| 帧 | 20 ms / 960 样本 / 单个 raw Opus packet；本地逐包解码校验 |

编码产物：zh_1 **195** 包 / 3900 ms、zh_2 **183** / 3660、zh_3 **186** / 3720，
包长 3–389 B，尾部补齐 582 / 549 / 303 样本（已记录，未假定与设备逐字节相同）。

## 结果（三次运行一致）

```
[sdp] answer m=audio 3478 UDP/TLS/RTP/SAVPF 111 | a=rtpmap:111 opus/48000/2 | a=sendrecv
[pc] state connected
[dc] server channel 'txt' opened
[cfg] session.created on 'oai-events'
[cfg] session.updated echo: {"type":"server_vad","threshold":0.5,"silence":800,"asr":"qwen3-asr-flash-realtime"}
[cfg] verified: YES
[play] zh_1 (195 packets, 3900 ms)
[vad] speech_started          ← 服务端确实检测到语音
[clip] zh_1 vad=1/0 completed=0 kw=0 item=null wait=21612ms ""
[media] packets=1335 bytes=160584 pkt=3..388 seq 28993..30328 sendErrors=0
VERDICT: FAIL
```

## 这是本轮的关键判别结果

| 观察 | 设备（esp_peer 1.5.5） | Host 标准栈 |
|---|---|---|
| SDP 被接受（`a=sendrecv` opus/48000/2） | ✅ | ✅ |
| DTLS/ICE 连通 | 间歇（5 次中 4 次超时） | ✅ 稳定 |
| 配置核验（server_vad/0.5/800/ASR 模型） | ✅ | ✅ |
| **服务端 VAD 检测到语音** | ❌ **从未触发** | ✅ **`speech_started` 触发** |

**结论（限定在可证范围内）**：
把同源音频换成一个标准 WebRTC 栈后，**服务端会把它识别为语音**；
而设备路径从未触发过任何 VAD 事件。因此
**"服务端/模型/配置无法处理这段音频"这一假设已被排除**，
问题被收窄到**设备侧媒体生成或发送路径**。

同时，标准栈本身也**没有取得完整正对照**：`speech_started` 触发后
**`speech_stopped` 始终未触发**，因而没有 completed ASR。已尝试的处置：

1. 尾静音由"数字零"（3 B/包）改为**接近真实的底噪**（约 91 B/包），
   确保不是退化到 DTX/舒适噪声 —— 结果不变
2. 让 **RTCP SR 的 `config.timestamp` 与 RTP 头逐包对齐** —— 结果不变

**因此标准栈侧的 `speech_stopped` 未触发的原因本轮未定位**，
不能据此判定服务端不支持，也不能把它当作设备侧根因。

## 边界与未完成

- 标准栈通过只证明**此次**栈/模型/配置能处理同源音频；
  **不证明**设备发出相同字节，**不证明**设备媒体、AEC、全双工或产品移植通过
- **M1 仍未通过**；本实验**不能**代替 M1 验收
- 设备侧 SRTP/UDP 实际计数、DTLS 失败阶段元数据、item_id 绑定、
  wall/loop 范围、sender 信号量、60 ms、回归与隔离重建**均仍未完成**
  （按 Review 要求，不在本次 host 单任务内修改）

## 重跑命令

```bash
python3 tools/webrtc_probe/encode_fixture.py /tmp/host_media
# 生成 silence_packets.json 后：
NDC_PATH=/tmp/wrtc-test/node_modules/node-datachannel \
  node tools/webrtc_probe/standard_media_control.js /tmp/host_media /tmp/host_media/result.json
```

凭据仅从 gitignore 的 `firmware/sdkconfig` 读取；stdout 不输出
API key、Authorization、完整 SDP、ICE 密码或音频负载。
