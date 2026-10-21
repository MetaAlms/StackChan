# U1 结果：短模式冻结与 esp_peer 1.5.5 / 1.5.7 对照

状态：**源码冻结完成、两臂构建与归档完成、设备实测待接入**。
U1-4 采用决定待 Codex 评审；本轮**不宣告 1.5.7 已采用**。
任务书见 [U1 任务书](webrtc-media-u1-task.md)，接线纠正见 [U1-S 预检](webrtc-media-u1-preflight.md)。
上一复核：[U1-0 复核结果](webrtc-media-u1-0-review-result.md)（full `bcd4e35`，U1-0 通过）。

## 边界（本轮未越界）

- 只改 `main/idf_component.yml` 的 peer pin 与短模式相关源码；**未改媒体参数**：
  真实 48 kHz PCM、16→48k 重采样、20 ms 单声道 Opus、RTP 48k clock 均保持。
- **未改 fixture**、未改 cipher/角色/STUN/SSRC/码率/ptime，未重跑已通过的云端 host 对照。
- WebSocket 回退未动；M1.5 与产品验收 HOLD；**未 merge/发布/改上游**；
  未扩张到其他 M1 诊断 TODO（`my_id`、record ring、`NotePoll/NoteDtlsSocket/NoteRtpSocket` 等保持原边界）。
- 未跟踪生成目录不是唯一交付：本文件与归档清单共同构成可复核记录。
- 归档内的 `sdkconfig.local-sensitive` 是含凭据的完整配置，**只在本机私有归档**，
  不进 Git/PR。本文只给脱敏摘要与 digest。

## U1-S：两臂共用的短模式

短模式是发送任务上的一段有界序列，替换原来的"三条 clip + 300 秒资源 loop"：

```
2 s 前置静音 -> 仅 zh_1 -> 800 ms tail -> 有界 ASR 等待(20 s)
```

结构保证（可测，不靠文字声明）：

| 保证 | 实现 |
|---|---|
| 短模式永不进入 300 秒 loop | `m1_short::Sequencer` **没有 loop 步骤**，失败路径同样不可达 |
| 最多进入 1 条 clip，且是 zh_1 | 进入次数计数，编译期与运行期都断言 |
| 每条终止路径都结算观察窗口一次 | 所有失败先转到 `Settle`，再 `Done` |
| 区间统计不重启观察窗口 | 两臂用**非破坏快照相减**，`rtp_probe::Arm()` 不在发送中调用 |
| 前置静音不能冒充 clip 活动 | `HasClipWriteActivity()` 只接受 clip 区间 |

### 五处接线纠正（`docs/webrtc-media-u1-preflight.md`）

1. **媒体时钟初始化**：`RunShortDiag` 原先没有 `g_clock.Arm()`，`deadline_us` 初值 0，
   开头若干帧会对着过去的 deadline 发送。现在短序列开始处**初始化一次**，
   各步骤共用同一时间轴，且**不重置** RTP 观察窗口。
2. **短/完整验收分离**：原 FINAL VERDICT 要求三条 clip；短模式下它只会给出与 M1 无关的
   FAIL，且 `g_loop_complete` 初值 true 会打印未运行循环的 COMPLETE。现该验收块
   **只在完整模式编译**（`#if` 守卫），短模式打印自己的有界结论与未覆盖项。
3. **证据命名准确**：`HasSpeechEvidence` 改为 `HasClipWriteActivity`。fixture
   `lead_silence_ms=500`，clip 开头静音写出一包后断链也能满足旧的"计数>0"，
   所以它只说明 clip 区间到达过 SRTP/UDP 边界，**不证明非静音声音抵达服务端**；
   声音证据仍由 VAD/turn 事件与转写承担，并在日志与结论里分开列。
4. **区间边界与"有无新包"分离**：`Interval` 拆出 `has_start_boundary` /
   `has_new_activity` / `has_end_boundary`。开头静音本来就没有 before 包，却会有大量新包，
   旧实现会打印 "no packet observed"；反之无新包时 before/after 都留有旧包，
   旧实现会显示一个有效范围。现在缺起始边界与无新活动分别表述，且无新活动不继承范围。
5. **无 ASR 时保留 partial 证据**：`WaitForClipAsr` 在超时/断链返回 false 前，
   用 `SnapshotTurnEvidence()` 快照该 turn 已有的 VAD/item/transcript 证据，
   **不清掉也不编造**挂起标志。

## 冻结的同源指纹

两臂由**完全相同的源码与 fixture** 构建；归档脚本对同一组文件取 hash：

