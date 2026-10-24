# M1 无 VAD：标准栈同源 fixture 对照设计（未实施）

Codex 子任务只进行依赖/源码/规范只读核查，设计转录到仓库供 Review 5447065335 的 M1 诊断实现参考；这不是新的产品阶段，也不是媒体通过证据。没有获取凭据、创建联网会话、编码 fixture、操作设备、构建或改仓库。

## 1. 范围与目标

绑定设备复核提交 `c3e658200d8365ac4bbb19a28290d491f2c04df3`。
已有 M0 标准栈测试只证明控制面可建连，不证明媒体上行正常。下一步最低成本判别实验：同一 Mac 网络、端点、workspace/model、VAD/ASR 配置与同源 16k PCM fixture，换成 host libopus + node-datachannel 标准传输。先发送第一条 zh_1；有效正对照成立后再完成三条。先冻结 20ms，不在该诊断中顺带降 48k、改 VAD 阈值或手动 commit。

这只验证 host 媒体/服务端配置组合。host 成功、设备失败可把问题收窄到设备音频转换/编码/封包/发送路径，但不能直接判定 esp_peer 的哪一层有 bug；host 也失败不能直接判定阿里故障，仍需查对照本身。

## 2. 可复用与必须补的现有代码

提交内 `tools/webrtc_probe/standard_webrtc_probe.js`：

- 20 行起 HTTPS SDP POST 可复用；现有请求与非 trickle gather/candidates 流程可复用。
- 56–58 行已正确注册 Opus 音频 m-line 0，但丢弃 `pc.addTrack(audio)` 返回值；必须保留 Track 才能发送。
- 64 行起 DataChannel 发现可复用；应沿收到 `session.created` 的实际 channel 配置，不固定 `'oai-events'` 或 stream id。
- 67–70 行 substring、74 行缺少 ASR 对象、89 行在 client channel 先发 update、98 行 updateSent 抑制正确通道回复，均不适合媒体对照。改成精确 JSON type、发现的实际 channel/会话 ID、检查配置回显后放行。
- 当前脚本只等待客户端 dcOpen 并打印 M0 verdict，需替换为 transport → discovered event channel → session echo → media → VAD/ASR 分层结果。不得把“客户端通道没开”当作失败，如果真正事件通道和媒体已可用。
- README 的“标准栈成功所以网络/端点无问题”“失败则端点不可达”结论过宽；本实验分别报告每层，并保留具体错误。

依赖：仓库 package.json 声明 `node-datachannel:^0.33.4`，仓库目录没有 node_modules/lock。既有 `/tmp/wrtc-test/node_modules/node-datachannel/package.json` 精确为 0.33.4；其 CMakeLists.txt:39 绑定 libdatachannel v0.24.5。已将本地源码、官方 tag 源码与 URL/SHA256 记录保存本目录。实现时应复用/冻结精确版本并记录运行时 `getLibraryVersion()`；不要假设 ^ 范围锁住该版本。

## 3. Track API 与 packetization 的准确边界

`node-datachannel` v0.33.4 Track：

- `sendMessage(msg: string)` 是 string API；不能拿它传 Buffer/raw Opus/PCM。
- `sendMessageBinary(buffer: Buffer)` 把字节直接传给 C++ Track::send；不是 encoder。
- **无 RtpPacketizer 时**，C++ Track 期望完整 RTP/RTCP。
- **挂 generic RtpPacketizer 时**，应送单个 raw Opus payload；generic fragment 原样返回一个 payload，然后生成 RTP header。不要传带 RTP header 的 packet，也不要传 Ogg/OpusHead/长度前缀，否则会二次封包。
- JS 未导出 `OpusRtpPacketizer`；有 generic `RtpPacketizer`。C++ 的 Opus alias 只是 `AudioRtpPacketizer<48000>`，该模板没有改变 generic 逻辑，可用 generic + clock48000 达到同一封包逻辑。
- generic 自动 sequenceNumber++，**不自动 timestamp+=960**。Node 的 sendFrame/FrameInfo 没有暴露；JS必须在每包前写 rtpConfig.timestamp。
- v0.24.5 generic 在每条单包 frame 上设 M=1；RFC7587 §4.1 引用 RFC3551 §4.1，连续无 silence suppression 的音频必须 M=0。为避免让对照带入该变量，建议本次直接构造12字节完整 RTP，并仅挂 RTCP handler。

