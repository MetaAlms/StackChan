# WebRTC M0 独立复核结果

复核日期：2026-10-07。对应请求：[webrtc-m0-review-request.md](webrtc-m0-review-request.md)。

## 1. 判定与边界

**成立：本次使用的 esp_peer v1.5.6 / ESP32-S3 静态库确有 ICE 实现缺陷，
其具体分支能够解释已记录的 NAT 下 PAIRING → CONNECT_FAILED。**

依据是实际发布二进制的反汇编及 DWARF 类型/行号信息，另有独立复核交叉校验；
不再仅凭警告文案、changelog 或 Mac 对照实验推测。

结论限定于本次 controlling、非 relay、XOR-MAPPED 与 host 地址不同的处理路径。
尚未验证修复后的设备连接，也未进行 v1.5.5/v1.5.6 同设备 A/B，因此不能断言
“所有 NAT 都失败”或“该问题首次由 v1.5.6 引入”。

本轮未刷机、未修改固件、未读取 API Key、未重新调用阿里端点。

## 2. 证据身份

| 项目 | 实际核验值 |
|---|---|
| 仓库分支 | `feat/aliyun-omni-v2v` |
| 复核时 HEAD | `2f5df7bee1b0869550d6352163b40917259681e9` |
| M0 代码提交 | `d78f319fbef9405d9966c6f5f36a521d2ac8de1b` |
| 复核请求文档提交 | `2f5df7bee1b0869550d6352163b40917259681e9` |
| 组件 | `espressif/esp_peer` v1.5.6（`firmware/dependencies.lock`） |
| 组件源码坐标 | `38a697f3b8142d23823eda5209edb89ca76119dc`（组件 `idf_component.yml`） |
| 检查的库 | `firmware/managed_components/espressif__esp_peer/libs/esp32s3/libpeer_default.a` |
| 库 SHA-256 | `6b2f3856b9a1639b0480c011688c070c132b6399e9863a74d20d20a271dc7da3` |
| 库字节数 | `1620292` |

