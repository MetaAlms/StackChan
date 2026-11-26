# Host 同源 fixture → 20 ms raw Opus：只读核查与待实施设计

状态：2026-10-08 已核查可用工具、头文件、源码及 frozen PCM。没有执行编码、解码、网络会话、设备访问、构建或仓库修改。此处伪代码/命令仅供后续实现，不能视为已跑通的 host 媒体正对照。

## 可直接采用的路径

推荐现有 `ffmpeg` 只负责 16→48 kHz PCM 重采样，Python stdlib `ctypes` 调用本机现有 libopus 1.5.2 的 `opus_encode` 获取逐个 raw packet。每包来自 960 个单声道样本（1920 PCM 字节），占 20 ms。临时 pipe/file 使用 `uint32BE payload_length + raw_opus_payload` framing；Node 剥离四字节长度字段后再交给 RTP packetizer。编码器与 decoder 状态按连续媒体流保持，不能每 20 ms 重建。

本机执行 `opus_get_version_string()` 实际得到 `libopus 1.5.2`；绝对路径 `/opt/homebrew/opt/opus/lib/libopus.0.dylib`。`ffmpeg`/`ffprobe` 均已安装，FFmpeg 8.0 的构建启用了 libopus。项目 package.json 仅含 node-datachannel ^0.33.4；无需新增 npm 依赖。完整机器/fixture 核查事实见同目录 `facts.json`，原始帮助输出也已保存。

