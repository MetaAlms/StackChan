# WebRTC 媒体移植调度记录

## 授权与范围

2026-10-08，用户授权 Codex 接管完整 WebRTC 媒体移植，按
`blue-pr-review-fix-loop` 调度现有 DSH **stackchan** 会话。DSH 为实现者，
Codex 负责设计与每阶段代码/证据复核及后续派单。

- 工作目录：`/Users/amtf/Documents/Git/StackChan`
- 分支：`feat/aliyun-omni-v2v`；仓库：`MetaAlms/StackChan`；默认 PR base：`main`
- 设计原始基线：`02917f425c415a21063bfca15638fda69bf9cc85`
- 已落盘复核：`87995030464534ff914da0cb1835748b5490b763`，见
  [评审结果](webrtc-media-review-result.md)
- 最新明确范围：媒体上/下行、48kHz PCM 编解码与显式重采样、事件/UI、
  本地打断与旧音频隔离、资源/重连验证、完整 WebSocket 回退。
- RTP 48k clock 与 PCM rate 独立；48k PCM 是本轮用户指定的实现选择，
  不再将上采样写成 WebRTC 规范要求。现有 AFE 16k 无法由上采样恢复高频。
- 服务端 AEC 仍是待验证假设；全双工/不自打断必须实测，不用半双工门控掩盖失败。
- 不 merge、auto-merge、发布或改上游；不得泄露凭据或以生成目录的未跟踪改动交付。

## 当前状态

已从桌面应用窗口确认目标标题 **stackchan — DeepSeek Harness**，
会话中可见原三份媒体文档的交付。浏览器 `127.0.0.1:3080` 是另一实例，
不作为本任务派发对象。实际工作目录由首轮派单要求 DSH 在修改前再次核对。

## 首轮（D0）：设计修订

派单见 [webrtc-media-d0-task.md](webrtc-media-d0-task.md)。
范围为**仅文档**：重写 SPEC / PLAN / 评审请求，同步调度与事实记录；
不改固件、不刷机；设计标为待 Codex 复审，不自行宣告通过。

修订要点见 SPEC §0 的对照表，覆盖 P1 五项与 P2 十项。
**未接受**评审 Q2 建议的"16k 编码 / 24k 解码"最小路径——
用户最新明确要求 48 kHz PCM 与显式重采样，本版按用户要求设计，
并在 SPEC §2.2 标注其为实现选择、写明代价，在 §8 Q1 保留回退决策点。

上一轮评审结果 [webrtc-media-review-result.md](webrtc-media-review-result.md)
**保留为历史证据，未修改**。

## 流程约定

- 每轮以**实际 PR Review ID、reviewed full HEAD 与新 full HEAD** 为准，
  不把旧证据自动当作新提交的验证
- 共享目录不 reset、不覆盖他人改动
- 设备维持当前状态；设计修订无需先刷回 WebSocket
- 不 merge、不 auto-merge、不发布、不改上游、不泄露凭据

## 实际评审与实现进度