```
frozen source fingerprint (sha256 of the per-file sha256 list)
  arm-a: 4c14aaed542797bc1f9d43fe4aa88b02654b578441b0e3162961c1057c6517eb
  arm-b: 4c14aaed542797bc1f9d43fe4aa88b02654b578441b0e3162961c1057c6517eb   <- identical
```

冻结集合含 `webrtc_m1.cc`、`m1_short_plan.h`、`dtls_probe_core.h`、DTLS/RTP 探针、
`webrtc_transport.*`、`main/CMakeLists.txt`、`main/Kconfig.projbuild` 与三条 fixture PCM + manifest。
逐文件 hash 在两臂归档的 `component/SOURCE_HASHES.txt`，`diff` 结果为空。

fixture（未改动，与 `fixture/manifest.json` 一致）：

| clip | PCM sha256 | 样本 | 时长 |
|---|---|---|---|
| zh_1 | `31fc3eef…8526` | 62206 | 3887.9 ms（含 500 ms 前置静音）|
| zh_2 | `b3e7fbe7…361f` | 58377 | 3648.6 ms |
| zh_3 | `49fbc922…840e` | 59419 | 3713.7 ms |

脱敏配置摘要（完整配置只在私有归档）：

```
CONFIG_IDF_TARGET="esp32s3"            CONFIG_STACKCHAN_WEBRTC_M1=y
CONFIG_STACKCHAN_WEBRTC_M1_FRAME_20MS=y  CONFIG_STACKCHAN_WEBRTC_M1_SHORT_DIAG=y
CONFIG_STACKCHAN_WEBRTC_M1_LOOP_SECONDS=300  CONFIG_SPIRAM=y
non-sensitive config digest (凭据行以固定标记替换后取 sha256)
  141e58a675d32ed8379a24f909fd67a2e81a08e043d79874d1f5b26fa66882c8
```

## 两臂构建与归档

顺序构建（两臂共用 `managed_components/`，不能并行改组件）。归档在
`~/.codex/dev-harness/artifacts/stackchan-webrtc-media/u1-archive/`（本机私有，不进 Git），
每臂含 ELF/map、一个自洽的 `bin/` flash 目录、`dependencies.lock`、peer 组件与 S3 库、
`ARM_VERIFY.json`、`ARCHIVE_VERIFY.json`、`SOURCE_HASHES.txt`、`FROZEN_SOURCE_FINGERPRINT.txt`。
**旧 WIP 1.5.5 归档已更名为 `superseded-arm-a-wip-1.5.5/` 并附说明，不作为基线。**

| 项 | arm-a | arm-b |
|---|---|---|
| pin / 实际解析 | 1.5.5 / 1.5.5 | 1.5.7 / 1.5.7 |
| `component_hash` | `97aac8b5…e0ca` | `92bc1b80…7845` |
| S3 `libpeer_default.a` sha256 | `25a338fc…3a58` | `e6fa4ccb…6285` |
| `stack-chan.elf` sha256 | `8d85e0d7…0ffa` | `b1fb4a8e…7a5d` |
| `stack-chan.bin` sha256 | `17fe8c19…53c5` | `e313ac0e…cb99` |
| `stack-chan.map` sha256 | `66fb90c6…fb60` | `d2578da4…d08d` |
| bin size | 0x311390 | 0x311e00 |

1.5.7 的 `component_hash` 与 S3 库 sha256 **与本任务书预先审计值逐字一致**，
且与注册表实际解析产物一致（不是照抄：由组件管理器解析后从归档实测）。

### 锁差异：必须先解决的一个真实问题

只把 peer pin 改成 1.5.7 后，组件管理器**重新求解了整个依赖图**，一次就改动了
**20 个无关组件**（B15 的同一陷阱）：

```
78/esp-wifi-connect 3.1.2->3.1.5     78/esp_lcd_nv3023 1.0.0->1.0.1
78/uart-eth-modem 0.3.4->0.3.5       espressif/adc_battery_estimation 0.2.1->0.2.2
espressif/adc_mic 0.2.2->0.2.3       espressif/bmi270_sensor 0.1.1->0.1.2
espressif/button 4.1.6->4.1.7        espressif/esp32-camera 2.1.5->2.1.8
espressif/esp_codec_dev 1.5.11->1.5.4  espressif/esp_image_effects 1.0.1->1.2.0
espressif/esp_lcd_co5300 2.0.3->2.2.0  espressif/esp_lcd_panel_io_additions 1.0.1->1.0.1~1
espressif/esp_lcd_touch_cst816s 1.1.1->1.1.2  espressif/esp_lcd_touch_gt1151 1.1.0->1.1.1
espressif/esp_lcd_touch_gt911 1.2.0~1->1.2.1  espressif/esp_lcd_touch_st7123 1.0.1->1.0.2
espressif/esp_mmap_assets 1.4.0->2.0.1  espressif/knob 1.0.2->1.1.0
lvgl/lvgl（条目录入变化）              txp666/otto-emoji-gif-component 1.1.1->1.3.0
waveshare/custom_io_expander_ch32v003 1.0.1->1.0.2
```

