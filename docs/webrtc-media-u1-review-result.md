# U1 候选复核：固件对照成立，归档需修复

Reviewed exact HEAD: `c173d19b62673d2eac29f0116b2829cfad034ec2`。
[RQ6137813252](https://github.com/MetaAlms/StackChan/pull/1#issuecomment-6137813252)。

结论：短模式五项纠正及两版本同源构建可接受；最小锁恢复处置可接受。
**归档有两处待修，不放行按当前归档刷机。**
设备仍离线，U1-4 的最终版本采用、完整 M1 与媒体移植验收保持待验证。

## 已接受范围

实际短入口初始化媒体时钟一次，20ms同一时间轴；SHORT排除完整三clip/长循环验收；
区间写入活动不称作非静音语音证据；起点缺失/无新活动分开；ASR无结果保留partial证据。
真实sender停止→主任务等待→settle的接线保留，不扩大旧RTP诊断欠项。

两臂source fingerprint均为 `4c14aaed542797bc1f9d43fe4aa88b02654b578441b0e3162961c1057c6517eb`。
实际锁diff只有peer version/component_hash与必要manifest_hash；新版官方S3库hash匹配。
组件管理器重新配置接受该最小锁，保留20个无关组件基线值符合原任务，不需用户重复批准。
两臂构建通过；137项受控行为检查与实际源码接线检查仅证明各自范围，不证明设备通过。

## A1 / P1：归档刷写参数引用不存在的文件

`firmware/tests/archive_u1_arm.sh` 把 app bin 放在arm根，把bootloader/partition bin
扁平复制到 `bin/`。但复制的 `bin/flash_args` 原样引用：

```
0x0     bootloader/bootloader.bin
0x20000 stack-chan.bin
0x8000  partition_table/partition-table.bin
```

当前归档的 `bin/bootloader/bootloader.bin`、`bin/stack-chan.bin`、
`bin/partition_table/partition-table.bin` 均不存在；`flasher_args.json`同样保留这些路径。
结果文档指定从 `bin/flash_args` 刷写，执行会失败，归档还不能独立用于两臂测试。

修归档脚本：保持flash参数相对目录与所有bin一致，核验flash_args及JSON中每个文件引用。
修已有两臂归档时只使用**各臂自己已归档**的bin，保留hash/ELF/map/源指纹；
当前生成组件是1.5.7，不能从当前build伪造1.5.5臂，不重编媒体源码来补文件布局。
更新实际路径/产物清单与结果记录，新增实际归档引用存在、app等bin hash相符的验证证据。
这是大构建产物的检查，覆盖边界与controller兼容性如实报告。

## A2 / P2：归档自身没有兑现本机私有权限

原任务要求含凭据的归档私有。当前 `u1-archive` mode755，
`sdkconfig.local-sensitive` mode644；脚本未设私有umask/权限。
这是元数据观察，未读取或输出凭据，不主张已经发生泄露。

未来归档使用umask077；收紧**本任务**归档root为700、敏感文件为600，
覆盖两臂与含凭据的superseded候选，保留现有文件与hash。
验证权限元数据；不改全局home/Keychain/其他工作区权限，不把敏感文件放入Git/PR。

## 修复交付

只修A1/A2，保留root文档；复用本轮已通过的固件与行为检查，
完成实际归档布局/hash/权限核验及相关controller文档检查。
归档脚本/文档修复本身无需重新跑两臂固件构建或云端会话。
commit/push→新full exact HEAD REVIEW_REQUEST→STOP，Codex随即复核。
之后设备恢复可按归档跑有界两臂对照；没有设备结果不宣布正式采用1.5.7或M1通过。
