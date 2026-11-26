# U1-0 第二次复核：握手观察的代际与透传

Reviewed exact HEAD: `2403b287faeefe06942ab008513472cadb787c89`。
PR1 / [RQ6137250166](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6137250166)。
结论：需要下述小范围修复；本轮仍为 U1-0，peer 保持 1.5.5。

STREAM/DATAGRAM 宏、同一成功 setup 的身份、完成后 profile 读取、真实
void/free 符号及最终 ELF 引用已经修正。旧 HTTPS `profile=0` 结论已正确撤回。
当前缺口不证明 esp_peer 或真实 SRTP 有缺陷，不重跑已通过的云端对照。

## F1 / P1：旧握手可以写入新 Arm 报告

`firmware/main/hal/webrtc/dtls_probe_core.h:490–578` 未保存入口 generation。
真实握手返回后才分类；分类与最终写报告之间还有 getter。
Arm 若发生在 real 调用内，旧调用计入新报告的 hs_calls/unknown_ctx；
Arm 若发生在 DATAGRAM 分类后、写报告前，旧 completion/profile 可进入新报告。
这些反例只重置观察窗口，不修改真实 SSL/config，不违反 Mbed TLS 对活跃配置的限制。

在入口下锁保存代际与身份，真实调用不持锁；真实调用返回后、读取 profile 前
以及最终提交下锁时核对同一代际。过期调用不得修改新报告，保留真实返回/errno。
补确定性 hook：real 内 Arm、完成 getter 内 Arm；断言新报告没有旧 completion/profile
或旧调用计数，真实函数调用及返回/errno 不变。现有人工设置 stale entry 的测试不覆盖它。
仅修该握手观察路径；RTP token/record ring 等后续 M1 项继续保持原边界。

## F2 / P2：前置计时改变真实握手入口 errno

core:493–495 保存入口 errno 后执行 now_us，再进入 real，却没有恢复入口 errno。
计时函数若改变 errno，真实调用会收到观察器的值；real 不覆盖 errno 时调用方也收到该值。
当前后置保存/恢复正确，不主张 esp_timer 已经实测发生此现象。

所有前置观察结束后，紧邻 real 调用恢复入口 errno。增加计时函数故意污染 errno、
real 记录入口且可不覆盖 errno、getter 污染 errno 的测试，验证入口与出口透传。

## F3 / P2：真实签名的编译证明与受控行为执行缺失

测试:51–68 仅把 `__real_*` 声明与手写函数类型比较，没有直接比较实际公开 API。
当前签名核对正确，但这不能支撑“真实 API 改变就编译失败”的验收主张。
将六项断言直接比较 `decltype(&__real_mbedtls_ssl_X)` 与
`decltype(&mbedtls_ssl_X)`，同步源码中的证明说明。

前轮 Review5498100335 已要求拆分编译与受控执行。编译驱动/IDF 构建仍在控制器
覆盖外；独立已编译测试二进制若不 fork/联网，通过原台账 controller 运行，cache=never。
把构建产物与输入源码指纹绑定；不以 regex 源码检查替代行为检查。
若实际 sandbox/格式不兼容，提交具体失败与限制，不能仅以“编译用了子进程”
把可拆开的独立执行排除。原台账：
`/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`。
先 status，按 README 在同台账 replan 接续；不 recover/另建/reset used。

## 不作为阻塞条件的反例

本机公开 ssl.h:2065–2072 规定活跃 session 的 config 不得修改/释放。
peer 正常释放顺序是 ssl_free→config_free，重建角色时重新 setup。
因此不要求活跃 STREAM→DATAGRAM 切换或释放 config 后继续真实握手，
不把这类非法调用序列声称为本设备故障。

## 本轮交付与后续

只修 F1–F3，保留 root 文档改动，targeted 行为验证 + IDF5.5.4 build，
commit/push，提交新 full exact HEAD 的 REVIEW_REQUEST，然后 STOP。
无串口无需等待，不能编造设备结果。Codex 复核通过后单独派 U1-1～3：
修正后 1.5.5 基线 → 同代码精确 1.5.7 对照；设备离线时可先准备两版本构建与归档，
实测/采用仍等真实证据。48kHz 媒体需求、WebSocket 回退及 M1.5/产品验收不变；
不 merge/发布/改上游，不扩张到其他诊断 TODO。
