# M1 结果：48k PCM 上行探针

状态：**待 Codex 复核。四项验收中三项达成，VAD/ASR 一项未达成。**
基线 `57ab76e`；本阶段实现 `hal/webrtc/webrtc_transport.*`、`webrtc_m1.*`、
`fixture/`、`tools/webrtc_probe/make_fixture.py`。

## 1. 结论

| 层 | 结果 | 证据 |
|---|---|---|
| 配置 | ✅ **通过** | `session.updated verified: turn_detection=server_vad thr=0.50 silence=800 transcription=qwen3-asr-flash-realtime` |
| 媒体发送 | ✅ **通过** | 16521 包 / 1467673 B / 最大 325 B（预算 1352）/ 媒体钟 0..330400 ms |
| VAD | ❌ **未达成** | `speech_started=0 speech_stopped=0` |
| 完整 ASR | ❌ **未达成** | `completed=0 failed=0`，三条 clip 均 `kw=0/n`，`transcript=""` |

**即：RTP 层面零错误地持续发送了 330 秒，但服务端未把它当作语音。**
不能据此宣称 M1 通过。

## 2. 已修并已验证的契约

| 契约 | 状态 |
|---|---|
| `rate_cvt` 的 `out_sample_num` 是 in/out（传容量而非 0） | ✅ |
| `get_frame_size` 字节/样本换算（1920 B / 960 样本，48k mono 20 ms） | ✅ 实测吻合 |
| 单包 raw Opus，超限丢弃 | ✅ oversize=0，max 325 B |
| 绝对媒体期限 pacing，丢帧仍推进 | ✅ pacing_late=0，media==wall（222470/222470 ms）|
| 等 ASR 期间继续按节拍发静音（不压缩时间轴） | ✅ |
| 配置核验（非仅"收到 session.updated"） | ✅ 缺失/错误值会判配置失败且不发 fixture |
| 逐条 ASR 归属（item 槽位，关键词计分） | ✅ 已实现（本轮无事件可归属）|
| 统计容器预分配、每帧路径零动态扩容 | ✅ 改为定长 `LatencyLog` |
| 可观察的 OOM 失败路径 | ✅ `oom stops` 计数（本轮 0）|
| 独立 sender 任务 + 退出同步 | ✅ join 信号量 |
| M0 共用传输层、不发媒体、判据语义 | ✅ 三配置均编译通过 |
| Kconfig 真正拒绝 M0/M1 同时启用 | ✅ `depends on !STACKCHAN_WEBRTC_M0` |

## 3. 本轮定位的三个真实缺陷（按发现顺序）

### 3.1 每帧路径上的动态扩容 → `__cxa_allocate_exception` 断言

`assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))`。
`addr2line` 显示 `std::vector<long>::_M_realloc_append` → `operator new` →
`__cxa_allocate_exception` → NULL mutex 断言。
**异常分配是次生现象**；已在每帧路径移除动态扩容（定长 `LatencyLog`、
`Open()` 内预分配 `pcm48_`/`encoded_`/`g_pad`）。

### 3.2 main 任务栈溢出（**不是内存不足**）

`***ERROR*** A stack overflow in task main has been detected.`，
当时内部 heap 仍有 148871 B、最大块 63488 B。
`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`，48k Opus + 重采样放不下。
改为独立 `m1_sender` 任务。

### 3.3 `xTaskCreate` 的单位是**字节**（ESP-IDF 与 vanilla FreeRTOS 不同）

`task.h` 原文 "NUMBER OF BYTES ... differs from vanilla FreeRTOS"。
误写 `kSenderStackWords = 8192` 仍只给 8 KB，等于重现 3.2。
修正为 `kSenderStackBytes = 32768`；`uxTaskGetStackHighWaterMark` 同样返回字节。
**实测高水位 10232 B 空闲**，即用量约 22.5 KB——8 KB 确实不够，32 KB 有余量。

### 3.4 `%lld` 在 ESP-IDF 上失效

