# U1-0 复核结果

2026-10-13，Reviewed exact HEAD `bcd4e35ebeecab620debbb572277682d99959799`。
[RQ6137430285](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6137430285)，
修复来源 Review5498323405 / full `2403b287faeefe06942ab008513472cadb787c89`。

**结论：F1–F3 无阻塞，U1-0 通过；放行 U1-1～3。**
这只确认诊断源码与离线/构建证据，不是设备互通、M1 或完整移植通过。

Codex 与独立只读复核核对最终源码：入口固定握手代际与身份，real 不持锁；
返回后/profile 前及最终提交核对代际，过期结果不写新报告。
紧邻 real 恢复入口 errno，返回立即保存 real errno；六项类型断言直接比较实际 API。
不设向新报告写入的 stale 计数符合本轮隔离要求，不需要追加清理提交。

Codex 读取原台账 action `u1-d3-behaviour-3` 的真实日志：10 个场景，177/0 PASS，
cache=never，fingerprint `949441e279b1f2dd31dc1a889d691ad12b0c29d75b69bd91da20627fc7c1f264`。
行为产物 SHA256 `d27a7bc8ef6ddf63ca6f5a10afa3ee4ccd46325d539a1ac554adfd83563e04db`；
最终 core/test/runner 指纹与 RQ 及独立审阅一致。编译与 IDF5.5.4 build 独立报告，
不冒充 controller 覆盖；原台账 epoch4 / used9/24，失败历史保留。
后续行为检查继续用 build-only + controller，不将 runner 的直接执行当受控证据。

本机最终固件 ELF SHA256 `366339161d57ac505df50a04af950f89a4fce02b1415f22d826b8ee1169a104f`，
bin SHA256 `7f268cac2d71f176168c59cb1900ab49ccc7d88bd0de6e56987c453979197e52`。
map 保留真实 DTLS/esp-tls wrapper 引用。peer 仍精确1.5.5，依赖未在本轮混改。
设备仍离线：没有新的真实 profile、session 或语音结果，旧 HTTPS UNSET 结论保持撤回。

下一步为独立新任务 [U1 任务书](webrtc-media-u1-task.md)：
先冻结两臂共用短模式，再顺序构建/归档1.5.5与精确1.5.7，设备恢复后按各臂归档实测。
1.5.7 正式采用待 U1-4；48kHz 媒体与重采样、WebSocket 回退、M1.5/产品要求不变。
不 merge/发布/改上游，不修扩张的其他 M1 TODO。
