# 真实媒体接线准备（只读）

2026-10-13：Codex 独立只读调查，不构成 M1/M1.5/M2 放行，未派 DSH 实施。
完整静态证据临时位于 `/tmp/stackchan-media-integration-audit/README.md`；
关键约束在此落盘。当前 U1 构建使用同源短模式；这些接线修改留给后续阶段。

| 接线处 | 当前代码事实与后续最小改动 |
|---|---|
| 采集/AFE | `audio_service.cc` AudioInputTask / Initialize 已持有唯一采集任务与24→16k转换。AFE输出是拥有式vector，当前只送旧encode queue；新增WebRTC PCM sink分支，回调只做有界非阻塞移交，回退保留原路径。 |
| 60ms→20ms | `afe_audio_processor.cc` Initialize 当前每块960个16k样本。M1 MediaSender::EncodeFrame每调用只编码一帧并留下carry；直喂60ms块会积累余量。上行先拆320样本或排空完整20ms帧，按采集媒体时间推进并独立pacing，不能改全局WS60ms常量。 |
| 48k编解码/转换 | 复用M1已有16→48转换与48k Opus的容量/单位调用。下行使用独立持久48k mono decoder及48→24转换器，按实际consumed/decoded_size/needed_size处理，保持raw单包边界。 |
| 播放 | AudioOutputTask是现有硬件输出owner；目前没有公开ownedPCM入口。将转换后的PCM入此任务并带playback epoch，不另起第二个硬件writer。codec Write目前返回请求样本而OutputData丢弃返回值，验收前需真实提交结果。 |
| 取消 | ResetDecoder只清队列，不能撤销已出队解码/写入，也未reset输出转换器。处理前后及硬件提交边界都核验epoch；UI speaking不能成为首帧准入条件。网络RTP→response归属仍是M1.5独立未解决问题，不能仅把接收时的当前epoch当归属。 |
| 生命周期 | AudioService Stop / AFE Stop 没有task join；AFE Stop还保留output_buffer。维持长生命周期对象，关闭sink准入后确认桥接worker退出才释放，隔离旧partial PCM/carry。Transport Stop保留impl_，同对象Start会拒绝；旧owner退出后新建对象重连。 |

源码定位：`firmware/xiaozhi-esp32/main/audio/audio_service.cc` Initialize/AudioInputTask/
AudioOutputTask/ResetDecoder/Stop，`audio_service.h:39–43`，
`audio/processors/afe_audio_processor.cc` Initialize/Stop/AudioProcessorTask，
`firmware/main/hal/webrtc/webrtc_transport.cc` Start/Stop/cleanup/cb_audio，
`firmware/main/hal/board/cores3_audio_codec.cc` Read/Write。
生成目录修改必须通过 tracked patches / fetch_repos.py 可恢复，不直接以忽略文件交付。

**事实纠正**：当前 audio_service.h 的decode/send均为 `(2400 / 60)=40` 包，
不是历史133 decode包；默认60ms下均2.4秒。SPEC §4.7与M1.5准备记录已同步。
应用层20ms提交只限定块长；24k的6×240 DMA名义60ms与底层最长1000ms write，
仍须真实停音实测，不能据块长宣称实际停音≤20ms。

下一阶段按已通过的SPEC/PLAN和U1实际候选复核冻结：AFE sink、桥接编解码worker、
现有播放任务PCM/epoch入口，随后协议/UI接线。保持WebSocket完整回退、48k PCM需求，
不预判服务端AEC有效；没有真机结果不宣称全双工、打断或产品通过。
