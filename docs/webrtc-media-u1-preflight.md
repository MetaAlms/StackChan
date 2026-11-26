# U1-S 开发中预检

2026-10-13，基于 full bcd4e35 之后尚未提交的短模式 WIP。
这是当前 U1 任务的接线纠正，不是最终 RQ 复核，也不扩大到其他 M1 TODO。
由 Codex 与独立只读复核静态定位；未运行设备或新增云端会话。

冻结两臂前修正以下五处；已经生成的旧 WIP 1.5.5 归档不可作为对照基线，
保留为被替代的候选，修正共同源码后重新构建、归档两版本。

1. **媒体时钟未初始化**：`webrtc_m1.cc` 的 RunShortDiag 未调用 g_clock.Arm；
   MediaClock.deadline_us 初值0，只有 RunFullM1 调用了 Arm。短模式从启动后的
   esp_timer 时间与零比较，会把开头帧过快发送。短序列开始初始化媒体时钟一次，
   各步骤继续同一时间轴；不重置 RTP 观察窗口。实际 adapter 的入口必须覆盖，
   Sequencer 模型测试不能发现漏调用。
2. **短模式仍执行完整 M1 最终验收**：原 final verdict 仍要求三条 clip，且
   g_loop_complete 初值 true 会打印未运行的长循环 COMPLETE。只在完整模式运行
   原验收块；短模式报告自身有界结果及未覆盖项，不伪造完整 M1 PASS/FAIL。
3. **clip 活动被称为 speech evidence**：HasSpeechEvidence 只检查计数>0，
   fixture manifest 的 lead_silence_ms=500。clip 开头静音一包写出后断链也能满足它。
   将该证据准确标为 clip interval write activity；它不证明非静音声音已抵达服务端。
   保留 ASR/VAD/输入与编码能量各自证据，不改 fixture，不扩张 RTP 关联实现。
4. **区间边界与有无新包混淆**：Diff.have_range 要求 before 已有包，开头静音
   正常没有 before 包却记录到大量新包，日志会说 no packet observed；无新包时
   before/after 都留有旧包又会显示有效范围。区分缺少起始边界与无新活动，
   不把已有窗口元数据作为本区间首/末包的新观察。
5. **无 ASR 时丢失 partial VAD/item**：WaitForClipAsr 只在 completed/failed 分支
   拷贝 turn 的 VAD/item，超时/断链返回 false 后短模式打印默认0/0。
   无结果返回前也快照该 turn 的已有证据，不清掉或编造字段。

Targeted 检查覆盖这些实际接线/诊断反例，并收窄纯 plan 测试对真实 pacing/join 的声明；
真实 owner 停止→主任务等待→SettleWindow 位置正确，无需为本轮重写 join 或旧 RTP token。
随后按原任务完成共同源码冻结、两臂构建与归档、官方精确1.5.7解析/最小锁差异、
原台账检查、commit/push/full exact RQ/STOP；设备离线如实留待实测。

原生 UI 插话发送使第3轮停止而纠正仍留在“发送中”队列，未形成消息或应答。
随后 Codex 在同会话正常发送一次恢复指令：第4轮已读取本文件、确认没有运行任务，
WIP与文档完整；清单1.5.7但实际组件仍1.5.5，先修共同源码再重建两臂。
媒体时钟 Arm 已在实际短入口补上，其余纠正与最终证据仍待交付复核。