依据：[Opus encoder API](https://opus-codec.org/docs/opus_api-1.5/group__opus__encoder.html) 定义输入 frame_size 为每声道样本数，返回 encoded packet 的实际字节数。

## 冻结来源与与设备对应关系

直接读取 `firmware/main/hal/webrtc/fixture/*.pcm` 与现有 `manifest.json`，不再运行 macOS say。重新生成 TTS 不能保证现有 frozen hash。

已逐字节检查三段 SHA256 与 manifest 一致；三段开头 8000 个样本（500 ms）、末尾 19200 个样本（1200 ms）均为零。格式是 16 kHz / mono / s16le。关键词分别为：zh_1 测试/语音/连接，zh_2 蓝色/书，zh_3 一加一/等于。

| clip | source samples | ceil(samples/320) | 当前设备 StreamClip 20 ms 调用数 |
|---|---:|---:|---:|
| zh_1 | 62206 | 195 | 196 |
| zh_2 | 58377 | 183 | 184 |
| zh_3 | 59419 | 186 | 187 |

当前 `webrtc_m1.cc:886` 循环条件为 off < clip.size + in_bytes；这三段均不是整帧，除补齐末帧还会额外调用一次全静音帧。Host 严格对照当前发送时间线时，可在末尾补齐到 ceil(samples/320)*320，再加 320 个零样本；或者只补齐末帧，但须记录与设备相差额外 20 ms，不应误报 payload/帧数完全相同。这个循环观察不是无 VAD/ASR 的根因证明。

设备编码设定（`webrtc_m1.cc:286–297`、`esp_opus_enc.h:103`）：48000 Hz / mono / s16，20 ms 时 960 samples，application AUDIO，complexity 5，bitrate 默认 90000，VBR true，FEC false，DTX false。设备代码也支持 60 ms 分支；本 host 对照固定 20 ms，需在运行 metadata 指明。

Host FFmpeg 重采样与 ESP rate converter 是不同实现，只能声称使用相同源 PCM；不能声称重采样后的 PCM 字节相同或 encoder payload 字节相同。记录 source SHA256、keywords canonical JSON SHA256（facts.json 已保存）、派生 pcm48 SHA256、padding samples、libopus 版本及 CTL 回读值，使这个差异可复查。

## 可执行命令形态（未执行）

对于已经补齐末帧/额外静音的 16 kHz s16le，后续可使用：

```sh
ffmpeg -hide_banner -loglevel error -nostdin \
  -f s16le -ar 16000 -ac 1 -i pipe:0 \
  -af aresample=48000 -ar 48000 -ac 1 \
  -c:a pcm_s16le -f s16le pipe:1
```

Python subprocess.run(argv, input=padded_pcm16, capture_output=True, check=True) 捕获 stdout，检查总字节数是 1920 的整数倍且对应预期帧数。不能把普通 pipe 的 read() 返回块当成音频包或音频帧；先按固定 PCM frame 字节数分割。

## ctypes 编码/验证伪代码（未执行）

此片段表达所需 API 签名、参数和校验；需要在实现中补齐日志、退出清理及错码处理。Apple ARM64 对 variadic 函数有特殊 ABI；声明 ctl 的两个固定参数，额外传 ctypes.c_int。见 [Python ctypes 官方说明](https://docs.python.org/3/library/ctypes.html#calling-variadic-functions)。

```python
import ctypes as C
import hashlib, struct

I = C.c_int
I16P = C.POINTER(C.c_int16)
U8P = C.POINTER(C.c_ubyte)
L = C.CDLL('/opt/homebrew/opt/opus/lib/libopus.0.dylib')
L.opus_encoder_create.argtypes = [I, I, I, C.POINTER(I)]
L.opus_encoder_create.restype = C.c_void_p
L.opus_encoder_ctl.argtypes = [C.c_void_p, I]  # fixed variadic args
L.opus_encoder_ctl.restype = I
L.opus_encode.argtypes = [C.c_void_p, I16P, I, U8P, C.c_int32]
L.opus_encode.restype = C.c_int32
L.opus_packet_get_nb_samples.argtypes = [U8P, C.c_int32, C.c_int32]
L.opus_packet_get_nb_samples.restype = I
L.opus_decoder_create.argtypes = [I, I, C.POINTER(I)]
L.opus_decoder_create.restype = C.c_void_p
L.opus_decode.argtypes = [C.c_void_p, U8P, C.c_int32, I16P, I, I]
L.opus_decode.restype = I
L.opus_encoder_destroy.argtypes = [C.c_void_p]
L.opus_encoder_destroy.restype = None
L.opus_decoder_destroy.argtypes = [C.c_void_p]
L.opus_decoder_destroy.restype = None

err = I()
enc = L.opus_encoder_create(48000, 1, 2049, C.byref(err))  # AUDIO
assert enc and err.value == 0
# set/get pairs from installed opus_defines.h
settings = [
    (4002, 4003, 90000, 'bitrate'),
    (4006, 4007, 1, 'vbr'),
    (4010, 4011, 5, 'complexity'),
    (4012, 4013, 0, 'fec'),
    (4016, 4017, 0, 'dtx'),
]
for set_req, get_req, value, name in settings:
    assert L.opus_encoder_ctl(enc, set_req, I(value)) == 0
    actual = I()
    assert L.opus_encoder_ctl(enc, get_req, C.byref(actual)) == 0
    assert actual.value == value, (name, value, actual.value)
    # Record actual.value in run metadata.
app = I()
assert L.opus_encoder_ctl(enc, 4001, C.byref(app)) == 0
assert app.value == 2049

# Produce pcm48 by the subprocess command above, using source PCM which has
# passed frozen SHA256 verification, zero-padded to the defined timeline.
assert len(pcm48) % 1920 == 0
pcm48_sha256 = hashlib.sha256(pcm48).hexdigest()

dec = L.opus_decoder_create(48000, 1, C.byref(err))
assert dec and err.value == 0
encoded = (C.c_ubyte * 4096)()  # allocation cap, not network payload budget
pcm_out = (C.c_int16 * 960)()
packets = []
decoded_pcm = bytearray()
for offset in range(0, len(pcm48), 1920):
    frame = (C.c_int16 * 960).from_buffer_copy(pcm48[offset:offset+1920])
    n = L.opus_encode(enc, frame, 960, encoded, len(encoded))
    assert n > 0
    assert L.opus_packet_get_nb_samples(encoded, n, 48000) == 960
    packet = bytes(encoded[:n])
    # Compare n against the configured RTP payload budget before transmission.
    # Keep tiny silence packets: do not apply energy filtering or skip them.
    packets.append(struct.pack('>I', n) + packet)
    decoded = L.opus_decode(dec, encoded, n, pcm_out, 960, 0)
    assert decoded == 960
    decoded_pcm.extend(bytes(pcm_out))
L.opus_decoder_destroy(dec)
L.opus_encoder_destroy(enc)
# Emit packets with per-packet boundary preserved; never concatenate payloads
# without lengths. Record packet-count/hash/length distribution and PCM hashes.
# Decode output is a local media positive control, not server-ASR success.
```

用于连续 live timeline 时可按需要编码后续静音，不应把每段 encoder reset 当成与设备相同的实现。若先离线编码一个完整脚本时间线再发送，生成顺序和实际发送顺序应一致；不要跳过中间已编码的包而仍宣称连续状态相同。

正对照需要报告：全部 opus_packet_get_nb_samples==960、全部 decoder 返回 960、编码/解码错误数为零、源 PCM/pcm48/decoded PCM voiced 区段 RMS/peak、源与 decoded 试听句子可辨识。Opus 有延迟且有损，不能要求 decoded PCM hash == source/pcm48 hash；如计算波形相关性必须先补偿 codec lookahead。这里只证明 host 输入到可解码 Opus，不证明服务端实际收包或 VAD/ASR。

CTL 行为由 [Opus CTL 官方 API](https://opus-codec.org/docs/opus_api-1.5/group__opus__encoderctls.html) 定义；实际数字同时读取了当前机器 opus_defines.h。decoder API 见 [Opus decoder 官方说明](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html)，签名可直接对照本机 `/opt/homebrew/opt/opus/include/opus/opus.h`。

## FFmpeg 直接编码备选为何需要 framing

`ffmpeg -h muxer=opus` 明确表示 Ogg Opus（MIME audio/ogg）。`.opus` 文件与 `-f opus` 不能直接当作 RTP payload。Ogg 包头包括 OpusHead、OpusTags；audio packets 还可能跨页，不能简单剥固定头或“一页一包”。如采用此路线，需 Ogg demux（本机 ffprobe 可用）取得每个 audio packet，再写上述 length framing。可用未执行命令形态：

```sh
ffmpeg -hide_banner -loglevel error -nostdin \
  -f s16le -ar 16000 -ac 1 -i frozen.pcm \
  -ar 48000 -ac 1 -c:a libopus -application audio \
  -frame_duration 20 -b:a 90000 -vbr on -compression_level 5 -fec 0 \
  -f opus out.opus
ffprobe -v error -select_streams a:0 -show_packets -show_data \
  -show_entries packet=pts,duration,size,data,side_data_list -of json out.opus
```

从 ffprobe 的 per-packet hex dump 解码，只接受 audio packet 的 bytes，检查恢复长度等于 size；不能把 codec extradata 当 audio payload。为降低“手写 Ogg parser”的成本，可由现有 ffprobe 负责解封装。若手写 Ogg demux，需 lacing value 255 跨 segment/跨 page 累积、终止值 <255 才结束 packet，并剔除两个头包。[RFC 7845 §3](https://www.rfc-editor.org/rfc/rfc7845.html#section-3)

FFmpeg `-f data` 的 raw writer 只是依次写出 AVPacket.data，不保留长度/边界；有 byte stream 不等于有 individual packets，不能依靠 pipe read 块分割 Opus。源码见 [FFmpeg 8.0 rawenc.c](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavformat/rawenc.c#L29)。如果配合单独 per-packet size 输出才可重新 framing，但复杂度高于直接 native API。

FFmpeg 8.0 libopus 的实际帮助与 [同版本 option table](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavcodec/libopusenc.c#L501) 不含 dtx 选项，也没有 OPUS_SET_DTX 调用。不要编造 `-dtx 0` 参数。基础 libopus DTX 默认关闭；仍须验证未修改该默认及包时间线。直接 native CTL 设置并回读可减少此不确定性。Ogg 还涉及 pre-skip、EOS trim/flush；ffprobe 的最后一个 duration/side-data 可能描述文件裁剪，不应直接用其 duration 决定 RTP 增量，必须检查 raw packet 的 opus_packet_get_nb_samples==960。

## opusscript / opusenc

opusenc 本机未安装；即使安装，产物通常是 Ogg Opus，仍需 demux，因此不节约工作。opusscript 0.1.1 仅出现在另一应用 openclaw 的全局依赖树，声明 libopus 1.4/WASM，当前 probe 项目没有它。未执行 opusscript 编码验证，不建议引用别的应用的绝对 node_modules 路径或为这次探针新增未经实测 npm 依赖。

## Host probe 接入需要保留的证据

每次 run 记录 source fixture ID/hash/keywords，派生 PCM48 hash，padding 和帧数，libopus 版本/CTL 回读，逐包 samples/length，RTP PT111、SSRC 与 SDP 的绑定，显式 RTP timestamp 每包 +960、monotonic 20 ms pacing 及发送成功/失败数。剥离四字节长度前缀，仅 raw Opus 进入 packetizer。SDP Opus mapping 仍为 /48000/2，即使编码 PCM 是 mono。

与设备共享关键词验收及配置 ACK gate：配置 ACK 完成后开始发 fixture，尾静音/等待 ASR 时继续发静音包，不将“没有媒体”伪装成静音。按 item_id 归属完成转写，判断 frozen keywords 是否命中。网络会话和凭据的执行由父任务负责，本只读核查没有作出任何 host 媒体成功结论。
