# WebRTC M0 第二轮复核结果

复核日期：2026-10-07。请求：[webrtc-m0-review-request-2.md](webrtc-m0-review-request-2.md)。
复核基线：`feat/aliyun-omni-v2v`，HEAD `45e097793f149a627caf9ed480d1fe419bf5a832`。

## 1. 判定

**v1.5.6 的 NAT 响应处理缺陷仍成立；临时精确锁定 1.5.5 合理。
但“公网 host 特有”“只有 HELLO 构成风险”“严格单变量 A/B”三处表述需要纠正。**

| 待复核主张 | 判定 | 应采用的表述 |
|---|---|---|
| 锁定 1.5.5 的取舍 | 成立，限于临时基线 | 当前配置已经完成信令与传输握手；媒体和重复建连稳定性未验证 |
| HELLO 缺失带来中等长连接风险，其他修复无影响 | 证据不足 | 缓存用于 DTLS 建连窗口；当前设备为 DTLS client，典型已报告场景适用性较低 |
| 回归只在对端把公网地址标为 host 时触发 | 不成立 | 公网 host 合规；应按实际 mapped-address 拒绝条件描述 |
| 已排除历史版本对照的全部混淆变量 | 不成立 | 除 M0 改动，还改变了 21 个其他组件；回调本身未发现影响 ICE 的直接路径 |

本轮接受已记录的 1.5.5 互通事实，未重复调用端点或刷机。
结论来自两版实际发布库的比较、M0/锁文件提交差异、RFC 与上游原始报告；
不把二进制等价逻辑称为公开源码，也不由一次成功推定持续语音可用。

## 2. 两版二进制新增了什么

检查对象均为 ESP32-S3 的 `libpeer_default.a`，SHA-256 与相应官方库一致：

| 版本 | 组件源码坐标 | 库 SHA-256 |
|---|---|---|
| 1.5.5 | `c8650846b512e6e1375e5f78c1c41619b8d645eb` | `25a338fc6b05f702947c2af0e924f2dd3006503908e889dcdc512782d0ea3a58` |
| 1.5.6 | `38a697f3b8142d23823eda5209edb89ca76119dc` | `6b2f3856b9a1639b0480c011688c070c132b6399e9863a74d20d20a271dc7da3` |