若就这样构建，两臂会在**无关组件上也不同**，对照不再是单变量，
且其中若干是实质功能性升级（`esp_codec_dev` 甚至是**降级** 1.5.11→1.5.4、
`esp_mmap_assets` 1.4.0→2.0.1 大版本变化）。按任务书"不能静默升级"，
**已把 20 条无关条目逐条恢复为 arm-a 基线值**（保留已知的基线 hash，不猜新 hash），
只保留 peer 条目与 `manifest_hash` 的必要变化。恢复后重新 `reconfigure`，
组件管理器**接受并沿用了该锁**（未再次重求解），并把 `managed_components/` 中
无关组件恢复为基线版本（实测 `78/esp-wifi-connect` 3.1.2、`esp_mmap_assets` 1.4.0、
`knob` 1.0.2、`otto-emoji` 1.1.1、`esp_codec_dev` 1.5.11、`lvgl` 9.4.0）。

最终锁差异（相对 arm-a 基线）**只有三处**：

```
  espressif/esp_peer  component_hash: 97aac8b5…e0ca -> 92bc1b80…7845
  espressif/esp_peer  version:        1.5.5        -> 1.5.7
  manifest_hash:                      e9e1fdc2…8493 -> e53d2928…a292
```

即任务书要求的"**仅 peer 依赖块及必要 manifest 一致性元数据变更**"。
这一处置及其理由在此**明示**，供 U1-4 复核；如需其他处置请指示。

## 最终 ELF / map 复核

两臂均为 **6/6** wrapper 定义且都有真实调用者（linker Cross Reference Table）：

| wrapper | 实际调用者（两臂相同）|
|---|---|
| `__wrap_mbedtls_ssl_handshake` | `dtls_srtp.c.obj`、`esp_tls_mbedtls.c.obj`、`ssl_msg.c.obj` |
| `__wrap_mbedtls_ssl_setup` | `dtls_srtp.c.obj`、`esp_tls_mbedtls.c.obj` |
| `__wrap_mbedtls_ssl_conf_transport` | `dtls_srtp.c.obj` |
| `__wrap_mbedtls_ssl_config_defaults` | `dtls_srtp.c.obj`、`esp_tls_mbedtls.c.obj` |
| `__wrap_mbedtls_ssl_config_free` | `dtls_srtp.c.obj`、`esp_tls_mbedtls.c.obj` |
| `__wrap_mbedtls_ssl_free` | `dtls_srtp.c.obj`、`esp_tls_mbedtls.c.obj` |

**peer 库未定义符号差异**（1.5.7 相对 1.5.5）：

```
新增（3，均为 IDF pthread 互斥属性实现，且在本臂 ELF 中全部解析为 T）:
  pthread_mutexattr_init / pthread_mutexattr_settype / pthread_mutexattr_destroy
不再需要（1）: ice_candidate_create
```

arm-a 的 ELF 里这三个符号因无引用而被 `--gc-sections` 丢弃；arm-b 因 1.5.7 的引用被拉入并解析，
与预检"新增 pthread mutexattr 符号本机 IDF5.5.4 已实现"一致。
map 中的 wrapper 引用两臂均保留（未被内联或绕过）。

## 受控离线检查

台账沿用原路径 `/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`
（`status` 后按 README 用 `replan` 接续，未 `recover`、未清 used、未另建库）。

| 检查 | 覆盖 | 结果 |
|---|---|---|
| `u1-short-wiring`（cache=snapshot）| 五处接线纠正的**实际源码**断言 | `U1_SHORT_WIRING_PASS` |
| `u1-short-behaviour`（cache=never）| 已编译短模式二进制的**受控执行**（不 fork/不联网）| 137 项断言 0 失败 |
| `u1-docs` | 文档相对链接与状态 | `U1_DOC_REFERENCES_AND_STATUS_PASS` |
| `u1-d3-probe-source` | U1-0 探针源码契约回归 | `DTLS_PROBE_ACTUAL_SOURCE_PASS`（复用快照）|

拆分边界（如实）：**编译**（`run_m1_short_offline_check.sh` / `verify_u1_arm.py`）
与 **IDF build** 需要子进程，**不在控制器覆盖内**，单独报告。
行为产物在工作区之外（控制器 sandbox 拒绝读工作区，区内二进制无法执行）。

