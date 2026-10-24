# WebRTC 媒体重新开发记录（2026-10-23）

## 本轮结论

保留已通过复核的 M1 上行、精确 esp_peer 1.5.7、SRTP SHA1_80 修复与真实 48k PCM 编解码。
替换 `2b69e06` 的自动启动限时探针，重新实现受控会话、停止准入和证据边界。
80df789交付前仅做离线开发。之后三轮设备观测分别取得授权：首轮停在准备超时；
694b695第二轮没有同步到有效近端刺激；4e53790第三轮先弹窗确认再START，
用户确认三句已说完，观察到3个独立Accepted ASR item的关键词覆盖；
结合ASR配置、真实采集/编码/发送、喇叭关闭及无错误，经独立人工复核，**SPEC §6.2 的T1单项通过**。
随后duplex04出现用户安静时反复“从前+电流声”，播放质量与本轮行为未通过；正式T2证据不足，AEC未定。
T3/T4、M1.5整体、插话、重连或完整产品移植仍未通过。

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
  程序不单凭关键词覆盖自动裁决T1，人工复核还须核对§6.2的必要控制；不打印转写、原始事件或凭据。
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
该修复交付为694b695；当时尚未刷入。之后另获用户“开始”授权，执行下方第二轮，
真机已确认十进制日志正常。本修复未改变握手行为，不把第二轮建连成功归因于日志修复。

## 第二轮授权设备观测（694b695，2026-10-23）

用户新一轮明确“开始”后，只执行 `START near_20261023_02 near 60`，没有顺带双向或重连。
app写入/hash校验成功；写后启动曾报分区校验错误。esptool读回0x8000起4096字节，
前3072字节与本地partition bin逐字节一致，其余均FF；第二次watchdog_reset后正常启动Idle。
**启动异常原因未定，未改分区表；不能据一次读回定位或排除所有启动问题。**

- Idle实际观察约44.34秒（至少20秒），START前无POST/peer/AFE标记，STATUS为Idle。
- 本轮进入RUNNING，随后报告stage=6、config=1；`t0_us=52914616`、
  `elapsed_ms=60000`等64位字段均为正常十进制，`speaker=0 real_mic=1`。
  这验证Nano日志修复；不证明此前DTLS停滞已修复或长期连接稳定。
- 60秒结束，FINAL为`Done reason=run_timeout`，sent=2948、mic_full=46，
  停止排空mic=6；三者合计3000个20ms帧，与`afe_samples=960000`相符。
  `max_send_gap_us=90471`、`max_queue_age_us=190235`，不能登记无丢帧或连续时序达标。
- ASR items、speech、responses均0。本轮先START开启60秒倒计时，RUNNING后才发读句提示；
  用户随后明确反馈当时正在开发别的、没有看到消息。**测试没有同步到有效近端刺激，
  不构成近端正对照；不能据0 ASR判采音、识别或AEC失败。**
  真正START前未先弹窗确认用户已就绪，是本轮流程错误；这次反馈不作为新开测授权。
- HARDWARE本轮read_ok=6020、write_ok=0；drained_source_frames=2880，
  read/write、转换、drain、AFE reset错误均0。read_ok只表示成功读取API次数；
  write_ok=0与speaker关闭一致，不是声学测量或AEC验证。
- LIFECYCLE的capture_quiescent、transport_joined、reusable均1，之后STATUS回Idle。
  只支持本轮清理观测，未执行第二次START，不能外推设备连续多轮/资源稳定性。

脱敏证据：[nano-near-02-counts.log](device_diag_evidence/2026-10-23-m15/nano-near-02-counts.log)。
该轮结束已关闭串口，没有自动重跑，当时设备为694b695；T1–T4/AEC均未验收。

### 随后离线缩短Running日志

同一个媒体worker每5秒同步调用完整Report；Running每组5行、总量约1KB，
本轮PROGRESS至HARDWARE日志跨度约50–70ms，
实际发送最大间隔约90ms，mic_full除最初阶段外每5秒约增加4帧。
源码路径与时间相关性支持“同步完整日志扰动发送、形成积压”的判断；当时尚未跑短日志候选，
后续较短运行的观测见下节，不能视为等时长/等刺激的严格A/B对照。

