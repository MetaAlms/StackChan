# D1 开发中预检（不是新HEAD正式评审）

原任务为 Review 5448296466，reviewed HEAD 7317037 full SHA。
以下是该Review已有D1-1/D1-2契约的具体实现核查，不扩展到产品阶段。

## DTLS钩子先修这几处

- 本机3.6.5没有 `mbedtls_ssl_get_srtp_profile` 或 `mbedtls_ssl_get_endpoint`。
  用实际公开 `mbedtls_ssl_get_dtls_srtp_negotiation_result`（返回void）与
  `mbedtls_ssl_context_get_config`/`mbedtls_ssl_conf_get_endpoint`/`mbedtls_ssl_is_handshake_over`。
  后三者为inline，直接调用，不能靠--wrap截获。
  完成之前profile只能标未确认，不能据它说已协商成功。
- 目前wrapper把所有SSL握手汇总；信令HTTPS先成功会使hs_ok>0，
  后续真正WebRTC DTLS超时也可能被当成功。必须做Review已有的
  config_defaults(DATAGRAM)→setup映射，排除STREAM/HTTPS，free/代际清登记。
- parser必须核FEFD/FEFF和 record_length<=剩余字节-13；完整迭代多record。
  现在仅type20–23、固定头读取会把TLS/截断输入误记DTLS，还漏后续record。
  seq需high16+low32；当前只保存high16，常见小sequence都看起来为0。
- TX统计要保存实际rc/errno；满长才是本地写成功，尝试不能叫“已发出”。
  RX解析最多min(rc,len)；未匹配FD/transport标关联未知，不能说“对端从未回答”。
- 在真实调用后**立即**保存errno，之后才读取timer/getter或锁。
  原参数/返回/errno透传；不要拿到mutex后再调用真实网络/握手。
- Arm先清Report再 ++其generation会每次回到1；使用独立单调generation。
  固定ring记录有界按时序样本和采样缺失，不能仅留下末条覆盖全过程。

具体公开签名与 U/T callsite证据见
[可行性核验](webrtc-media-m1-dtls-wrap-feasibility.md)，完整离线材料在
`/tmp/stackchan-dtls-wrap-feasibility-050a2f4/`。这是只读核查，没有设备尝试。

## RTP计数继续遵守已有状态契约

`ObserveSendto` 定义必须在 `rtp_probe` namespace中，与header声明匹配。
保护成功的record短写/负返回后仍保持“保护已成功”属性；
不能因state变kWriteFailed把重试算成protect-failed。
Snapshot不能无条件退休仍在进行的真实call，需区分非破坏快照与已结束窗口结算。
这些状态序列须测试真正转换，不是只声明新增counter恒零。

不降48k、不升级peer、不预设早到HELLO/算法/profile根因，M1.5/产品HOLD保留。
