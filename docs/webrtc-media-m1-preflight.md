# M1 开发中预检提示

这是共享工作区的临时预检，不是已提交版本的正式 PR 评审；M1 任务继续执行。

## 长测前先修 rate_cvt 的输入容量

2026-10-08 读取当前 `webrtc_m1.cc` 时，`MediaSender::EncodeFrame` 的
`uint32_t actual_out = 0` 随即传给 `esp_ae_rate_cvt_process`。
实际 `esp_ae_rate_cvt.h:126–127` 说明该参数是 **in/out**：输入为可用输出样本容量，
输出才是实际生成样本数。前一步查询得到 `max_out`，缓冲也已扩容，
但没有将可用容量传入 process，可能导致每次转换均失败且无 RTP 媒体。

最小修复：调用前设为实际可用的每声道容量（例如查询的 `max_out`，且确实已分配），
调用后检查返回码与 actual_out 不超过传入容量，再按实际长度处理。不要硬填理论生成长度。

请在5分钟真机循环前修正，并将返回码及输出样本数纳入验证。
其他代码仍在开发，本提示不要求停止或另起需求；完成原 M1 的实现／验证／交付／STOP 流程。

Codex 另已补 `firmware/.gitignore` 的 `/build-*/`，避免正常配置构建产物被误提交，
并已添加 findings B19 的多帧拼接证据；保留这些本轮记录。

## 补充：长测前应修齐的必要契约

下列内容核对了当前本地候选 `e6d7158c89448346261ef3aa3b47dab276687e6d`，
并有两份独立只读检查支持。该提交尚未完成 M1 的正式评审请求；这仍是开发中反馈，
不要求停止原任务。修复后继续本轮构建／真机／交付。

1. **真实退出／初始化失败清理**：`webrtc_transport.cc:383/387` 创建 task 时传入
   `nullptr`，`loop_task/signal_task` 永远为空；`Stop:476–489` 因而直接 close peer、
   删除信号量，任务仍可能在30秒 SDP等待／HTTP请求／peer poll中使用它们。
   保存句柄还不够，2秒等待也不能覆盖这些阻塞。使用同步停止与真实退出通知／join，
   唤醒或有界取消信令，任务确实不再访问后才释放资源；不要把延时当join。
   Start任一步失败走同一清理，MediaSender.Open的失败也释放已打开codec/resampler。
   M1配置超时分支 `663–674` 必须**先释放 `g_obs.mtx` 再调用 Stop**，
   否则等待正要取该锁的回调退出会死锁。
   信令task中的局部 `std::string` 必须先离开作用域再 `vTaskDelete(nullptr)`，
   FreeRTOS self-delete 不会执行C++栈析构；可用普通helper返回后通知退出并删除task。
2. **核验配置，不能只打印**：`webrtc_m1.cc:404–409` 任何 `session.updated`
   均令 gate通过；`SummariseSession` 只有摘要。核对当前会话／已发送update与实际字段：
   `server_vad`、阈值0.5、静音800ms、期望的转录模型。
   缺失、null或错误值应报告配置失败，不发送fixture；实际兼容性由服务端回显判定。
3. **逐条ASR归属与关键词**：`684–719` 仅凭全局completed计数增加，关键词没有检查。
   前一条超时后迟到的completed可被下一条误认。保存每条clip对应的item／结果／关键词命中，
   从实际VAD／committed事件关联completed／failed；第一轮超时或failed不得让下一条吞掉迟到事件。
   failed记录item_id、code、message、param。数字“一加一”可接受预先定义的等价转写，
   不把数字格式差异误判成传输错误，也不在测试失败后任意放宽关键词。
4. **时间轴不能压掉等待**：等待ASR期间无发送且不推进pts，下一条重置墙钟deadline并沿用旧pts，
   会把真实几秒／几十秒的间隔压成0。使用同一sender媒体时钟；可以在等ASR时继续按节拍发静音，
   或明确推进未发送间隔。不得以每条局部media/wall统计隐藏这些间隔。
5. **资源证据应能真实采集**：`SampleResources` 的调用均传nullptr，栈余量恒0；
   当前sender就在app_main，传实际task handle即可。补充 INTERNAL+8BIT、DMA minimum、
   编码／转换耗时的有界采样及分位；记录持续loop实际elapsed与是否完整达到300秒，
   提前断线不能算5分钟通过。fixture时长应先乘1000后除采样率，避免三条均误报3000ms。
