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
