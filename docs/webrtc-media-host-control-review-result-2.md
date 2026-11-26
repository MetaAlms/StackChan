# Host 正对照残余问题复核

被复核 full HEAD：`050a2f43410d5246239b1a1357078e3b52c33412`。
对应 [RQ](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6046543591)，
前轮 Review 5448053831，当前分支 `feat/aliyun-omni-v2v`。

结论：人工逐条核对原始日志，**三条不同 item 的完整 VAD/转写/关键词成功观察成立**；
这是一项实际进展，不撤销该观察。但 H1–H4 未完全修齐，不能把现有 runner 当可靠验收器。
本轮仅修以下 H6-1～H6-4，再实际重跑一次三条对照。
设备/M1.5/产品阶段不在本轮修改范围；其他 M1 未完成项保留。

## H6-1 / P1：生成的静音没有用于实际通过运行

`standard_media_control.js:257` 从 `__dirname` 读取旧静音 JSON，
生成器 `encode_fixture.py:208` 写在 `framesDir`。
`run_pass.log:12` 实际打印 mode/decoded samples/peak 全部 undefined；
实际 accepted=924=564语音+3×120静音包，而归档 manifest 声称60包/1200ms。
因此本轮运行用的是旧120包/2400ms文件，不能借新文件的960/peak49证明它。

从本次生成目录读取，并将**实际读取**的 manifest、语音容器、静音文件 hash
记录到结果，校验本轮包数/长度/20ms解码与能量元数据；缺失或不符判 INVALID。
保留前次真实通过证据，订正它的来源描述；新运行归档同一次实际输入和结果。
需要检查 encoder CTL 返回码与逐包 voice decode ==960，不能仅 >0。

## H6-2 / P1：item 不匹配只告警，仍可 PASS

`standard_media_control.js:338` 仍用全局 ASR 数组长度释放等待；
`:352–363` 取最后一个新 ASR，item 不在当前 VAD 中仅告警；
`:393–395` PASS 不检查对应关系。
若同窗口收到旧/陌生 completed，它可以提前结束当前等待；
两个不同 item 的 started/stopped 也会被合并成一次有效 VAD。

当前结果中真实三条 item 对齐，但该代码不能防止下一次误通过。
按 started/stopped/committed 公告和本条窗口绑定当前 item；
只有同一归属 item 的有效 completed 才完成当前条，failed 亦按 item 归属。
旧/未知/重复事件不能释放新条；冻结已完成结果。
PASS 必须检查同一 item 的 VAD 起止、completed、关键词，不只是计数。
用不联网的轻量用例验证正常、旧/错误 item、不同 VAD item、重复、failed、
缺字段和缺关键词；不要只打印“交叉核对”而不把它加入判据。

## H6-3 / P1：失败落盘仍有 TDZ；时间轴非单调

配置失败在 `:251` 调用 finish，`:408` 读取的 `extraSilenceFrames`
直到 `:267` 才初始化，仍会 ReferenceError 丢失失败产物。
所有结果字段在任何失败入口前初始化，轻量执行配置失败路径验证可落盘/退出/清理；
HTTP/输入加载等失败也保留阶段结果，勿仅 console FATAL 后退出。

`:265/266/300/379` 仍为 Date.now，不能称单调；
晚帧只累计，追赶突发与 lateFrames 不影响有效性。
用实际单调时钟贯穿绝对 deadline、边界和 wall；
定义迟到失效/丢弃规则，避免堆积后突发补发仍 PASS。
`r.window.endMs` 当前直接写 RTP tick 差，必须按48 ticks/ms换算。
send bool/exception 计数互斥；非 true 不能默认当 accepted。

## H6-4 / P2：根因声明没有单变量证据

文档 `webrtc-media-m1-host-control-result.md:30–37` 与 RQ把 deadline 重置
认定为 stop 缺失根因；本轮同时改变实际回复通道、初始 seq/ts、尾静音长度等。
前次两个负尝试未改变结果，也不能证明这两项永远无关。
当前只能声明“本轮组合修复后完整对照通过，具体根因未隔离”。
不用为此追加多次云实验；订正文档/RQ与实现注释即可。

## 本轮执行

只修上述 host 残余问题，保持真实48k PCM/冻结 fixture/同模型0.5/800配置。
纯逻辑与失败路径检查 → 本次生成输入的一次真实三条对照 →
归档脱敏同次输入hash/日志/结果 → commit+push → 新full HEAD的RQ → STOP。
不降采样、不手动commit音频、不改设备、不进入M1.5、不merge/发布。

host通过只证明该次标准栈路径；不替设备媒体、AEC、完整M1验收。