推荐媒体处理链：`RtcpSrReporter(config) → RtcpNackResponder()`，**不挂 RtpPacketizer**。SR reporter 的 outgoing 函数明确读取完整 RTP header 的 SSRC/timestamp，累计 payload 与包数，每1秒送 SR+SDES；NACK handler 缓存完整 RTP packet。这样 SRTP/DTLS/ICE 仍由标准栈负责，应用仅构造简单 RTP header。

## 4. SDP/SSRC/PT/时间戳

- `new ndc.Audio('0','SendRecv')`，audio 在 DataChannel 前创建，保持现有 M0 的顺序与方向。
- `audio.addOpusCodec(111)` 输出 `/48000/2`，即使 PCM mono 也必须 /2（RFC7587 §7）。保留生成器自己的 SDP，不覆写 Answer。
- 会话内随机非零32bit SSRC、稳定 CNAME，通过 `audio.addSSRC(ssrc,cname,'m1-probe','audio0')` 宣告；RTP header、RtcpSrReporter config 使用同一 SSRC/CNAME。
- PT 111 是这次唯一 offered Opus PT，需核对 Answer 当前 audio m-line 接受它并映射 `opus/48000/2`，方向允许接收。若未协商成功，报 SDP/媒体配置失败，不继续盲发，不手改 SDP。
- 初始 sequence 和 timestamp 随机。每个20ms包 sequence+1、timestamp+960，网络字节序；clock固定48000，不是16k fixture PCM采样率，也不是毫秒。
- 一个 RTP payload 恰好含一个 libopus encode 返回的 Opus packet（RFC7587 §4.2）；分帧960 samples@48k mono s16le=1920 bytes。20ms帧不能以字节长度猜duration，应读 opus_packet_get_nb_samples(packet,48000)==960。
- leading500ms、trailing1200ms 均作为编码后音频发送；不因为PCM全零就省包。ASR等待/clip间隙可继续发送编码静音，使该20ms时间轴连续；若暂停则下一次timestamp必须体现未发送的时间，不能恢复时倒退或重新从0。

如下是待实施的媒体核心设计片段（未执行）：

