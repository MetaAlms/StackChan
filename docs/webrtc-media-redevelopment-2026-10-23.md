# WebRTC 媒体重新开发记录（2026-10-23）

## 本轮结论

保留已通过复核的 M1 上行、精确 esp_peer 1.5.7、SRTP SHA1_80 修复与真实 48k PCM 编解码。
替换 `2b69e06` 的自动启动限时探针，重新实现受控会话、停止准入和证据边界。
80df789交付前仅做离线开发。之后用户明确“开始”，已完成下文首轮设备启动门/准备超时观测；近端媒体未运行。
没有登记 T1–T4、AEC、插话、重连或完整产品移植通过。

历史“安静时反复 ASR”和文本重合不能独立证明声学回声；
旧“三句3/3”仅为未对时会话中的关键词覆盖，不是正式 T1。

## 当前可运行的实现

- `main.cpp` 的 M1.5 路径不再自动启动网络，控制入口默认 Idle。
  仅显式 START 后才初始化网络、编解码器和音频任务。
- USB Serial/JTAG 驱动读取命令，不假定主 UART 的 stdin 就是 USB；
  解析器只执行完整合法行，128 字节上限，超长行整行丢弃至换行。
- 一个持久 worker 拥有状态机、媒体发送、编解码器、事件消费和清理；
  第一次START时才创建32KB栈，后续从单元素命令队列接收新Run，避免自删除任务等待Idle回收造成峰值分配。
  采集与 peer 回调只移交拥有数据的有界队列；AudioService/AFE任务持久存在且只初始化一次。
- 输入任务在WaitBits前取generation，在同一CaptureLifecycle锁内重查当前位、读取、Feed；
  Stop清运行位后等待旧读/Feed结束，再等待AFE fetch/回调退出，清输入/输出缓存。
  硬件电源timer也使用此锁，输出开关/提交时刻与timer共用输出提交锁。
- 新轮先实际读取丢弃12×10ms源音频（当前24k源共2880帧，DMA容量约1440帧），
  然后重置输入转换器/AFE ringbuf；失败结束准备并计数，不打开媒体准入。
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

`e956b4b` 曾限制一轮/上电；当前离线代码已补安全重开边界。
清理确认输入/AFE回调静止、Transport已join、ringbuf reset成功后，worker完成全部局部析构和FINAL日志，
才发布可重开状态；STATUS返回Idle，此后可显式START一个新ID。旧Run在此前不会被删除。
稳定采集sink只捕获全局atomic activeRun指针，不捕获会被释放的Run引用；Stop/AFE静止后先detach。
每轮重新创建Transport/Codec/队列/媒体时钟/ASR集合，并对持久硬件统计取独立基线。
join或reset失败则保留owner并要求重启；不自动重试或开始下一轮。

这些结论是源码审查与共享同步组件的离线验证，**尚无设备连续多轮/资源证据**。
保留DSP模型实例不等于清空NS/VAD模型内部历史；没有ADC绝对时戳或物理尾音的测量，不能外推到声学停止。

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
5. 仅AFE Stop仍可能让旧输入读取跨轮Feed：加入读取generation/互斥和timer同步。
6. 120ms睡眠未排空DMA，IN关闭还可能因TX工作而延期：改实际读出丢弃120ms，再reset。
7. worker完成标记不代表自删除栈已回收：改为持久worker和命令队列，消除重开分配第二个32KB栈的路径。

最后的输入/AFE/固定worker集成由独立只读复核确认无未关闭的可行动发现，未代替设备运行。

原台账始终为 `/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`，
在原used=21基础上 replan epoch8→9→10→11，未重建或清预算。四个cache=never原生二进制检查均通过：

| 检查 | 结果 | 台账 action |
|---|---|---|
| session-run-control | 194断言，状态/期限/独立item/重复/溢出隔离 | redevelop-session-1 |
| console-command | 16651断言，分片/CRLF/非法字节/参数/超长整行拒绝 | redevelop-console-1 |
| media-admission | 530断言，永久取消及128轮线程竞争 | redevelop-admission-1 |
| capture-lifecycle | 60检查，读取/Stop/Start/Access实际线程互斥及旧generation隔离 | restart-capture-lifecycle-1 |