6. **保留M0配置确实可用**：`main.cpp:13–21` 把Board／WifiManager／esp_log的include
   只放在M1 guard，M0分支仍使用它们；共享include用M0||M1并核对M0配置。
   Kconfig只在help声称互斥，需真正拒绝M0/M1同时启用。
   目前M0仍保留整套旧信令而Transport复制了它；按任务要求共用已验证传输层，
   保持M0不发媒体／判据语义，并以相关配置检查确认回归。

这些都是原 M1 任务已经要求的单位、时间轴、生命周期、验收与隔离契约；
无需新增产品音频服务或复杂bench框架。修复并完成原任务后，仍按实际新HEAD交正式REVIEW_REQUEST。

## 首轮真机崩溃证据：先定位再继续长测

2026-10-08 读取当前 `/tmp/m1-run.log`，同一次采集已出现两次：

```text
assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))
rst:0xc (RTC_SW_CPU_RST)
```

两次均在配置回显已通过、开始第一条 fixture 后，尚无 completed ASR。
用该次 `firmware/build/stack-chan.elf` 的 `addr2line -pfiaC` 解析：

```text
0x4200814d pthread_mutex_lock
0x4214029a __gnu_cxx::__scoped_lock / eh_alloc.cc:259
0x4214036f __cxa_allocate_exception
0x42140e29 operator new
0x4202db69 std::vector<long>::_M_realloc_append
0x4202e9c9 EncodeFrame webrtc_m1.cc:287 / SendOneFrame:624
0x4202eb53 StreamClip:664
0x4202f53a WebRtcM1Run:946
```

调用链经过 timing vector 扩容与异常分配，**提示分配失败或先前内存损坏，但尚未定位根因**；
不能只把日志表面的 semaphore assert 当生命周期缺陷修复。
请核对采集对应的 ELF，停止无效长测，解析崩溃，记录首次编码前后分能力 heap／largest block
及任务栈，检查真实编码缓冲容量／转换输出和库对返回值的语义。
统计容器应预先分配、明确预算且避免在每帧路径动态扩容；内存不足要有可观察失败路径。
修复后重新跑验收，断言重启的旧采集不能算五分钟稳定运行。

### 修正容器后的新证据

`/tmp/m1-run2.log` 第一帧前的内部heap为148871B、最大块63488B，PSRAM约7.9MB，
随后出现 `***ERROR*** A stack overflow in task main has been detected.` 并重启。
因此“每帧分配”还不是已证根因，必须进一步检查main栈被48k编解码耗尽／破坏的可能。
实际配置 `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`；当前M1编码运行在app_main上，
声明的 `kSenderStackWords` 并未创建独立task或扩大main栈。
现有WS `AudioService::Start` 给Opus worker分配 `2048 * 12` 字节栈，
是有用参照，不是48k路径所需栈的保证。
请为M1 sender使用实际足够的独立task栈、退出同步与实测high-water mark，
不要用全局修改normal固件main栈来代替本阶段的独立owner。

### 独立sender的栈参数单位仍须纠正

最新WIP改成 `kSenderStackWords = 8192; // 32 KB` 并直接传给 `xTaskCreate`。
**ESP-IDF 5.5 的 `xTaskCreate` 参数单位是字节，不是普通FreeRTOS的words**：
本机`components/freertos/FreeRTOS-Kernel/include/freertos/task.h:315–316`明确写NUMBER OF BYTES，
394–398直接将该值交给PinnedToCore。
因此当前 `m1_sender` 仍只有8192B，和刚溢出的main一样。
请用实际字节值（例如32768B作为待实测起点）并按真实high-water API单位记录余量。
“永远不是allocation问题”也不能从一次栈溢出推得；记录已证实的8KB栈溢出即可。

## 后续真机证据：日志格式与媒体接收仍须分开定位

`/tmp/m1-run6.log` 已通过配置 gate 和首帧编码，但第一段无 VAD/ASR，
随后在 `[clip]` 日志中发生 `LoadProhibited`、`EXCVADDR=0`。
该次 ELF `971ed8d55` 当时定址到 nano vfprintf / `sender_task` 日志路径；
后续 ELF 已覆盖，不把新 ELF 当旧回溯的直接证据。
本机 newlib Kconfig 明确 nano formatting 不支持64位整数；
独立反汇编核查 `%lld` 不消费该实参，会使后续 `%s` 读到错位的零值。
当前源码已把 M1 / transport 的这些格式改为32位显式转换。
证据摘要 `/tmp/stackchan-media-review-v155/nano-printf-evidence.md`。
这是日志崩溃的强解释，**不能解释此前无 VAD/ASR**。