```js
const crypto = require('node:crypto');
const { performance } = require('node:perf_hooks');
const { setTimeout: delay } = require('node:timers/promises');
let ssrc = 0;
while (!ssrc) ssrc = crypto.randomBytes(4).readUInt32BE(0);
const cname = 'm1-standard';
const audio = new ndc.Audio('0', 'SendRecv');
audio.addOpusCodec(111);
audio.addSSRC(ssrc, cname, 'm1-probe', 'audio0');
const track = pc.addTrack(audio); // keep reference; before creating dc
const config = new ndc.RtpPacketizationConfig(ssrc, cname, 111, 48000);
const sr = new ndc.RtcpSrReporter(config);
sr.addToChain(new ndc.RtcpNackResponder());
track.setMediaHandler(sr); // complete RTP route; NO RtpPacketizer
track.onMessage((_packet) => { /* count/discard downlink; do not block */ });
let seq = crypto.randomBytes(2).readUInt16BE(0);
const ts0 = config.timestamp >>> 0;
let mediaIndex = 0; // never reset for the same SSRC/session

function makeRtp(opus, index) {
  const rtp = Buffer.allocUnsafe(12 + opus.length);
  rtp[0] = 0x80; // V2, P=0, X=0, CC=0
  rtp[1] = 111;  // M=0, PT=111; confirmed in negotiated Answer
  rtp.writeUInt16BE(seq, 2);
  seq = (seq + 1) & 0xffff;
  const ts = (ts0 + index * 960) >>> 0;
  rtp.writeUInt32BE(ts, 4);
  rtp.writeUInt32BE(ssrc, 8);
  opus.copy(rtp, 12); // exactly one raw Opus packet; no framing bytes
  return rtp;
}

// Call only once track.isOpen(), connected and validated session.updated.
// packets include fixed source clip plus final padded silence; host encoder
// must also supply silence packets during asynchronous ASR wait between clips.
async function send20msPackets(packets) {
  const start = performance.now();
  const firstIndex = mediaIndex;
  for (const opus of packets) {
    const deadline = start + (mediaIndex - firstIndex) * 20;
    for (;;) {
      const wait = deadline - performance.now();
      if (wait <= 0) break;
      await delay(wait);
    }
    const lateMs = performance.now() - deadline;
    if (lateMs > 20) throw new Error('pacing invalid; record lateness');
    if (!track.isOpen() || pc.state() !== 'connected')
      throw new Error('transport closed during media');
    const rtp = makeRtp(opus, mediaIndex++);
    if (!track.sendMessageBinary(rtp)) throw new Error('local send rejected');
    // Count bytes/packets, mediaIndex*20, deadline/actual monotonic wall time,
    // send result and selected RTP metadata; never log credential/key material.
  }
}
```

上述片段不是完整脚本；分段示例的局部 start 不应作为最终 sender 的连续 deadline 设计，实际实现应从同一会话起点计算所有 clip/静音/等待的期限并报告漂移。网络、Answer校验、session gate、raw包读取与停止/关闭需由复用探针实现。若采用 generic 替代路线，则 chain packetizer→SR→NACK，sendMessageBinary 只送 raw Opus；记每包 M=1 的实现行为，并不能称其符合连续无DTX的marker规范。

## 5. 会话配置与事件

在收到 session.created 的实际 `ch` 上发以下配置（同设备），待该会话 session.updated 回显验证通过：

```json
{
  "type":"session.update",
  "event_id":"event_m1_standard_cfg",
  "session":{
    "modalities":["text","audio"],
    "turn_detection":{"type":"server_vad","threshold":0.5,"silence_duration_ms":800},
    "input_audio_transcription":{"model":"qwen3-asr-flash-realtime"}
  }
}
```

精确检查 `event.type`，保存/比较 session ID 与发现的 channel ID/label。同一session若有多个channel事件要观察但不可混到另一session；不能靠 substring 命中。核对 turn_detection.type/threshold/silence_duration_ms、input_audio_transcription.model，缺失/null/错误或error均不得释放sender。不可仅“收到session.updated”就认为配置通过。

事件按 `item_id`、clip编号与单调时间记录：

- input_audio_buffer.speech_started / speech_stopped
- input_audio_buffer.committed（若有）与 conversation.item.created
- conversation.item.input_audio_transcription.completed 的完整 transcript/item_id
- conversation.item.input_audio_transcription.failed 的 error
- 普通 error、连接/Track关闭分开记

completed transcript匹配manifest的关键词。delta/stash与模型回复文本不能替代输入ASR完成事件。ASR是独立识别模型，不能用模型“答了正确问题”替代ASR验收。

阿里WebRTC官方说明媒体由 RTP track 送，JSON沿 DataChannel；不要再把音频 base64 append 到事件通道。server_vad 基线不发送 input_audio_buffer.commit 或 response.create，不把它变成manual测试。

## 6. Host编码与有效性最低证据

现有本机依赖提供 FFmpeg8.0/libopus1.5.2，可使用 Python stdlib ctypes→libopus 每帧960样本 encode，直接获得raw Opus并写 `u32BE length + payload` 本地离线容器（Node读后剥前缀）。具体编码设计见 [host 编码设计](webrtc-media-m1-host-encoding.md)，工具/hash事实见 [facts.json](/tmp/stackchan-host-opus-encoding/facts.json)。该独立工具核查记录当时HEAD为9e45e63；本复核另用git show c3e6582读取三段PCM并确认与当前文件逐字节相同、hash匹配原manifest，因此同源fixture坐标仍成立。

