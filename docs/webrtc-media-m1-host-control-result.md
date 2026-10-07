# R4-1 / H1–H4：标准栈 host 媒体正对照 —— 最终结果

被复核 HEAD：`3b0844e0b6b3908995ad4a4023b1bf7b84b62f89`（Review 5448053831）。
本文件取代此前 R4-1 的结论小节；早先的"三次尝试 + speech_stopped 未触发"
作为**历史**保留在文末。

## 结论：标准栈三条全部通过

```
[play] zh_1 "今天我们测试语音连接" (195 packets, 3900 ms)
[vad] speech_started item=item_SSPJhK9ceXEhriMXEeHWY
[vad] speech_stopped item=item_SSPJhK9ceXEhriMXEeHWY
[clip] zh_1 vad=1/1 completed=1 kw=3/3 "今天我们测试语音连接。"
[play] zh_2 "桌上有一本蓝色的书" (183 packets, 3660 ms)
[clip] zh_2 vad=1/1 completed=1 kw=2/2 "桌上有一本蓝色的书。"
[play] zh_3 "请回答一加一等于几" (186 packets, 3720 ms)
[clip] zh_3 vad=1/1 completed=1 kw=2/2 "请回答一加一等于几。"
[media] accepted=924 refused=0 exceptions=0 bytes=121325 pkt=3..389 lateFrames=0
[time] mediaMs(18480) vs wallMs(18482) over the same run
VERDICT: PASS
```

三条的 `item_id` **互不相同**，且每条 **VAD 的 item 与 completed 的 item 一致**。
转写文本与冻结原文完全吻合（仅多句末句号）。

**已证**：同一 endpoint / workspace / model / `server_vad 0.5 / 800ms` /
`qwen3-asr-flash-realtime` 配置下，**标准 WebRTC 栈能对同源冻结 fixture
产生完整 VAD start+stop、完整 ASR 与关键词**。

## 根因：**未隔离**（H6-4 订正）

此前本节把"每段重置 pacing 期限"认定为 `speech_stopped` 缺失的根因。
**该认定没有单变量证据**：那一轮同时改变了回复通道、初始 seq/ts、尾静音长度等
多个因素，因此不能把结果归给任何单一改动。

**当前可支持的表述只有**：*本轮组合修复后完整对照通过，具体根因未隔离。*

已排除的负结果（各自试过、都不改变当时的失败）：
数字静音 vs 底噪、`config.timestamp` 与 RTP 头对齐。
**但这也不证明它们永远无关**——它们只是在当轮组合下未改变结果。

## H6-1～H6-4 处置

| # | 处置 |
|---|---|
| **H6-1** | 静音文件改为**从本次生成目录读取**（此前从 `__dirname` 读到旧文件，`run_pass.log` 里 mode/decoded/peak 全是 undefined，而归档 manifest 声称 60 包/1200 ms —— 证明的是另一个文件）。**校验** voice 容器包数与 manifest 一致、`decoded_samples_per_packet==960`、encoder CTL 返回码与回读码率；缺失或不符判 **INVALID**。结果里记录**本次实际消费**的 manifest/静音 hash 与每条 voice 容器 hash |
| **H6-2** | 等待不再按全局 ASR 数组长度释放，改为只由**本条 VAD/committed 公告的 item** 释放；`sameItemVad`、`ownedByClip` 从"告警"变成**判据**，进入终止条件与 PASS；旧/外来/重复/缺字段 completed 一律不释放当前条 |
| **H6-3** | 全部结果字段**在任何失败入口前初始化**（配置失败也可落盘）；改用 **`performance.now()` 单调时钟**贯穿 deadline/边界/wall；`r.window.endMs` 按 **48 ticks/ms 换算**；send 的 `bool`/exception **互斥计数**，非 true 不默认计入 accepted；**迟到帧使本轮无效**而非仅累计 |
| **H6-4** | 根因表述订正为"组合修复后通过，根因未隔离"（见上） |

### 离线检查（H6-2/H6-3 要求）

判定逻辑抽到 `tools/webrtc_probe/control_logic.js`，**runner 与测试共用同一实现**，
因此测试的就是实际判据：

```
$ node tools/webrtc_probe/test_control_logic.js
16 passed, 0 failed
```

覆盖：正常归属、旧/外来 item、重复、缺 `item_id`/`transcript`、
start/stop 属不同 item、关键词缺失、无 VAD stop、未归属 completed、
failed、拒发/异常/迟到使本轮无效、输入证据无效、条数不足。

