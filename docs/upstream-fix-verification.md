# 上游修复后的验证步骤（待触发）

本文件是为 issue [#208](https://github.com/espressif/esp-webrtc-solution/issues/208)
的回应预置的验证流程。**上游一给修复库，照此执行即可**，不需要重新梳理。

当前状态：等待中（issue OPEN，我们的评论为第 4 条）。

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
#    然后同步把锁文件里 esp_peer 条目的 version 与 component_hash 改成新值，
#    保持清单与锁一致，避免触发整体重解析。
#    构建后必须复核：
git diff -- firmware/dependencies.lock | grep -E '^[+-]\s+(version|component_hash):'
#    期望：只出现 esp_peer 的 version 与 component_hash 两处

# 3. 构建并烧录
source ~/esp/esp-idf-5.5/export.sh
idf.py build && idf.py -p /dev/cu.usbmodem1101 flash
```

**判据**（M0 自动打印）：

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
| `session.created` / `session.updated` 均为 YES | ✅ 修复有效，可继续做媒体链路移植 |
| 仍为 `ICE FAILED` | ❌ 未修好，附日志回报 |
| `PARTIAL` | ⚠️ ICE 通了但 session 未建立，附日志回报 |

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