R0离线阶段used=25，limit=32；下方Nano修复后used=26。检查仅覆盖实际共享的可移植组件，
不覆盖 USB 驱动、设备调度、音频库/硬件、网络或模型服务。
编译与以下操作在控制器覆盖之外，单独记录：

- ESP-IDF5.5.4 `idf.py reconfigure build`及最终增量构建通过：媒体应用 `0x3cb870`，分区余量23%。
- 独立配置 WebSocket 回退构建通过：应用 `0x3a37a0`，分区余量26%。
- dependency HEAD→vendor→Aliyun→media→AFE lifecycle patch 重建两份AudioService及两份AFE源码，与live逐字节一致。
- 四个检查脚本 `bash -n`及源码/文档diff空白检查通过；统一patch的格式上下文单独由重建验证。

构建与运行日志保存在私有目录：
`/Users/amtf/.codex/dev-harness/artifacts/stackchan-webrtc-media/m15-takeover/`。
含凭据的构建产物不加入仓库，不把原始 SDP/转写放进报告。

## 首轮授权设备观测（80df789，2026-10-23）

用户明确“开始”，授权先观察Idle20秒，再最多60秒near；没有授权顺带双向或重连。
自动复位连接三次未成功，用户按官方步骤长按RST进入下载模式后app写入/hash校验成功。
RTS硬复位仍停DOWNLOAD，改watchdog复位后正常从0x20000启动候选。

- 默认Idle实际观察约48秒；显式START前没有POST/peer/AFE启动标记，USB STATUS为Idle。
- START near_20261023_01 near 60后HTTP200、ICE配对，停在DTLS CONNECTING；没有CONNECTED/DataChannel/RUNNING。
- 25次-0x6900对应MBEDTLS_ERR_SSL_WANT_READ；它单独不是致命错误证明，亦不证明SRTP策略或AEC问题。
- 准备30秒超时，FINAL reason=prepare_timeout；MEDIA sent/decoded为0、ASR items=0、drained_source_frames=0。
  **未提示用户读句子，近端测试未执行。**
- LIFECYCLE capture_quiescent=1 transport_joined=1 reusable=1，随后STATUS Idle。
- 当前Nano配置不支持%lld/%llu，打印出ld/lu并影响后续参数；这些行的后续config、时间和播放样本字段不采信。
  只有32位MEDIA/HARDWARE/EVIDENCE/LIFECYCLE/STATUS及前置stage/reason用于上述结论；旧候选未报告write_ok，不能声称硬件零次写。
- 独立对照旧device-echo-fallback.log也有65次WANT_READ且未连接；旧成功日志约620–630ms进入CONNECTED。
  这说明失败形态早已存在，不能据本轮归因重构；当前新候选传输也未获设备放行。

脱敏证据：[restart-near-01-counts.log](device_diag_evidence/2026-10-23-m15/restart-near-01-counts.log)。
本轮结束后没有自动重启会话/重刷。设备保持Idle；启动门/超时清理观察有效，T1/媒体/AEC不作结论。

### 随后离线日志修复

ESP-IDF components/newlib/Kconfig明确Nano不支持64位整数格式，与设备ld/lu及后续字段错位吻合。
M15改为固定栈缓冲手动十进制转换后用%s输出，不改全局libc配置、不改握手行为；
同时新增per-run read_ok/write_ok增量，下一轮才能核对硬件提交次数。
原台账epoch11→12、used25→26、limit32，新增decimal-log-value（41检查）通过；
独立只读复核参数顺序/临时buffer生命周期/INT64_MIN和UINT64_MAX边界均通过；
媒体固件构建通过，应用0x3cbae0、分区余量23%。
该修复未刷入设备，设备当前仍80df789。修复不宣称解决DTLS停滞；下一轮另等“开始”。

## 后续设备操作顺序（待新“开始”）

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
还需验证设备多轮与真实输出停止、完成response/RTP边界与产品接入，才能推进正式T3/T4。
