# 第二轮复核请求：锁定 esp_peer 1.5.5 的取舍是否成立

第一轮复核（[webrtc-m0-review-result.md](webrtc-m0-review-result.md)）判定
"esp_peer v1.5.6 存在可解释 NAT 握手失败的 ICE 缺陷"成立，
并指出"尚未证明是 v1.5.6 首次引入的回归，需同设备 A/B"。

**该 A/B 已完成，结论与第一轮互补**：v1.5.6 失败、v1.5.5 成功。
真机端到端跑通，见 [webrtc-m0-review-result.md](webrtc-m0-review-result.md) 第 10 节。

本轮**不再复核"是否互通"**——那是真机直接观测，不需要再证。
本轮只复核**由该结论推出的三个决定/推断**，这三处我自认证据较弱。

---

## 0. 环境与位置

| 项 | 值 |
|---|---|
| worktree | `/Users/amtf/Documents/Git/StackChan` |
| 分支 | `feat/aliyun-omni-v2v` |
| 复核时 HEAD | `60796e06b3389c59c93842f24ece454a974eef89` |
| 硬件 | M5Stack StackChan（ESP32-S3，8MB PSRAM） |
| ESP-IDF | v5.5.4 |
| 组件 | `espressif/esp_peer`，**现已精确锁定 `1.5.5`** |
| 改动文件 | `firmware/main/idf_component.yml`、`firmware/main/hal/webrtc/webrtc_m0.cc` |
| 构建 | `source ~/esp/esp-idf-5.5/export.sh && cd firmware && idf.py build` |

---

## 1. A/B 实测结果（本轮的事实基础）

变量控制：同一台设备、同一网络、同一端点、同一份 M0 代码。
唯一差异：`espressif/esp_peer` 版本。

| 版本 | 组件 commit | libpeer_default.a SHA-256（前 16） | 结果 |
|---|---|---|---|
| 1.5.6 | `38a697f3b8142d23` | `6b2f3856b9a1639b`（全文 `6b2f3856b9a1639b0480c011688c070c132b6399e9863a74d20d20a271dc7da3`） | `CONNECT_FAILED` |
| **1.5.5** | `c8650846b512e6e1` | `25a338fc6b05f702` | **全链路成功** |

### v1.5.6 失败（节选）

```
I AGENT: 0 0 Send binding request (cand:0) local0:192.168.1.7:59813 remote0:39.105.73.166:3478 id:5851f42d40b18ccf4bb5f646
W AGENT: XOR-MAPPED 124.126.137.141:13174 is not local candidate, skip nominate
W AGENT: XOR-MAPPED 124.126.137.141:13174 is not local candidate, skip nominate
   ...（约每 400ms 重发，每次 3 行，持续约 10 秒）...
I WebRTC-M0: [state] CONNECT_FAILED (8)
```

### v1.5.5 成功（节选）

```
I AGENT: 0 0 Send binding request (cand:0) local0:192.168.1.7:49914 remote0:39.105.73.166:3478 id:5851f42d40b18ccf4bb5f646
I AGENT: 0 0 LocalBinding resp 5851f42d40b18ccf4bb5f646 local1
I AGENT: 0 Select pair 39.105.73.166:3478        ← 无任何 skip nominate
I AGENT: 0 Candidate responded
I AGENT: 0 Connection OK 39.105.73.166:3478
I WebRTC-M0: [state] PAIRED (5)
I WebRTC-M0: [state] CONNECTING (6)
I WebRTC-M0: [state] CONNECTED (7)
I WebRTC-M0: [state] DATA_CHANNEL_CONNECTED (9)
I WebRTC-M0: [channel open] label='txt' stream_id=1
I WebRTC-M0: [state] DATA_CHANNEL_OPENED (10)
I WebRTC-M0: [data stream=1] {"type":"session.created",...}
W WebRTC-M0: >>> session.created received
I WebRTC-M0: sent session.update on stream 1 -> 0
I WebRTC-M0: [data stream=1] {"type":"session.updated",...}
W WebRTC-M0: >>> session.updated received
```

---

## 2. 待复核问题一：锁定 1.5.5 的取舍是否正确

### 我的分析（请判断是否成立）