`/tmp/m1-run7.log` 的20ms路径已成功返回16521次发送，最大payload325B，
编码/重采样/发送失败计数均0，sender栈最低余量10232B/32768B，未再panic。
但三段均20秒ASR超时，VAD started/stopped、ASR completed/failed均0；
随后连接中断，sustained loop标记INTERRUPTED。
因此这一采集证明sender运行与资源余量，**未通过任何一条ASR或完整loop验收**。
当前 media/wall 输出222470/222470ms来自重复墙钟计量，
与0..330400ms RTP媒体范围不符，仍须独立核算样本时钟/墙钟/loop起止。

本地fixture16k s16le核算：三条peak分别18545/23715/21671，
全段RMS约-19.77/-20.26/-20.34 dBFS，非零样本RMS约-17.25/-17.52/-17.66 dBFS。
这排除了输入文件全零，未证明重采样/编码/实际RTP仍有语音。
先补最小诊断：设备PCM输出能量、编码packet有效帧/时长、实际发送RTP的PT/时戳/计数，
只打印非敏感元数据；配置echo和send API=0不能替代服务端可用媒体证据。

### DTLS timeout不能归为session配置失败

run4/run5的`-0x6800`为本机Mbed TLS3.6.5的`MBEDTLS_ERR_SSL_TIMEOUT`。
`dtls_common.h:211` 的 `SRTP connected OK` 在导出密钥时创建SRTP context，
并不证明 `mbedtls_ssl_handshake()==0`；真正成功在该文件643行。
两次缺少CONNECTED/DataChannel/session.created，属于传输握手失败，
当前gate超时日志写“not a transport failure”应修正为实际截止阶段。
run1/2/3的后续重启也有相同timeout，因此不是32KB sender引入的回归。
HELLO缓存是1.5.6的已知修复，但尚无报文到达/丢弃证据证明本次命中；
不得据此升级1.5.6、改SDP、降48k或宣称已定根因。
# Review 5447065335 修复中：发包观测预检（非正式复核）

第67轮新增 `rtp_send_probe.cc` 的WIP需要先保证观测本身可信：

- 环形槽位的 `used` 未释放、没有expiry，而 `g_generation` 未用于匹配。
  第65个包后覆盖已完成发送的旧记录也被计为overflow，不能据此报告真实覆盖缺失。
  需区分pending、已匹配成功、失败/重试和过期，按设计保留短寿命记录并核验generation。
- SRTP返回后要把status和仅成功时有效的output length更新到本次record，
  UDP匹配须按同一record报告保护结果，不能只凭tuple计成功。
  区分write attempts与packet-with-success，保留真实失败status和缺失写入计数。
- `std::string dummy` 无用途；前置观测也须保持调用前errno不变。
  报告当前把header length硬写0，需报告真正解析值。
- 本机组件目录名为 `espressif__esp_libsrtp`，不能把CMake target写成
  `idf::esp_libsrtp`；使用实际alias或查询 `COMPONENT_LIB`，并验证隔离重建。
- 先短时输出新观测并核对链接callsite覆盖。当前仍把观测汇总放300秒循环之后，
  不应重复已完成的持续本地编码来取得第一次发包证据。

以上是实现参考[实际发包观测设计](webrtc-media-m1-send-observation.md)的具体落实，
属于既有R2-4，不是新正式Review或阶段放行。观察到UDP成功仍不证明云端接收。

### 刷机前：短时诊断不能重新在8KB main上编码

新WIP把100次 `SendOneFrame` 直接插在 `WebRtcM1Run` 中、sender任务创建之前。
这是main的8KB栈路径，会重现本轮已实测定位的Opus栈溢出；
不能因只有2秒就认为栈预算不同。诊断和正式发送必须由同一个32KB sender执行。
同样保持唯一codec/sender所有权和样本时间轴，不从main调用编码/重采样。
先输出短诊断，再决定是否继续验收；本条属于既有M1任务栈契约与R2-4。

run11现已实测重现：ELF `85c5b00dc`，配置回显通过，
`[diag] short send-path observation starting` → first-frame-before →
`A stack overflow in task main has been detected`，随后重复重启。
其他回溯发生在栈损坏后的重启，不作为新的独立根因。
这是无效诊断；Codex尝试查找匹配run11的串口采集Python进程，但检查时未发现匹配目标，
未发送终止信号。原日志保留，既有修复需求继续。不得用增大main栈替代唯一sender所有权。
