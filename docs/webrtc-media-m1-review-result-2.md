# M1 修复复核：继续修复，不放行 M1.5

Reviewed exact HEAD: `9e45e63d9ce9083959a50e4420220b26a31da05e`。
PR：[MetaAlms/StackChan #1](https://github.com/MetaAlms/StackChan/pull/1)。
请求：[REVIEW_REQUEST](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6044837128)。
正式评审：[Review 5447065335](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5447065335)，
已回读核对 Review `commit_id` 等于本完整 HEAD。
前次评审：Review `5446817816`，bound `c3e658200d8365ac4bbb19a28290d491f2c04df3`。

结论：**部分修复有效，M1 仍未通过；继续本阶段修复。**
M0的update逻辑已恢复；配置失败分层已修；run9完成持续loop，
本地16351个Opus packet解码成功且有非零语音能量，排除本地全静音/无效Opus这一方向。
这些不能替代实际RTP/SRTP/socket观测和服务端VAD/完整ASR。

**撤回前次M1-7关于patch上下文空行的空白要求。**
patch中表示源文件空行的单个前导空格是合法语法，不应为diff检查破坏补丁。
保留该语法，源码空白检查排除patch语法行并单独验证patch可应用；该项无需修复。

## R2-1 [P1] 超时路径仍删除活动任务的 Impl，并撤销停止状态

`webrtc_transport.cc:493–533` 在等待失败时保持peer/信号量，却把stopping设回false，
且cleaned已提前设true。析构 `~Transport:384–388` 无条件delete impl_。
M0的Transport是局部对象：只要退出未确认，函数返回就会让仍运行的task访问已释放ctx。
把peer留下不是保留全部仍被访问的资源；撤销停止还让loop继续跑。

修复：停止请求保持有效，cleanup成功状态仅在双方确实退出并释放后成立；
失败时连Impl自身也不能析构/释放，必须有明确安全的保留/取消/后续退出路径。
初始化失败与重复Stop沿同一规则；M1 sender join semaphore在创建失败及成功join后释放。
验证正常stop、SDP等待中stop、部分task创建失败、真实/受控退出超时与重复Stop，
确认没有活动ctx被析构以及退出后回调访问观察对象。

## R2-2 [P1] 首条 failed 仍继续，必要字段与实际归属仍未验证

`webrtc_m1.cc:1082–1104` 只在 `!got` 时停止。
WaitForClipAsr遇到failed也返回true，因此失败项还会进入下一条；
`completed:718–725` 对缺失/错误类型的item_id/transcript也设置completed，
VAD只设flag，从未建立实际item匹配。
保存item_id字符串并没有解决前次M1-3。

修复：至少timeout或failed均停止本次短句验收，必要JSON字段验证；
用实际VAD/committed的item_id绑定完成/失败，未知或旧item不得释放当前clip。
结束验收后的资源循环事件不得改变已冻结的clip结果。
通过轻量事件输入验证正确item、迟到/重复/错误item、failed、缺失字段；
不需要重跑5分钟来验证这类纯事件逻辑。

## R2-3 [P1] media/wall 范围仍不同，loop计时与持续负载采样未修

`webrtc_m1.cc:883,906` 仍在clip/静音开始重新Arm。
`wall_us:899` 仅累计clip；frames_media却累计整个发送（含静音/等待）。
327020/252723ms是不同范围，相等变成不等不代表时间轴正确。
`loop_elapsed_ms:1308` 仍从三条验收前的g_run_start_us算起，
未保存专门loop真实起止；`LatencyLog`仍只存最先4096帧（20ms时81.92秒）。

修复：同一范围独立核算媒体样本/帧时长与墙钟；sender一次初始化连续期限，
记录边界/补齐/丢帧而不是重置掩盖漂移。独立记录loop开始/结束/elapsed、COMPLETE，
耗时用有界抽样覆盖持续loop或明确只报告预热范围；不要把前81秒当全循环分位。
验证短句、等待、持续loop三个范围，pts/样本/帧数/墙钟能对齐；提前断线loop失败。

## R2-4 [P1] 完成本轮已经授权的发包诊断，不能以“还需要诊断才能继续”交付

前次M1-5已明确授权并要求实际RTP/SRTP/UDP观测；
`webrtc_m1.cc:1368–1397` 仅做PCM/本地解码，RQ却停在“需要RTP…实际写入结果才能继续”。
这是本次修复的剩余工作，尚无外部条件阻止执行。

修复范围包括以下必要诊断与由其确定的本阶段修复：

- 白名单输出Offer/Answer的音频m-line、direction、rtpmap/fmtp、mid/ssrc等媒体参数，
  不打印ICE密码、Authorization或完整SDP；检查服务端是否接受音频。
- 在可核验的public/weak扩展或跟踪的局部源码补丁处观察实际发包，
  记录RTP PT/seq/ts/SSRC/长度、SRTP处理结果/后续实际UDP写入与错误；
  先核对实际库/符号，不修改未知内部对象布局。
- 本地能量/解码已经验证，不重复用5分钟循环代替上述新观测。
  诊断可以短时、少量包，输出覆盖阶段/命令/ELF；不是完整验收。
- 若裸peer仍无VAD，做**同源fixture的标准WebRTC栈媒体对照**，
  配置同server_vad/ASR，区分服务端配置/模型与esp_peer的RTP互通。
  使用现有tools/webrtc_probe的依赖和官方API；不能把原有“仅握手31ms成功”当媒体对照。

确认并修复根因后，再做相同fixture三条20ms与三条60ms、实际item/VAD/completed/关键词，
及完整300秒资源loop。发送API=0依然不证明对端接收；48k PCM与esp_peer1.5.5保持。
实际阻塞须给出具体失败、尝试、证据与可执行方案；未做下一诊断不是阻塞。
不允许自动改16k、手动commit掩盖server_vad或半双工掩盖目标。

## R2-5 [P2] 交付表述仍旧，验证记录与最终 VERDICT 缺失

结果文档前半仍把旧媒体发送/时钟/ASR归属写为已通过；
后半称持续loop独立记录起止，与R2-3实现不符。
PR标题/正文仍“D0设计修订/只包含文档/未改固件”，与已经推送的固件不符。
新增陷阱未按AGENTS补findings；M0握手/60ms/正常WS回归/隔离重建没有可核查记录。
`WebRtcM1Run` 最后仍只有“probe finished”，g_acceptance_incomplete并未产生最终失败判据。

修复：结果文档围绕当前提交重写，旧采集明确放历史区，不并列保留错误当前结论；
更新SPEC/PLAN/调度/PR当前范围。补实际命令、非敏感flags、组件与库/hash、
实现HEAD/ELF绑定、脱敏原始结果及未完成项，按AGENTS落已证新发现。
在隔离checkout恢复fetcher/patch/lock并编译M1及正常WS，记录实际结果；
新增M0行为需真实握手回归。源码diff检查与patch应用检查分开，不破坏合法patch语法。
最终VERDICT由真实条件计算：3条VAD/完整ASR/关键词、配置成立、零codec/超限错误、
完整loop等；失败/只完成诊断不能被称PASS。

## 执行与边界

这是继续修复前次M1 findings，不是新的产品阶段。
授权完成上述必要实现、诊断、真机/纯逻辑/回归/隔离重建，不设未经用户要求的“剩余验证预算”。
M1.5、产品下行/AudioService/UI继续HOLD；不merge/发布/部署/改上游。
只修本Review → verify → commit/push → 绑定实际新full HEAD的REVIEW_REQUEST → STOP。
