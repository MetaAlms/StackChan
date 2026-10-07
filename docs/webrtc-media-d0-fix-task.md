# D0 复审修复任务

会话：既有 DSH `stackchan`；工作目录 `/Users/amtf/Documents/Git/StackChan`；分支 `feat/aliyun-omni-v2v`；PR <https://github.com/MetaAlms/StackChan/pull/1>。

权威评审：Review ID **5445638146**  
评审 URL：<https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5445638146>  
reviewed full HEAD：`7acb6f398961fbdad4c5e454a273867d82bceeba`

先核对实际 HEAD，再直接读取上述 native PR review。评审正文同时落盘在 `docs/webrtc-media-d0-review-result.md`。修齐 D0-1～D0-4，并同步其“后续验证约束”；不要重复旧轮次已修项或扩展需求。范围仍为文档：SPEC、PLAN、review-request、调度／事实记录的必要一致性修订；本轮不改固件，不刷机。

`docs/webrtc-media-d0-review-result.md` 与本文件由 Codex 写入，是本轮记录，提交时一并纳入，保留此前评审历史。用户指定 48k PCM；不得自行降低该要求。

完成后做相关文档检查及 `git diff --check`，提交、推送，更新 PR 描述使其匹配当前范围。在 PR 发针对**实际新完整 HEAD**的 `REVIEW_REQUEST`，逐项说明 D0-1～D0-4 的处置和验证，报告 PR URL 与 full SHA，然后 **STOP**。Codex 复核通过后会另行下达 M1 开发任务。
