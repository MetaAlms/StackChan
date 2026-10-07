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