最小修复为：Running每5秒只输出一条LIVE短计数；Preparing仍可完整报告，FINAL保留全部明细。
不改队列容量、调度、编解码、运行期限；发送迟于计划超过60ms时重置计划的既有策略也未改。
短行按115200 baud、8N1及字节数估算线速约14ms，**不是实际执行时间或阻塞上界**，
不能据此声称符合20ms期限或消除了丢帧。该修复已通过ESP-IDF5.5.4 `idf.py build`，
应用大小0x3cbba0、分区余量23%，交付为4e53790。原台账保持epoch12、used26、limit32；
本次只有日志输出缩短，未新增可移植行为，不重复旧检查或增加镜像实现测试。
构建交付时未刷写；之后另获授权执行下方第三轮，不自动操作设备。

## 第三轮授权近端观测（4e53790，2026-10-23）

候选为`4e53790b5bd87855d154b9e0cc91aad9038be2eb`，应用大小0x3cbba0。
本轮把准备/刷写与说话测试分开对表：自动刷写/hash校验成功，RTS硬复位直接正常启动Idle，
未观察到第二轮的分区校验异常；这不证明该历史异常的根因已消除。
Idle至少20秒无POST/peer/AFE标记，**先弹窗Q用户，等用户明确点“开始”后才发送START**；
RUNNING后提示具体读句，用户确认三句已说完。

- `START near_20261023_03 near 60`进入RUNNING，stage=6、config=1、speaker=0、real_mic=1。
  三个独立ASR item均status=0，dt_ms为15330/19289/22989，keyword_mask依次1/3/7；
  `near_observed=1`，最终`reason=completed elapsed_ms=23010`，提前结束于本轮关键词覆盖完成。
- sent=1146、decoded=60、failures=0，mic/rx/event的full及invalid均0；
  停止排空mic=3、down=0、events=1。AFE输出367680样本，即1149个20ms帧，
  与1146发送+3停止排空闭合。最大SendAudio间隔30317us、队列年龄90634us。
- 硬件read_ok=2321、write_ok=0，drained_source_frames=2880，read/write、转换、drain、reset错误均0。
  下行已接收/解码60包，但近端模式没有准入到扬声器，播放提交样本/次数为0；
  不把decoded=60当成双向播放或声学回声证据。
- speech=3、responses=2、asr_failed=0，ASR缺失/非法ID、重复、溢出均0。
  LIFECYCLE三项为1，之后STATUS Idle；串口已关闭，未再START。

本轮经独立人工复核，**按SPEC §6.2判T1 PASS，范围仅本配置/本轮**：
启动前弹窗确认与RUNNING后读句对表，用户确认三个不同短句完成；3个独立Accepted item覆盖其关键内容，
config=1、采集/编码/发送正常、硬件write_ok=0且无错误，满足喇叭关闭的近端正对照。
§6.2未要求T1运行满60秒，也没有额外分句时间窗或声学仪器指标，不增加未规定的验收门槛。

本轮没有队列满，
发送间隔较第二轮约90ms下降到约30ms，支持缩短完整报告后扰动减轻的迹象。
但本轮23.01秒，前轮60秒且无有效近端刺激，不能据此宣称严格单变量对照或长期零丢帧；
ASR等短日志仍在发送worker同步输出，约14ms线速估算仍不是执行上界。

原始日志保留`T1_T4=NOT_ACCEPTED AEC=UNDETERMINED`：这是程序不自动验收的硬编码标记，
不妨碍依据原始观测和§6.2进行人工单项T1复核。**T2–T4、M1.5整体与AEC仍未通过**，
本轮不代替双向回声、插话、同进程多轮、长期资源或产品验收。
该轮结束时设备为4e53790，Idle；台账仍epoch12、used26、limit32。
脱敏证据：[live-near-03-counts.log](device_diag_evidence/2026-10-23-m15/live-near-03-counts.log)。

## 第四轮授权双向观测（duplex04，4e53790）