`Guru Meditation (LoadProhibited)`，`EXCVADDR 0x00000000`，
回溯落在 `vprintf` 内的 `_printf_i`，调用点是 `sender_task` 的日志。
newlib-nano 不支持 `ll` 长度修饰符，会误读可变参数。
这是**我方台账已记录过的坑**（findings B 系列），本轮又引入一次；
已清空全部 `%lld`。

## 4. 资源与耗时（330 秒持续负载，20 ms 帧）

| 项 | 值 |
|---|---|
| 内部 heap free / min | 114491 / 113235 B |
| 内部最大块 | 63488 B（恒定）|
| DMA free / min | 106703 / 105447 B |
| PSRAM free / min | 7944800 / 7905664 B |
| sender 栈高水位 | 10232 B 空闲 / 32768 分配 |
| 重采样 | p50 275 µs，p95 325 µs，p99 342 µs |
| 编码 48k mono 20 ms | p50 2474 µs，p95 6572 µs，p99 6744 µs |
| 失败计数 | send 0 / encode 0 / resample 0 / oversize 0 / oom 0 / pacing_late 0 |
| 趋势 | 无持续下降；PSRAM 持平 |

帧预算 20 ms，编码 p95 6.57 ms，**单帧有约 3 倍余量**（未计 AFE 与 UI，
M2 才做完整产品预算）。

## 5. 未完成项与下一步假设

**VAD/ASR 未触发是当前唯一阻塞项**，尚未定位。可能方向（均未验证）：

1. **RTP 时间戳语义**：`pts` 为毫秒、库内 `pts*48` 是我方据二进制得出的结论；
   若与实际不符，服务端可能因时间轴异常而丢弃媒体。
2. **Opus 载荷有效性**：需确认发出的是服务端可解析的 Opus 流
   （而非仅本地编码成功）。
3. **采样率协商**：`audio_info.sample_rate` 传 48000，
   但 RTP 时钟与 PCM 采样率的组合是否被服务端接受，未验证。
4. **是否需要 `input_audio_buffer.commit`**：`server_vad` 下理论上不需要，
   但未排除。

**已排除**：本地发送失败（计数为 0）、超限丢弃（0）、OOM（0）、
媒体时间轴压缩（media==wall）、配置未生效（已核验回显）。

## 6. 本轮未做的事

- M1.5（AEC 假设、双讲、打断）保持**未开始**，按指令 on hold
- 未做下行播放、未做产品 AudioService/UI 接入
- 未宣称 48k 路径产品级可用

---

# 复核修复轮（M1-1 ～ M1-7）

修复对象：Review 5446817816，reviewed exact HEAD `c3e6582`。

## 修复对照

| # | 处置 |
|---|---|
| **M1-1** | M0 恢复在**本会话发现的 stream** 上发送 `session.update`，成功后标记；channel 回调提供重试；发送前释放观察锁避免死锁；不发媒体语义不变。M0 结果新增 `session.update sent` 一行。 |
| **M1-2** | `cleanup()` 改为**幂等**（`cleaned` 交换）；只等待**确实创建**过的任务；以任务自己的 `*_confirmed_exit` 标志为准，**超时不再释放 peer/信号量**，而是报告失败并保留仍被访问的资源；`signal_body` 在等待 SDP 前后与 HTTP 返回后**均检查取消**；task 栈显式命名并注明**字节**单位。 |
| **M1-3** | 采用评审给出的**最小安全策略**：首条 clip 无结果即**停止验收序列**，不再开始后续 clip，避免迟到结果落入下一条槽位。实测已按此停止（本轮只出现 `[clip] zh_1`）。关键词计分保留。 |
| **M1-4** | 媒体时长**由帧计数推导**（`frames_media × kFrameMs`），墙钟独立测量；两者不再共用同一表达式。实测 `media/wall = 327020 / 252723 ms`，**不再相等**。持续 loop 独立记录起止与 COMPLETE。 |
| **M1-5** | 加入受控诊断：重采样输出 RMS、**本地 Opus 解码往返**（能量 + 失败数）、包长分布。全部为白名单非敏感元数据，不含 key/Authorization/完整 SDP。 |
| **M1-6** | 失败按**实际阶段**报告。实测输出 `VERDICT: FAIL - transport layer. stage=server answer received(4) peer_state=6` —— 不再把未完成的握手说成配置失败。 |
| **M1-7** | 本文件补齐；SPEC/PLAN/调度/PR 状态同步；patch 行尾空白清理（见下）。 |

