# 上游修复后的验证步骤（1.5.7 已发布，按 U1 执行）

本文件是为 issue [#208](https://github.com/espressif/esp-webrtc-solution/issues/208)
的回应预置的验证流程。**上游一给修复库，照此执行即可**，不需要重新梳理。

当前状态（2026-10-13核实）：上游1.5.7已发布，issue仍OPEN；
[维护者发布确认](https://github.com/espressif/esp-webrtc-solution/issues/208#issuecomment-6059327698)、
[官方changelog](https://components.espressif.com/components/espressif/esp_peer/versions/1.5.7/changelog?language=en)。
用户已授权将升级并入SPEC/PLAN；现由新原生DSH会话
**StackChan WebRTC U1 repair session** 执行（2026-10-13授权替代旧stackchan）。

**先决条件**：先修当前 b45aaa7 的 DTLS 探针：DATAGRAM=1/STREAM=0被写反，
profile0实际取自被误判为DTLS的HTTPS context。不能用该日志证明SRTP未协商，
更不能据此另报上游。真实conf_transport为void，释放名为config_free。
按PLAN U1-0修复并经Codex复核后，冻结相同诊断代码，再做1.5.5→1.5.7对照。

**U1-0 修复状态（2026-10-13）**：
`2403b287` 的分类/API错误已由
[Review5498100335](https://github.com/MetaAlms/StackChan/pull/1#pullrequestreview-5498100335) 复核，
其 [第二次复核](webrtc-media-m1-diagnostic-review-4.md) 指出的 F1/F2/F3 已修（见下述判据）。
U1-0 仍**待 Codex 放行**：放行后才派 U1-1～3，本文件下方 A 节的两版本对照**仍未开始**。
**设备两臂实测未做**（本机无 `/dev/cu.usbmodem*`），不得把修复当作升级验证结果。当前判据：

| 项 | 结果 |
|---|---|
| 分类改用公开 `MBEDTLS_SSL_TRANSPORT_*` 宏（本机 STREAM=0/DATAGRAM=1，ssl.h:301–302）| 已修，离线断言覆盖 |
| 只有同一次成功 setup 登记的 (ctx, conf) 才算 DATAGRAM；配置单独存在不足以定身份 | 已修，含 b45aaa7 反例回归 |
| 未完成/失败的 DATAGRAM 握手不读 profile | 已修，`profile_skipped` 单独计数 |
| `conf_transport` 真实 void，且更新已登记配置不 early return | 已修 |
| 释放符号 `mbedtls_ssl_config_free`，并新增 wrap `mbedtls_ssl_free` | 已修（旧 `conf_free` 在 ELF 中无对应函数，属死代码）|
| 每个 wrapper 保留真实返回、errno 按真实调用保存/恢复（**F2**：前置计时/锁/分类后紧邻 real 恢复入口 errno）| 已修，行为场景 10 + 负对照覆盖 |
| Arm/新代际与 free 终止身份；**F1** 跨 Arm 的在途握手不得写入新报告 | 已修，行为场景 9 + 负对照覆盖 |
| **F3** 六项 `__real_*` 与真实公开 API 的签名断言 | 已修：`decltype(&__real_X)` 直接对 `decltype(&mbedtls_ssl_X)` |
| 行为检查（独立编译的二进制）| 经控制器 `u1-d3-probe-behaviour` 运行（cache=never），177 项断言通过 |
| 控制器注册的实际源码检查 | `firmware/tests/check_dtls_probe_actual_source.py`（`DTLS_PROBE_ACTUAL_SOURCE_PASS`）|
| 编译/IDF 构建（控制器覆盖外，单独报告）| `run_dtls_probe_offline_check.sh` 编译通过；IDF v5.5.4 build 通过 |

锁文件/依赖**未改**：peer 仍为 1.5.5。1.5.7 的静态指纹见 findings B25。

---

## 可能的三种回应与对应动作

| 上游回应 | 动作 |
|---|---|
| 发布修复版本（如 1.5.7） | 走下方「A. 验证新版本」 |
| 直接给一个 `.a` 或分支名 | 走下方「B. 验证分支/定制库」 |
| 要求补充信息 | 参照「C. 可立即提供的材料」 |

---

## A. 验证新版本

```bash
cd /Users/amtf/Documents/Git/StackChan/firmware

# 1. 记下当前基线（应为 1.5.5 成功）
grep -A 2 'espressif/esp_peer:' main/idf_component.yml
shasum -a 256 managed_components/espressif__esp_peer/libs/esp32s3/libpeer_default.a

# 2. 只改清单中的版本号（不要删 dependencies.lock——会重解析约 21 个无关组件）
#    锁文件保持，只接受官方注册表解析/校验过的 esp_peer 新版本、hash、元数据；
#    不猜hash，不只改version伪装解析成功。若工具修改无关组件，恢复无关差异；
#    新版真实传递依赖不兼容时先记录必要变化并交Codex评审。
#    构建后必须复核：
git diff -- firmware/dependencies.lock | grep -E '^[+-]\s+(version|component_hash):'
#    期望：只改变 esp_peer 依赖块及必要的清单一致性元数据，无关组件版本/hash不变

# 3. 构建并烧录
source ~/esp/esp-idf-5.5/export.sh
idf.py build && idf.py -p /dev/cu.usbmodem1101 flash
```

**本次执行**：按PLAN U1使用修正后的同一短M1诊断代码、同一fixture/配置，
分别记录两版本的库hash、ELF、ICE、真实DATAGRAM context的ret/role/completion/profile、
SCTP与session回显；连接健康才送2秒静音+zh_1+有界静音等待。
给语音区间单独的真实protect/UDP计数、媒体时长、VAD/ASR，不先运行300秒loop。
1.5.7须重新核对头文件/ABI及本方案依赖的pts、payload容量、真实wrapper callsite。
不同时调cipher/SSRC/角色/编解码配置。失败也保留阶段与覆盖证据。

**M0 回归判据**（另行标注，不替代媒体验收）：

```
==================== M0 VERDICT ====================
  last peer state   : DATA_CHANNEL_OPENED (10)
  session.created   : YES
  session.updated   : YES
  server channel    : 'txt'
===================================================
```

| 结果 | 含义 |
|---|---|
| `session.created` / `session.updated` 均为 YES | 本设备控制链路可用；不等于SRTP/VAD/ASR通过 |
| 仍为 `ICE FAILED` | 本次ICE失败，按实际配置/拓扑定位，不能直接断言上游修复无效 |
| `PARTIAL` | ⚠️ ICE 通了但 session 未建立，附日志回报 |

**版本采用**：Codex按真实两臂结果复核是否采用精确1.5.7；不再使用范围pin。
没有回归证据就不宣告替换完成；1.5.5归档可回退，但不覆盖/删除当前生成目录。
M1/M1.5/产品验收保持各自门槛，组件升级不自动放行。

## B. 验证分支或定制库

```bash
# 记下库指纹，便于回报
shasum -a 256 <收到的 libpeer_default.a>

# 覆盖到组件目录（先备份原库）
cp managed_components/espressif__esp_peer/libs/esp32s3/libpeer_default.a{,.bak-1.5.5}
cp <新库> managed_components/espressif__esp_peer/libs/esp32s3/libpeer_default.a

# 注意：ESP-IDF 会因组件 hash 变化重新拉取，可能覆盖手放的库。
# 若发生，改为在 CMake 中指向本地副本，或把整份组件放到 components/ 下托管。
```

**同时要跑回归**：修复版不应破坏 1.5.5 已验证的路径。
两臂都跑一遍（原始库 / 修复库），确认差异只在预期的提名分支。

## C. 可立即提供的材料

上游若要求补信息，以下本机已有、无需重新采集：

| 材料 | 位置 |
|---|---|
| 完整 AGENT 日志（1.5.6 失败 / 1.5.5 成功）| `docs/firmware-findings.md` A8 |
| Offer / Answer SDP | `docs/webrtc-m0-review-request.md` 第 3 节 |
| 两版库 SHA-256 与官方提交坐标 | `docs/webrtc-m0-review-result-2.md` 第 2 节 |
| 反汇编与 DWARF 复核命令 | `docs/webrtc-m0-review-result.md` 第 9 节 |
| 可复现的对照脚本（标准 WebRTC 栈）| `/tmp/wrtc-test/test.js`（需另存，`/tmp` 会被清理）|

**注意**：`/tmp/wrtc-test/test.js` 在 `/tmp` 下，重启会丢。若要长期保留应移入仓库。

---

## 与此并行的两件事（不依赖上游）

这两项与 issue 结果无关，可以独立推进：

1. **媒体链路移植**——48kHz Opus、重采样、RTP 收发时序。
   前提是 ICE 可用；**在 1.5.5 上已经可用**，所以不必等上游。
2. **设备的可用性**——当前烧的是 M0 探测固件（只验证握手，不能对话）。
   若要恢复日常使用，需刷回常规 WebSocket 固件。

---

## 回退路径（若上游始终不修）

| 方案 | 说明 |
|---|---|
| 长期锁定 1.5.5 | 已可用；代价是放弃 1.5.6 的 5 项修复，其中仅 DTLS 建连窗口缓存与本方案相关 |
| 换可改源码的实现 | `sepfy/libpeer` 源码完整（含 `agent.c`/`ice.c`/`sctp.c`），但需自接 Opus、重采样与音频时序，并实测 PSRAM 占用 |