锁定 1.5.5 意味着放弃 v1.5.6 的全部修复。我逐条评估了影响：

v1.5.6 变更日志（[来源](https://components.espressif.com/components/espressif/esp_peer/versions/1.5.6/changelog)）共 6 项：

| # | v1.5.6 修复项 | 我的影响评估 | 依据 |
|---|---|---|---|
| 1 | Fixed connection fail due to **TCP and UDP use same IP and port** (map candidate wrongly) | 🟢 低 | 阿里的 Answer 只提供 UDP 候选，未出现 TCP 候选 |
| 2 | Fixed **handshake HELLO request is dropped if received before connected** (add cache logic) | 🟡 **中** | **这是一个竞态修复。回退后该竞态重新存在。M0 只握手一次未暴露；长连接语音可能偶发失败** |
| 3 | Fixed connection fails due to **ICE lite attribute** set in SDP and no manual set ICE lite mode | 🟢 低 | 阿里的 Answer 不含 `a=ice-lite` |
| 4 | Fixed **ICE nominate wrongly if responded XOR mapped address not match local sent candidate** even transaction ID matched | 🔴 — | **这正是导致我们失败的那条，也是回退的理由** |
| 5 | Refined comment for SDP and candidate message string structure | 🟢 无 | 注释整理 |
| 6 | Fixed **H264 profile** use `4d001f` not match actual device capability | 🟢 无 | 不使用视频 |

**我的结论：6 项里只有第 2 项（HELLO 缓存）构成真实风险，其余对我们无影响。**

### 请复核

1. **上述逐条评估是否成立？** 尤其第 2 项"HELLO 请求"具体指什么
   （SCTP INIT？DTLS？自研握手？），在"先 SDP 交换、后 DTLS、再长时间双向语音"
   的时序下，缺这个缓存会造成什么可观测的后果？
2. **是否存在第三条路？**
   - 有无办法只绕过 v1.5.6 的提名检查而保留其余 5 项修复？
     （该函数在预编译 `libpeer_default.a` 内，但整体可替换 impl / 换库）
   - 是否应降到比 1.5.5 更早的版本（例如 1.5.2/1.5.1 也有 ICE 相关修复）？
     还是 1.5.5 作为"最近一个不含该回归的版本"本就正确？
   - 有无社区已知的 v1.5.6 workaround（配置层面）？
3. **风险对比**：一边是"已知会导致 ICE 必然失败的提名缺陷"（我们已绕开），
   一边是"可能偶发失败的握手竞态"（我们已引入）。**哪个更该担心？**
   这个取舍该怎么向使用者说明？

---

## 3. 待复核问题二：回归的触发条件是普遍的，还是阿里特有的

### 我的推断（请判断是否成立，我认为这里最可能出错）

第一轮复核发现的两处分支中，第一处是
`agent_pair_candidate` 要求 `local->type == remote->type` 才成对。

而阿里接入节点的 Answer 是：

```
a=candidate:0 1 UDP 2130706431 39.105.73.166 3478 typ host
```

**一个公网地址被标为 `typ host`**，而不是通常的 `typ relay`。
阿里对同一端点每次返回的 IP 不同（观测到 `182.92.183.85`、
`39.105.176.241`、`39.107.142.202`、`39.105.73.166`），像是动态分配的接入节点。

### 我的推断

**类型不匹配的过滤只在"对端用 `typ host` 暴露公网地址"时才会触发。**
若对端是常规部署（中继标 `typ relay`，或对端确实在局域网内 host 可达），
v1.5.6 可能工作正常。

### 请复核

1. 这个推断是否成立？**"v1.5.6 有 ICE 缺陷"这个说法是否应该加上
   "当对端以 `typ host` 提供公网候选时"的限定？**
2. 阿里把中继标成 `typ host` 是否符合规范？
   若不合规，会如何影响我们向 Espressif 提 issue 的说服力——
   对方会不会回应"这是对端 SDP 不规范，不是我们的问题"？
3. **如何描述才能让 issue 既准确又容易被接受？**
   是否应强调 RFC 8445 §6.1.2.2 本就不要求类型相同，
   因此无论对端怎么标，按类型过滤候选对都是错的？

---

## 4. 待复核问题三：我的 A/B 混淆变量排除是否成立

### 我的坦白

我最初写 A/B 时**没有意识到**：两次测试之间我不只改了组件版本，
**还改了 M0 代码**（修第一轮复核指出的两个缺陷）。严格说这不算单一变量。

### 我的排除论证（请复核）

两轮测试间 `webrtc_m0.cc` 的全部改动如下（`git diff 2f5df7b 4ba415f`）：

- 新增 `on_channel_open` 回调，记录 label/stream_id
- `session.update` 改为在收到 `session.created` 的 stream 上发送（原为固定 stream 0）
- 判据由 `peer_state >= CONNECTED` 改为显式区分 `CONNECT_FAILED`
- 日志格式

**关键论证：**
1. 上述改动全部作用于 **DataChannel 阶段**，而失败发生在 **ICE 阶段**，
   时序上早于任何被改代码执行。
2. ICE 相关配置在两次测试间**零改动**。以
   `git diff 2f5df7b 4ba415f -- firmware/main/hal/webrtc/webrtc_m0.cc` 过滤
   `cfg.` 前缀，结果只含 `+ cfg.on_channel_open = on_channel_open;`
   （注册回调），以及被移动的 `frame.type` 赋值行。
   以下配置行**均未出现在 diff 中**：

```
cfg.server_lists / cfg.server_num       （STUN 服务器）
cfg.role                                （CONTROLLING）
cfg.ice_trans_policy                    （ALL）
cfg.audio_info.{codec,sample_rate,channel}
cfg.audio_dir / cfg.enable_data_channel / cfg.manual_ch_create
```

3. 两轮测试的失败/成功模式属于**同一函数的不同分支**
   （`skip nominate` vs `Select pair`），不是"有时通有时不通"的抖动。

### 请复核

1. 这个排除是否成立？**注册 `on_channel_open` 回调本身有没有可能影响 ICE/DTLS 行为？**
2. 若要严格重做单一变量 A/B（保持 M0 代码完全一致，仅切版本），
   是否值得？会带来什么此前未覆盖的信息？

---

## 5. 已被第一轮复核纠正、本轮不再主张的部分

为免重复讨论，以下我在第一轮的过度推断**已被纠正且我不再主张**：

- ~~"同一字符串即可排除配置问题"~~ —— 过强
- ~~`raddr 0.0.0.0 rport 0` 是缺陷~~ —— RFC 9429 §7.3 的 relay 隐私示例即如此，
  且 RFC 8445 附录 B.3 说明该字段不参与 ICE 算法，失败分支也不读取
- ~~"预编译所以绝对无法自行修补"~~ —— 过宽

---

## 6. 仍未验证的部分

M0 **不发送任何媒体**。因此以下仍未证明，本轮也不主张：

- 服务端 AEC 是否实际生效
- 双向语音是否可用
- 语音打断是否可用
- 48kHz Opus 编解码、重采样、RTP 收发时序在 ESP32-S3 上的实际表现与内存占用

---

## 7. 复现方式

- M0 代码：`firmware/main/hal/webrtc/webrtc_m0.cc`
  开关 `CONFIG_STACKCHAN_WEBRTC_M0=y`（`sdkconfig.defaults.local`）
- 切版本：修改 `firmware/main/idf_component.yml` 中
  `espressif/esp_peer.version` 后 `rm -f dependencies.lock && idf.py build`
- 对照脚本（Mac，标准 WebRTC 栈）：`/tmp/wrtc-test/test.js`
- 凭据：macOS Keychain，`blue-keychain-save` skill 的
  `keychain.sh get --service blue-stackchan-bailian-key`；
  workspaceId 为 `ws-ik1clk14dycbzaqx`

**请勿在输出中回显 API Key。**

---

## 8. 期望的输出

1. 问题一：锁定 1.5.5 的取舍判定（成立 / 不成立 / 证据不足），
   以及是否推荐第三条路
2. 问题二：回归触发条件的判定，以及给 Espressif 的 issue 该如何措辞
3. 问题三：混淆变量排除是否成立；是否值得严格重做 A/B
4. 任何我在本轮引入的**新**错误或过度推断
