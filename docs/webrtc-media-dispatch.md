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
