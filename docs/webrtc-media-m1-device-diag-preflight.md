# D1 开发中预检（不是新HEAD正式评审）

原任务为 Review 5448296466，reviewed HEAD 7317037 full SHA。
以下是该Review已有D1-1/D1-2契约的具体实现核查，不扩展到产品阶段。

## DTLS钩子先修这几处

- 本机3.6.5没有 `mbedtls_ssl_get_srtp_profile` 或 `mbedtls_ssl_get_endpoint`。
  用实际公开 `mbedtls_ssl_get_dtls_srtp_negotiation_result`（返回void）与
  `mbedtls_ssl_context_get_config`/`mbedtls_ssl_conf_get_endpoint`/`mbedtls_ssl_is_handshake_over`。
  后三者为inline，直接调用，不能靠--wrap截获。
  完成之前profile只能标未确认，不能据它说已协商成功。
  具体本机 `ssl.h:2124/2298/4383/5114`；公共返回的
  `mbedtls_dtls_srtp_info` 在`:1245`定义。
  可按官方类型读取 `info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile)` 枚举，
  这是声明的getter输出成员，不是猜测/读取ssl内部对象布局；不要读取/打印MKI成员。
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

## 首次新候选真机证据

`/tmp/m1-dtls.log`，ELF前缀7fb588264：配置实际通过；2秒短媒体有
100 protect成功/100对应UDP整长写入，零保护失败/短写/长度不符，PT111/SSRC6，
seq0..99、ts0..95040，rtp15/srtp25（第一包数字静音）。
这是本地保护/写出成功，仍不证明服务端解密。
随后zh_1等待20秒VAD/ASR仍0；不需为了同一未知结果先继续300秒资源loop。
该DTLS摘要4 calls/2 ok仍混HTTPS，role/profile为n/a，多record/seq解析未齐；
不能据它排除SRTP profile或方向。优先修公开映射/profile观察，保留这次日志。

不降48k、不升级peer、不预设早到HELLO/算法/profile根因，M1.5/产品HOLD保留。

### 已交新HEAD后的独立确认

DSH已交 `a3a7b9f4ccf7aeb7a25f75cc39c1010262edf570` /
[RQ6046920386](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6046920386)。
上述问题转入该 exact HEAD 的正式复核，不发送旧HEAD的在途指令。
公开 getter 片段按当前 `compile_commands.json` 的固件参数编译退出0，
见 `/tmp/stackchan-public-dtls-getters.cc`、`.o`；因此 n/a 是尚未实现，非接口不可用。
另外100包在 zh_1之前，仅覆盖前置静音；本轮应继续给同一条短语音 fixture
单独观察 protect/UDP结果。详见 findings B23，不能提前排除编码/SRTP互通。

### D2 已开始后的实际签名预检（同一 Review 的纠正）

当前WIP出现数值/符号错误，构建前按本机头文件修正：

- `MBEDTLS_SSL_TRANSPORT_DATAGRAM == 1`，`STREAM == 0`，不能写反。
  判断直接使用这两个公开宏，不用裸数值与相反注释。
- `mbedtls_ssl_conf_transport` 返回**void**（ssl.h:2143），
  `__real`与`__wrap`也必须void，不读取不存在的rc来决定是否登记。
- 释放函数是 `mbedtls_ssl_config_free`（:5741），没有`mbedtls_ssl_conf_free`。
  需按真实符号wrapper/CMake，另`mbedtls_ssl_free`清context身份。
- 已存在config身份的`conf_transport`调用必须更新其transport，不应early return。
  `TransportOf`用到的CtxEntry/table先声明，避免顺序编译错误。
- 原Review需要固定小record sample ring、覆盖/溢出；当前只有累计/末条仍未齐。
  仅把满长UDP计TX成功；保存每条方向/时间/epoch/seq high-low/len/rc/errno。

这些是已授权D2-1/2实现正确性，不是新任务、参数改动或下一阶段。