本机库与上述提交中的[官方库文件](https://raw.githubusercontent.com/espressif/esp-webrtc-solution/38a697f3b8142d23823eda5209edb89ca76119dc/components/esp_peer/libs/esp32s3/libpeer_default.a)
SHA-256 相同，排除了本地库被修改这一解释。
检查了库成员 `agent.c.obj` 与 `ice.c.obj`。以下 `agent.c` 行号来自 DWARF；
该源文件未公开包含在组件中，伪代码是二进制的等价表达，不是取得了官方源码。

## 3. 第一处问题：只允许相同类型的候选成对

`agent_pair_candidate`，原源位置 `agent.c:642`，包含：

```c
// 根据实际二进制恢复的筛选条件；省略外围循环。
if (local->type != remote->type ||
    local->addr.family != remote->addr.family ||
    local->transport != remote->transport ||
    pairs_num >= max_paired_candidates) {
    continue;
}
// agent.c:650
pair->type = remote->type;
```

关键汇编（地址为函数内偏移）：

```text
3f: l32i.n a8, a11, 16       # local->type
41: l32i.n a4, a7, 16        # remote->type
43: bne    a8, a4, b4        # 不相等跳过该组合
7b: s32i.n a4, a8, 0         # pair->type = remote->type
```

DWARF 明确 `ice_candidate_t.type` 偏移为 16，枚举 HOST=1、SRFLX=2、PRFLX=3、RELAY=4。
本次 Answer 的远端候选为 `typ host`，因此本地 srflx 即便已收集并写进 Offer，
也不会与该远端候选成对；留下的是 host/host 对。

[RFC 8445 §6.1.2.2](https://www.rfc-editor.org/rfc/rfc8445.html#section-6.1.2.2)
按媒体流、component 和地址族形成候选对，不要求 host/srflx/relay 类型相同。
[§6.1.2.4](https://www.rfc-editor.org/rfc/rfc8445.html#section-6.1.2.4)
另规定本地 reflexive 候选替换为 base 后去重；这不等同于按类型丢弃组合。

## 4. 第二处问题：拒绝当前 host 对的正常 NAT 映射

`agent_bind_mapped_matches_local`，原源位置 `agent.c:1409–1418`：

```c
// 根据实际二进制恢复的等价逻辑。
bool agent_bind_mapped_matches_local(agent_t *agent,
                                    ice_candidate_pair_t *cur,
                                    stun_msg_t *stun_msg) {
    if (agent->mode != AGENT_MODE_CONTROLLING ||
        cur->type == ICE_CANDIDATE_TYPE_RELAY) {
        return true;
    }
    if (stun_msg->mapped_addr.port == 0) {
        return true;
    }
    return agent_same_addr(&stun_msg->mapped_addr, &cur->local->addr);
}
```

`agent_same_addr`（原源位置 `agent.c:205–214`）比较地址族、端口和 IP 原始字节。
这里没有遍历本地候选、复用已知 srflx 或创建 prflx。

关键汇编：

```text
03: l32i.n a8, a2, 0         # agent->mode
05: bnez.n a8, 25            # 非 controlling 放行
07: l32i.n a8, a3, 0         # cur->type
09: beqi   a8, 4, 2a         # relay 放行
0c: l16ui  a8, a4, 46        # mapped_addr.port
0f: beqz.n a8, 30            # port 为 0 放行
11: l32i   a11, a3, 8        # cur->local
14: addi   a11, a11, 20      # &cur->local->addr
17: addi   a10, a4, 44       # &stun_msg->mapped_addr
1d: callx8 a8                # relocation: agent_same_addr
```

DWARF 交叉确认：agent.mode 偏移 0；pair.type 偏移 0；pair.local 偏移 8；
candidate.addr 偏移 20；stun_msg.mapped_addr 偏移 44；地址结构端口偏移 2。
AGENT_MODE_CONTROLLING=0，CONTROLLED=1。

`agent_process_stun_response`（原源定义 `agent.c:1599`）先进行 `stun_msg_is_valid`
校验，并尝试按 transaction 查找候选对。在 `agent.c:1621` 调用上述 helper；
不匹配则在 `1623` 打出 `XOR-MAPPED ... is not local candidate, skip nominate`，
在 `1625–1627` 清理已选中的该对的连接状态，在 `1629` 返回。
这条拒绝分支没有构造映射地址对应的 valid pair。

本次代入值为：

```text
cur->local->addr       = 192.168.1.7:59813       # host/host 对
stun_msg->mapped_addr  = 124.126.137.141:13174   # NAT 映射
agent_same_addr(...)   = false
```

即使 `local_candidates` 中存在同地址的 srflx，这个 helper 也不会读取它。
这解释了“已广播该地址，却仍拒绝”的表面矛盾：实际比较对象是**当前 pair 的 local**，
而非**全部本地候选集合**。警告文案容易造成误解。

[RFC 8445 §7.2.5.3.1](https://www.rfc-editor.org/rfc/rfc8445.html#section-7.2.5.3.1)
要求成功回应中的未知映射创建本地 prflx，已知映射使用已有候选。
[§7.2.5.3.2](https://www.rfc-editor.org/rfc/rfc8445.html#section-7.2.5.3.2)
要求用回应映射地址构造 valid pair，允许它不同于发起检查的候选对。
仅因公网映射不同于内部 host 而返回，阻断了该标准流程。
这也不是[§7.2.5.2.1](https://www.rfc-editor.org/rfc/rfc8445.html#section-7.2.5.2.1)
规定的请求/回应实际源目的地址对称性检查。

## 5. 原论证需要修正的部分

- **v1.5.6 changelog 的解读方向得到二进制支持。**
  [官方记录](https://raw.githubusercontent.com/espressif/esp-webrtc-solution/38a697f3b8142d23823eda5209edb89ca76119dc/components/esp_peer/CHANGELOG.md)
  中相应修复与当前 helper 一致；但仅此不能证明其首次引入时间，也不能认定旧版一定可用。
- **同一字符串本身不能排除配置问题。** SDP 中存在候选，不保证该候选已形成当前候选对。
  本轮取得的具体类型筛选和比较分支，才补齐了缺陷证据。
- **全零 related 字段不能单独判为缺陷或本次根因。**
  [RFC 8839 §5.1](https://www.rfc-editor.org/rfc/rfc8839.html#section-5.1)
  允许隐私隐藏（其规定的隐藏端口为 9）；现行 JSEP 的
  [RFC 9429 §7.3](https://www.rfc-editor.org/rfc/rfc9429.html#section-7.3)
  relay 隐私示例实际使用 `raddr 0.0.0.0 rport 0`。
  [RFC 8445 Appendix B.3](https://www.rfc-editor.org/rfc/rfc8445.html#appendix-B.3)
  说明 related 信息不用于 ICE 算法；上述实际失败分支也不读取 candidate.raddr。
  不能从 relay 示例推定本库 srflx 输出完全规范，但它不足以解释本次拒绝。
- **Mac 对照证明互通能力，不等于完全排除路径差异。** ESP32 远端为
  `39.105.73.166:3478`，Mac 为 `39.107.142.202:3478`；Node 候选还包括 IPv6 和
  `198.18.0.1`，没有 selected pair 与公网 UDP 映射原始记录。
  `/tmp/wrtc-test/test.js` 的成功/失败二分判词过强。
- 请求文档中的日志是加注释、脱敏并省略重试的节选，发 issue 时应称“日志节选”。
  HTTP 200 证明信令被接受，不单独证明所有 SDP/ICE 语义正确。

## 6. 配置与我方代码复核

未发现能解释上述 XOR 拒绝的我方配置错误：CONTROLLING + ALL 合理，
max_candidates 默认 16；main_loop 的循环与 20ms 延时符合官方 peer_demo；
open 时提供 server_lists 后 new_connection 不要求额外 update_ice_info；
Answer 保存在全局字符串中；cfg/tuning 的局部变量写法与官方示例一致。
v1.5.6 changelog 明确自动处理远端 SDP 的 ice-lite，不能把未手动开启 lite 当成漏配。
这些检查不等于彻底排除所有资源不足或并发问题，但没有找到能推翻实际拒绝分支的依据。

另外发现两个独立的我方问题，均发生在当前 ICE 根因之后：

1. `firmware/main/hal/webrtc/webrtc_m0.cc:308` 用 `peer_state >= CONNECTED` 判成功。
   API 中 CONNECTED=7、CONNECT_FAILED=8，因此当前失败会被误报为 PARTIAL。
   原始 CONNECT_FAILED 状态仍可信。
2. 同文件 `174–182` 对首个 DATA_CHANNEL_OPENED 固定发送到 stream 0，
   发送前即设 update_sent=true，不区分服务端 txt 与 oai-events，也不按返回值重试。
   API 提供 on_channel_open 的 label/stream_id；对照脚本实际在服务端 txt 上回送 update。
   ICE 修复后仍可能由此出现 M0 假阴性。

**状态：本轮只记录，未修复这两项代码。**

## 7. 后续验证与绕过，按成本排序

| 方案 | 能验证什么 / 限制 | 状态 |
|---|---|---|
| 同设备、同 M0 的 v1.5.5/v1.5.6 A/B | 最直接验证版本回归；旧版仍可能有其他 ICE 问题，不应承诺必成功 | **已执行，见第 10 节** |
| 向 Espressif 提交二进制分支、SHA 与日志，获取修复库 | 保持现有媒体/DTLS/SCTP 集成；这是优先修复路径 | 尚未提交 issue |
| 公网 IPv4 或已证实可达的直连 IPv6 | 验证 mapped 与 host 相同是否消除拒绝；当前 Answer 只有 IPv4，不能直接强制 IPv6 | 未执行 |
| 完整 ICE 对端下试 controlled 角色 | helper 对 controlled 放行，但还依赖服务端角色、提名和冲突处理；远端若 ice-lite，full 客户端必须 controlling | 未执行，不作为标准修复 |
| TURN/relay | helper 有 relay 例外，但相同候选类型筛选可能先阻止 relay/host 成对，不能承诺绕过 | 未执行 |
| 换可修改源码的实现 | 能修 ICE，但需要重验媒体、DTLS、SCTP、内存与实时性 | 仅评估 |

不建议继续靠更换 STUN、调候选上限或修改 related 字段碰运气；它们不改变已确认的比较。
不建议伪造候选类型或仅删除校验：正确修复须保留 STUN 校验并实现映射 valid-pair 流程。
“预编译因此不能直接改 ICE 源码”成立；“绝对无法自行修补”过宽，仍可实现其他
esp_peer_impl 或替换库，但不是修改本组件公开 C 文件即可修好。

Node 补证据应记录 getSelectedCandidatePair()，约束 LAN IPv4 并排除虚拟接口影响。
设备后续成功判据依次为 PAIRED、CONNECTED、明确 stream 上的两个 session 事件；
M0 不发送媒体，因此即使成功，也尚未证明服务端 AEC/双向语音/打断实际可用。

## 8. 源码替代实现的可行性

[sepfy/libpeer](https://github.com/sepfy/libpeer) 官方资料列出 Opus、DataChannel、STUN/TURN；
依赖 usrsctp，[SDP](https://github.com/sepfy/libpeer/blob/main/src/sdp.c)、
[RTP](https://github.com/sepfy/libpeer/blob/main/src/rtp.c) 与
[PeerConnection 接口](https://github.com/sepfy/libpeer/blob/main/src/peer_connection.h)
有 48kHz Opus 运输和双向已编码音频接口。
其[ESP32 示例](https://github.com/sepfy/libpeer/blob/main/examples/esp32/README.md)
主要是 JPEG over DataChannel，不能据此保证 ESP32-S3 上的持续双向语音。
本项目仍需接入 Opus 编解码、重采样和音频时序，并实测 8MB PSRAM 中的峰值内存与 CPU。
它值得作为可修改源码的备选，尚无本设备/阿里互通验证，不应现在承诺替换即成功。

Espressif 的[Amazon KVS C SDK 移植](https://github.com/espressif/esp-port-for-amazon-kvs-sdk)
可作为另一实现评估，但信令/SDP 和平台依赖适配成本高；也尚未验证与本阿里端点互通。
封装 esp_peer 的 Arduino 库不构成独立 ICE 替代实现。
优先验证旧版与获取官方修复库，成本低于重建完整栈。

## 9. 复核命令

在仓库根目录执行，仅提取库对象到临时目录，不需要设备或凭据：

```bash
review_tool_dir="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin"
review_lib="$PWD/firmware/managed_components/espressif__esp_peer/libs/esp32s3/libpeer_default.a"
review_out="$(mktemp -d /tmp/stackchan-ice-review.XXXXXX)"
shasum -a 256 "$review_lib"
cd "$review_out"
"$review_tool_dir/xtensa-esp32s3-elf-ar" x "$review_lib" agent.c.obj ice.c.obj
"$review_tool_dir/xtensa-esp32s3-elf-objdump" -dr agent.c.obj > agent-disassembly.txt
"$review_tool_dir/xtensa-esp32s3-elf-objdump" --dwarf=info agent.c.obj > agent-debug.txt
"$review_tool_dir/xtensa-esp32s3-elf-objdump" --dwarf=decodedline agent.c.obj > agent-lines.txt
```

查看三个函数：agent_pair_candidate、agent_bind_mapped_matches_local、
agent_process_stun_response；用 DWARF 核验成员偏移、枚举值和源行。
本轮完整临时输出位于 `/tmp/stackchan-esp-peer-review/`，长期证据以本文中的
库 SHA、等价逻辑、关键汇编和可重复命令为准。

---

## 10. A/B 实测结果（2026-10-07，复核之后补做）

复核指出"尚未证明 v1.5.6 首次引入回归，需要同设备 A/B"。该 A/B 已完成。

变量控制：同一台设备、同一份 M0 代码、同一网络、同一端点，
**唯一差异是 `espressif/esp_peer` 的版本**。

| 版本 | 组件 commit | 库 SHA-256（前 16） | 结果 |
|---|---|---|---|
| 1.5.6 | `38a697f3b8142d23` | `6b2f3856b9a1639b` | `skip nominate` 刷屏 → `CONNECT_FAILED` |
| **1.5.5** | `c8650846b512e6e1` | `25a338fc6b05f702` | **`Connection OK` → 全链路成功** |

### 1.5.5 的完整日志（节选）

```
I AGENT: 0 0 Send binding request (cand:0) local0:192.168.1.7:49914 remote0:39.105.73.166:3478
I AGENT: 0 0 LocalBinding resp 5851f42d40b18ccf4bb5f646 local1
I AGENT: 0 Select pair 39.105.73.166:3478        ← 无任何 skip nominate
I AGENT: 0 Candidate responded
I AGENT: 0 Connection OK 39.105.73.166:3478
I WebRTC-M0: [state] PAIRED (5)
I WebRTC-M0: [state] CONNECTING (6)
I WebRTC-M0: [state] CONNECTED (7)
I WebRTC-M0: [state] DATA_CHANNEL_CONNECTED (9)
I WebRTC-M0: create data channel 'oai-events' -> 0
I WebRTC-M0: [channel open] label='txt' stream_id=1      ← 服务端通道，stream 1
I WebRTC-M0: [state] DATA_CHANNEL_OPENED (10)
I WebRTC-M0: [data stream=1] {"type":"session.created",...}
W WebRTC-M0: >>> session.created received
I WebRTC-M0: sent session.update on stream 1 -> 0
I WebRTC-M0: [data stream=1] {"type":"session.updated",...}
W WebRTC-M0: >>> session.updated received

==================== M0 VERDICT ====================
  last peer state   : DATA_CHANNEL_OPENED (10)
  session.created   : YES
  session.updated   : YES
  server channel    : 'txt'
  => INTEROP OK: esp_peer talks to Aliyun.
===================================================
```

从启动到 `session.updated` 约 3 秒。

### 由此确立的结论

1. **是 v1.5.6 引入的回归。** 1.5.5 在同条件下成功，
   且全程不出现 `XOR-MAPPED ... is not local candidate, skip nominate`。
   第 3、4 节从二进制恢复的两处分支，与"v1.5.6 收紧提名条件"一致。
2. **ESP32-S3 与阿里 WebRTC 端点完全互通**：SDP 交换、ICE、DTLS、
   SCTP DataChannel、`session.created` / `session.updated` 全部打通。
3. **服务端事件的通道是 `txt`、stream 为 1**，而非客户端创建的
   `oai-events`、stream 0。第 6 节指出的 M0 缺陷 2 由此得到实证。
4. 第 6 节指出的 M0 缺陷 1（判据把 `CONNECT_FAILED` 误报为 PARTIAL）
   修复后，判据输出与真实状态一致。

### 当前处置

`firmware/main/idf_component.yml` 已从 `^1.5.6` 改为**精确锁定 `1.5.5`**，
并在文件内注明原因与解除条件（待上游修复）。

### 仍未验证的部分（复核第 7 节结尾的提醒仍然有效）

M0 不发送任何媒体。因此即使互通成功，**尚未证明**：
服务端 AEC 实际生效、双向语音可用、语音打断可用。
这些需要完整移植媒体链路（48kHz Opus、重采样、RTP 收发）之后才能验证。
