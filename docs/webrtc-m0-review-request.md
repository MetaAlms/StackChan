# 复核请求：ESP32 上 esp_peer 与阿里云 WebRTC 端点握手失败，是否为 esp_peer 缺陷

> **后续**：本请求的结论见 [webrtc-m0-review-result.md](webrtc-m0-review-result.md)；
> 由该结论推出的取舍另有[第二轮复核请求](webrtc-m0-review-request-2.md)。

请独立复核下面这个结论。**不要默认我判断正确**——请重点找反例、找我自己配置错误的可能。

---

## 0. 环境与位置

| 项 | 值 |
|---|---|
| worktree | `/Users/amtf/Documents/Git/StackChan` |
| 分支 | `feat/aliyun-omni-v2v` |
| 当前提交 | `d78f319fbef9405d9966c6f5f36a521d2ac8de1b` |
| 硬件 | M5Stack StackChan（ESP32-S3，8MB PSRAM，16MB Flash） |
| ESP-IDF | v5.5.4（`~/esp/esp-idf-5.5`） |
| 相关组件 | `espressif/esp_peer` v1.5.6，commit `38a697f3b8142d23823eda5209edb89ca76119dc` |
| 关键源文件 | `firmware/main/hal/webrtc/webrtc_m0.{h,cc}`、`firmware/main/main.cpp` |
| 构建 | `source ~/esp/esp-idf-5.5/export.sh && cd firmware && idf.py build` |

`managed_components/` 与 `xiaozhi-esp32/` 是 `firmware/fetch_repos.py` 的产物，
不在 git 里，需要先跑一次才能完整构建。

---

## 1. 背景

目标：让 StackChan 直连阿里云百炼的 WebRTC 端点（不经中间服务器），
以获得服务端内置的回声消除（WebSocket 协议官方明确不做 AEC，导致无法语音打断）。

阿里官方文档称：
> WebRTC 使用媒体轨道传输音视频，使用 DataChannel 传输模型事件和文本。
> **其他端可使用支持标准 WebRTC 的库。百炼不提供专用 WebRTC SDK。**

信令流程（非 trickle）：
```
POST https://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/api/v1/webrtc/realtime?model=...
  Content-Type: application/sdp
  Authorization: Bearer <API_KEY>
  body = Offer SDP   →   200 + Answer SDP
```

为此写了 M0 最小验证程序（`firmware/main/hal/webrtc/webrtc_m0.cc`）：
只做 SDP 交换 + ICE/DTLS/SCTP + DataChannel，**不发任何媒体**，
判据是能否收到 `session.created` 与 `session.updated`。

---

## 2. 待复核的结论

**主张：**
> esp_peer v1.5.6 的 ICE agent 在 NAT 场景下拒绝提名候选对，
> 导致 ICE 永不完成（CONNECT_FAILED）。这是 esp_peer 的缺陷，
> 且因其 ICE agent 为预编译静态库而无法自行修补。

**支撑证据（见第 3、4 节）：**
1. esp_peer 能生成合法 Offer，阿里返回 HTTP 200 + 合法 Answer
2. esp_peer 能产出 srflx 候选，且服务端确实回应了连通性检查
3. 但 esp_peer 拒绝提名，理由是该地址"不是本地候选"
4. **同一网络、同一端点，用标准 WebRTC 栈（node-datachannel）40ms 完成 ICE，并收到 session.created / session.updated**

---

## 3. esp_peer 侧的实际日志（ESP32-S3 串口）