## M1-5 诊断结果：上行音频是真实有效的

| 诊断项 | 值 |
|---|---|
| 重采样 48k RMS | peak **0.2010**，avg 0.0507，16351 帧 |
| 本地 Opus 解码往返 | peak **0.2019**，avg 0.0506，**16351 成功 / 0 失败** |
| raw Opus 包长 | min 3 B，max 325 B |

**结论：重采样确实产出真实音频，编码产物确实是可解码的 Opus 音频。**
因此"无 VAD"**不能**归因于本地静音、重采样失败或编码产物无效。

评审已指出 `SendAudio` 返回 0 不证明 SRTP/socket 写入成功
（`rtp_encoder_encode_generic` 调 void 回调后固定 return 0，
`write_rtp_packet` 不向上传递 SRTP/agent/socket 失败）。
本轮**未**推翻该判断，也**未**据此宣称服务端已收到。
仍未定位的是 **RTP/SRTP 发送链路或服务端侧**，需要评审所指的
RTP PT/seq/ts/SSRC/发送长度与实际写入结果才能继续。

## 本轮稳定运行证据

| 项 | 值 |
|---|---|
| 持续 loop | **COMPLETE**，325831 ms（预算 300000 ms）|
| clips / packets / bytes | 66 / 16351 / 1660625 B |
| sender 栈高水位 | 10172 B 空闲 / 32768 分配 |
| 重采样 | p50 344 µs，p95 358 µs，p99 417 µs |
| 编码 48k mono 20 ms | p50 2742 µs，p95 7022 µs，p99 7435 µs |
| 失败计数 | send/encode/resample/oversize/oom/pacing_late **全 0** |
| 堆趋势 | int 114835 → 114887（无下降）|
| VAD / ASR | `started=0 stopped=0` / `completed=0` —— **仍未达成** |

## 仍未完成

- **VAD/ASR 仍未触发**；根因未定位，方向已收窄到 RTP/SRTP 发送链路或服务端侧
- 60 ms 帧长证据、隔离重建逐条记录、完整命令与 hash 仍在补齐中
- 本轮只修 M1 findings；**M1.5 与产品下行/AudioService/UI 保持 HOLD**

## M1-7 部分未完成：patch 行尾空白

评审指出 `firmware/patches/stackchan-aliyun.patch` 第 47/67/100/119 行有行尾空白。
本轮尝试清理时发现：**这些"空白行"在 patch 语法里是内容为单个空格的上下文行**
（代表源文件中的空行），把它改成真正的空行会改变 hunk 语义，
而评审同时要求"修空白须保持 patch 仍可正向／重复应用"。

在剩余验证预算内无法完成"清理 + 完整可应用性验证"两件事，
故**按正确性优先回退清理**，该 P2 项**未完成**，如实记录。
建议在下一轮与隔离重建验证一并处理。

---

# 复核修复轮 2（R2-1 ～ R2-5）

修复对象：Review 5447065335，reviewed exact HEAD `9e45e63`。

## R2-4 已完成的实际发包诊断

### 1. SDP 媒体参数（白名单输出，无 ICE 密码/Authorization/完整 SDP）

```
OFFER : m=audio 9    UDP/TLS/RTP/SAVPF 111 | a=rtpmap:111 opus/48000/2 | a=sendrecv
ANSWER: m=audio 3478 UDP/TLS/RTP/SAVPF 111 | a=sendrecv | a=rtpmap:111 opus/48000/2
        a=fmtp:111 minptime=20
```

**服务端接受音频 m-line**（PT 111、opus/48000/2、`a=sendrecv`）。
故"服务端不接受音频"这一假设**已排除**。

### 2. 实际 SRTP / UDP 发包观测（链接器包装）

