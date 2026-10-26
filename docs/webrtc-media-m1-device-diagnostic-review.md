# M1 返回设备短诊断：exact-head 复核与修复范围

被复核 full HEAD：`7317037231fbdba21a1cc029205fca0efb2ca264`。
[RQ](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6046696531)，
前轮 Review 5448188682；分支 `feat/aliyun-omni-v2v`。

结论：**本次实际 host 三条正对照观察通过**。
原始日志和JSON逐条一致：744 accepted、零refused/exception/late、
媒体14880ms、单调wall约14882ms，三条不同item各有完整VAD/转写/关键词。
实际输入hash与归档一致，修复后使用本次生成的60包/1200ms静音。
不据此认定设备发相同字节或ESP组件根因。

**M1整体仍未通过。** 本轮转回原R3-4未完成的设备短DTLS/真实发包诊断，
并修一项host验收残余。不是M1.5或产品阶段放行。
其余设备item归属/wall-loop范围/sender信号量/60ms/回归/隔离重建留后续，
不视作解决；本轮不先运行完整300s资源验收。

## H7-1 / P1：辅助判据仍可将不同 item 拼成 PASS

`control_logic.js:31` 的 `announced.size === 0` 会接受未公告的 OLD completed。
`sameItemVad` 只检查任意item的start/stop，不检查completed自己的item。
当VAD为A start/stop、committed为B、completed为B时，当前evaluateClip仍PASS。
Codex独立不联网反例输出：

```
foreignWhenNoAnnouncement=true
differentCompletedItemAccepted=true
```

16个现有检查通过，但缺少这两个真实状态序列。
只有同一当前item的start/stop/committed/completed能组成成功条，
未公告、旧/重复item不能释放等待；等待中重新读取本条公告集合，
不能在等待之前把空集合冻结后接受任意结果。
failed保留自己的item（当前runner只push m.error，丢了item），按归属终止。
补上述反例以及failed/等待期间公告出现的实际逻辑验证；
测试必须驱动事件/状态而非仅手工给 completed=0/owned=true 后测试布尔判据。
当前已核对的三条正对照结果保留，**本轮不要求再开云会话重跑host**。
结果文档开头改为最新三条/744包；旧924包用旧静音的观察作为历史标明，
不继续混在“最终结果”开头。

## D1-1 / P1：DTLS 失败前无可判别的真实观测

设备最近run12在DTLS超时退出，当前RTP probe到M1:1366才Arm，
失败路径没有DTLS回包/握手返回/协商profile元数据。
实现已落盘的 [公开短诊断方案](webrtc-media-m1-dtls-short-probe.md)，
补充 [同步握手输入预研](webrtc-media-m1-dtls-pump-prestudy.md)。

必须：

- 独立诊断generation在Transport.Start之前Arm，失败/成功路径都输出。
- public config_defaults(DATAGRAM/endpoint)→setup映射，只比较opaque handle身份；
  HTTPS STREAM握手不能混入WebRTC成功计数。free/代际切换清登记，表溢出显式报。
- wrap真实mbedtls_ssl_handshake取原ret/单调耗时/公开handshake-over getter；
  只记录role、cipher name、选中SRTP profile枚举与完成状态，不输出密钥/证书/MKI。
- 现有lwip_sendto观察扩展DTLS TX，并wrap lwip_recvfrom记录真实RX。
  只解析有界13B classic DTLS record头，完整迭代多record；长度不足/不支持分别计数。
  保存方向/epoch/seq48 high-low/record长度/实际rc/errno/时间；不输出payload/IP/SDP密码。
- wrappers透传原参数/返回/errno，不动态分配或printf、不持锁跨真实调用。
  固定摘要+有界样本ring，报告覆盖/丢记录，不能把采样丢失当网络丢包。
- Transport一次poll进/出单调耗时记录；不能把“单次poll”注释当短耗时保证。
  同步握手BIO自己调用agent_recv，不能凭poll阻塞就宣称收包停止。

先取得UDP TX/RX、握手ret和阶段；若RX存在但仍超时，再用公开BIO注册/透传
回调计数区分进入Mbed与仅到达socket。不要在没有该证据时预设早到HELLO、
AES profile或exporter为根因，也不盲升级1.5.6/改cipher/延长timeout。
签名以本机IDF5.5.4/Mbed3.6.5头文件为准；公开inline getter不能靠--wrap截获。

## D1-2 / P1：RTP实际发送计数新增字段没有实现

`rtp_send_probe.cc`仍与1cbd79b一致：
`:123–132`过期protected记录直接释放；`:147–164`把protected当completed覆写；
`:175–178`允许protect-failed匹配，UDP满长即被计written。
header新增protected_unwritten/length_mismatch/wrote_after_protect_fail字段恒零，
Format也不打印；因此不能据这些零数值说完整覆盖。

实现真实状态转换：保护成功且 UDP请求长度==该记录srtp_len、实际rc==请求长度
才能将该record首次记written。保护失败后write只记诊断，错误长度不能成功。
重试/重复UDP不能重复计成功；未写protected退休/覆写计unwritten，
pending退休/覆盖计观察缺失。Snapshot结算到期与未决范围，Format输出实际新字段。
不改变原SRTP/socket执行与返回行为；以真实API声明为准。
轻量状态序列验证protect-fail后整长、长度不符、短写/负返回、重试、
未写退休/覆写与正常成功，确保FINAL不能借新增恒零字段误过。

## 本轮实际验证与交付

1. 修改tracked代码/patch，M1-only wrapper link flags；新增源必须reconfigure。
2. targeted逻辑检查、IDF5.5.4构建、实际link map/nm callsite证据；
   保持esp_peer1.5.5锁定及真实48k PCM，不在main编码/降采样。
3. flash后先**一次短设备尝试**，记录当前候选源/ELF、串口实际阶段、
   DTLS报告；失败仍交付报告，不盲重跑完整媒体。
4. 若握手/配置成功，在现有32KB sender上跑2秒媒体短诊断，
   交付API调用→protect→对应UDP整长的实际计数/PT/SSRC/seq/ts/长度。
   诊断不是M1最终PASS；若发现具体可修本地错误，可在本轮必要范围修后短复测，
   保留每次原始脱敏证据与失败阶段，不用自设预算替代未执行步骤。
5. 订正PR标题/描述与SPEC/PLAN开头为D0已通过、M1进行中；
   不写未完成媒体/AEC/产品已通过。发现带证据落盘firmware-findings。
6. commit+push → 当前实际新full HEAD的REVIEW_REQUEST → STOP。

只授权H7-1、D1-1、D1-2及上述必要验证/记录。
M1.5/产品AudioService/UI仍HOLD；不merge/发布/改上游，不破坏生成依赖或凭据。