- Draft PR：[MetaAlms/StackChan #1](https://github.com/MetaAlms/StackChan/pull/1)。
- D0首轮：HEAD `7acb6f398961fbdad4c5e454a273867d82bceeba`，
  [Review 5445638146](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5445638146)
  要求修正D0-1～D0-4。
- D0修复：HEAD `57ab76eba38b9b8c9bacc95c704698b6316f0ac2`，
  [Review 5445711841](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5445711841)
  **放行进入M1**，未放行未实现的媒体/AEC/产品阶段。
- M1任务：[完整任务书](webrtc-media-m1-task.md)；同一原生`stackchan`会话第63轮已接单实施。
  首批候选本地提交为`e6d7158`，随后另修正重建补丁和tracked DTLS/SRTP配置。
  **这些是开发中候选，尚无M1实际HEAD的正式REVIEW_REQUEST或放行。**
- Codex开发中反馈落在[预检记录](webrtc-media-m1-preflight.md)：容量入参、真实任务退出、
  配置回显、逐条ASR与时间轴、资源统计、M0互斥/复用等。通过原生UI送入原任务，
  第63轮持续修正；不能把WIP预检当已提交PR的最终评审。
- 设备恢复在线后，首轮串口采集在第一条fixture发生断言重启；修统计容器后明确main栈溢出。
  独立检查又发现sender栈8192被误认为32KB，ESP-IDF实际按字节计。
  Codex终止失效的第63轮，并在**同一个会话第64轮**恢复未完成M1；
  当前源码已使用`kSenderStackBytes = 32768`，等待重新构建/真机结果。
  两条较早诊断在UI仍显示发送中；第64轮已直接读取完整预检，不以这两条队列状态当送达证据。
- [M1.5准备记录](webrtc-media-m15-preparation.md)为独立只读调查，**尚未派发**。
  M1通过实际HEAD评审后再冻结下一阶段任务，保持用户要求的48k PCM与WebSocket回退。

### M1 第一次正式交付与修复轮

- DSH第64轮提交并推送 `c3e658200d8365ac4bbb19a28290d491f2c04df3`，
  [REVIEW_REQUEST](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6044400269)，并明确STOP。
- 真机20ms sender未再panic、栈余量10232B；3条VAD/ASR均0、持续loop中断。
  本地send API返回0不证明SRTP/socket成功；没有60ms通过证据。
- Codex正式[Review 5446817816](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5446817816)
  绑定该完整SHA，回读commit_id已核对，决定**需要修复、不放行M1.5**。
  正文见[复核结果](webrtc-media-m1-review-result.md)：M0 update丢失、真实退出、
  ASR归属、独立时间轴、媒体诊断与验收、失败分层、可复核交付。
- 已向同一原生`stackchan`发送绑定Review ID/SHA的最小修复指令。
  新第65轮可见且进行中（1064步），源码已针对该Review恢复M0 update、
  更正失败分层并增加媒体能量/本地解码诊断；不是另一会话或重复派单。
  后续1065步可见一次`Read PR #1 ... Review 5446817816`用户消息与DSH对该轮的回应。
  run8再次在DTLS阶段停住，现已正确报告transport失败，任务确认退出后才释放。
  **仍为修复中，未交新HEAD复审。** M1.5、产品下行/AudioService/UI继续HOLD。

### M1 第二次正式复核

- 第65轮已交付并STOP：HEAD `9e45e63d9ce9083959a50e4420220b26a31da05e`，
  [REVIEW_REQUEST](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6044837128)。
- Codex [Review 5447065335](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5447065335)
  已回读核对 `commit_id` 与该HEAD、远端PR HEAD一致；
  [复核正文](webrtc-media-m1-review-result-2.md)记录R2-1～R2-5。
  run9完成持续loop且本地Opus全可解码、有语音能量，但无服务端VAD/ASR，M1未通过。
- 撤回前次M1-7对合法patch上下文空行的清理要求；源码空白与patch应用分别核验。
- 补充[标准栈媒体对照设计](webrtc-media-m1-standard-control.md)与
  [host编码设计](webrtc-media-m1-host-encoding.md)，状态均为**只读核查、待实施**。
  成功只能收窄设备媒体链，不能单独归因esp_peer；同源fixture不代表派生PCM/包逐字节相同。
- 同一原生`stackchan`的第65轮STOP已在UI确认。Review ID/full SHA最小指令已填写、
  完整草稿核对后发送。新第66轮（1078步）可见一次
  `Read MetaAlms/StackChan PR #1 Review 5447065335`消息及DSH的补丁恢复回应，
  媒体SDP白名单日志已出现WIP修改；**送达与继续实现已确认，尚无新HEAD交付**。
  授权本Review必要诊断、修复和验证；M1.5及产品下行/AudioService/UI继续HOLD。
- 第66轮在生成目录补丁恢复前暂停，没有commit/push或新REVIEW_REQUEST。
  追加[实际发包观测设计](webrtc-media-m1-send-observation.md)，仅为既有R2-4的实现参考，
  以公开SRTP/UDP函数的linker wrapper避免猜测内部对象布局。
  同一会话已发送一次续接剩余工作的指令，第67轮（1084步）显示进行中；
  不将此续接视为新的正式评审或新产品阶段。
- 第68轮（1097步）继续同一Review的R2-4预检与诊断；ring/SRTP记录绑定/重试计数已出现WIP修正。
  run10在DTLS层失败；run11在配置通过后因短诊断于8KB main编码而实测栈溢出，不能作为发包证据。
  Codex补充预检并尝试插话；新纠正与两个历史诊断仍在UI显示发送中，不据此宣称全部送达。
  **实际源码已把短诊断移到32KB sender任务**，保持唯一编码所有权；仍未交新HEAD。
  后续以真实诊断、全Review修复及exact-head RQ为准，不重复派发已有执行任务。