**负对照**（证明检查有效，全部只在 `/tmp` 副本上做，未改工作区）：

| 临时移除的修复 | 触发 |
|---|---|
| 媒体时钟 `g_clock.Arm()` | `U1_SHORT_WIRING_FAIL`：必须恰好初始化一次 |
| FINAL VERDICT 的 `#if` 守卫 | `U1_SHORT_WIRING_FAIL`：验收块必须受短模式开关守卫 |
| `HasClipWriteActivity` 命名 | `U1_SHORT_WIRING_FAIL`：不得称为 speech evidence |
| `HasRange` 的边界条件 | `U1_SHORT_WIRING_FAIL`：不得要求起始边界 |
| 断链路径的证据快照 | `U1_SHORT_WIRING_FAIL`：两条无结果路径都必须快照 |
| 短模式 plan 的 loop/settle 不变式 | 行为检查：clip 反复进入、未结算、trace 不符 |

行动账本：`u1-docs` 曾因 root 新增文档链接**正确失败**（快照内看不到未声明的输入），
按 README `replan` 扩充输入清单后复跑通过；该失败历史保留，未改写。

## 设备实测：待接入

本机**没有** `/dev/cu.usbmodem*`，因此本轮**没有**任何真实 ICE/DTLS/SCTP/config/SRTP/UDP/VAD/ASR
结果，也**没有**新 profile、session 回显或语音写出。**不空等串口、不编造通过、不宣告 1.5.7 采用。**

设备恢复后按归档产物执行（顺序固定）：

1. 用 `arm-a-esp_peer-1.5.5/bin/flash_args` 刷 1.5.5，跑一次短模式；
2. 用 `arm-b-esp_peer-1.5.7/bin/flash_args` 刷 1.5.7，跑一次同参数短模式；
3. 两次都记录真实库/ELF 标识、ICE 阶段、同一已登记 DATAGRAM 的 ret/role/completion/profile、
   `session.created/updated`、静音/语音分段 SRTP/UDP 与 VAD/ASR；
4. **不要让 `idf.py flash` 在共享的新 `managed_components/` 下重建旧臂**——旧臂产物已在归档里。

刷写命令以 `bin/` 为工作目录（flash 参数里的相对路径就相对它解析）：

```bash
cd <archive>/<arm>/bin
esptool.py --chip esp32s3 -p /dev/cu.usbmodemXXXX write_flash @flash_args
```

`flash_args` 已包含 flash_mode/freq/size；本机 esptool 4.12 将这些参数定义在
`write_flash` 子命令下，不能放在该子命令之前。Codex 复核时已修正命令示例。

## 归档修复（Review5498605495 A1 / A2）

### A1：flash 参数引用了不存在的文件

`flash_args` 与 `flasher_args.json` 是从构建目录**原样复制**的，其中的路径相对于
它们自己所在目录：`bootloader/bootloader.bin`、`stack-chan.bin`、
`partition_table/partition-table.bin`、`ota_data_initial.bin`、`generated_assets.bin`。
原归档把 bootloader/partition 两个 bin **扁平**放进 `bin/`、把 app bin 只放在臂根，
于是 `bin/bootloader/bootloader.bin`、`bin/stack-chan.bin`、
`bin/partition_table/partition-table.bin` **三者都不存在**——按归档刷机会失败。

修复：`bin/` 现在复现 flash 参数所命名的目录形状
（`bin/stack-chan.bin`、`bin/bootloader/bootloader.bin`、
`bin/partition_table/partition-table.bin`，另两个 bin 原位不变），
archiver 之后自动运行 `firmware/tests/verify_u1_archive.py`。
**只使用各臂自己已归档的文件**：app bin 由该臂臂根的 `stack-chan.bin` 复制，
bootloader/partition 由该臂自己扁平的那两份**原位移动**（字节不变）。
**没有重新构建固件，也没有用当前 1.5.7 的生成目录去补 1.5.5 臂的文件。**

验证证据（`ARCHIVE_VERIFY.json`，逐臂）：

```
arm-a  flash references resolved 10/10   app bin 17fe8c19…53c5 == 参数所选 17fe8c19…53c5
arm-b  flash references resolved 10/10   app bin e313ac0e…cb99 == 参数所选 e313ac0e…cb99
```

（10 = `flash_args` 5 条 + `flasher_args.json` 5 条，两份文件都逐条解析。）

