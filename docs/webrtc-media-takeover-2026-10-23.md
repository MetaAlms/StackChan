# 2026-10-23 Codex 接替开发记录

## 接替坐标与既有交付复核

- 已从 Chrome 的 DSH `http://127.0.0.1:8080/` 读取用户指定会话
  **查看 stackchan worktree 开发进展**。会话停在 U1-4 完成、M1待复核、M1.5未开工。
- 分支 `feat/aliyun-omni-v2v`，接替 HEAD `253fcf5`，接替时工作树干净。
- 复核 `eb33f58` 的 SRTP offer 修复、M1完整串口日志和修复后两臂日志：
  协商profile=1，三clip VAD/关键词通过，300秒资源循环完成；
  两臂短诊断均有正确转写。**放行M1上行范围，接受1.5.7基线**。
- 此结论不覆盖真实mic、下行、AEC、打断、重连或长期稳定性。
  原始交付仍保留其当时“待评审”表述，以本接替记录补充后续状态。

## 当前实现范围

新增 `CONFIG_STACKCHAN_WEBRTC_M15`（默认关闭），与M0/M1互斥：

1. 共用SRTP修复从M1观测单元移出；M0/M1/M1.5均链接该修复。
2. 持久编解码器：AFE16k每320样本→48k/20ms mono Opus；
   raw下行最大120ms→48k mono PCM→24k播放。检查容量、实际写出、完整消费。
3. AFE输出按20ms拆帧，拥有数据的有界队列非阻塞移交；队列满时保留媒体时钟空隙。
4. 使用现有唯一AudioOutputTask；拥有PCM按最多20ms提交。
   结束时推进epoch、清本地排队项，与硬件提交互斥。
5. 真实硬件读写成功/失败以及输入24→16转换错误计数；转换失败关闭该帧。
6. 60秒有界探针；首次用已验证的zh_1 fixture触发一轮下行，随后全部使用真实mic。
   fixture的ASR不作真实mic识别证据；采集全程持续。
   第二轮显式response.create后收到error，未定位错误原因，不据此断言API不支持。
7. 生成目录改动落在 `patches/stackchan-webrtc-media.patch` 并登记到repos.json。

**未交付**：自动插话、网络迟到RTP的response归属、重连、T1–T4和产品UI。
本地epoch只能隔离本地任务，不能证明网络旧包被正确关联；不得重新包装成AEC已通过。

## 验证与修正

- 原台账：`/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`。
  保留used=19，从epoch7 replan至8；登记实际共享队列的容量/FIFO/所有权/并发计数检查，PASS。
  编译、IDF构建、烧录和设备/云端检查在控制器覆盖外，分别记录。
- M1.5构建成功；独立只读复核提出并已关闭两项P2：FreeRTOS自删除跳过C++析构、
  现有输入转换器为空或错误时仍输出PCM。已改Body正常返回、trampoline删除任务；转换错误计数并丢弃。
- 第一轮真机：会话配置校验成功、真实硬件读取成功、48k上行无编解码错误；
  发送约40fps且mic_drop持续增加。相对`now+20ms`导致处理耗时累计，已改绝对deadline推进，
  后续同固件复位验证约50fps。第一轮无麦克风发言/下行，不宣称双向通过。
- 第三轮首次建连未进入DataChannel会话；同固件复位后成功进入双向：
  `t=62857ms sent=2645 decoded=980 write_ok=980`，编码/硬件/转换错误均为0。
  完整60秒内mic_drop=5、rx_drop=19、event_drop=118，不能宣称前置全部通过。
- 用户确认该轮无人说话、环境基本安静；播放后仍多轮speech/ASR/response，
  **疑似自身回声触发**。控制事件有丢失，尚未取得转写匹配证据，不据此归因到服务端或宣布AEC定论。
- 随后改为单生产者单消费者固定容量队列，避免mutex争用丢帧；只保留关键控制事件，
  过滤delta/UI。独立复核确认生产者归属与release/acquire正确；受控SPSC行为检查PASS。
- 增加ASR与此前回答的规范化精确匹配计数（仅内存中比较，不写出转写正文）；
  此指标阳性可补充回声证据，阴性不能证明AEC有效。
- 当前源码M1.5、M1、WebSocket三种配置均构建成功（控制器覆盖外）。
  vendor→Aliyun→WebRTC补丁在临时目录从dependency HEAD重建两份AudioService文件，与live逐字节一致。
- 最新候选刷写失败：`Failed to connect to ESP32-S3: No serial data received`。
  串口节点仍在；已请用户短按电源唤醒，尚待唤醒后验证新队列/事件过滤/匹配计数。
- 脱敏计数：`device_diag_evidence/2026-10-23-m15/*-counts.log`；
  原始含SDP日志仅在私有artifact目录，不提交。

## 后续顺序

先完成基础双向硬件观测与队列/pacing修复，再补stop的实际输出测量、
控制与媒体乱序实验、RTP关联规则；前置未满足时不执行有结论意义的T3/T4。
随后按既定PLAN推进M1.5行为测试、M2产品接入、M3稳定性和M4回退回归。
