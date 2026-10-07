# M1.5 接入准备：只读调查记录

状态：**准备记录，尚未派发 M1.5，也不是 M1 的放行结论**。
2026-10-08，M1 真机验证进行中；调查基于当前 esp_peer 1.5.5 和 AudioService。
后续阶段的执行基线以 Codex 的实际 M1 放行评审为准。

## 可复用的部分与必须新增的边界

- `xiaozhi-esp32/main/audio/audio_service.cc:283` 的输入任务按10ms读取板级24k，转换到16k后送AFE。
  `audio/processors/afe_audio_processor.cc:155–184` 的VAD只通知状态，输出仍含静音PCM。
  因而可保持同一采集任务、AFE实例连续运行，播放期间不切换voice-processing enable。
  反复enable会清下行、复位转换器并引入120ms warmup（`audio_service.cc:613`）。
- 当前AFE输出回调固定进入旧Opus编码队列（`audio_service.cc:101`），尚无公开PCM sink。
  最小接入是明确选择的WebRTC PCM sink、有界非阻塞移交，再由唯一worker做16→48转换及48k编码。
  旧发送队列已经是16k/60ms/DTX Opus，不能代替用户要求的48k PCM路径。
  AFE现有60ms PCM可按实际输出积累并切成20ms，无需改全局帧常量。
- `ReadAudioData` 的转换器初始化固定24→16（`audio_service.cc:86,199`）；传入48000并不会重新配置。
  输入转换错误必须可观察，不能忽略返回码并继续将不完整采集当有效实验。
- 下行需固定48k mono raw Opus decoder、持久48→24转换器及拥有PCM的播放入口。
  现有decoder会随帧时长重建（`audio_service.cc:463`），不适合直接复用为WebRTC实现。
  `esp_opus_dec.h:56` 的INVALID帧时长只按60ms算容量；最大120ms包需要明确足够的容量，
  检查consumed、decoded_size、needed_size与错误，不因packet时长变化重建decoder。
- 网络回调与AFE回调不得等待队列腾空。原WS路径的133/40包及无限等待契约
  （`audio_service.h:56`、`audio_service.cc:516,533`）不能直接移植；预算应以时长、字节和延迟计。
  原Application的speaking状态和Aliyun WS发送门控会停采集或丢上行，本阶段探针需绕开。

## 本地停止与资源所有权

取消时先关闭下行准入、推进playback epoch、清待处理下行；插话上行继续。
拥有的packet/PCM携带session和playback epoch，worker在处理前、处理后和每次硬件提交前核验。
输出按不超过20ms的小块执行，检查与提交之间要有同步边界，避免取消后仍提交旧PCM。
decoder/reset/resampler由同一worker串行管理，不允许处理与reset并发。

现有`ResetDecoder`只清待处理队列，不能撤销锁外正在解码或输出的任务
（`audio_service.cc:313,359,396,702`）。`AudioService::Stop`和AFE Stop没有join，
重连宜保留长期音频服务，只切epoch并清未完成数据；若销毁对象，须先补真实停止确认。

CoreS3的Read/Write在底层错误时仍返回请求样本数（`main/hal/board/cores3_audio_codec.cc:272–283`），
OutputData又丢弃返回值（生成源码`audio_codec.cc:17–19`）。计数本身不证明真实采集或播放成功。
需要最小的硬件成功/失败计数以及实际输出时间观测。
DMA为6×240、24k，名义约60ms；底层write允许1000ms阻塞。
清队列、write返回或response.done均不能证明扬声器已停。
不得关闭共享TX时钟来停音并污染RX；尾音预算要实测。

## RTP归属尚不能冻结

`esp_peer.h:139` 的音频回调只给pts/data/size，无seq、SSRC、marker、原始timestamp或response_id。
当前1.5.5接收pts为`floor(raw_timestamp * 1000 / 48000)`，未展开回绕。
公共receiver RTP transformer（`esp_peer.h:565`、`esp_peer_types.h:65`）
可在连接前注册，在SRTP解密后、jitter入队前观察完整RTP元数据；它也不会自动提供response_id。
内置jitter无单独flush/reset接口，缓存timeout不是网络旧包迟到上限。

二进制证据：esp_peer源码commit `c8650846b512e6e1375e5f78c1c41619b8d645eb`，
S3库SHA256 `25a338fc6b05f702947c2af0e924f2dd3006503908e889dcdc512782d0ea3a58`；
反汇编`peer_insert_rtp_payload`先调用transformer，再调用`rtp_jitter_add`。

[阿里WebRTC文档](https://help.aliyun.com/zh/model-studio/realtime-webrtc-access)将媒体放在RTP，
事件放在DataChannel，当前未找到跨通道顺序或RTP与response_id的映射保证。
[客户端事件](https://help.aliyun.com/zh/model-studio/client-events)的response.cancel没有排空旧RTP契约；
[服务端事件](https://help.aliyun.com/zh/model-studio/server-events)的response.audio.done表示生成结束，
包括取消/不完整生成。[RFC 3550](https://www.rfc-editor.org/rfc/rfc3550.html#section-5.1)
的seq/SSRC/timestamp标识包顺序、媒体源、采样时间，并不是响应编号。

关键反例：A的未到达RTP在B的response.created之后才到达；接收时套当前epoch会把A误标成B。
本地generation隔离可验证，但不能凭此宣称网络迟到旧包已隔离。
静音等待、seq水位、response.created/done不能未经依据成为重新开放播放的可靠边界。

## 下一阶段应执行的实验

1. 真实mic→AFE16→48编码/RTP，与raw下行→48解码→24播放同时工作；先验证输入/输出硬件状态和上行连续性。
2. 用可离线辨认的A/B回答，记录单调时间、response/item/status、RTP元数据、callback pts、
   queue epoch、解码起止、PCM提交/停音和上行计数；不提前按事件给packet标A/B。
3. 覆盖首包前取消、播放中取消、生成结束但PCM未播完的取消，以及B开始后旧A事件/包迟到。
   分开验证本地延迟decode/队列项的epoch隔离，以及媒体与控制跨通道迟到/乱序。
4. 按PLAN 1.5.0f评估可重复规律与协议依据；若仍无法建立可靠边界，报告前置未完成，
   不伪造T3/T4通过，也不把前置缺陷归因AEC。Codex据实测决定后续处理。

所有生成源码改动必须由tracked patch恢复；WebSocket路径和公共提示音保留。
