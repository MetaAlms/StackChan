# D3 / U1-0：先修真实DTLS归属，建立可升级的诊断基线

Reviewed exact HEAD: `b45aaa70ee324e2f9997412c1d573dd31b1269be`。
[RQ6047356544](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6047356544)，
前轮Review5448554040；PR1 / feat/aliyun-omni-v2v。

**结论：needs fixes。当前 profile0 不是有效的 WebRTC SRTP 协商证据。**
用户2026-10-13已授权将esp_peer1.5.7升级验证并入SPEC/PLAN。
按PLAN U1执行：本轮只修U1-0，验证后提交RQ/STOP；Codex复核通过后再派U1-1～3。
不要求整个M1通过才测新版，不把修探针与改依赖混成同一对照变量。
root已修改的SPEC/PLAN/上游验证步骤/台账为用户授权的文档，保留并纳入交付。

## D3-1 / P1：DATAGRAM和STREAM分类相反，UNSET实际来自HTTPS

本机Mbed3.6.5 `ssl.h:301–302` 为STREAM=0、DATAGRAM=1。
`dtls_short_probe.cc:467–470` 相反，所谓dtls1实际上是STREAM，
六次DATAGRAM被记为stream_excluded。288ms/RSA cipher/profile0不能证明
真正WebRTC DTLS已完成，更不能推导失败在SRTP之后。
修为公开 `MBEDTLS_SSL_TRANSPORT_DATAGRAM/STREAM` 宏，取同一成功setup
登记context的真实endpoint/completion，只有该DATAGRAM完成后才读profile。
未知身份保持未知，不能fallback只凭config身份认定有效setup。

把旧run_d2_profile_unset.log明确标为**归属错误的历史日志**，
撤回findings首条B23及RQ中的“DTLS完成/SRTP未协商/失败在DTLS后”结论，
可另发纠正评论引用本Review，不删除旧证据，不发新上游issue。
移除另一条B23编号冲突。上游ICE修复真实，当前设备媒体根因仍未确定。

## D3-2 / P1：真实API签名与身份生命周期不正确

`mbedtls_ssl_conf_transport`真实返回void（ssl.h:2143），
当前__real/__wrap写int并读不存在的ret；更新已有config transport不能early return。
释放名是 `mbedtls_ssl_config_free`（:5741），不存在conf_free；当前wrapper/CMake
用错符号，真实释放未被观测。还须wrap `mbedtls_ssl_free` 清context登记。
真实参数/返回/errno透传；调用前保entry errno，真实返回立即保errno，之后再观测。
Arm与free/新代际不能留下旧身份或让旧调用结果污染新report；固定表显式覆盖缺失。
以本机头文件/最终ELF实际callsite核验，wrapper“有定义”不等于真正有调用。

补有意义的**实际源代码**离线检查：STREAM成功被排除、DATAGRAM成功/profile1、
DATAGRAM未完成/profile不读、未知setup、conf_transport从STREAM切DATAGRAM、
free/Arm身份终止、返回及errno保持。stub采用本机API真实void/宏常量，
不能在stub里重复错误裸数值。编译驱动产生子进程，不能把整个编译+运行脚本
冒充controller覆盖；编译好的独立单进程测试程序若不fork/联网，则通过controller运行，
cache用never，复用原台账，报告编译与受控执行各自证据。

## 尚未关闭的M1项（不扩大本次U1-0修复）

重试保护事实与非破坏Snapshot已有修复，保留。当前 `my_id` 声明却未赋值/使用，
protect返回仍找同tuple最新pending；旧调用跨Arm污染g_rep仍在。绑定唯一token/gen，
观察缺失与真实protect失败分开。真实pending不能仅因TTL就当调用已结束。
`SettleWindow()`只有声明/定义，没有调用；须在sender停止/join后结算窗口。
后续复用 `/tmp/stackchan-device-probe-review-a3a7b9f/` 的API-stub反例，修复后保留tracked测试。
目标：short→full/negative→full各written1，duplicate仍1，protect-failed/错长度不成功，
in-flight snapshot与Arm/旧返回不伪造新保护失败，completed可用时不淘汰pending。

DTLS `NotePoll/NoteDtlsSocket/NoteRtpSocket`目前未接线，不能把恒零/unknown当观测。
后续完成poll耗时、DTLS/RTP实际FD与目的tuple相等/未知的匿名比较；
固定小record样本ring给方向/时间/epoch/seq48/len/实际rc/errno与覆盖，
不输出payload/密钥/证书/MKI/地址/凭据。
U1-1～3的短模式只2秒静音+zh_1+有界静音等待，语音区间单独protect/UDP及pts/seq/PT/SSRC范围；
发送失败/无识别均按实际层结束本短模式，不能再无条件进300秒loop。
BIO只在正确识别的DTLS RX存在但握手仍失败时按原方案加透明观测，不预判根因。
这些未完项继续记录，不据此宣告M1通过；**本次只派D3-1/2，先取得可靠profile归属**，
不以全部探针完善作为受控新版验证的无限前置。

## 验证、离线控制器与后续升级

当前未发现`/dev/cu.usbmodem*`；先完成可做的离线与build验证。
设备两臂实测留给U1-1～3统一安排；本轮如实提交“诊断代码已修/待设备实测”，
保留修复代码/构建证据，不能编造profile或升级结果，也不等待串口死循环。
controller首次为本任务登记的固定台账为：
`/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`。
先status，复用原路径/epoch/used；兼容单进程离线检查按README通过controller启动。
新增check登记封闭时在同台账按replan接续，不recover清预算/另建台账。
C++编译/子进程构建、设备和云调用不在控制器覆盖内，分别报告。

保持1.5.5完成本repair；精确1.5.7升级已授权且排入U1后续，不在本轮同时改。
48k PCM与重采样、WebSocket回退、M1.5/产品HOLD保留；不merge/发布/改上游。
依据PLAN更新PR当前状态，源码修复+相关离线检查+reconfigure/build，
commit/push、新full exact HEAD REVIEW_REQUEST，然后STOP，Codex继续安排U1。
