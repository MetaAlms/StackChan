# DTLS同步握手与输入pump条件预研（只读）

锁定esp_peer 1.5.5官方SHA c8650846b512e6e1375e5f78c1c41619b8d645eb；应用Transport SHA 3b0844e0b6b3908995ad4a4023b1bf7b84b62f89。官方源重新下载，dtls_common.h/dtls_srtp.c/udp.c与本机、既有v155快照逐字相同。所有文件hash见provenance.json。DTLS common SHA256 ec2bf924bf661a2674102d22a28ddf1b6b2476411a91cad9f4e267fa7233e129；libpeer_default.a SHA256 25a338fc6b05f702947c2af0e924f2dd3006503908e889dcdc512782d0ea3a58。

结论：同步握手会占住peer主poll调用，属于确实存在的执行路径；但握手BIO自己收包，因此“poll没返回→UDP没有被pump”不是必然。当前无法确认早到HELLO丢弃或某个阻塞模式导致run12超时。固件/设备/组件锁/代码/DSH/PR未操作；HOLD不变。

## 公开源码已确认

- src/dtls_common.h:533-574同步调用mbedtls_ssl_handshake，WANT_READ/WANT_WRITE后继续；启用CH BIO时WANT_READ达到8次（正在分片重组则32次）才跳出，WANT_WRITE分支没有该次数限制。:612-627 client将任何非0结果记日志并返回-1（包括WANT_READ）。此行为需要实际ret/调用耗时计数才能知道是否触发；不能把次数上限换算为固定毫秒上限。
- src/dtls_srtp.h:61-63的CH BIO条件为Mbed TLS版本<3.6.6；本机build_info.h:36-38是3.6.5。src/dtls_common.h:542-549设置会话timer、send/recv BIO，recv_timeout为NULL。:491-529 BIO直接调用cfg提供的udp_recv；它处理并返回datagram，不依赖Transport完成下一轮poll。
- :453-461对未分片DTLS保存完整UDP datagram，不只收ClientHello；设备client收到ServerHello也走这里，多record组合不会仅保留第一record。:424-432跳过非DTLS及首record长度超出datagram的输入。公开BIO没有“在ICE尚未连接时缓存早到HELLO”的状态/接口，亦不能由函数名称推断ServerHello被丢。
- src/dtls_srtp.c:158-161保存cfg role、ctx、udp_recv/send；回调具体由agent如何生成不是此源码可见内容。:190-191设置read_timeout=1000、handshake timeout min=1000/max=6000；这不是总握手deadline 6000ms。
- src/transport/udp.c:267-286底层select/recvfrom确实会读取系统socket；:314-333使用配置timeout或nowait=0超时。不能把Transport的agent_recv_timeout=100直接当成某一次完整BIO调用上限，因为中间agent可循环/筛包。

## 计时器的意义

本机IDF Mbed TLS子模块SHA ffb280bb63c78bfec1e1ab55040671768c85c923、版本3.6.5：library/ssl_msg.c:299-307读get_timer结果2表示final已到；:2229-2248在读取前检查timer并调用BIO；:2257-2272取消timer、倍增重传timeout并重发，达到max后最终返回MBEDTLS_ERR_SSL_TIMEOUT（-0x6800）。library/ssl_msg.c:486-509的max是退避重传区间上限。源码没有要求另一个独立主poll任务才能让timer经过。

本机library/timing.c:86-99和IDF port/esp_timing.c:30-46都以gettimeofday差值取elapsed，set/get_delay不创建硬件回调任务。future trace应同时记录esp_timer_get_time单调时间与timer set/get结果，以识别真实等待；当前没有时间跳变证据，不将其当新根因。

## 当前Transport与调用图

