# M1 第三次代码复核：仍需修复并完成媒体诊断

Reviewed exact HEAD: `1cbd79bbe60f53f688cdcf512a0f60ec9cd8f42f`。
PR: <https://github.com/MetaAlms/StackChan/pull/1>。
REVIEW_REQUEST: <https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6045775796>。
前次 authoritative Review: `5447065335`。
本次正式 Review: [5447785013](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5447785013)，
已 read-back 核对 `commit_id` 与上述 full SHA 相同（自有 PR 采用 COMMENT，正文结论为 needs fixes）。

结论：**needs fixes，M1 未通过，M1.5/产品接入继续 HOLD**。
实际源码与两项独立复核确认了下列问题；不把已修好的 failed 后停止、
连续 deadline、32KB sender 归属重新列为缺陷。

## R3-1 [P1] 实际发送判据仍可把保护失败或短写判为成功

`webrtc_m1.cc:1498–1499` 的 `send_ok` 只要求 protect_calls>0、udp_attempts>0、
write_failed==0，没有要求 protect 成功、实际成功包数、零短写或完整覆盖。
`rtp_send_probe.cc:177–178` 允许 protect-failed 记录参与匹配，
`:376–381` 又在 UDP 返回整长时无条件计 packets_written，未检查 protect_ok
或写入长度与该记录成功的 srtp_len 一致。
因上游 void 回调不传播保护失败，保护失败后仍发生 UDP 调用是应诊断的真实路径；
此时不能宣称 SRTP+UDP 成功。一次短写且零负返回也会使当前 send_ok 为 true。

`:127–132,154` 只把 kPending 当未完成，把成功保护但未写出的 kProtected
当可正常退休/复用的 completed；这会吞掉本来要定位的“保护完成但没到 socket”。

修复：保留“实际调用原样转发”的观察原则；分别统计保护失败、观察丢失、
成功保护但未匹配写出、短写、负返回、唯一完整写出包及重试。
关联成功必须来自同一保护成功记录，且长度与其 srtp_len 一致；
失败调用仍可观测，不能改记为有效 SRTP。
未完成记录不能静默退休，采集窗口结束报告 pending/未写覆盖。
最终 send_ok 要验证完整链路与覆盖，不能仅凭发生过调用。
用可控的 observer 输入/真实函数 stub 验证 protect 失败后 UDP 成功、
短写、保护后无 UDP、重复写、正常成功，避免为纯计数逻辑反复刷机。

## R3-2 [P1] ASR 尚未绑定实际 item，最终判据也漏了 VAD

`webrtc_m1.cc:728–737` 的 VAD 只设置 flag，没有读取/绑定 item_id；
`:752–756` 任意有效 completed 直接释放当前槽位，
`:759–775` 的 failed 连非空字符串 item_id 都不验证。
前条已成功后迟到的重复 completed 能在下一条清槽后进入下一槽，
旧 item 的 failed 或缺 id 的 failed 则会误停当前条。
“timeout/failed 后不开始下一条”已修，但没有解决成功后重复/旧事件。

`:1486,1516` 计 clips_ok 与 PASS 时不要求该 clip 的 speech_started/stopped，
可在三条 completed 有关键词而无有效 VAD 的情况下报 PASS，违反四层验收。

修复：按当前官方事件字段，从实际 VAD/committed 建立本条 item 归属，
完成/失败只接受所属 item；未知、旧、重复、字段缺失事件不得释放或失败当前条。
验收结束冻结结果，资源 loop 的事件不得改变它。
PASS 必须逐条满足有效 VAD start/stop、所属 item 的完整 ASR 和关键词。
轻量测试应覆盖正确 item、重复旧完成、错误 item、旧 failed、无 id/无 transcript、
验收冻结后的事件；将真实测试结果落盘，不以注释声称完成匹配。

## R3-3 [P1] 时间范围仍不同，新的耗时抽样仍偏置

连续 pacing 的逐段重置已经修复。剩余问题是：
`webrtc_m1.cc:932` 的 wall_us 仍只累计 clip，`:903` 的 frames_media 却包含
诊断/clip/静音/ASR wait/持续 loop；`:1387–1390` 仍并列比较不同范围。
`:1372` 的 loop_elapsed 仍从 `:1358` 诊断和短句验收前的 g_run_start_us 算起，
而实际 loop 在 `:1168` 才开始。前次 R2 已要求独立 loop 起止，尚未修复。

`LatencyLog:267–280` 混用最初 4096 个逐帧样本与之后每 4 帧一个样本，
没有权重修正，因此不是全循环分位。可复现反例：16351 次输入，
仅第 3300–3599 次是慢值（1.83%），当前保留集合慢值占 7.32%，
真实 p95 为快值而当前 p95 为慢值。“后段有样本”不能推出无偏分位。

修复：分别对诊断、短句/等待、持续 loop 记录同一范围的样本/帧媒体时长和
独立墙钟起止；保存真实 loop_start/loop_end/elapsed，提前断线必须失败。
末帧补齐、转换 carry 丢弃及边界延迟有计数，不重置时间轴掩盖。
采用始终相同采样率、可靠 reservoir 或明确限定覆盖范围的有界统计，
不能把不等权集合标为整个 loop 分位；记录采样数量及覆盖窗口。
用纯逻辑时间/统计检查验证范围与上述反例，随后才跑媒体验收和 300 秒 loop。