复用设备上的4e53790，不重刷；串口打开时虽未请求复位，实际仍出现USB_UART_CHIP_RESET并从Idle启动。
因此**本轮不是同进程near→duplex重开证据**。Idle阶段无POST/peer/AFE标记，
先弹窗Q，用户明确点“开始”后才发`START duplex_20261023_04 duplex 180`。
RUNNING后用户只说一次故事请求，随后明确确认一直安静、没有其他音源。
本轮沿用已保存音量，实际NVS值未读取，未覆盖正常/最高预期两档。

用户听到设备无限重复“从前”并交替电流/静态噪声，root获报后立即STOP；用户确认声音已停。
运行实际58180ms，以`reason=requested`结束，没有等到180秒超时。

- stage=6、config=1、speaker=1、real_mic=1，sent=2886、decoded=780、failures=0。
  mic_full=17，mic/rx/event的invalid及rx_full/event_full均0；停止排空mic=4、down/events=0。
  AFE930240样本=2907个20ms帧=2886发送+17队列满+4停止排空。
  最大SendAudio间隔40262us、队列年龄129469us，不能登记持续无丢帧。
- 780个下行包被解码并准入播放，submitted_calls=780、write_ok=780，排队/处理中样本最后为0。
  submitted_samples=354000，在24k下相当于**累计API音频14.75秒**，
  不是实际连续播放时长，不满足完整T2要求的“多次连续≥30秒”的证明。
- ASR独立item=17、speech=18、responses=17，缺失/非法/重复/溢出及ASR失败均0。
  这些计数包含最初合法故事请求，不把总数直接等同回声自触发次数；
  用户安静确认与重复可听输出证实本轮异常行为，但不能唯一归因为声学回声或服务端AEC缺陷。
- HARDWARE read_ok=5835、write_ok=780，源排空2880帧，read/write、转换、drain、AFE reset错误均0。
  API成功不能证明样本格式、音质、物理回声或实际播放完成。
- LIFECYCLE的capture_quiescent、transport_joined、reusable均1，STATUS回Idle。
  用户确认STOP后声音停，但没有声学停止延迟测量；串口关闭，无后续START或自动重试。

**结论**：双向收发/播放API链路有观测，本轮重复回答与播放质量未通过；**正式T2证据不足、AEC未定**。
缺少两档音量、多次连续≥30秒、实际声学回声对照与播放期间连续上行的充分控制，
不把本轮直接判为服务端AEC失败，也不掩盖可听异常。T1的near03单轮通过保持，
T3/T4、M1.5整体、同进程多轮、重连与产品验收仍未通过。
证据：[duplex-04-counts.log](device_diag_evidence/2026-10-23-m15/duplex-04-counts.log)。
原台账保持epoch12、used26/limit32；本轮设备观测不计作离线检查。

### 随后离线修复与观测补充（2026-10-24，验证通过、已完成刷写准备）

只读复核实际AFE管线为`[input] -> VAD(WebRTC) -> [output]`，near03与duplex04一致；
源码AGC/AEC关闭，不支持“本轮本地AEC/NS/BSS误开”的归因。
MR仍把第二物理麦放在参考位置；代码没有将下行PCM直接注入输入/参考，输入与输出转换器独立。
TX打开时从stereo/mask3重配到mono/mask1，仍24k/16bit/WS16，未见RX重配；
这既不证明输入被污染，也不能仅凭API返回成功排除格式/模拟音频问题。

已定位独立的日志扰动：M15的fetch等待20ms短于512/16k=32ms音频块，
near03打印731次、duplex04打印1845次`Error code:-1`，预期等待与真实故障没有在日志中区分。
库接口定义的是等待结果的超时，不是固定睡眠。最小改为仅M15有限等待100ms（至少1tick），
保留running检查、输入/fetch锁、Stop迭代静止边界及全部错误处理，普通模式仍portMAX_DELAY。
100ms不是Stop硬上界，锁/调度/回调耗时还在其外；本改动不改模型、AGC、AEC或音频数据。
跟踪AFE lifecycle patch已同步，独立复核通过；重建/两配置构建结果如下。
之后仅完成下方刷写/Idle准备，尚无媒体效果验证，不声称高频等待日志已消失或“从前+电流声”已修复。

事件元数据与Codec样本统计已完成纯观测实现：

- TURN使用独立8项compact metadata队列，原essential事件队列仍为8项；
  诊断队列满只增加diagnostic_dropped，不挤占essential或令会话失败。
  worker最多保存64项，媒体停止后才打印；溢出、字段缺失/非法、停止丢弃单列。