**范围说明**：校验器在整棵归档 root 上运行；若只修两臂，superseded 候选会**必然**
报出 6 处引用缺失，留下一个"注定失败"的归档容易误导后续使用。因此对
superseded 候选也按**同一 A1 规则**、同样只用它自己的文件修好了布局
（其 `SUPERSEDED.md` 仍声明它不是基线、不建议刷写）。
这一步**没有改动任何已有字节**。

**hash 保留核对**（修复前后对全部文件取 sha256 清单对比）：
**已存在的文件 0 个被修改**；变化只有 4 处**原位移动**（字节相同）、
2 份 app bin 自我复制（与该臂记录的 `stack-chan.bin` 相同）与新增的校验 JSON。
`stack-chan.elf` / `.bin` / `.map`、`dependencies.lock`、peer 库、
`SOURCE_HASHES.txt`、两臂源指纹 `4c14aaed…6517eb` **全部不变**。

### A2：归档的私有权限没有兑现

原归档 root 为 755、`sdkconfig.local-sensitive` 为 644（元数据观察，未读取或输出凭据，
不主张已发生泄露）。修复：

- `archive_u1_arm.sh` 增加 `umask 077`，并显式 `chmod 700` 归档 root 与臂目录、
  `chmod 600` 含凭据的 `sdkconfig.local-sensitive`；校验器同时检查权限位（不只假设）。
- 既有归档：本任务 `u1-archive` root 与三个臂目录收紧为 **700**，
  三份 `sdkconfig.local-sensitive`（含 superseded 候选）收紧为 **600**。
- **只改本任务归档路径**，未改 home/Keychain/其他工作区权限；
  校验器还断言 archiver 不对本归档之外的路径 chmod；
  敏感文件不进 Git/PR。

### 覆盖边界（如实）

产物级校验（`verify_u1_archive.py`）要读 67 MB ELF、32 MB map 与数 MB 镜像，
远超离线控制器的单输入 2 MB / 快照 10 MB 上限，**因此在控制器之外运行、单独报告**。
控制器侧新增 `u1-archive-contract` 只断言 archiver 的**契约**
（bin 目录形状、不得回退扁平布局、必须运行校验器、umask/权限、只动本归档路径）。
两者都有负对照：把脚本改回扁平布局或缺 `umask` 会触发 `U1_ARCHIVE_CONTRACT_FAIL`；
把校验器指向旧的扁平归档会逐条报出 6 处引用缺失与 2 处权限不符。

## 本轮不下的结论

- 不宣告 M1 通过：短模式有界，**不覆盖 M1-1..M1-3**；完整三 clip 入口仍保留（关掉短模式开关）。
- 不宣告 1.5.7 改善建连或媒体：无设备证据。
- 不把静音区间的成功写出当作语音抵达服务端的证据。
- 不主张两臂已在设备上等价或不等价；本轮只交付同源、单变量、可复现的构建与归档基线。

> 2026-10-22 后续：两臂"无 VAD"的共同根因已定位为 **DTLS-SRTP profile 不匹配**
> （协商 SHA1_32、实现 SHA1_80），修复后 VAD/ASR 已出现。见
> [根因记录](webrtc-media-m1-srtp-profile-rootcause.md)。本页 U1 结论不变：版本本身无差异。

## 2026-10-22 补记：设备接入后的两臂实测

离线准备已通过复核、设备也已接上，本轮补齐 U1-3 的真实设备部分。
**详细数据、日志附件与结论见 [U1 设备实测结果](webrtc-media-u1-device-result.md)**，
这里只同步与本页相关的状态：

- 每臂各刷一次归档固件，之后用 `esptool.py run` 复位重复运行；
  **两版本共 7 次连通运行**（1.5.5 四次、1.5.7 三次），参数完全相同。
- 7 次全部：ICE PAIRED、DTLS 握手成功（profile=2）、会话配置回显校验通过、
  SRTP 1339/1339、UDP 1339/1339 全写出、本地 Opus 解码 0 失败。
- **7 次全部 VAD=0、ASR=0、`transcript=""`、`outcome=asr-timeout`**：
  服务端从未对 `zh_1` 产生 turn。这是 M1 的真实阻塞点，且**与 peer 版本无关**。
- 1.5.5 另外出现过 1 次传输层失败（18 次 DTLS 握手超时，23.3 s，未建会话）
  和 1 次 2 重试后成功；1.5.7 三次都首握成功。样本太小，**不据此宣称 1.5.7 改善**。
- 推荐 U1-4 **保持 1.5.5 基线**、1.5.7 归档保留为候选；该建议待评审确认。
- 仍**不宣告 M1 通过**，不宣告 1.5.7 采用；短模式不覆盖 M1-1..M1-3。
