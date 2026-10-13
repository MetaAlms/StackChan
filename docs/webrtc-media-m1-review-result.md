# M1 正式复核：需要修复，尚不放行 M1.5

Reviewed exact HEAD: `c3e658200d8365ac4bbb19a28290d491f2c04df3`。
PR：[MetaAlms/StackChan #1](https://github.com/MetaAlms/StackChan/pull/1)。
请求：[REVIEW_REQUEST](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6044400269)。
正式评审：[Review 5446817816](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5446817816)
（同账号PR使用COMMENT提交，正文决定为需要修复；commit_id已回读核对）。
本文件复核已提交的源码与证据；工作区后来补充的预检不是该提交的内容。

结论：**需要修复；M1 未通过，M1.5 保持 HOLD。**
32KB 独立 sender、实际16→48k转换容量、单帧编码字节换算、nano printf修复已有进展。
但 VAD / ASR 未达成，且已声明修齐的生命周期、ASR归属与时间轴仍有实际缺陷。
不是把发送API成功或一次长采集当成完整媒体验收。

## M1-1 [P1] 恢复 M0 的 session.update

`firmware/main/hal/webrtc/webrtc_m0.cc:47–60,79–87` 仅记录 created/updated，
没有任何 `SendJson`。基线 `57ab76e` 的 `send_session_update` 在收到
session.created 后沿该事件stream发送，channel回调允许重试；这段逻辑被重构删除。
当前M0仍等created+updated才PASS，因此正常互通也可能永远只有created。
编译通过不能证明其探针语义保留。

修复：恢复本会话正确stream上的M0 update与成功后标记，避免持观察锁发送。
保持不发媒体；用真实M0握手记录created/update发送/updated/最终判据，
同时核对连接失败不会被报告为PARTIAL成功。

## M1-2 [P1] cleanup 忽略退出等待结果，超时仍释放任务使用中的对象

`webrtc_transport.cc:442–463` 不检查两个 `xSemaphoreTake` 的结果，
随后关闭peer并删除全部信号量。task创建失败时也等待根本未创建任务的退出信号；
`signal_body:300–319` 在Stop唤醒SDP等待后不检查stopping，仍发HTTP、向peer喂answer。
25秒超时不是任务已退出的证明，HTTP/DNS/TLS/peer内部阻塞不能靠注释保证。
正常Stop之后信号量字段已置空，串行重复Stop本身未发现重复释放；
修复超时/部分初始化路径时仍须维持这种幂等性。

修复：只有实际创建且尚未确认退出的task才等待；真实确认双方不再使用p/peer后
才能释放；超时报告失败并保留仍被访问的资源（包括Impl自身，析构不能删除活动task的ctx），
或使用可核验的取消/退出路径，
不能以延时/超时当join。信令唤醒后与HTTP返回后均检查停止。
Stop/初始化失败/析构统一幂等清理；M1 sender创建失败也释放join semaphore，
MediaSender.Open任一步失败清理已打开资源。

验证：正常close、等待SDP时stop、信令失败/超时、task创建失败/部分初始化、重复Stop；
记录退出顺序，证明没有close/send/poll并发或后台对象释放后的访问。

## M1-3 [P1] 保存 item_id 不是逐条归属，迟到事件仍进入下一条槽位

`webrtc_m1.cc:591–610` 没从VAD/committed事件绑定item；任意completed都写当前槽。
`sender_task:938–960` 清空槽后，上一条超时仍继续下一条；
`WaitForClipAsr:820–837` 只读flag，不比较实际item_id。
例如zh_1超时后zh_2开始，zh_1迟到completed会被算作zh_2。
缺失/错误类型的item_id或transcript也会直接设置completed。
文档“迟到完成永不归给下一条”与实现不符。

修复：按本会话VAD/committed的实际item建立归属，completed/failed仅更新匹配项，
验证必要字段并保留无法归属事件。最小安全策略可以首条超时/failed后停止验收，
不再开始后续clip；不能清槽后继续吞旧结果。
独立记录每条VAD/ASR、item、失败与关键词；预先定义数字等价，不在失败后放宽关键词。

验证：正常三条、第一条超时后迟到、错误item、failed、缺失字段，
确认旧结果不能算新clip通过。

## M1-4 [P1] media/wall 记录同一个墙钟，且循环时长包括验收等待

`webrtc_m1.cc:774–776` 给media_us与wall_us加入同一表达式。
相等是代码必然结果，不是独立时间轴证据。只统计StreamClip，漏掉静音/ASR等待。
`Arm:660` 在每个clip和静音开始重置期限，隐藏边界开销与累计漂移。
run7实际媒体范围0..330400ms，报告却是222470/222470ms。
`g_run_start_us:1134` 在三条验收之前，而循环deadline在973行之后创建；
报告329395ms超过300秒仍是INTERRUPTED，实际持续loop只有约256秒。

修复：样本/帧计数推导媒体时长，独立测同一范围的墙钟，单一连续pacing期限，
记录丢帧/转换余量/补齐；独立记录持续loop的真实开始、结束、elapsed与COMPLETE。
报告时清楚区分总发送、三条验收、持续循环，不能把总墙钟当300秒loop通过。
当前定长耗时容器只收前4096帧，即20ms路径的前81.92秒，
不能把该分位当整个持续loop负载；采用有界覆盖持续阶段的采样，或明确限制覆盖范围。

验证：20/60ms固定fixture与ASR等待，帧数/每帧样本/pts范围/墙钟互相核对；
故意提前断线必须得到loop失败与实际时长。

## M1-5 [P1] send=0 不证明 SRTP/socket 成功，补诊断完成媒体验收

`docs/webrtc-media-m1-result.md:12–16,26–29,87–99` 与RQ把“媒体发送通过”、
“本地发送失败已排除”、“时钟已排除”写得过强。
锁定1.5.5二进制的 `rtp_encoder_encode_generic` 调void packet callback后固定return0；
`write_rtp_packet` 自身不向上传递SRTP encrypt或agent/socket失败。
因此16521次SendAudio返回0连底层socket成功也不能保证，VAD0不能直接归为
“服务端收到但没当作语音”。Opus `pts*48` 仍有二进制依据，不能无证据推翻；
两条SDP输出路径已独立确认固定 `opus/48000/2`，mono PCM不是非法/1 SDP根因。

修复：增加本阶段必要、受控且可重建的诊断，核对设备重采样输出能量、raw Opus
packet帧数/时长与本地解码可听/能量，SDP媒体是否接受以及RTP PT/seq/ts/SSRC/
发送长度/实际SRTP或UDP写入结果。只输出白名单非敏感元数据，不输出key/Authorization/
完整SDP密码。可用公开扩展/weak接口或跟踪的局部补丁，不改未知内部对象布局。
先定位无VAD根因，修复后用同源3条20ms、3条60ms核对配置/发送/VAD/completed
四层及关键词，并获得完整300秒loop。不得降48k、用手动commit掩盖server_vad基线，
或以半双工代替未验证的媒体链路。若遇真实组件/服务阻塞，交具体证据和可执行方案复核。

## M1-6 [P2] 握手失败被误报为会话配置失败

`webrtc_m1.cc:1110–1117` 无论当前状态都说configuration failure/not transport failure。
run4/5的-0x6800是Mbed TLS3.6.5 `MBEDTLS_ERR_SSL_TIMEOUT`，
尚无CONNECTED/DataChannel/session.created。`dtls_common.h:211` 的
SRTP connected OK是导出密钥时创建context，握手成功在643行，不能混用。
同型timeout在run1/2/3后续重启已出现，不是32KB sender回归；HELLO race尚未证实。

修复：按SDP/ICE/DTLS-CONNECTED/DataChannel/会话更新的实际截止阶段报告，
超时和断线尽早停本次测试；保留有次数上限的全新连接诊断记录，不能无限重试掩盖。
若DTLS再次阻塞，用非敏感datagram/handshake状态元数据定位，不能预设根因升级1.5.6。

## M1-7 [P2] 交付证据与 PR 范围仍不完整

结果文档没有所称“全部命令、参数、脱敏日志”，没有60ms证据、完整loop，
编译/隔离重建只有结论；SPEC/PLAN仍是设计阶段状态，PR正文还称仅文档/未改固件。
全范围 `git diff --check 57ab76e c3e6582` 发现
`firmware/patches/stackchan-aliyun.patch` 第47/67/100/119行行尾空白。

修复：补实际命令、精简flags、IDF/组件版本与lib/hash、实现HEAD/ELF fingerprint、
逐条原始脱敏结果与统计，分清通过/失败/未做。保存对应ELF关联，避免覆盖后误定址。
候选跟踪源码在隔离checkout从fetcher/patch/lock恢复，验证M1及正常WS构建；
本轮改掉M0行为后补其握手回归。修空白须保持patch仍可正向/重复应用。
更新SPEC/PLAN/调度/PR为实际阶段；新增已证缺陷按AGENTS落findings，
首轮异常分配原因仍不足以宣告已定位OOM，不应推广栈溢出证据为“永远不是分配问题”。
末尾必须有根据真实验收计算的最终VERDICT，失败与仅完成采集不可混用。

## 修复授权与交付

只修上述 M1 findings 及其必要诊断/验证，保持48k PCM、esp_peer1.5.5、正常WebSocket回退。
M1.5/下行产品AudioService/UI继续HOLD；不merge、发布、部署、改上游。
核对Review ID与reviewed exact HEAD → 修复 → targeted/真机/回归/隔离重建 →
commit+push → 绑定新的full HEAD提交REVIEW_REQUEST → STOP。
不要重写既有历史，不以旧采集当新HEAD验收。