用现有 fixture/manifest 的 sha256_pcm，不能重新say生成另一份同文本文件。转换16k→48k host重采样可以用FFmpeg raw s16le；记录host重采样工具/版本、48k PCM hash与能量，不声称和设备esp_ae重采样字节相同。固定 mono/AUDIO/complexity5/VBR on/FEC off/DTX off，尽量匹配设备实际90000bps。raw包应做packet duration检查和离线decode，源语音及解码能量/听感为有效正对照；不能只看encode返回正数。

FFmpeg `-f opus` 是 Ogg Opus，不能直接chunk后当raw payload发。若选择该路线必须正确Ogg demux，剥OpusHead/OpusTags与pages/lacing；整文件、页、随机固定字节切片都不构成合法raw packets。直接libopus可避免该解析成本。

## 7. 最低结果判据

每条记录层级：SDP/ICE/DTLS → Track/event channel → 配置回显 → 编码可解码与pacing/本地发送 → VAD start/stop → 完成ASR关键词。

有效成功：first zh_1在同配置下有VAD和completed且关键词匹配；随后同一设置跑3条，日志可按item/clip对应。有效host正对照成功而设备同源仍全零VAD，是设备媒体链问题的支持证据，后续优先验证设备重采样后PCM与raw包可解码，以及真实SRTP/socket发包/服务端媒体接收，不能把本地SendAudio=0当已接收。

无VAD但有合法媒体计量：仍属于媒体/服务端接收未定位，不是“服务端不支持server_vad”的证明。任何gate失败、无真正track open、包不能解码、pacing失效/早退，结果归无效对照。三条及300秒设备M1验收仍须设备实测，host成功不能代替。

## 8. 一手来源

- Node 0.33.4 API与native wrapper：https://github.com/murat-dogan/node-datachannel/blob/v0.33.4/src/lib/index.ts ，https://github.com/murat-dogan/node-datachannel/blob/v0.33.4/src/cpp/media-track-wrapper.cpp ，https://github.com/murat-dogan/node-datachannel/blob/v0.33.4/src/cpp/media-rtppacketizer-wrapper.cpp
- 精确lib版本：https://github.com/murat-dogan/node-datachannel/blob/v0.33.4/CMakeLists.txt#L39
- 无packetizer期望完整RTP：https://github.com/paullouisageneau/libdatachannel/blob/v0.24.5/src/impl/track.cpp#L164
- Opus alias与generic packetization：https://github.com/paullouisageneau/libdatachannel/blob/v0.24.5/include/rtc/rtppacketizer.hpp#L56 ，https://github.com/paullouisageneau/libdatachannel/blob/v0.24.5/src/rtppacketizer.cpp#L20
- SR handler：https://github.com/paullouisageneau/libdatachannel/blob/v0.24.5/src/rtcpsrreporter.cpp#L40
- 官方完整media例：https://github.com/paullouisageneau/libdatachannel/blob/v0.24.5/examples/streamer/main.cpp
- Opus SDP/clock/payload：https://www.rfc-editor.org/rfc/rfc7587.html#section-4.1 ，https://www.rfc-editor.org/rfc/rfc7587.html#section-4.2 ，https://www.rfc-editor.org/rfc/rfc7587.html#section-7
- Audio marker：https://www.rfc-editor.org/rfc/rfc3551.html#section-4.1
- 阿里WebRTC媒体/初始化：https://help.aliyun.com/zh/model-studio/realtime-webrtc-access
- 阿里会话字段：https://help.aliyun.com/zh/model-studio/client-events
- 阿里VAD/ASR事件：https://help.aliyun.com/zh/model-studio/server-events
