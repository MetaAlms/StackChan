# M1 设备短诊断复核：先补实际 profile 与语音写出证据

Reviewed exact HEAD: `a3a7b9f4ccf7aeb7a25f75cc39c1010262edf570`。
PR [#1](https://github.com/MetaAlms/StackChan/pull/1)，
[RQ6046920386](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6046920386)。
前轮 Review5448296466。本轮只续修 D1，M1/M1.5/产品阶段仍未放行。

已成立：host三条744包的正对照保留；H7的两个反例修复，Codex重跑
共享判据测试21 passed。真机实际连接与配置通过，前置2秒静音
protect100/UDP整长100成立。该观察不等于语音fixture写出或远端解密。
不再重复host云实验，不以300秒loop代替判别性诊断。

## D2-1 [P1] 公开接口可用，仍缺DTLS身份/实际role/profile

`dtls_short_probe.cc:187–225` 包装所有 SSL 握手，HTTPS也被计为成功；
仅用ret0置complete，未用公开completion getter。源码、RQ与findings第二个B20
宣称本机没有公开role/profile接口，事实错误。`Arm():131`清Report后自增，
generation每次都回到1。当前摘要不能据此排除profile或实际DTLS阶段。

本机IDF5.5.4/Mbed3.6.5有：
`mbedtls_ssl_context_get_config`、`mbedtls_ssl_conf_get_endpoint`、
`mbedtls_ssl_is_handshake_over`（均inline，直接调用），以及返回void的
`mbedtls_ssl_get_dtls_srtp_negotiation_result`。
读取公开返回类型 `info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile)` 枚举合法，
不能将宏名称误当“猜内存偏移”。仅完成后取profile，不取MKI。
Codex用当前firmware compile_commands中同一编译参数编译验证退出0，
片段/对象 `/tmp/stackchan-public-dtls-getters.cc`、`.o`。源码行与U/T符号证据
在 [已落盘可行性核验](webrtc-media-m1-dtls-wrap-feasibility.md)，请完整读取。

实现原Review要求的 config_defaults(DATAGRAM)/conf_transport→成功setup
固定表；free/config_free清身份，独立单调generation，表溢出/未知身份显式报告。
只将DATAGRAM context握手计入DTLS；STREAM/HTTPS独立计排除数。
同一context记录实际role、ret、耗时、handshake-over，完成后cipher/profile。
真实调用后立即保存errno再读timer/getter；原参数/返回/errno透传，锁不跨真实调用。
Transport poll进入/退出单调耗时也按原Review补齐。
**不得继续以不存在的API名称或MBEDTLS_PRIVATE为理由跳过实际profile。**

## D2-2 [P1] 当前record摘要仍把尝试当写出，解析范围不完整

`ParseDtls:62–80` 不校验version/声明长度，只看type20–23，
每datagram只取首record、seq48仅留high16。TLS/截断数据可被误计DTLS，
后续record和常见低位序号丢失；TX即便负返回也计record已发。
`recvfrom:257` 用rc而不限制实际buffer len，`LogReport`的“对端从未回应”
没有socket/context与覆盖证据。

按原Review：校验classic DTLS FEFD/FEFF、13B头与declared_len<=remaining-13，
有界迭代全部record；保存seq high16+low32、epoch/type/len、时间、原rc/errno。
区分TX尝试与实际整长、非DTLS/截断/不支持、采样溢出；RX最多min(rc,len)。
固定小ring可看时序，禁止payload/密钥/证书/MKI/凭据日志。
未取得socket/context关联时只记未知，不能给“peer从未回答”的结论。
若仅通过socket观察，保留与BIO交付/Mbed接受的区别；本次握手已成功，
BIO重装不是必要前置，只有实际RX存在但握手失败才按原Review加透明BIO观测。
对parser做非DTLS、截断长度、多record、低位非零序号与TX失败的离线验证。

## D2-3 [P1] RTP重试会丢保护成功事实，Snapshot破坏进行中记录

`ObserveSendto:409` 从state推protect_ok，而短写/负返回在429/436置kWriteFailed。
同一成功protect记录随后整长重试因此被当protect-fail，packets_written不增加；
已有 `Record.protect_ok` 应保留与尝试结果分开。
`Snapshot:219–229`无条件退休所有slot，若真实protect/send尚未结束会伪造
retired_pending/unwritten，并使原成功结果找不到record。Acquire的注释称优先
completed，实际只选最老slot，可能覆盖尚在进行的调用。

修真实状态：保护结果不可因write失败消失；正确长度整长重试首次计written、
重复不重计。非破坏快照仅处理确实到期且不在real-call中的记录；
结束窗口结算要在sender停止/join后明确执行。唯一record token绑定真实protect
返回，不能只找“同tuple最新pending”。优先释放已完成记录，活动记录丢失需
明确计覆盖缺失而不是protect失败。轻量测试驱动实际探针状态/真实API stub：
正常成功、protect-fail后write、错长度、short→full、negative→full、重复full、
protected未写退休/覆盖、pending中snapshot/新代际。测试与实现共用代码，
仅21项host测试不覆盖这些设备状态。

独立agent使用`git show`的exact源文件与API stub、ASan/UBSan已实际复现：
short→full、negative→full均protect_ok1却written0/after_protect_fail1；
真实protect未返回时Snapshot伪造pending退休，随后rc0计fail；
64环中63条completed可复用仍淘汰最老pending。旧protect跨Arm也污染新代际。
证据/可复用harness：`/tmp/stackchan-device-probe-review-a3a7b9f/`；
这是真实转换复现，不是测试手工填Report。请将修复后同类验证保存在tracked工具中。

## D2-4 [P1] 100包仅为前置静音，不能排除语音编码/SRTP互通

`/tmp/m1-dtls.log:289` 的100包摘要在`:292` zh_1开始前，首包RTP15/SRTP25。
RQ把它结合fixture VAD0归为“只剩RTP/SDP而非编码/SRTP/socket”超出证据。
SSRC6固定选择可留作规范/路由检查，但数值小不是非法条件，
该同次日志`:216–217`的Offer已经宣告`a=ssrc:6`，与探针SSRC一致，
因此也不能直接认定“设备未在SDP声明发送SSRC”。
[RFC3550 §5.1/§8](https://www.rfc-editor.org/rfc/rfc3550.html#section-8)
随机性要求用于避免会话内碰撞，不能将小值当根因。
静态key/salt方向正确，反向wildcard在本机libsrtp3保护时强制sender，
也不支持盲改密钥方向。协商profile非1才触发已记录默认80位policy条件风险；
DTLS cipher名字不是SRTP profile。见findings B23与只读role-policy报告。

本轮修诊断后构建/flash，只执行一次短真实握手+2秒静音+**zh_1与有界静音等待**，
给语音区间单独的protect/UDP输出、pts/seq/PT/SSRC与有效payload长度范围；
上下边界一致，保留失败记录。记录DTLS与RTP发送FD/目的tuple是否一致
（可输出相等/未知与匿名编号，不输出IP或凭据），区分实际测得与静态推断。
不得先盲改SSRC/profile/cipher/版本或跑300秒；若本轮发现确定错误，
修复其直接原因并短复测，记录改动变量与前后证据，不预判根因。

## 交付与范围

修正错误B20/RQ结论，编号避免两个B20；B22改为固定SSRC/SDP关联待验证。
SPEC/plan顶部标D0已通过但M1待验收，host结果顶部标最新有效744包、旧结果历史。
PR标题/正文更新为当前M1状态；不改冻结架构/验收或历史原Review。
保持48k PCM与明确重采样、esp_peer1.5.5，不改产品AudioService/UI/半双工/上游。
保留root新增docs及原日志。reconfigure/build、检查最终wrap callsite和仅一处sendto；
上述有意义离线测试/21项host回归，实际短设备日志落盘并标ELF/hash。
其余M1 item/时间范围/信号量/60ms/完整资源回归仍待后续，不宣告关闭。
commit+push，提交新完整current exact HEAD的REVIEW_REQUEST，然后STOP。