- receive_ms是回调接收时间，snapshot_ms是worker处理/取统计的时间；
  独立队列的处理顺序不保证原始接收顺序，API快照也不是原子声学播放边界。
  FNV32标签仅供诊断关联，不是无碰撞身份或RTP归属规则。
- 依据[阿里云服务端事件](https://help.aliyun.com/zh/model-studio/server-events)的`response.done.response.status`观察响应结束；
  `status_details`只在实际返回时观测，不假定必有或具有固定原因字段。
  `create_response`与`interrupt_response`仅记录true/false/unknown三态，不改变会话配置。
- 结束后报告成功Decode的48k样本、转换后的24k样本、解码样本数直方图、
  播放accepted/submitted样本及PCM24最大绝对值/精确s16端点计数。
  这些是音频数据的只读统计，不改变编解码器/滤波器状态、VAD或播放样本；端点计数本身不证明失真。
  不输出原始JSON、转写正文、原始事件ID或凭据。

独立复核已通过AFE有限等待、CodecStats及最终TURN实现。
TURN初稿共享essential队列的风险已改为上述独立诊断队列，最终复核确认该发现关闭。
root在2026-10-24执行最终验证：

| 验证 | 实际结果 |
|---|---|
| ESP-IDF5.5.4媒体构建 | 通过；应用0x3ccc30，分区余量23% |
| 独立WebSocket回退构建 | 通过；应用0x3a37a0，分区余量26% |
| 依赖补丁重建 | xiaozhi HEAD `e77dedb1309153bb63fed285772962c920c97dd4` → vendor → Aliyun → media → AFE；AudioService h/cc及AFE h/cc四源码均与live逐字节一致 |

原台账仍epoch12、used26/limit32；此次只补计数/记录，没有新增可移植行为检查或镜像实现测试。
两配置构建与补丁重建在离线控制器覆盖外单独记录，不伪计为受控测试。
实现提交`4d5fa0b80c82765ed869c202a8d7087532fc1364`已推送并刷入；
随后完成下方05准备超时与06短诊断，仍不宣称噪声或重复回答已修复。

### 诊断候选设备准备（2026-10-24，仅启动/Idle）

首次default_reset未收到串口数据；弹窗确认用户手动进入下载模式后，no_reset刷写/hash校验通过。
RTS硬复位仍停ROM下载，单独watchdog_reset chip_id后正常启动Idle。已观察20秒，
未发送START、无POST/peer/AFE启动标记。该恢复步骤不改变分区或归因历史启动异常。

准备完成时设备为4d5fa0b、Idle，已弹窗Q等明确开始；没有因准备授权自动START。
随后05/06分别获新弹窗确认才执行，结果如下。


## 第五/六轮短诊断（4d5fa0b，2026-10-24）

05由新弹窗明确点击开始后START，停stage=4，准备30秒超时，未到RUNNING、未提示读句。
发送/解码/采集/播放/ASR均0，清理后STATUS Idle，不能当媒体失败或AEC测试。
用户再于新弹窗点开始；保持同一串口/进程，中间无reset/重刷，再START 06。
06到RUNNING，实际运行30002ms，reason=run_timeout；结束三项LIFECYCLE均1、STATUS Idle，串口已关、无后续START。
这是一例**prepare失败→同进程显式重开→有效运行**，不代表长时或大量轮次稳定性。

06在RUNNING后提示读指定句。首条ASR在22.075秒；全轮4个ASR item、4次speech、3个response。
**用户本轮已确认读完后一直安静、无其他说话/音源；听到“还是一直从前从前，说3次后没声音”。**
用户未明确声音停止与30秒自动结束的时间对齐；日志明确三个响应均cancelled/turn_detected，
整轮在30.002秒因run_timeout结束，不能把“没声音”解释为AI正常讲完。
未打印的转写正文不能由item序号推定为已验证的故事文本；取消触发输入的声学/电气/噪声来源仍未定。
已收TURN 22项，overflow/diagnostic_dropped/diagnostic_stop_discard均0，三个不同response标签记录如下：

| response标签 | created接收ms | done接收ms | 相邻新speech_started接收ms | done状态/原因 |
|---|---:|---:|---:|---|
| 855f0cc4 | 22179 | 24653 | 24653 | cancelled / turn_detected |
| 33fe92c8 | 24853 | 26074 | 26079 | cancelled / turn_detected |
| 4f59cf28 | 27480 | 28718 | 28719 | cancelled / turn_detected |

两种事件接收时间相差0/5/1ms；独立队列处理顺序不当作接收顺序。
会话回显create_response=true、interrupt_response=true，客户端没有主动cancel发送代码。
可确定这三次响应**被新turn检测取消**；仍不能由该字段或时间接近确定输入来自声学回声、电气串扰、噪声或其他来源。
FNV标签只作诊断关联，不提供RTP与响应的归属证明。

- 下行125包：10×480+115×960=115200个48k解码样本；
  converted24=accepted24=submitted24=57600，比例闭合，约为**2.4秒累计API音频**，不是连续声学播放时长。
  write_ok=125、错误0；PCM24 max_abs=11161、精确端点0，仅排除本轮观测数据命中s16端点，不能证明无噪声或音质正确。
- AFE输出479040样本/320=1497帧；sent1490+mic_full3+停止排空3=1496帧，
  **尚有1帧未被当前计数单列，不伪造闭合**。源码存在转换预热返回空包或Pop后Continue超时的可能路径，
  但本轮没有路径专用计数，具体原因未实证。
- 其他队列full/invalid及failures为0，max_send_gap_us=44870、max_queue_age_us=125411。
  硬件read_ok=3018/write_ok=125，源排空2880帧，read/write、转换、drain、reset错误均0。
- 本轮实际AFE `Error code:-1`为0，相比near03的731/duplex04的1845条，
  100ms等待对高频等待日志的改善已有本轮实据，不承诺所有条件均不超时或声音已修复。

**结论**：用户已确认静默期间仍重复“从前”，本轮可听回答异常；新turn取消回答的机制已有直接事件证据，
声音/输入根因未定，正式T2仍未验收。用户要求暂停新测试，先核对真实StackChan硬件与官方实现，未追加设备操作。
本轮有效下行API音频仅2.4秒，不能替代完整T2的音量/播放/回声控制；T1单项状态保持，T3/T4、M1.5整体和产品未通过。
代码未改，台账仍epoch12、26/32。原始脱敏证据：
[duplex-05-counts.log](device_diag_evidence/2026-10-23-m15/duplex-05-counts.log)、
[duplex-06-counts.log](device_diag_evidence/2026-10-23-m15/duplex-06-counts.log)。

## 后续设备操作顺序（待新“开始”）

设备准备/刷写与实际说话测试分开对表；准备结束保持Idle，不因刷写许可自动START。
真正START前，**先弹窗Q用户说明该轮内容与时长；等用户看到该弹窗并明确点“开始”后，
才发送START、打开采集与倒计时**。不得先开启60秒窗口再等待用户注意消息。
看到RUNNING后才提示具体读句，时间从新t0开始；用户未回应就不启动这一轮。
本次05/06已结束并关串口，当前设备4d5fa0b、Idle；没有新一轮授权，不自动START或追加重连。
后续每轮仍先Q明确开始，短诊断不当完整T2。
近端模式核对speaker提交为0；双向诊断核对下行样本/提交与TURN序列、独立item、错误/丢弃及停止原因。
不得自动把关键词mask7登记T1通过。
后续双向及任何重启/采集均重新对表，不沿用上一轮“开始”。

设备端命令（LF或CRLF，单空格分隔）：

```text
STATUS
START near_01 near 60
STOP near_01
```

双向模式为 `START duplex_01 duplex 60`，**不在首轮顺带运行**。
还需验证设备多轮与真实输出停止、完成response/RTP边界与产品接入，才能推进正式T3/T4。

## 2026-10-24 DSH 接续：硬件/驱动审计与 AUDIT 候选（无设备操作）

用户授权按[DSH交接](webrtc-media-dsh-handoff-2026-10-24.md)接续；坐标
`feat/aliyun-omni-v2v`、HEAD `da4df168a9c89ae31e62e117977e1f7819fe4f19`、工作树干净。
本轮**不打开串口、不刷机、不 START**，只做离线对照、文档、最小诊断候选与构建。

### 审计结果

- 官方 C156 页面所链原理图第4页：`SPK_VOP/VON` 经 150K/1%+22pF+1uF 形成
  `AEC_P/AEC_N` 接 ES7210 `MIC3P(31)/MIC3N(32)`；MIC1/2 为物理麦，MIC4 短接 GND；
  无 MAX98357/其他功放/扬声器连接器。官方固件 `eff0d6b5` 与本项目 codec 同源。
- **MIC3 参考不在 2-slot 读取范围**（按 ES7210 TDM 顺序与 2-slot 重配推断；实板线上 slot 映射仍未实测）：ES7210 因选 3 麦进入 TDM；I2S RX 初始化虽写 4 slot，
  `esp_codec_dev 1.5.11` 在 `channel=2` 打开时把 `total_slot=2, ws_width=16,
  slot_mask=0x3` 重写，4-slot 初始化是死配置。AFE `input_reference=true` ⇒
  `"MR"`，把 ch1 当参考（按 TDM 顺序应为 MIC2；实板 slot 映射未测）；
设备 AEC 曾把用户语音一起消掉（峰值 4036→120）。
- 撤回“最多2通道”：库只拒绝 0/奇数通道；4ch 合法，但 RX 4×16bit（64 bit/帧）与
  TX 2×16bit 共享 BCLK/WS 不同频，AW88298 也是 16-bit 立体声，故本轮不启用 4ch。
- #303/#308 的 fs/2 镜像音来自 M5Unified 自己的 32-bit 打包，本项目原生 16-bit 写入，
  **不能照搬 swap**；#303 仍开，“电流声”仍无解释。
- 因此双向循环不能归因“参考串入”（参考未被读）；最简约仍是声学耦合 + 无设备 AEC +
  服务端 AEC 未证。实板 revision/帧长/slot 对齐/参考幅度仍未示波器验证。

### 最小诊断候选

新增 M15 USB 命令 `AUDIT <3..10>`：本地 1 kHz 正弦（1024 ≈ -30 dBFS，不改音量）+
现有 2ch 采集，三段 tone/silence/tone，输出每通道 RMS(dBFS×10)/peak 与
`tone1 - silence` 差；不联网、不开 AFE、不启 AEC，不改持久化音量、结束关闭 IO；
200 ms 保护段 + 200 ms 预热、`seconds+2` 软时限、10 次失败中止。
纯逻辑 `channel_audit.h`；命令解析只接受 3..10。

### 离线验证与构建

- 原台账 `/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`
  经 `replan` 新增 `channel-audit`（26 断言，cache=never），`console-command` 复跑
  （17153 断言）；**epoch 13，used 28/32**，未 recover/新建/清预算。
- 编译与 IDF 构建在控制器覆盖外，单独记录（见[审计评审请求](webrtc-media-audit-review-request.md)）。
- **未做设备运行**；下一轮须新弹窗 Q：`AUDIT 6`，以及正常双向 vs 遮住 MIC1/MIC2
  出声孔的对照，再决定 4ch/电气路径。

## 2026-10-24 候选刷入与 AUDIT 6 首次实测

- 刷入 `97864ef`（M15 + AUDIT，app `0x3cd8f0`）到 app 分区；bootloader/分区/NVS 未动。
  复位后 `IDLE; USB commands: ... AUDIT <3..10 seconds>` 可见，设备 Idle。
- 用户在新弹窗中点“开始”后执行 `AUDIT 6`（本地 1 kHz，幅度 1024，不改音量）：
  tone1 ch0 -22.8 dBFS / ch1 -19.8 dBFS，silence -46.5 / -47.1 dBFS，
  `echo_delta=+23.7/+27.3 dB`，读写失败 0，结束回 Idle，未联网。
- 结论：扬声器输出显著进入当前 2ch 上行，支持“无 AEC 时播放导致新 turn”的回路；
  声学 vs 电气仍未定，下一步用“遮住 MIC1/MIC2 出声孔后重跑 AUDIT”区分。
- 脱敏日志：`docs/device_diag_evidence/2026-10-24-m15/audit-06-idle.log`；详见 findings B56。