```
I WebRTC-M0: pre-generating DTLS certificate...
I WebRTC-M0: cert -> 0
I WebRTC-M0: esp_peer_open -> 0
I WebRTC-M0: [state] NEW_CONNECTION (2)
I WebRTC-M0: esp_peer_new_connection -> 0
I WebRTC-M0: [state] CANDIDATE_GATHERING (3)

I AGENT: Start agent as Controlling
I AGENT: Send STUN binding request          ← 向 stun.miwifi.com 收集 srflx
I AGENT: 0 Get candidate success user: psw:
I WebRTC-M0: [msg] local SDP, 1117 bytes
W WebRTC-M0: [msg] offer carries a=candidate: YES (33 lines)

----- OFFER SDP（节选）-----
v=0
a=candidate:0 1 UDP 2129241599 192.168.1.7     59813 typ host
a=candidate:1 1 UDP 1681094399 124.126.137.141 13174 typ srflx raddr 0.0.0.0 rport 0
---------------------------

I WebRTC-M0: POST https://ws-ik1clk14dycbzaqx.cn-beijing.maas.aliyuncs.com/api/v1/webrtc/realtime?model=qwen3.8-omni-flash-realtime
I WebRTC-M0: SDP exchange: err=ESP_OK http=200
I WebRTC-M0: response body is 1254 bytes

----- ANSWER SDP（节选）-----
v=0
o=- 2000 2 IN IP4 39.105.73.166
a=group:BUNDLE 0 1
a=ice-ufrag:<server-ufrag>
a=ice-pwd:<server-pwd>
a=fingerprint:sha-256 6E:17:...
a=setup:passive
a=candidate:0 1 UDP 2130706431 39.105.73.166 3478 typ host
m=audio 3478 UDP/TLS/RTP/SAVPF 111
a=rtpmap:111 opus/48000/2
a=sendrecv
m=application 3478 UDP/DTLS/SCTP webrtc-datachannel
a=sctp-port:5000
---------------------------

I AGENT: 0 Add remote type:1 39.105.73.166:3478
I WebRTC-M0: [state] PAIRING (4)
I WebRTC-M0: fed answer to peer -> 0

I AGENT: 0 0 Send binding request (cand:0)
         local0:192.168.1.7:59813 remote0:39.105.73.166:3478 id:5851f42d40b18ccf4bb5f646
W AGENT: XOR-MAPPED 124.126.137.141:13174 is not local candidate, skip nominate
W AGENT: XOR-MAPPED 124.126.137.141:13174 is not local candidate, skip nominate
W AGENT: XOR-MAPPED 124.126.137.141:13174 is not local candidate, skip nominate
   ...（约每 400ms 重发一次，每次刷 3 行，持续约 10 秒）...

I WebRTC-M0: [state] CONNECT_FAILED (8)

==================== M0 VERDICT ====================
  last peer state   : CONNECT_FAILED (8)
  session.created   : no
  session.updated   : no
===================================================
```

**注意点（我的解读，请验证）：**
- `local0:192.168.1.7:59813` 是发出请求所用的**内网候选**
- `124.126.137.141:13174` 是该 socket 经 NAT 后的**公网映射**，
  也正是 esp_peer 自己刚在 SDP 里广播的 srflx 候选
- 两者指向同一条路径，但 esp_peer 判定"不是本地候选"而放弃提名

---

## 4. 对照实验：标准 WebRTC 栈在同一网络成功

工具：`node-datachannel` v0.33.4（Node v24），脚本在 `/tmp/wrtc-test/test.js`
（`iceServers: []`，与阿里文档的浏览器示例一致；offer 由库生成，
候选地址由 `onLocalCandidate` 注入 SDP 后一次性 POST，即非 trickle）。

实际输出：
```
[     6ms] [candidate] 0 a=candidate:1 1 UDP 2116026367 240e:...:3529 60618 typ host
[     6ms] [candidate] 0 a=candidate:2 1 UDP 2114977535 192.168.1.6    60618 typ host
[     6ms] [candidate] 0 a=candidate:3 1 UDP 2114977279 198.18.0.1     60618 typ host

[   713ms] HTTP 200, answer 1274 bytes
[   714ms] remote candidates (2):
      a=candidate:0 1 UDP 2130706431 39.107.142.202 3478 typ host
[   714ms] remote setup=passive
[   715ms] answer applied
[   746ms] [ice] connected            ← 31 毫秒
[   758ms] [ice] completed
[  2789ms] [pc] connected
[  2789ms] [server-opened channel] txt
[  2800ms] <<<[txt] {"type":"session.created",...}
[  2864ms] >>> sent session.update on 'txt'
[  2946ms] <<<[txt] {"type":"session.updated",...}

  session.created : YES
  session.updated : YES
```

**这条实验的意义：同一台 Mac、同一网络出口（公网 IP 均为 124.126.137.141）、
同一端点，标准栈毫秒级完成 ICE。** 所以问题不在这条网络路径、不在端点、不在账号。

---

## 5. 我已排除的可能性

