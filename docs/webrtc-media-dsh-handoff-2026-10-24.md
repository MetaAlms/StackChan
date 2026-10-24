# WebRTC 媒体移植：DSH 接续开发交接（2026-10-24）

## 当前结论

**尚未调通可用的双向对话。** M1 预录上行已有历史验收，M1.5 的近端 T1 在 near03 单轮通过；
双向播放仍重复“从前”并伴随用户报告的电流声，T2/T3/T4、M1.5 整体和产品移植均未验收。
需要完成根因定位和针对性修复后的验证，当前不安排无改动的重复说话测试。

用户最新授权：整理交接，交给 DSH 接续开发。Codex 本轮只整理证据和交接，不操作设备。
设备最后观测为 Idle、串口已关闭；后续每一轮实际测试仍须新弹窗 Q，用户点击“开始”后才 START。

## 坐标与复现材料

| 项目 | 值 |
|---|---|
| 实施目录 | `/Users/amtf/Documents/Git/StackChan`，复用当前 checkout，不覆盖或重新 clone |
| 分支 / PR | `feat/aliyun-omni-v2v` / [Draft PR #1](https://github.com/MetaAlms/StackChan/pull/1)，base `main` |
| 本交接前已提交 HEAD | `14958fc`（仅文档；开始前读取实际完整 HEAD） |
| 当前设备实现 | `4d5fa0b80c82765ed869c202a8d7087532fc1364`；交接文档提交不等于重新刷机 |
| DSH 目标会话 | `查看 stackchan worktree 开发进展`，既有 Chrome DSH `http://127.0.0.1:8080/`；发送前核标题 |
| 当前阶段 | M1.5 双向音频异常定位，先完成硬件/驱动审计；M2 产品接入保持 HOLD |
| 主记录 | [重新开发与逐轮结果](webrtc-media-redevelopment-2026-10-23.md)、[spec](webrtc-media-spec.md)、[plan](webrtc-media-plan.md) |
| 最近脱敏证据 | [05：准备超时](device_diag_evidence/2026-10-23-m15/duplex-05-counts.log)、[06：新 turn 取消](device_diag_evidence/2026-10-23-m15/duplex-06-counts.log) |

先读 `AGENTS.md`。工具链是 ESP-IDF **5.5.4**，`source ~/esp/esp-idf-5.5/export.sh` 后在 `firmware/` 构建。
Kconfig 生效入口为 `firmware/main/Kconfig.projbuild`；新增源文件后 reconfigure。
生成的 xiaozhi / managed_components 不在 git 中，改动须同步可重建补丁。

原离线台账：`/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`。
本次已读取 status：epoch **12**，used **26** / limit **32**，held **0**，active。
兼容离线检查通过 `/Users/amtf/.codex/dev-harness/controller.py`，沿用原台账，不新建或 recover 清预算。
ESP-IDF 构建/设备验证不在该单进程控制器覆盖内，须单独如实记录。

私有产物在 `/Users/amtf/.codex/dev-harness/artifacts/stackchan-webrtc-media/m15-takeover/`。
候选归档子目录 `duplex-diag-candidate/`；固件 SHA256：
`938a172cef63735667d06348ab24ef692745f1120f413f7e1841527428e42d00`。
此目录含固件/配置/原始日志，不得复制进 git、PR 或聊天；使用脱敏计数证据。
凭据仍在 gitignored 的 `firmware/sdkconfig.defaults.local`，禁止打印完整 sdkconfig/SDP/转写正文。

## 已证事实与不能推出的结论

| 观察 | 支持的结论 / 边界 |
|---|---|
| near03：三句均收到独立 ASR，关键词命中；speaker write 0 | 该配置该轮 T1 通过，不证明双向或 AEC |
| duplex04：用户只说一次故事请求后安静，仍循环“从前＋电流声” | 双向听感/行为失败；当时尚无响应取消原因诊断 |
| 05：stage4 准备超时，未 RUNNING，媒体计数 0 | 属于连接准备失败，不能作为回声/AEC 测试 |
| 06：05 后同串口/同设备进程新 START 成功 | 一例准备失败后重开成功，不代表长时稳定 |
| 06：用户确认读完后安静、没有其他音源；听到三次“从前” | 复现异常；无声与本轮 30 秒自动停止的对齐不明，不能当 AI 正常讲完 |
| 06：三个不同响应均 `cancelled / turn_detected`，与相邻新 speech 接收相差 0/5/1ms | 新 turn 取消回答的机制已证；声学回声、电气串扰、噪声或其他输入来源尚未定位 |
| 06：48k decoded 115200 → 24k converted/accepted/submitted 均 57600 | 这段下行样本计数闭合；累计 API 音频仅 2.4 秒，不是连续声学播放时长 |
| 06：125 次 API 写成功，PCM24 max_abs 11161、精确 s16 端点 0 | 不证明实际扬声器时钟/格式正确，也不证明没有失真或电流声 |
| 06：AFE 1497 帧，sent1490 + full3 + stopdiscard3 =1496 | 仍有 1 帧未单列，具体路径未实证，不伪造闭合 |
| 06：AFE `Error code:-1` 0；停止清理后 Idle | 100ms 有限等待在本轮改善日志；不证明音质修复或长期稳定 |

设备当前是受控诊断固件：默认 Idle，USB Serial/JTAG 命令显式启动，屏幕可能仍显示旧 Starting up 文案。
准备期限30秒；worker在同步API返回后检查，不保证可抢占的硬上限。
到 RUNNING 后才开始本轮运行窗口。故事测试实际窗口只有30秒，不能要求它证明一分钟完整回答。

## 硬件审计：需要纠正的旧前提

准确审计对象是 [StackChan Core C156 官方页面](https://docs.m5stack.com/en/core/StackChan_Core)，
不要仅凭 `CoreS3AudioCodec` 类名认定套错板，也不要把普通 CoreS3、SE、Lite 或 Thread BR 页面当作实板版本证明。

该 C156 页当前所链接的 [原理图 PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/490/Sch_M5_CoreS3_v1.0.pdf) 第4页，
显示 AW88298 的 SPK_VOP/VON 经电阻/电容网络接 AEC_P/N，再接 ES7210 MIC3P/N。
因此旧文档和 `config.h` 的“硬件没有扬声器回采”是过强断言，应撤回。
**这只证明官方链接设计存在该通路**：PDF 名仍为 CoreS3_v1.0，用户实板 revision、参考波形和 slot 映射未实测，不能反向宣称设备侧 AEC 已可用。

此前只读源码对照：本地音频 GPIO/I2C、TX STD / RX TDM、24k、AW88298 初始化与官方 StackChan 实现同路；
官方核对点 `m5stack/StackChan` SHA `eff0d6b5109663faec8f1d98848b4b0e5246bbfc`，
相关 codec/config 与本项目基线 `1b5765599fba8aaad1811d9a79358ccc7051f5f3` 一致。
这不是实际声音正确的证据。继续逐项核对输出 mono 重配置、RX slot/channel_mask、共享时钟和放大器格式。

当前 `esp_codec_dev/platform/audio_codec_data_i2s.c` 的 `_i2s_valid_fmt` 拒绝 0/奇数通道；
历史 3 通道报错不足以推出“最多2通道”，仍须审计 4ch TDM/通道选择是否可行。
不要现场直接改成4ch或启AEC；先给出代码路径和数据布局依据。

参考 [M5Unified issue #303](https://github.com/m5stack/M5Unified/issues/303)：该库曾修复 HW v2 的 PCM 样本对顺序问题，
但本项目实际走 ESP-IDF + esp_codec_dev，不能直接照搬 M5Unified 的 swap 或参数。
对照其修复的具体打包路径与 [ESP-IDF 5.5.4 ESP32-S3 I2S 契约](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/peripherals/i2s.html)，再判断是否适用。
当前 Codex 工具目录未暴露 Context7，不能写成已经用 Context7 完成验证；可用时再查询，否则用官方固定版本源码。

## 给 DSH 的接续任务与退出条件

1. **先校正事实与 spec/plan**：完成真实板型、功放初始化、I2S 时钟/格式、ADC 输入和通道映射的对照表，
   区分官方设计、当前驱动和实板未验证项；纠正“无回采/最多2ch/服务端必定AEC”等绝对结论。
2. **据证据做最小修复**：在 `cores3_audio_codec`、AudioService/AFE、media_codec 和受控 M1.5 路径中定位声音异常。
   先查离线 PCM/格式/采样率/布局和生命周期，再选单一变量修复。没有代码依据时不要盲改 VAD、增益、字节序或音频队列。
3. **准备有区分力的验证**：先把本地已知 PCM 的扬声器播放与云端回答分开，再比较关喇叭近端与双向，
   并确保留出真正观察回答的窗口。每轮写清唯一变量、预期观察、截止条件；代码/构建可以准备，实际采集/播放须等新 Q。
   单纯关闭 `interrupt_response` 最多作为明确标注的诊断对照，不能冒充回声消除或最终验收。
4. **提交可复核候选**：相关离线检查、IDF 媒体构建、受影响的回退构建及补丁重建；按实际改动选择检查，不重复无关全套。
   记录 exact HEAD、构建配置摘要、固件 hash、发现和剩余风险，commit + push 到当前 PR，提交 `REVIEW_REQUEST` 后停下等待 Codex 评审。

本轮是新接续任务，**没有虚构的 PR Review ID**；后续具体修复轮才按真实 Review ID + exact reviewed HEAD 执行 blue-pr-review-fix-loop。
若尚未找到根因，提交可验证的审计结果/最小实验候选即可，如实写“未修复”，不得用更多无改动设备试验替代定位。

## 必须保留的边界

- 每轮实际设备测试先用弹窗 Q，用户点击“开始”才 START。旧“开始”不复用；RUNNING 后再完整提示要说的话。
  故事原句为：“请用普通话慢慢讲一个至少一分钟的完整故事，不要向我提问。” 准备阶段不提示用户说话。
- 准备/刷写授权与测试授权分开；用户可能在忙，不能靠持续盯聊天配合。异常循环立即 STOP，确认 Idle/停音。
- 不恢复半双工门控来掩盖双向失败，不把 WebRTC 协议表的“内置AEC”解释为服务端已保证此设备 AEC。
- 不移除现有 SRTP profile 修复；esp_peer 已采用1.5.7，当前回答循环不是旧1.5.6 ICE 提名缺陷的同义问题。
- M2 产品整合、语音打断、同会话迟到 RTP 归属和长期重连尚未完成，不顺带声称通过。
- 不 merge / auto-merge / 发布 / 提上游 issue；本次无需新增外部沟通。

## 交接投递记录

- 目标：既有 DSH 会话 `查看 stackchan worktree 开发进展`（可见 URL `http://127.0.0.1:8080/`）。
- 交接前设备状态：最后观测 Idle、串口关闭；本次没有设备操作。
- 当前状态：交接材料已整理，UI 投递与 DSH 自读工作目录/本文件的确认以实际会话为准。