新增 `main/hal/webrtc/rtp_send_probe.{h,cc}`，以 `-Wl,--wrap=srtp_protect`
与 `-Wl,--wrap=lwip_sendto` 观测两个真实边界（两者在 S3 目标文件中均为未解析外部符号，
因此可用 GNU ld wrap 重定向，无需改动 managed 源码或猜测不透明对象布局）。
仅观测：所有调用原样转发，包括返回值与 errno。

按预检要求修正的观测逻辑本身：

| 问题 | 处置 |
|---|---|
| 记录从不退休，第 65 包被误报为 overflow | 记录有 pending/protected/written/failed 状态与 TTL；**回收已完成记录不算 overflow**，只有回收或退休 **pending** 记录才计入丢失 |
| SRTP 状态未绑定到对应记录 | 保护结果写回**本次插入的那条记录**，`*srtp_len` 仅在成功时解释 |
| attempts 被当成成功包数 | `udp_attempts`（写入尝试）与 `packets_written`（至少一次完整写入的包）**分开计数** |
| 多余的 `std::string dummy`、header 长度硬写 0 | 已删除；报告真实解析的 `hdr_len` |
| 调用前 errno 被扰动 | 前置快照与后置更新**都**保持 errno |
| CMake 写成 `idf::esp_libsrtp` | 改为按 `espressif__esp_libsrtp` 组件查询实际 target |
| 观测汇总放在 300 秒循环之后 | 短诊断前移到 **sender 任务内**、任何长跑之前 |

### 3. 短诊断的栈归属（预检最终条）

短诊断最初插在 `WebRtcM1Run` 中、sender 任务创建之前，**在 main 的 8 KB 栈上编码**，
run11 实测重现了 Opus 栈溢出（ELF `85c5b00dc`：
`[diag] short send-path observation starting` → first-frame-before →
`A stack overflow in task main has been detected`）。
已移入**同一个 32768 字节 sender 任务**；不再从 main 调用编码/重采样。

## 未完成：设备侧发包观测未取到

本轮 5 次尝试中 4 次停在 **DTLS 握手超时**
（`stage=server answer received(4)`、`peer_state=6 CONNECTING`、
Mbed TLS `-0x6800 MBEDTLS_ERR_SSL_TIMEOUT`），
唯一连通的两次也都未跑到短诊断输出。

**因此 `srtp_protect` / `lwip_sendto` 的实际计数本轮没有取得。**
评审已指出该握手超时需用非敏感 datagram/handshake 元数据定位，
本轮**未能完成**这一项；也未预设根因、未升级 1.5.6。

## R2-1 ～ R2-3、R2-5 的代码处置

| # | 处置 |
|---|---|
| **R2-1** | 退出未确认时**保持停止请求有效**（不再撤销）；新增 `leaked` 标志，此时**连 Impl 本身也不释放**；析构检查 `leaked`，宁可泄漏也不用悬垂指针；重复 Stop 沿同一规则 |
| **R2-2** | `completed` 必须具备 `transcript` 与 `item_id`，否则计入 `malformed_events` 且**不释放任何 clip**；`failed` 与 timeout 一样**终止验收序列** |
| **R2-3** | 移除 StreamClip/StreamSilence 的逐段 `Arm()`，改为**单一连续 pacing 期限**，边界开销与漂移不再被重置掩盖；`LatencyLog` 改为跨调用的**跨步采样**，分位覆盖整个循环而非前 4096 帧 |
| **R2-5** | 新增**由真实条件计算**的 `FINAL VERDICT`：配置核验、3 条关键词全中、无 codec/超限错误、SRTP+UDP 实际发送成功、持续 loop 完整，全部满足才 PASS；并明确打印"完成的采集或通过的诊断不等于通过" |
| **R2-5 撤回项** | 评审已**撤回**前次关于 patch 上下文空行的空白要求，本轮不再改动该文件 |

## 当前状态

**M1 仍未通过。** 已排除：本地静音/无效 Opus（16351 包解码成功、RMS 0.20）、
SDP 不接受音频（Answer 明确 `a=sendrecv` opus/48000/2）、配置不成立（回显已核验）。
未完成：设备侧 SRTP/UDP 实际发包计数、VAD/ASR、60 ms 证据、隔离重建逐条记录。
