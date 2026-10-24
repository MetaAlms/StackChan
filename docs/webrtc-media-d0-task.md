# D0：设计修订任务

此任务由用户授权的 Codex 调度，目标 DSH 会话为 `stackchan`。
这是本轮新增的任务文件，允许随设计修订一起提交。

+D0 / WebRTC 完整媒体移植：先修订 SPEC/PLAN，提交后由 Codex 复审。
用户已授权 Codex 接管完整 WebRTC 移植，直接调度本 stackchan 会话作为实现者；此前 SPEC/PLAN 是你写的，继续由你修改和开发，不新开会话。Codex 负责逐阶段评审及放行。
仓库 MetaAlms/StackChan；worktree /Users/amtf/Documents/Git/StackChan；分支 feat/aliyun-omni-v2v。
修改前核对工作目录、分支、git status。当前已推送 full HEAD：f3db45cbbf102b9f87a34550ad5ae08dec1a83da。此前媒体设计基线02917f425c415a21063bfca15638fda69bf9cc85；Codex复核已提交87995030464534ff914da0cb1835748b5490b763。共享目录不reset/覆盖他人改动。

先完整读取 AGENTS.md、docs/webrtc-media-review-result.md、docs/firmware-findings.md 的 A9/B16–B18，以及docs/webrtc-media-dispatch.md。当前没有该分支的PR，本轮是新设计修订，不能假造 Review ID 或套用只有ID的短修复指令。

本轮授权仅文档：重写 docs/webrtc-media-spec.md、docs/webrtc-media-plan.md、docs/webrtc-media-review-request.md；必要时同步总计划/事实/边界/调度记录，保留原评审结果作为历史证据。本轮不修改固件、不刷机；设计标为待Codex复审，不自行宣告通过。
最新用户明确要求“媒体链路 + 48kHz重采样”。采用上行24k采集→现有24→16/AFE16k→独立16→48 mono重采样→48k PCM Opus编码；下行raw Opus→48k mono解码→48→24重采样→板级播放。说明这是用户指定的PCM实现选择；SDP/RTP时钟固定48k不强制PCM48k，升采样不能恢复AFE丢失的高频。原review推荐16k最小方案不覆盖用户最新48k选择，但其余事实/缺陷仍须修。
务必修齐报告中的P1/P2，尤其：
1. 服务端AEC改为待验证假设；M1.5用真实同会话下行播放、持续上行计数和麦克风信号、近端正对照、远端独说、双讲及插话后再说话。分目标通过/失败/实验无效；无ASR不是AEC证明。有效失败交Codex评审后决策，不能半双工掩盖。
2. pts单位毫秒，媒体时间轴独立于旧timestamp_queue_；20ms +20/RTP+960，丢帧仍推进时间。规定session.updated/转录配置/实时pacing/尾部静音/失败可观测；M1保留5分钟资源检查。
3. 48k转换具体契约：sample_num每声道；容量查询+process检查返回值、只用actual_out，保留跨帧余量/滤波状态。48k encoder配置、get_frame_size、packet元数据一致；初始关闭DTX。20ms/60ms选择限WebRTC路径，不能全局破坏WS/AFE。
4. 完整raw Opus单包边界、容量/MTU校验；esp_peer整包buffer1428B需扣头/扩展，不能直接当payload容量。接收回调借用数据先复制，有界非阻塞队列，编解码/重采样在worker，处理真实包时长至120ms。同流ptime变化不能重建decoder；转换失败不得把48k PCM按24k播放。
5. 全双工连续采集；产品UI状态不作为唯一RTP门控。本地停止旧播放、在途解码/输出隔离、晚到旧RTP与DataChannel跨通道边界、重连清理必须有可实现任务。服务端cancel不代表本地停声，generation也不能自动关联response_id与RTP。
6. 队列按时间/字节预算，分别验证jitter/PCM播放缓冲、DTLS峰值、持续SRTP+48k编解码+3段转换+AFE/UI资源。测heap能力/最大块/栈/耗时/欠载/重连，不以M0读数或>8KB替代完整负载验证。
7. 保留完整WS回退、Ogg提示音/背压/半双工依赖，M4仅清WebRTC路径。统一SPEC/PLAN的阶段编号。xiaozhi-esp32及managed_components未被主repo跟踪；实现修改必须进入可重建patch/桥接代码，fetch_repos.py重建可复现。

交付：检查文档链接/设计自洽和git diff --check → commit+push（不改写历史）→创建或复用draft PR，base main、head feat/aliyun-omni-v2v，注明已有WS/M0基础与当前设计修订范围→在PR发布绑定实际full HEAD的REVIEW_REQUEST，回报PR URL/full HEAD/文档位置/未解决项→STOP。Codex通过设计后会直接下发M1实现，不再让用户转贴。禁止merge/auto-merge/发布/上游issue及泄露凭据。