官方库：[1.5.5](https://raw.githubusercontent.com/espressif/esp-webrtc-solution/c8650846b512e6e1375e5f78c1c41619b8d645eb/components/esp_peer/libs/esp32s3/libpeer_default.a)、
[1.5.6](https://raw.githubusercontent.com/espressif/esp-webrtc-solution/38a697f3b8142d23823eda5209edb89ca76119dc/components/esp_peer/libs/esp32s3/libpeer_default.a)。

- `agent_pair_candidate` 的候选类型相等筛选，**1.5.5 已经存在**。
  两版函数内偏移 `0x3f/0x41` 读取候选的 type（结构偏移 16），
  `0x43` 在类型不等时跳过；不能把该限制称为 1.5.6 新增。
- `agent_bind_mapped_matches_local` 及响应路径上的调用，**1.5.6 新增**。
  1.5.5 的 `agent_process_stun_response` 没有这段拒绝处理。
  1.5.6 的调用位置对应 DWARF 原源 `agent.c:1621–1629`。
- 早到 non-STUN 数据报的缓存也是 1.5.6 新增，见第 3 节。

响应通过有效性检查并找到候选对后，1.5.6 的 mapped-address 拒绝条件为：

```c
// 二进制恢复的等价条件，非官方源码。
agent->mode == AGENT_MODE_CONTROLLING &&
cur->type != ICE_CANDIDATE_TYPE_RELAY &&
stun_msg->mapped_addr.port != 0 &&
!agent_same_addr(&stun_msg->mapped_addr, &cur->local->addr)
```

该分支不按映射地址查找已有候选或构造 valid pair，直接跳过响应处理。
本次检查从 `192.168.1.7:59813` 发出，映射为 `124.126.137.141:13174`，
正好满足拒绝条件。它没有判断阿里端点、公网地址或 remote host。
这是本次确定的错误路径；是否最终失败还取决于是否存在其他可成功的路径。

[RFC 8445 §7.2.5.3.1/2](https://www.rfc-editor.org/rfc/rfc8445.html#section-7.2.5.3.1)
规定从响应的 mapped address 得到本地候选并构造 valid pair。
valid pair 与原 checklist pair 的本地地址不同，是 NAT 下规范考虑的正常情形。

## 3. 锁定 1.5.5 的取舍

### HELLO 是 DTLS 建连报文，不是 SCTP

1.5.6 `agent_recv_one_packet`（DWARF 原源 `agent.c:1872–1885`）在
ICE 配对尚未完成、存在远端候选时，缓存 ICE 检查路径提前读到的一个 non-STUN
数据报，大小最多 1400 字节。`early_len` 结构偏移为 5008；已有缓存时不覆盖。
`agent_recv`（`1909–1917`）随后优先交给接收层。1.5.5 没有这一缓存。

1.5.5 `peer_on_pairing`（`peer_default.c:2015`）先完成 ICE，再重置 DTLS
并进入 CONNECTING；`peer_on_connecting`（`2090`）才调用 DTLS 握手，
成功后创建 SCTP。因此该缓存修复的是进入 DTLS 前的接收窗口。

[上游 #205](https://github.com/espressif/esp-webrtc-solution/issues/205)
报告设备为 DTLS server、浏览器 Answer 为 `setup:active` 时，早到 ClientHello
被丢弃；重传后恢复，配对至连接耗时约 7.7 秒，正常约 0.4 秒。
该报告另有独立 keepalive 问题，不能把其连接后的断开归因于 HELLO。

本次阿里 Answer 为 `setup:passive`，**设备是 DTLS client**。
正常情况下对端 server 等待设备 ClientHello，而本机在 ICE 就绪后才发送它。
因此 #205 的典型早到 ClientHello 场景直接适用性较低；ICE controlling
不等于 DTLS server。当前方案没有发生率证据，不能直接定为“中风险”。

可观测后果应描述为建连/重连时 DTLS 延迟、超时、停留 CONNECTING，
迟迟没有 CONNECTED 或 session 事件。[DTLS 有握手重传](https://www.rfc-editor.org/rfc/rfc6347.html#section-4.2.4)，
丢首包不必然导致永久失败。**连接已建立后，仅延长语音会话不会重新经过早到窗口；
重复建连、断网恢复和重连才会增加暴露次数。**

### 六项修复的取舍应按已观测配置限定

| 1.5.6 变更 | 回退后的评估 |
|---|---|
| TCP/UDP 同 IP、端口映射 | 当前 Offer/Answer 路径为 UDP，未看到触发条件；不能保证未来节点无此条件 |
| HELLO 缓存 | 保留 DTLS 建连时序缺陷；本次 DTLS 角色不同，影响和发生率未测 |
| ICE-lite 属性处理 | 当前 Answer 没有 `a=ice-lite`，此次未触发；节点行为改变后需重评 |
| mapped-address 提名检查 | 新增错误拒绝是当前回退理由；不代表 1.5.5 的 ICE 实现完全正确 |
| SDP/候选注释 | 未见运行时影响 |
| H264 profile | 当前禁用视频，此路径不适用 |

建议向使用者说明：**1.5.5 是已验证握手的临时基线，持续媒体与重连稳定性待验证。**
现有证据支持先绕开当前确定失败路径，没有理由仅凭旧版本 changelog 再降至
1.5.2/1.5.1，也不足以声称“只有一项真实风险”或“偶发失败先怀疑 HELLO”。
排查应先确认失败发生在 ICE、DTLS 还是应用阶段。

第三条路是获得修正 mapped-address/valid-pair 处理的官方库，保留其他修复。
本轮未找到已验证适用于本方案的配置绕过办法；该 helper 是库内静态函数，
注册应用回调不能覆盖它。替换库或另实现 esp_peer_impl 技术上可行，但成本更高。
修复不能只接受任何 transaction 匹配响应，应保留 STUN 校验和 valid-pair 流程。

## 4. 公网 host 合规，触发条件不是阿里特有

[RFC 8445 §5.1.1.1](https://www.rfc-editor.org/rfc/rfc8445.html#section-5.1.1.1)
以绑定本机接口取得地址定义 host candidate，没有私网地址限制。
[Appendix A](https://www.rfc-editor.org/rfc/rfc8445.html#appendix-A)
明确讨论公网可达设备；公网服务器使用 host 是正常部署。
[TURN relay](https://www.rfc-editor.org/rfc/rfc8656.html#section-7)
对应分配转发地址的机制。节点 IP 轮换、端口 3478、业务上转发给模型，
均不足以证明该候选来自 TURN allocation。没有证据认定阿里把 relay 错标为 host。

类型相同的配对筛选也会排除 host/remote srflx、host/remote relay 等组合，
不能限定成“只在公网 host 触发”。但本次 **local srflx/remote host 没进入
checklist 本身不能独立证明缺陷**：[§6.1.2.4](https://www.rfc-editor.org/rfc/rfc8445.html#section-6.1.2.4)
允许本地 reflexive 候选换成 base 后去冗余。标准去冗余与按类型过滤并不等价，
却可能在这一个组合上得到同样的 checklist。

应分开记录：1.5.5 已存在的跨类型配对限制，与 1.5.6 新增的错误响应处理。
不能声称必须保留另一组 srflx/host 检查才能解决本次问题；合法使用 host/base
发出检查后，也应接受正常映射并构造对应 valid pair。

## 5. 历史对照仍有混淆，严格 A/B 值得补做

`git diff 2f5df7b 4ba415f -- firmware/main/hal/webrtc/webrtc_m0.cc`
确实没有改变 ICE role、policy、STUN server、音频配置或 tuning。
新增 `on_channel_open` 用的是已有结构字段，不改变 API 布局。

实际 1.5.5 二进制/DWARF 提供以下补证：

- `esp_peer_cfg_t` 大小 116，`on_channel_open` 偏移 96；`extra_cfg` 偏移 60，
  `extra_size` 偏移 64。默认 extra 配置大小 68。
- `peer_open` 复制配置；`verify_config` 检查 extra size 与 `on_msg`，不读取该回调。
- `pc_data_channel_opened` 才读取并可选调用 `cfg+96`；SCTP 创建发生在 DTLS 成功后。

因此没有发现回调注册能改变原 ICE 拒绝路径的直接机制。全局状态和内存布局仍有
改动，不能把全部代码都说成“ICE 之前绝不执行”；静态证据支持的是阶段和调用关系。

**遗漏的混淆：锁文件改变了 22 个组件版本，除 esp_peer 外还有 21 个。**
证据命令：`git diff 2f5df7b 4ba415f -- firmware/dependencies.lock`。

| 组件 | 原版本 | 新版本 |
|---|---|---|
| 78/esp-wifi-connect | 3.1.2 | 3.1.5 |
| espressif/esp_codec_dev | 1.5.11 | 1.5.4 |
| espressif/esp_mmap_assets | 1.4.0 | 2.0.1 |
| espressif/esp32-camera | 2.1.5 | 2.1.8 |
| espressif/esp_image_effects | 1.0.1 | 1.2.0 |

其他变化涉及 LCD、触摸、按钮、IMU 等；`esp_libsrtp` 的版本和 hash 未变。
Wi-Fi 组件参与 M0 的 StartNetwork；探测前的 HAL 也初始化板、IO、IMU、LVGL。
不能一概排除网络、内存与任务时序影响。因此请求第 1 节以及第一轮结果第 10 节的
“同 M0、唯一版本差异”不成立。

严格补测应固定当前 M0、ESP-IDF/sdkconfig、其他依赖版本和 hash、STUN 配置与网络，
仅切换 esp_peer，按 1.5.5 → 1.5.6 → 1.5.5 记录结果。
**不要把删除整个 dependencies.lock 当作单变量实验步骤**；检查解析后的差异
仅含 esp_peer 及必需元数据，若引入其他依赖差异应先固定它们。
保存各次固件/库 hash、完整 SDP、远端候选与首个失败阶段；重复建连稳定性另作统计。

补测可直接验证“新 M0 + 原依赖集合 + 1.5.6”是否仍走同一拒绝分支。
历史实测不是严格单变量，不影响当前 1.5.5 成功事实；两版二进制差异及日志仍然
强力支持本次版本回归。当前没有执行补测或改变设备状态。

附带一处注释过强：`webrtc_m0.cc:214` 的“retrying until accepted”实际只会在
后续 session.created/channel-open 事件再尝试，没有持续定时重试机制。
“失败后允许再试”成立，“自动持续重试”不成立；它与本次 ICE 根因无关，未改代码。

## 6. 已有上游报告与提交措辞

[上游 #208](https://github.com/espressif/esp-webrtc-solution/issues/208)
已于 2026-09-30 报告 ESP32-S3/LiveKit Cloud 下同样的 1.5.6 NAT 拒绝，
其 Offer 同样已有映射 srflx，较早版本成功。2026-10-07 查验仍为 open；
现有报告是其他用户的独立复现，不等于维护者确认或修复承诺。
建议向该 issue 补充本项目证据，避免另建同症状 issue。

以下为可审阅草稿，本轮没有向 GitHub 发布：

> Additional ESP32-S3 reproduction with an Aliyun WebRTC endpoint: the controlling
> agent sends a connectivity check from a host candidate behind NAT. esp_peer 1.5.6
> rejects a transaction-matched response when XOR-MAPPED-ADDRESS differs from the
> original pair's local address, even though it equals an advertised local srflx.
> Binary comparison identifies the mapped-address guard as newly added in 1.5.6;
> the same-candidate-type pairing restriction already exists in 1.5.5.
> Version 1.5.5 completes ICE, DTLS, SCTP and application session events here.
> Our historical builds also differ in M0 code and 21 other dependency versions,
> so that runtime comparison is not a strict single-variable A/B.
> Please investigate constructing or locating the valid pair from the mapped
> address, while retaining the required STUN response validation.

## 7. 证据复查与当前状态

两版对象提取、反汇编和 DWARF 命令沿用[第一轮结果第 9 节](webrtc-m0-review-result.md#9-复核命令)。
额外检查 `agent_recv_one_packet`、`agent_recv`、`peer_on_pairing`、
`peer_on_connecting`、`pc_data_channel_opened` 和两版 `agent_process_stun_response`。
临时输出在 `/tmp/stackchan-esp-peer-review/` 与 `/tmp/stackchan-esp-peer-review-v155/`；
长期证据以库 SHA、官方提交、上述函数条件和提交差异为准。

发现已同步至 [firmware-findings.md](firmware-findings.md) A8/B15，
并纠正第一轮结果第 10 节的实验变量描述。未修改 M0、依赖、凭据或设备固件；
媒体/AEC/打断未验证的边界保持有效。