## R3-4 [P1] 必要媒体诊断仍未执行完，先做标准栈媒体对照

本轮 run12（ELF `82ef1c8a6`）的实际结果仍是：
`FAIL - transport layer; stage=server answer received(4); peer_state=6`，fixture 未发送。
这不能验证移入 sender 的短诊断，也没有产生 SRTP/UDP 计数。
失败如实披露是正确的，但未完成的诊断仍属于已授权 M1 修复。

下一步应并行推进两项具体工作：

1. **标准栈媒体正对照现在执行**：使用冻结的同源三条 fixture、现有
   tools/webrtc_probe 依赖及官方 API，启用并核对同样的 server_vad/ASR，
   按 item 验证完整 ASR/关键词。前次零 VAD 以及本轮 DTLS 失败都不能代替它，
   不需要等设备先连通才做。已有
   `docs/webrtc-media-m1-standard-control.md` 和 `webrtc-media-m1-host-encoding.md`
   是设计与接口证据，不是已经执行的对照。
2. **设备短 DTLS/发送观测**：用已核实的公开函数/链接扩展，获取非敏感
   datagram/handshake 元数据与实际结果，区分是否发出/收到 DTLS、重传/超时、
   协商阶段；到媒体后记录实际 PT/seq/ts/SSRC/长度/保护状态/UDP 写入。
   只做少量包、短窗口；不要继续用没有新增观测的重刷替代定位。

最小公开符号方案已核实于 `docs/webrtc-media-m1-dtls-short-probe.md`：
在 Transport.Start 前 Arm，在失败 return 前也导出；观测 handshake/sendto/recvfrom，
由 config_defaults/setup 的公开参数固定小表剔除 HTTPS 并识别角色，不读内部字段。

只记录公开 header 元数据、返回码、协商 profile enum/cipher/已知角色，
不记录负载、密钥、random、ICE 密码或完整 SDP，不猜 peer 内存布局。
应核对 actual linked ELF 的 wrapper 引用/实现，不能只凭源码写了 --wrap。

独立源码核查排除了“exporter 固定用 SHA256”的猜测；其 PRF 跟随 Mbed TLS 回调。
另一个**条件风险**是 1.5.5 宣告多个 SRTP profile，却始终使用 AES128-CM/SHA1-80。
实际 selected profile 还没观测：若 0x0001 则排除；若别的 profile，才依据结果修复。
已有 public getter 可以只报告 enum。不得把此假设当当前零 VAD 的已证根因，
不得据此盲改 cipher/升级 1.5.6。

修复确认的本阶段根因后，完成同源 3 条 20ms、3 条 60ms 的四层验收，
再跑完整 300 秒资源 loop。本地保护/UDP 成功仍不证明服务端认证/接收。
真实外部阻塞需给出命令、尝试、原始脱敏结果和可执行方案；
不以单次探针失败或自行设定的验证预算终止已授权移植。

## R3-5 [P2] 生命周期剩余项与交付证据/状态同步未完成

保留 Impl 并保持 stopping=true 的超时路径已改善了 UAF；
但 `webrtc_transport.cc:543` 仍在真正 cleanup 前设置 cleaned，超时后永久不再 join/release，
与 R2 要求“成功状态仅在释放后成立”的规则不同。
`webrtc_m1.cc:1359–1369` sender semaphore 仍在 task 创建失败及 join 成功后均未释放。
完成最小安全的退出/再次 cleanup 规则与这两个明确泄漏路径；
验证正常 Stop、SDP 等待中 Stop、部分创建失败、受控退出超时/重复 Stop，
确认任务与 callback 的 ctx/观察对象都保持有效，不能只有代码说明无验证。

当前结果文档仍在旧结论后追加新结论，没有按 R2 重写当前状态；
例如第 213 行附近“5 次中 4 次失败”后又称“唯一连通的两次”，数量矛盾。
SPEC/PLAN/findings 本轮未变化，PR 仍称“D0 文档/尚未开始媒体/未改固件”。
真实 M0 握手、正常 WS 回归、60ms、隔离重建逐条结果仍缺失。

修复：围绕实际当前交付重写结果与 PR/阶段状态，旧采集明示历史，
按 AGENTS 把已证新发现落 findings。记录实际命令、非敏感 flags、组件/库 hash、
Implementation HEAD、每次 ELF 与脱敏原始日志、测试覆盖及明确未完成项。
run12 结束于 UTC 19:56:54；当前本地 ELF 重建于 UTC 19:59:50，
不能把 run12 当成后来修订代码的真机证据。
隔离 checkout 恢复 fetcher/patch/lock 并构建 M1 和正常 WS，验证 M0 的真实会话握手。
源码空白检查与合法 patch 的应用检查分开；撤回的 patch 单空格要求不再执行。

## 执行边界

这是 M1 已授权工作。48k PCM/实际 16→48 重采样与 esp_peer 1.5.5 保持；
不以手动 commit、16k 降级或半双工掩盖 server_vad/目标失败。
M1.5、产品下行/AudioService/UI 继续 HOLD；不 merge、发布、部署或改上游。
只修本 Review → 验证全部 findings/必要诊断 → commit+push →
绑定当前实际 full HEAD 的 REVIEW_REQUEST → STOP。
