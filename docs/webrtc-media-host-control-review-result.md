# Host 媒体对照复核：实验有效性与 speech_stop 缺失仍待处理

Reviewed exact HEAD: `3b0844e0b6b3908995ad4a4023b1bf7b84b62f89`。
PR: <https://github.com/MetaAlms/StackChan/pull/1>。
RQ: <https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6046306584>。
前次 Review: `5447928154`。结论 **needs fixes，host正对照及M1均未通过**。
正式 Review: [5448053831](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5448053831)，
已 read-back 核对 exact commit_id。

本轮确实执行了 host 媒体实验，配置回显成立，观察到 speech_started，
没有 speech_stopped/completed。这是实测进展；仍需修下列有效性缺陷。
本次只派 host 修复/必要诊断，当前固件和设备不改，其他M1项保留未解决。

## H1 [P1] 发送 bool 返回与时间范围未验证，负结果不能归因

`standard_media_control.js:247` 忽略 sendMessageBinary 的实际 bool 返回，
无 exception 即计 packets；官方 wrapper 返回的是 Track 的真实接受结果。
方向不适配/密钥未就绪等路径会返回 false 而不 throw。
因此 packets=1335/sendErrors=0 没有证明1335次发送均被接受。

`:239–256` 每段重新 Date.now/deadline，缺少单一单调起点/持续实际发包时间与
相同覆盖范围的墙钟。新段重置掩盖边界开销；名义 timestamp+960不能代替 pacing 实测。

修复：检查 track.isOpen/媒体ready；按真实 bool 统计 accepted/false/exception，
任一异常使有效性门槛失败，仍与远端接收区分。共享 monotonic 绝对 deadline，
frames/样本/pts/实际 wall同覆盖，记录边界、迟到、额外静音及中断。
纯逻辑/可控 send stub 验证 false 与时间范围，再实际运行。

## H2 [P1] VAD/item未归属，关键词生成不完整会出现假通过

`standard_media_control.js:125–129` 丢弃 VAD 的 item_id，
`:285–290` 将全会话累积 VAD 和最后 ASR 填当前条。
后续条可以借用前条VAD，重复旧完成可释放下一条；failed 只累计错误，不终止当前。

`encode_fixture.py:148–161` 的 manifest 没有冻结 text/keywords。
虽然此次 /tmp manifest 被临时补上，重新运行 generator 又会丢；
runner 对缺少 keywords 回退 []，0/0 会被当作关键词全中。

修复：generator稳定生成三条实际文本/关键词，runner检查其非空/合法，
不得以缺失的验收数据通过。使用实际 VAD/committed item_id及本条窗口绑定，
完成/失败只接所属item；未知、旧、重复、缺字段不得释放或误失败当前。
逐条都要有效 start/stop/完整ASR/关键词；冻结已经完成的结果。
以轻量事件输入验证正常、旧/重复/错误item、failed、缺字段、关键词缺失，
然后实际运行三条，不把跨会话累计数当逐条证据。

## H3 [P2] 通道记录与失败结果产物尚不可靠

`onEvent` 未接收实际channel，server通道事件仍贴client标签；
eventChannel只是标签，sendUpdate始终使用client dc且忽略其bool。
按实际本会话channel对象回复、正确记录label、检查返回并保留重试。
此次配置确已回显，不能把这个问题当作当前VAD不结束的已证根因。

配置失败分支调用 finish 时，后面的 const PT/stats/clipResults/allOk
尚未初始化，触发 temporal dead zone，无法保存失败结果。
把所有结果状态先初始化，任何reached stage/失败都有可用证据；
清理本轮track/channel/peer，退出码与计算verdict一致。
验证配置失败、连接超时和正常结束，不新开云会话验证纯变量生命周期。

## H4 [P1] 继续完成 host 必要诊断，不能从 start 推出整个服务配置已排除

RQ声称“服务端/模型/配置无法处理这段音频已排除，问题收窄到设备侧”，
证据仅是 host start=1、stop=0、完整ASR=0。
它支持“某些输入到达并触发VAD起始”，不足以证明完整语音识别路径或排除共享问题。
订正文档/PR结论，保留三次真实尝试；不得把host失败叫完整正对照。

在H1–H3成立后，继续短时定位 stop缺失：

- 保存每次尝试独立的脱敏原始输出与结果文件，不覆盖前次；
  记录精确事件type、VAD/committed的非敏感item与audio_start/end_ms、
  完成/失败与相同媒体时间范围，未知事件只记录type，避免漏掉实际流程。
- 静音packet的实际PCM来源/生成参数和本地解码样本数/能量必须可核查。
  当前 silence_packets.json 有包字节，但没有可重跑生成过程与解码/能量证据。
  数字零与随机底噪不是同一波形，作为独立尝试记录，不能混写。
- RtcpSrReporter的公开实现直接读实际RTP头timestamp；本轮已对齐config.timestamp，
  没有证据支持它是 stop缺失的根因。撤回运行中预检的该条件猜测，
  不反复靠修改此字段重试。
- 保留0.5/800/同模型为第一配置基线。任何对照只改变一项并标注诊断条件，
  不用手动commit或换阈值冒充原server_vad验收。必要时核对当前官方接入/事件
  契约与实际字段，再修本实验的必要配置/媒体/事件问题。

可执行的条件诊断：对已本地核验的数字静音packet使用公开 `opus_packet_pad`
合法填到64B，再核验同前序decoder产生相同960样本/PCM；保持配置与RTP时间轴。
API成功返回0，发送长度应显式64，buffer必须有64B容量；失败停止该诊断。
这是保持音频内容、改变Opus framing/长度的对照，RTP P-bit仍0，
不是RTP padding。即便结果改变也不能单独证明服务端短包过滤。
可按该对照或实际事件/官方契约证据选择下一短诊断，不盲改配置。

独立离线验证已通过：本机 libopus1.5.2 的100个数字零20ms包均为3B，
分别从相同零前序和完整语音前序把后续包合法pad到64B，每包仍为960样本、
解码PCM逐字节相同且零段peak/RMS=0；冻结zh_1语音的单包/连续padding
也保持目标及全部后续解码PCM相同。结果与输入副本在
`/tmp/stackchan-opus-padding-verify/README.md`、`results.json`；
这是API条件验证，没有执行云端/设备实验，也没有替当前silence JSON补验证。
注意 `new_len == len` 会直接返回0，不能仅用pad返回码验证包合法性。

host三条真正通过后再交付；若出现已证外部阻塞，提供具体尝试/原始结果/
可执行方案，不能以“还需运行/原因未定位”或自设预算替代已授权诊断。
host结果不能独立证明设备媒体、AEC或产品已完成。

## 执行

只修 H1–H4 的 host代码/必要实验及记录，保持真实48k PCM/同源fixture；
固件/现有设备保持，其他M1项未解决，M1.5/产品接入HOLD。
不 merge、发布、部署、改上游。
读取本Review并核对full SHA → 修复+纯逻辑验证+实际host诊断/三条验收 →
commit+push → 实际新full HEAD的REVIEW_REQUEST → STOP。
