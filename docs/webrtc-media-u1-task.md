# U1：短模式与 esp_peer 1.5.5 / 1.5.7 对照任务

状态：Codex 2026-10-13 复核 full `bcd4e35ebeecab620debbb572277682d99959799`，
**U1-0 通过，授权派发本任务**；见 [复核结果](webrtc-media-u1-0-review-result.md)。
这是 PLAN U1-1～3 的新任务，不是扩张当前 Review5498323405 的返修范围。
实现者：原生 DSH **StackChan WebRTC U1 repair session**。
工作目录 `/Users/amtf/Documents/Git/StackChan`，分支 `feat/aliyun-omni-v2v`，PR1。

## 目标与边界

用修正后的同一诊断源码、fixture、配置与网络，对照精确 1.5.5 / 1.5.7。
区分 ICE、实际 DTLS、DataChannel/session 配置、SRTP/UDP、VAD/ASR。
失败也按实际达到阶段记录；不以 build/profile 单项宣告 M1 或全移植通过。
升级已经获用户授权，不再询问是否升级。最终采用由 Codex 的 U1-4 评审决定。

保持真实 48kHz PCM、16→48k 重采样、20ms mono Opus、RTP 48k clock 与原服务端配置。
不并改 cipher、角色、STUN、SSRC、码率/ptime、fixture；不重新跑已通过的云端 host 对照。
本轮不进入麦克风/下行/M1.5，不改 WebSocket 回退，不 merge/发布/改上游。
未跟踪生成目录不能作为唯一交付；不输出或提交凭据、完整敏感 sdkconfig/SDP。

## U1-S：先准备两臂共用的最小短模式

当前 `webrtc_m1.cc` 的“short diag”只是开头 100 帧静音：后面仍跑三条 clip 与
默认 300 秒 loop。仅设 LOOP_SECONDS=0 也不满足单条 zh_1 的对照要求。
因此先在两版本共同源码中实现一个探测专用的短模式，并冻结它后再构建两臂：

1. 保留现有 transport/config gate、握手失败路径报告、真正 sender 任务与退出/join。
2. 固定本对照 20ms，先 2 秒静音，再仅 `zh_1`，沿用已有 tail 与有界 ASR 等待。
3. 发送失败/配置失败/ASR 超时按实际阶段结束，不进入三 clip 或资源持续 loop。
   无论成功失败，短模式都不运行 300 秒 loop。
4. 分别记录前置静音与语音时段的观察边界、媒体 pts/seq/PT/SSRC、实际 protect/full UDP
   写出与未覆盖项；前置静音的 100 次成功不能冒充语音包成功。
   不为了区间统计在未结束的发送中 Arm/reset，避免制造新旧窗口混淆。
5. sender 停止并 join 后再结算现有观察窗口，报告短诊断结果及覆盖，明确不替代完整 M1。
   保留原完整 M1 入口的用途，采用最小探测开关，不扩张其他 RTP 诊断 TODO。
6. 有意义的离线反例覆盖：成功/无 ASR/发送失败均只能走 zh_1 的短路径、均不进长 loop，
   清理/join 不遗漏，静音计数不能计作语音证据。使用实际实现，不只测试复制的伪代码。

可先对该同源短模式做 targeted 检查与构建；两臂源码必须完全相同。
兼容离线检查通过既有 controller 台账，编译/IDF/云/设备仍分别报告覆盖边界：
`/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`。
先 status，按 README 在同台账接续，不能另建/recover/reset used；停止相关检查通过后的重复扫全仓。

## U1-1～3：顺序构建、归档、实测

两臂共用 managed_components，因此**顺序构建，不并行改组件**。
记录同源短模式的源码指纹、fixture manifest/PCM hash、非敏感配置摘要与 digest。
旧 1.5.5 的构建及所有刷机产物先完整归档，再改版本；不能用升级后的自动 rebuild
冒充旧版本基线。归档 ELF/map、所有 flash 参数引用的 bin、manifest/lock、库与产物 hash。
含本地凭据的产物仅保存在本机私有目录，不进 Git/PR；仓库交付经过脱敏的结果记录。

仅将 main/idf_component.yml 的 peer pin 改为精确 1.5.7；由官方组件管理器解析/校验，
保留 dependencies.lock，不删除整锁，不手猜 hash。锁文件仅 peer 依赖块及必要 manifest
一致性元数据变更；如需无关/传递依赖变化，提交具体差异给 Codex，不能静默升级。
更新旧 pin 注释，保留历史缺陷与回退证据；使用新版匹配的官方头文件、源码、S3 库。

官方 1.5.7 已审计指纹（仍应核对实际解析产物）：

- 来源 SHA `33a863f1a6c57bdcb7821f6f7a8e159035a0acf6`，见
  [静态审计](esp-peer-157-preflight.md)；以注册表/组件 manifest 实际值为准。
- component_hash `92bc1b80772acf8a0e2dc36193ec24278e0793140eca2a4060d8ce7b45c47845`。
- S3 lib SHA256 `e6fa4ccb3256007105bf7eff1e313dc5c06669f7d3b6df192571ebdba4496285`。

ESP-IDF 5.5.4 reconfigure/build；复核最终 ELF/map 的真实 wrapper callsites 与新 pthread
引用解析。复用已完成的公开 API/media 静态审计；只核对实际候选没有偏离其契约。

设备在线后用**各自已归档**的 flash 参数/产物刷 1.5.5→一次短诊断→1.5.7→同参数一次
短诊断。不要让 `idf.py flash` 在共享的新 managed_components 下重建旧臂。
两次均记录真实库/ELF 标识、ICE 阶段、同一已登记 DATAGRAM 的 ret/role/completion/profile、
session.created/updated 与验证过的配置、静音/语音分段 SRTP/UDP 与 VAD/ASR。
仅连接健康才发媒体；如失败，保存有界结束前的诊断，不猜独立 SRTP 根因或新上游 issue。

设备离线时先完成短模式与两版本构建/归档、提交候选 RQ；实测标为待设备，
不空等串口、不编造通过、不宣告 1.5.7 已正式采用。

## 交付

落盘结果：同源/单变量证据、两臂构建与库指纹、脱敏日志与达到阶段、未完成实测及具体后续。
同步 PLAN/SPEC 的当前状态、findings 与 dispatch，保留 root 的协调文档。
commit + push → full exact HEAD REVIEW_REQUEST（列明双臂证据及限制）→ STOP。
Codex 随即评审 U1-4，决定采用/继续定位，然后继续完整媒体移植。