## H1–H4 处置

| # | 处置 |
|---|---|
| **H1** | `sendMessageBinary` 的 **bool 返回值**现在被检查，分别统计 `accepted / refused / exceptions`；任一 refused 或 exception 使本轮判 `INVALID`。**单一单调绝对期限**贯穿全部 clip、静音与 ASR 等待；记录 `lateFrames`。媒体时长由**被接受的帧数**推导，墙钟同覆盖（`mediaMs 18480` vs `wallMs 18482`） |
| **H2** | 生成器 `encode_fixture.py` **自身冻结** `text` 与 `keywords` 并写入 manifest；runner 遇到缺失/空的 keywords **拒绝评分**（不再回退 `[]` 把 0/0 当全中）。VAD 记录实际 **item_id**；每条只取**本条的 VAD/ASR 窗口**；completed 的 item 若不在本条 VAD 公告的 item 中会告警；未达标即**终止序列** |
| **H3** | 事件回调接收**实际 channel 对象**并据其回复 `session.update`；记录真实 label（本次为 `txt`）；检查其返回值。**全部结果状态在函数开头初始化**，配置失败分支不再落入 temporal dead zone，失败也能落盘。结束时释放 track/channel/peer，**退出码与 verdict 一致**（PASS=0 / FAIL=2） |
| **H4** | 静音包改为**由生成器可重跑产生**并附**逐包本地解码样本数与峰值**证据（`mode=dither`、960 样本/帧、peak \|x\|=49）。**未知事件类型按 type 记录**——正是这一条暴露了此前被静默丢弃的 `conversation.item.input_audio_transcription.delta`、`input_audio_buffer.committed` 与整个 `response.*` 生命周期。结论订正为：host 通过**只**证明该标准栈/模型/配置能处理同源音频 |

## 保留的原始证据

`tools/webrtc_probe/host_control_evidence/`：

| 文件 | 内容 |
|---|---|
| `run_pass.json` | 本次通过运行的完整结构化结果 |
| `run_pass.log` | 同次运行的脱敏原始输出 |
| `encode_manifest.json` | 源 hash、pcm48 hash、包数/包长、尾部补齐、CTL 返回码与回读、逐包解码样本数 |
| `silence_packets.json` | 本次生成目录中**实际被消费**的静音文件（hash 记在 `run_pass.json`） |

`run_pass.json` 的 `inputEvidence` 记录本次运行**实际消费**的 manifest/静音 hash
与每条 voice 容器 hash，使结果自带输入证据。

## 重跑命令

```bash
python3 tools/webrtc_probe/encode_fixture.py /tmp/host_media
NDC_PATH=/tmp/wrtc-test/node_modules/node-datachannel \
  node tools/webrtc_probe/standard_media_control.js /tmp/host_media /tmp/host_media/result.json
```

依赖实测：node-datachannel **0.33.4**（libdatachannel **0.24.5**）、libopus **1.5.2**。
凭据仅从 gitignore 的 `firmware/sdkconfig` 读取；输出不含 key/Authorization/完整 SDP/ICE 密码。

## 边界

host 通过**不证明**设备发出相同字节，**不证明**设备媒体、AEC、全双工或产品移植通过。
**M1 仍未通过**，本实验不代替 M1 验收。设备侧 SRTP/UDP 计数、DTLS 元数据、
item_id 绑定、wall/loop 范围、sender 信号量、60 ms、回归与隔离重建**仍未完成**。

---

## 历史（已被上文取代）

早先三次运行停在 `speech_started` 有、`speech_stopped` 无，当时结论曾写为
"服务端/模型/配置无法处理该音频已排除，问题收窄到设备侧"——**该表述过强**，
当时证据只支持"某些输入到达并触发 VAD 起始"。现已按 H4 订正。

---

# D1-1 / D1-2：设备短诊断（实际执行）

被复核 HEAD：`7317037`（Review 5448296466）。本次为**设备侧**短诊断，非 host。

## D1-1：DTLS 观测（成功路径）

`dtls_short_probe.{h,cc}` 新增，独立 generation 在 `Transport.Start()` **之前** Arm，
成功与失败路径都输出。包装 `mbedtls_ssl_handshake`、`lwip_sendto`、`lwip_recvfrom`
（四者均为 S3 目标文件中的未解析外部符号，`nm build/stack-chan.elf` 确认四个
`__wrap_*` 各 1 个定义）。

