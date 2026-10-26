# 标准栈媒体对照的运行中预检

针对 Review 5447928154 的第71轮实现；本文件是已有实验契约的具体核查，
不是新阶段放行，也不是对未交付 WIP 的正式 PR 复核。

首次 `/tmp/host_media/result.json` 显示 config echo 已核验，zh_1 的
VAD start=1、stop=0、completed=0，1275 次发送、0 caught exception；
这是当前 host FAIL，不能据此归因服务端或设备。

当前 WIP 在形成可评估的对照前还需核对：

- `eventChannel` 存的是 client label，收到 server channel 的 session.created
  仍给它贴 client label、update 总用 client dc。应传入实际 channel 对象，
  按本会话发现的事件通道发送并检查真实 boolean 返回值。
- VAD 没记录 item_id，统计使用整个会话累计值；ASR 只按数组计数/最后结果。
  旧/重复完成可释放新条，failed 不停止当前；必须建立本条实际 item 关联。
- 编码 manifest 未生成关键词，而 runner 按 `(clip.keywords || [])` 打分，
  会得到 0/0 关键词要求。补冻结关键词；数字归一化明确规则。
- `sendFrames` 每段重新 deadline，等待/段间开销被重置。共享单一
  monotonic 起点/绝对期限；真实 wall 和 frames/ts 同范围记录。
- 已撤回首次提出的 sender-report 时间戳配置怀疑：实际安装的
  libdatachannel 0.24.5 `RtcpSrReporter::outgoing` 从完整 RTP header
  读取 timestamp。脚本随后补写 config.timestamp 的运行仍未取得 stop/ASR，
  因而不能把最初未赋该字段当作根因；继续检查真实媒体时间轴和发送接受结果。
- `track.sendMessageBinary` 和 DataChannel send 的 boolean 不应忽略；
  发生调用或无 exception 不等于实际被接受。保留本地接受与远端VAD/ASR区别。
- config 失败分支调用 `finish` 时，下方 PT/stats/clipResults/allOk 仍在
  const 的 temporal dead zone，会使证据保存自身失败。失败也要有有效产物，
  退出时关闭本轮 track/channel/peer；不以 exit0 代表通过。

读取实际 source/packet-encoder/helper 后再修；首次负结果不是已证服务端问题。
短时核对实际静音 packet 格式/解码/时长与媒体时间轴，保留配置不变作为第一基线；
不使用手动 commit 掩盖 server_vad，也不先改 VAD 阈值掩盖对照设计问题。