| 假设 | 排除依据 |
|---|---|
| 端点不可达 | 标准栈 31ms 完成 ICE |
| Offer 缺 `m=audio` 段 | 阿里对缺失者返回 HTTP 400 `remote_sdp_failed`；我们的 Offer 含该段且返回 200 |
| 音频段缺编解码器 | node 测试初期因未注册 Opus 收到 HTTP 400，注册 PT 111 后即 200；esp_peer 的 Answer 本身含 `a=rtpmap:111 opus/48000/2` |
| `esp_peer_main_loop` 用法错误 | 已改为官方 peer_demo 的 `while + 20ms` 写法，ICE 收集随即正常产出 |
| HTTP 响应读取错误 | 已改为在 `HTTP_EVENT_ON_DATA` 回调收集；现在能拿到 1254 字节完整 Answer |
| 未配 STUN | 已配 `stun:stun.miwifi.com:3478`，确认收集到 srflx 候选 |
| Kconfig/内存不足 | DTLS 证书生成成功、`esp_peer_open` 返回 0、状态机能推进到 PAIRING |

---

## 6. 我发现的官方侧面印证（也请核对是否被我误读）

`esp_peer` v1.5.6 变更日志（https://components.espressif.com/components/espressif/esp_peer/versions/1.5.6/changelog）写有：

> **Fixed ICE nominate wrongly if responded XOR mapped address not match local sent candidate even transaction ID matched**

我的理解：v1.5.6 把提名条件改严，要求应答中的 `XOR-MAPPED` 与**发出请求所用的那个本地候选**一致。
在 NAT 场景下发出的是内网候选（`192.168.1.7:59813`），
而 `XOR-MAPPED` 是公网映射（`124.126.137.141:13174`），二者必然不等，于是永不提名。

**请判断这个解读是否成立**，或它其实是修复了另一个（相反方向的）问题、
与我们的现象只是措辞相似。

---

## 7. 请重点复核的问题

1. **上述日志能否支持"esp_peer 侧缺陷"的结论？**
   有没有可能是我在 M0 里漏配了什么导致的？
   （例如 `ice_trans_policy`、`ice_use_lite_mode`、候选数量上限、
   `esp_peer_update_ice_info` 的调用时机、IPv6 处理等）

2. **`XOR-MAPPED ... is not local candidate, skip nominate` 的正确触发条件是什么？**
   若能取得 esp_peer / libpeer 的 ICE agent 源码
   （esp_peer 衍生自 https://github.com/sepfy/libpeer，但 esp_peer 以预编译 .a 分发），
   请给出该分支的确切判断条件，并判断它是否违反 RFC 8445 关于
   peer-reflexive candidate 的要求。

3. **是否存在我没想到的配置绕过方式？**
   例如：不使用 host 候选、强制 relay、调整 ICE 角色、
   在 answer 之后补送候选、或让 srflx 成为发送所用的候选等。

4. **`typ srflx raddr 0.0.0.0 rport 0` 是否为异常输出？**
   标准 srflx 候选应带真实基地址（如 `raddr 192.168.1.7 rport 59813`）。
   这个 0.0.0.0 是否可能与提名失败相关？

5. **如果确为 esp_peer 缺陷，除了等官方修复，直接用 libpeer 源码集成是否可行？**
   请评估：libpeer 是否支持 48kHz Opus、SCTP DataChannel、
   服务端 VAD 所需的双向媒体；以及在 ESP32-S3（8MB PSRAM）上的可行性。

6. **是否存在第三方替代实现**（如其他 ESP32 可用的 WebRTC 栈）值得评估？

---

## 8. 需要的话可以自己复现

- M0 代码：`firmware/main/hal/webrtc/webrtc_m0.cc`
  开关 `CONFIG_STACKCHAN_WEBRTC_M0=y`（在 `sdkconfig.defaults.local`），
  开启后设备直接进入探测模式，串口打印完整 Offer/Answer SDP 与 AGENT 日志
- 对照脚本：`/tmp/wrtc-test/test.js`（`node test.js <apiKey> <workspaceId> <model>`）
- 凭据：本机 macOS Keychain，`blue-keychain-save` skill 的
  `keychain.sh get --service blue-stackchan-bailian-key`；
  `workspaceId` 为 `ws-ik1clk14dycbzaqx`

**请不要在输出中回显 API Key。**

---

## 9. 期望的输出

请给出：
1. 对第 2 节结论的判定：**成立 / 不成立 / 证据不足**，并说明理由
2. 若认为是我方问题，指出具体是哪一项配置或调用方式
3. 若认为确为 esp_peer 缺陷，指出出问题的代码路径与 RFC 依据
4. 任何值得尝试的绕过方案（按成本从低到高）