本次实际输出：

```
[cоnnected] gen=1 | handshake calls=4 ok=2 fail=2 last_ret=0 last=13ms total=8250ms
            complete=1
            DTLS TX records=14 unparsable=4 epoch_changes=1 first_type=22 last_type=23
            epoch=1 seq_hi=0 len=52
            DTLS RX records=27 unparsable=16 epoch_changes=1 first_type=22 last_type=23
            epoch=1 seq_hi=0 len=552
            cipher=TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384
            sendto=18 recvfrom=43
```

**可支持的结论**：本次握手**双向都有 DTLS record 流动**（14 TX / 27 RX），
`handshake` 被调用 4 次（2 次成功、2 次失败重试，累计 8250 ms），最终完成并协商出 cipher。
这**不支持**"UDP 包根本没出去"或"对端无响应"的假设。

**只记录非敏感元数据**：13 字节 classic record 头（type/version/epoch/48 位序号高 16 位/
长度）、返回码、errno、cipher **名称**。
**未记录**任何负载、密钥、证书、MKI、IP 或 SDP 凭据。

**未实现 / 不可用**：本机 Mbed TLS 构建**没有**公开的 endpoint role 或
selected SRTP profile getter（`chosen_dtls_srtp_profile` 是 `MBEDTLS_PRIVATE`），
按"不猜内存布局"的要求**未读取**，报告中记为 `role=n/a profile=n/a`。
BIO 回调计数（区分"进入 Mbed"与"仅到达 socket"）**本轮未实现**。

## D1-2：RTP 实际发送观测（短诊断，100 帧 / 2 秒）

```
gen=2 | srtp_protect calls=100 ok=100 fail=0 unparsable=0
        udp attempts=100 packets_written=100 write_incomplete=0 write_failed=0
        unmatched=9 last_rc=0 last_errno=0
        retired=100 retired_pending=0 overflow_pending=0
        protected_unwritten=0 length_mismatch=0 wrote_after_protect_fail=0
        first: pt=111 seq=0 ts=0 ssrc=0x00000006 rtp_len=15 hdr_len=12
               srtp_cap=1428 srtp_len=25
        last: seq=99 ts=95040 | ssrc_changes=0 pt_changes=0
100 packets fully written in 100 UDP attempts
```

**本阶段发送路径在本地是完整成立的**：100/100 保护成功、100/100 UDP 整长写入，
无短写、无负返回、无长度不符、无"保护成功但未写出"、无观察丢失
（新字段已实际实现并输出，不再是恒零）。

## 关键线索：SSRC = 0x00000006

`ssrc=0x00000006` 是一个异常小的值。host 正对照中 SSRC 是**会话内随机非零 32 位**
并通过 `audio.addSSRC()` 在 SDP 中宣告；设备侧该值可疑，需在下一轮核对
esp_peer 是否在 SDP 中宣告了一致的 SSRC。

**同时注意**：即便本地发送 100/100 成功，设备 `vad=0/0`、
`completed=0`；而 host 标准栈对**同源 fixture** 三条全部通过。
两者结合把问题进一步收窄到**设备侧 RTP/SDP 的媒体参数**（SSRC 是当前首要线索），
而非本地编码、SRTP 或 socket 写入。

**这不构成根因结论**——SSRC 只是待验证的首要假设，本轮未做单变量验证。

---

# D2-1～D2-4：设备短诊断（第二次，含真实 profile）

被复核 HEAD：`a3a7b9f`（Review 5448554040）。

## 先更正一个事实错误

上一轮我（及 findings B20）宣称"本机没有公开的 role/profile 接口"——**这是错的**。
本机 Mbed TLS 3.6.5 中四个公开接口**都存在**：

| 接口 | 位置 |
|---|---|
| `mbedtls_ssl_conf_get_endpoint` | `ssl.h:2124`（inline，直接调用） |
| `mbedtls_ssl_context_get_config` | `ssl.h:2298`（inline） |
| `mbedtls_ssl_get_dtls_srtp_negotiation_result` | `ssl.h:4383`（公开 void，填充 `mbedtls_dtls_srtp_info`） |
| `mbedtls_ssl_is_handshake_over` | `ssl.h:5114`（inline） |

读取该**公开返回类型**的 `chosen_dtls_srtp_profile` 字段是使用公开契约，
**不是**猜内存偏移。已按此实现，B20 已订正。