- transport-3b0844e.cc:461-503先pre-generate cert/open peer；:511-524创建poll与signal任务；:526-528延时300ms后new_connection。:355-363按esp_peer_main_loop→20ms sleep串行调用，:359“not blocking”注释不能保证各状态下调用有短耗时。
- :379-413独立signal任务等待本地SDP、执行HTTP、调用esp_peer_send_msg送answer；此时poll任务可能并行。当前没有包入站注入泵，也没有持一个Transport mutex跨越整个main_loop调用；callback中的短mtx不证明agent内部时序/锁行为。
- :360 stopping仅在下一轮入口检查；若握手尚未返回，cleanup只能等退出确认（:568-592），无法靠设置stopping取消握手。此为执行属性，未证明此次Stop卡住。
- 官方include/esp_peer.h:624-626明确default没有内部额外线程，须重复调用loop；官方example peer_demo.c:176-181使用同样20ms调用节奏。因此仅20ms delay并非误用证据。
- 仅取已锁定.a的命名符号relocation调用图（named-call-graph-disassembly.txt）：peer_main_loop +0x5e→peer_on_connecting；后者+0x6→dtls_srtp_handshake；dtls_recv +0x62→agent_recv。没有据未知agent字段offset构造内存或注入包。同步握手期间同线程的下一轮peer主状态推进/SCTP不能执行，但BIO中agent_recv可以继续收包；实际筛选与计数待测。

## 早到HELLO：已知与未知

官方最新CHANGELOG v1.5.6确实记录握手HELLO在connected之前到达会被drop并增加cache；仅确认上游存在该类缺陷。原始出处：https://raw.githubusercontent.com/espressif/esp-webrtc-solution/main/components/esp_peer/CHANGELOG.md 。1.5.5预编译agent处理ICE/check阶段的具体缓存/丢弃分支不在公开源中；不把未知offset还原成源码，也不从一句changelog认定run12触发。同样，当前设备client的ServerHello与对端ClientHello应按实际role/record类型区分，不把所有HELLO统称一种竞态。

## 最小诊断计数建议（未实施）

1. 应用poll每次入口/出口：调用序号、单调时间、耗时、前后peer state。DTLS源码handshake入口/出口：实际role、Mbed state、每轮ret及is_handshake_over。先证明时间具体停在哪次调用。
2. BIO udp_recv前后：调用序号、单调时间、请求/返回长度、原始ret；BIO delivered/skipped/pending/CH assembly计数；只记录DTLS record头的content_type、epoch、sequence、length及明文epoch0可读握手type，不输出payload/cert/random/密钥/完整SDP。encrypted record不能假装解析内部handshake type。
3. 独立weak UDP recv统计socket select/recv实际读取数、nowait与耗时；再与BIO返回关联。只有UDP已读DTLS却BIO未得，才转查agent pump/筛包；UDP无包与BIO无包不能互相证明是同一故障。
4. 包住原有timer回调只记录int/final delay与get返回值，继续原调用。结合重发record/tx长度及ret，区分无服务端回包、回包未交BIO、BIO已交但SSL拒绝/未进展、timer退避耗尽。
5. 针对早到HELLO需证明：有效DTLS入站发生在ICE/handshake ready之前、该包被消费而未稍后交付、握手所缺flight确实与它匹配。若在握手期间BIO已连续收到对应flight，则该假说受反证，应转查后续flight/接受条件。

运行trace前需由parent按已授权scope安排；此文仅准备诊断，不给设备接线/补丁，不升级1.5.6、不篡改SDP、不降48k。run12根因仍未确定。

官方源码链接：
https://github.com/espressif/esp-webrtc-solution/blob/c8650846b512e6e1375e5f78c1c41619b8d645eb/components/esp_peer/src/dtls_common.h#L533
https://github.com/espressif/esp-webrtc-solution/blob/c8650846b512e6e1375e5f78c1c41619b8d645eb/components/esp_peer/src/dtls_srtp.c#L190
https://github.com/espressif/esp-webrtc-solution/blob/c8650846b512e6e1375e5f78c1c41619b8d645eb/components/esp_peer/include/esp_peer.h#L624
https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-3.6.5/library/ssl_msg.c
