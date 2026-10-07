# M1 未完成诊断的聚焦复核：先执行标准栈媒体对照

Reviewed exact HEAD: `3ab11e9823866fa0979298caa71184c15bb6c478`。
PR: <https://github.com/MetaAlms/StackChan/pull/1>。
REVIEW_REQUEST: <https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6046130821>。
前次 Review: `5447785013`。结论 **needs fixes，M1未通过**。
正式 Review: [5447928154](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5447928154)，
已 read-back 核对 exact `commit_id`。

## R4-1 [P1] 标准栈媒体正对照仍未执行，本轮先完成这一个必要实验

RQ 明确承认 R3-4 标准栈媒体对照未实现、未运行。
设备既有零 VAD 与后来的 DTLS 超时不能替代它；
当前无设备或云访问权限方面的已证阻塞，也不需要先修设备 DTLS 才能在 host 执行。
继续修改计数/追加文档并没有取得可区分服务配置与设备媒体互通的证据。

**本次修复只聚焦 host 媒体正对照**，先把它完成，再由 Codex 根据实际结果
安排剩余设备修复。M1 其他未完成项保持记录，不能以此次实验代替 M1 验收。

执行范围与验收：

1. 在 `tools/webrtc_probe` 新增可重跑的媒体对照脚本及必要编码辅助脚本。
   使用现有 node-datachannel 官方 API 与本机 libopus/ffmpeg；
   不改变当前固件、设备配置或已冻结 fixture。
   先读取 `docs/webrtc-media-m1-standard-control.md` 与
   `docs/webrtc-media-m1-host-encoding.md` 中已经核实的接口，
   这些文件是实现参考，不是已经执行的实验。
2. 源固定为 `firmware/main/hal/webrtc/fixture/zh_1.pcm`、`zh_2.pcm`、`zh_3.pcm`。
   记录源 hash/样本数，真实 16→48k mono s16 转换，编码为 20ms/960 samples
   的单个 raw Opus packet；检查帧长/包长及本地解码。
   48k PCM 是本任务要求，不以 16k 编码或 Ogg 数据块代替。
3. 同一 host 会话完成 SDP/ICE/DTLS/DataChannel，保留实际 Track 对象。
   沿 session.created 所在通道回复并等待 session.updated 的精确配置核验：
   server_vad、threshold 0.5、silence 800ms、qwen3-asr-flash-realtime。
   配置失败单独报告，不发送手动 commit 掩盖 server_vad。
4. 按 SDP 已宣告的 PT111/Opus48000/2/SSRC 发送媒体。
   使用官方 API 支持的完整 RTP 或正确配置的 packetizer，不能把
   addTrack 仅用于 SDP、把 Ogg/chunk 当 Opus、把 payload 当完整 RTP，
   也不能假设 sendMessageBinary 自动推进 timestamp。
   同一连续 20ms 绝对 pacing；RTP timestamp 每包按 960 推进；
   明确源 hash、转换/编码设置、实际 packet 数/时钟/墙钟/失败。
5. 每条包括冻结的前/后静音；等待期间持续按相同 cadence 送静音。
   精确处理实际 VAD/committed item_id、completed/failed，旧/重复事件不得
   释放当前条。三条分别记录 VAD start/stop、完整 ASR、item_id、关键词。
   遇超时/failed 停当前验收序列并如实 FAIL；结果归属未成立则实验无效。
6. 真正执行 host 对照，并保存脱敏原始输出、重跑命令、依赖版本、
   source/implementation full HEAD 与实际结果。stdout 不输出
   API key/Authorization/完整 SDP/ICE 密码/音频负载。
   凭据仍从 ignored 本地配置读取；不提交、复制到跟踪文件或文档。
   个别连接/ASR 有正常测试 timeout，但不设未经用户要求的总验证预算。

交付必须包含真实执行结果，不能仅交付“还需运行”的脚本或设计。
host 三条通过只证明此次标准栈/模型/配置能处理同源音频，
不证明设备发出相同字节，也不证明设备媒体、AEC、全双工、产品移植通过。
host 失败时保留准确 reached stage 与原始结果，先定位本 host 实验的
必要配置/时钟/编码/事件问题；不能以首次失败直接认定服务端不支持。

## 剩余项与边界

当前 `rtp_send_probe.cc` 与前次 HEAD 字节未变，新增
protected_unwritten/length_mismatch/wrote_after_protect_fail 只存在于头文件，
没有实际更新；RQ 对 R3-1 的部分“已修”描述与代码不符。
item_id、wall/loop、sender semaphore、DTLS 元数据、回归/重建等也仍未完成。
这些必须保留为未解决，后续由 Codex继续派修，不在本次 host 单任务里顺带修改。

保持 esp_peer1.5.5、48k PCM、M1.5与产品下行/AudioService/UI HOLD。
不 merge、发布、部署、改上游或改变现有设备固件。
只修 R4-1 → host实际执行与证据 → commit+push →
绑定实际新 full HEAD 的 REVIEW_REQUEST → STOP。