## D2-1 实现与实测

- DTLS/STREAM **按 setup 身份分流**（`config_defaults`/`conf_transport` 固定表，
  `conf_free` 清身份，表溢出显式计数），只把 DATAGRAM context 计入 DTLS
- 独立**单调 generation**（不再每次回到 1）
- 每 context 记录 role / ret / 单调耗时 / `is_handshake_over`
- `errno` 在真实调用后**立即**保存，再读 timer/getter
- 摘要含 transport poll 进出耗时字段（本轮未接线，见"未完成"）

实测（`run_d2_profile_unset.log`）：

```
gen=1 | setups=2 (datagram=1 stream=1 overflow=0)
hs calls=7 dtls=1 ok=1 fail=0 stream_excluded=6 unknown_transport=0
last_ret=0 last=288ms over=1 role=0 | cipher=TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256
profile=0 UNSET
DTLS TX records=31 in_dgrams=31 unparsable=3 truncated=0 short_write=0 failed=0
     epoch_changes=11 first_type=22 last_type=22 epoch=1 seq=0:5 len=48
DTLS RX records=35 unparsable=31 truncated=0 epoch_changes=0
     first_type=22 last_type=20 epoch=0 seq=0:29 len=1
socket assoc=unknown (dtls_fd=-1 rtp_fd=-1)
```

**四项重要结论：**

1. **分流生效**：上一轮把 7 次握手全算作 DTLS 成功；实际只有 **1** 次是 DTLS，
   6 次是 HTTPS 的 STREAM 握手。
2. **DTLS 握手本身成功了**：`ok=1 fail=0 last_ret=0 over=1 role=0`（client）。
   这**推翻了**"DTLS 握手超时"作为本阶段失败的解释——至少这一次它完成了。
3. **🚨 `profile=0 UNSET`**：**SRTP 保护 profile 未被协商**。
   这正是 Review 在 D2-4 中预判的"协商 profile 非 1"条件分支；
   本次观测到的是比 1 更严重的 **UNSET**。
4. **DTLS 之后仍失败**：握手完成但最终仍是
   `stage=server answer received(4)`、`peer_state=6 CONNECTING`，
   所以失败点在 **DTLS 完成之后**（SRTP/DataChannel 建立阶段），
   与 `profile=0 UNSET` 一致。

## D2-2 解析器

- 校验 record 层版本（DTLS 1.0 `FEFD` / 1.2 `FEFF`）与 `declared_len <= 剩余-13`
- **有界遍历 datagram 内全部 record**（不再只取首条）
- 保存 `seq` 的 **high16 + low32**、epoch/type/len
- TX **仅在真实调用整长写出时**计 record；短写/失败分别计数
- RX 受 `min(rc, len)` 约束
- 区分 unparsable / truncated / short_write / failed
- **无 socket/context 关联时不作"对端从未回应"的结论**（本次 `assoc=unknown`，
  故摘要只给数字不下结论）

## D2-3 RTP 探针状态

- `protect_ok` 改用记录自身的标志，不再从可变 `state` 推导
  （短写/负返回曾把同一条成功保护的记录标成 protect-failed，
  导致后续整长重试丢掉了保护成功的事实）
- 写失败**不抹掉**保护结果，记录仍可被后续整长重试匹配
- 首次整长写计 `packets_written`，重试/重复不重计
- `Acquire` **优先释放已完成记录**；只能丢掉活动记录时按覆盖缺失计数，不记为 protect 失败
- `Snapshot` **改为非破坏读取**；窗口结算拆成显式 `SettleWindow()`，
  应在 sender 停止并 join 之后调用

## D2-4 未完成

**本轮未取得语音区间（zh_1）的单独 protect/UDP 输出** —— 设备在 DTLS 完成、
配置门控之前就失败了（`session_created=0`），因此**没有进入媒体发送阶段**。
`ssrc=0x00000006` 的说明已按 Review 订正：该值**已在 Offer 中宣告**
（`a=ssrc:6`），小值本身不是非法条件，不构成根因。

**未完成**：socket 关联实测（`NoteDtlsSocket`/`NoteRtpSocket` 已实现但未接线）、
transport poll 耗时接线、BIO 观测、语音区间单独统计。

## 本轮不下的结论

`profile=0 UNSET` 是**实测到的条件**，与"DTLS 后失败"一致，
但本轮**未做单变量验证**，**不宣称它是根因**，也**未盲改 cipher/profile/版本**。
