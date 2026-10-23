# WebRTC 媒体重新开发记录（2026-10-23）

## 本轮结论

保留已通过复核的 M1 上行、精确 esp_peer 1.5.7、SRTP SHA1_80 修复与真实 48k PCM 编解码。
替换 `2b69e06` 的自动启动限时探针，重新实现受控会话、停止准入和证据边界。
本轮完成的是离线开发，**没有刷写、复位、读串口、开启设备采集或云端会话**。
没有登记 T1–T4、AEC、插话、重连或完整产品移植通过。

历史“安静时反复 ASR”和文本重合不能独立证明声学回声；
旧“三句3/3”仅为未对时会话中的关键词覆盖，不是正式 T1。

## 当前可运行的实现

- `main.cpp` 的 M1.5 路径不再自动启动网络，控制入口默认 Idle。
  仅显式 START 后才初始化网络、编解码器和音频任务。
- USB Serial/JTAG 驱动读取命令，不假定主 UART 的 stdin 就是 USB；
  解析器只执行完整合法行，128 字节上限，超长行整行丢弃至换行。
- 一个 worker 拥有状态机、媒体发送、编解码器、事件消费和清理；
  采集与 peer 回调只移交拥有数据的有界队列。
- Preparing 设30秒准备期限，由 worker 在同步初始化/API调用返回后检查；它不是可抢占硬上界。
  配置核对成功后启动 AFE 预热并丢弃预热输出，
  观察到首次 AFE 回调后才设置 t0、开放准入并发布 RUNNING。
  Running 的5–180秒不包含网络、模型初始化与首帧等待。
- 近端/双向模式均只发送真实麦克风，经 AFE16k→48k/20ms mono Opus；
  移除 M1.5 fixture 注入和编译期近端开关。M1 的三个 fixture 保留。
- 下行保存库提供的 pts 与本地接收时戳，48k解码→24k播放；
  pts 仍不是 response_id、SSRC 或序列号，不据它声称迟到响应归属已解决。
- STOP 永久取消本轮准入；迟到 Ready 无法再次打开。STOP/超时/断线/错误统一清理，
  由发送 worker 停止 transport，再关闭编解码器。
- 每轮 FINAL 报告 run、session相关标签、阶段、原因、t0起的运行窗、
  发送/接收 pts、实际 SendAudio 调用间隔、AFE输出/队列延迟、分原因丢弃及硬件错误。
  采集时戳是 AFE 回调时刻，**不是 ADC 采样时戳**；已知读取/转换错误结束运行。
- ASR 依独立 input item 去重；一个 item 最多满足一句，缺失/非法ID、重复和有界集合溢出单列。
  关键词覆盖不是正式 T1 判定，不打印转写、原始事件或凭据。
- 播放准入区分 Accepted/Invalid/Stopped/Stale/Contended/Full；
  报告排队样本、处理中样本、OutputData API提交样本/次数、提交起止及软件停止完成时戳。
  **提交次数不等于音频包数，提交样本不等于播放完成，软件 stop 不证明 DMA 无尾音。**

## 临时限制与未交付内容

当前只允许**一轮/上电**。Done/Failed 后拒绝再次 START，必须重启；重启仍停在 Idle。
AFE Stop 没有确认 fetch 线程退出或同步清空输出缓冲区，AudioService 也无完整 join。
本轮故意不调用状态机的 Reset 去冒充安全重连；长寿命 owner 不会被悬空回调访问。
因此 PLAN R0 的可重用生命周期、自动重连和返回 Idle 后多轮运行尚未完成。

R1 目前仅实现整轮 STOP 的本地隔离；没有把 speech_started 接成自动插话，
没有实现同会话 response 取消/恢复及迟到旧 RTP 的归属。产品 Application 的 speaking
半双工逻辑仍未替换。M2产品/UI、M3稳定性和M4收尾继续保留为待做，不能登记完成。

STOP acknowledged 仅表示已接收取消请求，当前API操作与清理可能尚未结束。
须等待 FINAL，并检查 transport_joined=1；即使如此也不表示 AFE任务已销毁或声学播放已停止。
close/join失败的owner保留，最终标Failed，不能以等待时间代替join证明。
仅确认join成功时才能把worker的队列排空视为最终清理；失败路径可能有已准入的迟到回调，资源继续保留。

## 独立复核与离线验证

两位只读复核指出并关闭了以下问题：

1. Ready 可能覆盖 console 的停止准入：改为永久取消的 MediaAdmission，并发竞争回归通过。
2. AFE回调等待超时后普通统计读写竞态：只有永久关闭准入且回调归零才读取；否则报告 unavailable。
3. 模型初始化吃掉短测试窗口：改为 Preparing 预热，首回调之后才记录 Running t0。
4. 发送间隔采用编码前时刻：改为实际 SendAudio 调用时刻。

原台账始终为 `/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`，
在原used=21基础上 replan epoch8→9→10，未重建或清预算。三个cache=never原生二进制检查均通过：

| 检查 | 结果 | 台账 action |
|---|---|---|
| session-run-control | 194断言，状态/期限/独立item/重复/溢出隔离 | redevelop-session-1 |
| console-command | 16651断言，分片/CRLF/非法字节/参数/超长整行拒绝 | redevelop-console-1 |
| media-admission | 530断言，永久取消及128轮线程竞争 | redevelop-admission-1 |

台账当前used=24，limit=32。检查仅覆盖实际共享的可移植组件，
不覆盖 USB 驱动、设备调度、音频库/硬件、网络或模型服务。
编译与以下操作在控制器覆盖之外，单独记录：

- ESP-IDF5.5.4 `idf.py reconfigure build`及最终增量构建通过：媒体应用 `0x3cada0`，分区余量23%。
- 独立配置 WebSocket 回退构建通过：应用 `0x3a37a0`，分区余量26%。
- dependency HEAD→vendor patch→Aliyun patch→media patch 重建两份 AudioService，与live逐字节一致。
- 三个检查脚本 `bash -n`及源码/文档diff空白检查通过；统一patch的格式上下文单独由重建验证。

构建与运行日志保存在私有目录：
`/Users/amtf/.codex/dev-harness/artifacts/stackchan-webrtc-media/m15-takeover/`。
含凭据的构建产物不加入仓库，不把原始 SDP/转写放进报告。

## 下一轮操作顺序（尚未执行）

每轮先说明内容和时间，**等用户明确说“开始”**，再操作设备。
首轮只验启动门与近端控制；先观察 Idle 不自动开云端会话，再显式 START。
必须看到 RUNNING 后才提示用户读句子；时间从新 t0 开始。
最终检查 speaker提交为0、独立输入item、错误/丢弃与停止原因；不得自动把关键词mask7登记T1通过。
第二轮双向及后续任何重启/采集均重新对表，不沿用上一轮“开始”。

设备端命令（LF或CRLF，单空格分隔）：

```text
STATUS
START near_01 near 60
STOP near_01
```

双向模式为 `START duplex_01 duplex 60`，**不在首轮顺带运行**。
还需完成AFE安全生命周期、真实输出停止测量、response/RTP边界与产品接入，才能推进正式T3/T4。
