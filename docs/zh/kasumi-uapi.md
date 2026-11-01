# Kasumi UAPI 18

[English](../kasumi-uapi.md)

API 18 冻结候选以 [`uapi/kasumi.h`](../../uapi/kasumi.h) 为唯一接口定义。
集成版 Kasumi、Kagami C++ 和 Manager JNI 直接包含该头文件。内核通过
`kernel/include/uapi` 软链接引用同一目录，不维护独立的协议副本或转发头。

## 规则绕过策略

API 18 将 `kasumi_syscall_arg.type` 的 bit 0 定义为
`KSM_RULE_BYPASS_POLICY`：`type = 0` 保持原有策略，显式传 `type = 1`
才让规则的显隐全局生效。API 17 和已有控制器默认将该字段置零，旧规则
不会自动变成全局规则。结构大小和 ioctl 编码不变；该字段表示标志位，
不再用作目录项文件类型。

此位用于 `ADD_RULE`、`ADD_MERGE_RULE`、`HIDE_RULE`；未知位返回 `EINVAL`，
其他命令必须传零。合并目录生成的子规则继承此位。托管用户 hide 在
`kasumi_user_hide_arg.flags` 的同一位传入 UPSERT，QUERY 返回保存的标志。
其他托管 hide 请求的输入 flags 仍须为零；RETRY 和存储重新绑定保留原标志。

下发前必须检查 `KSM_FEATURE_RULE_BYPASS`（能力 bit 15，名称 `rule_bypass`）。
Kagami 会拒绝不支持该能力的 bypass 请求，不会静默退回普通规则。

bypass 对策略排除的进程也生效，包括特权进程和存储服务，但仍遵循 Kasumi
总开关、退出流程以及内部控制和解析的防递归保护。相邻普通规则继续遵循
原策略。此位不会改变 mount hide、maps spoof 等其他功能。

Manager 与独立 Kagami WebUI 提供模块、单条规则和用户 hide 开关，默认关闭。
模块开关作用于该模块下发的全部 Kasumi 规则；路径规则仍采用最长前缀匹配，
最终 bypass 为“模块开关或选中路径规则开关”。OverlayFS、Magic Mount 不消费
此设置。模块修改在重建规则时生效，例如 Kasumi 热挂载或下次启动。YukiSU
Manager 保存并回读确认后，会立即重放本次启动中已由 Kasumi 接管的模块规则；
挂载后端变更仍需重启。应用失败时保留已保存的设置，并提示失败，不能将保存
成功视为运行时已生效。

用户 hide 保存后立即下发，继续按现有绑定状态显示结果。Manager 底部的
“规则选项”展开后显示“新规则全局生效”，只设置下一条待添加规则，路径和
选项会跨标签保留；点击添加后才会保存并下发。修改已有规则时点击路径行
展开开关；已开启全局生效的规则在折叠状态下仍显示“全局”标记。

```sh
ksud kagami module set-bypass example on
ksud kagami module add-rule example /system/etc/example kasumi --bypass
ksud kagami hide add /data/local/tmp/example --bypass
ksud kagami hide list --details
```

独立版将命令前缀换成 `kagamid`。不带 `--bypass` 时写零；
`set-bypass example off` 关闭模块开关。模块路径规则保存布尔 `bypass`；
模块和用户 hide 的覆盖值分别在 `module_bypass.json`、`hide_bypass.json`
中保存为 `"on"`，缺省为关闭。`hide list` 保留字符串数组格式，
`--details` 返回 `{path, bypass}` 对象。`LIST_RULES` 的 ADD、MERGE、HIDE
记录末尾报告数值标志。

集成版命令行完成模块设置后，可执行 `ksud kagami module apply example`。
该命令只重放本次启动已由 Kasumi 接管的模块，不会迁移 OverlayFS 或 Magic
Mount 挂载。返回 `{"applied":false}` 表示设置留待下次启动；应用失败返回
非零状态，修复报告的错误后可重试该命令。

## 调用与版本

通过现有 KernelSU 控制 fd 调用类型为 `'S'` 的 ioctl，仅授权管理器或 root
可调用。本集成仅支持原生 64 位客户端，compat 系统调用返回 `EOPNOTSUPP`；
冲突结构使用固定宽度字段不代表开放了 compat 支持。

`KSM_PROTOCOL_VERSION` 为 18。Kagami 的普通操作要求协议版本完全匹配；
Manager 从 JNI 获取由共享头定义的期望版本。API 17 的内核和用户空间需配套更新。
KernelSU supercall 的 UAPI 版本和命令定义保持原状。

只有下述两个冲突 ioctl 绕过 Kasumi 的 VFS 初始化检查。未激活或初始化被拒绝时，
它们仍然可用，即使普通 `GET_VERSION` 调用失败也可读取告警。查询不会激活 Kasumi。

## 冲突诊断

| 命令 | 编号 | ioctl 编码 | 结构大小 | 方向 |
| --- | --- | --- | --- | --- |
| `KSM_IOC_GET_CONFLICTS` | 46 | `0x8508532e` | `kasumi_conflicts_arg`，1288 字节 | 内核至用户 |
| `KSM_IOC_ACK_CONFLICTS` | 47 | `0x4008532f` | `kasumi_conflict_ack_arg`，8 字节 | 用户至内核 |

`kasumi_conflict` 大小为 80 字节，按 8 字节对齐：

| 字段 | 偏移 | 大小 | 含义 |
| --- | --- | --- | --- |
| `sequence` | 0 | 8 | 非零、单调递增的事件序号 |
| `kind` | 8 | 4 | 下表中的冲突类型 |
| `reserved` | 12 | 4 | 零 |
| `target` | 16 | 64 | 以 NUL 结尾的模块名或配置赋值 |

快照的 `count` 位于偏移 0，零值保留字段位于偏移 4，16 个事件槽位从偏移 8 开始。
仅前 `count` 项有效，其余槽位为零；事件不保证顺序。ACK 结构仅包含偏移 0 的
8 字节对齐序号。

| 类型 | 值 | 被拒绝的操作 |
| --- | --- | --- |
| `KSM_CONFLICT_LOADED_MODULE` | 1 | 存在冲突模块时激活 Kasumi |
| `KSM_CONFLICT_BUILTIN_CONFIG` | 2 | 检测到冲突内置配置后激活 Kasumi |
| `KSM_CONFLICT_MODULE_LOAD` | 3 | Kasumi VFS 防冲突保护生效时加载冲突模块 |

当前模块黑名单为 `kasumi_lkm`、`pathmask`、`procguard`、`nomount`、
`susfs_guard_lkm` 和 `hybridmount`。内置检查读取运行内核的 IKCONFIG，识别
`CONFIG_KSU_SUSFS=y`、`CONFIG_NOMOUNT=y` 和 `CONFIG_HYBRIDMOUNT=y`。
读取失败时沿用现有行为：记录日志并允许初始化。
因此，空告警快照不能证明运行内核不存在冲突功能。

事件保存在内核内存中，生命周期与承载它的 KernelSU 模块相同。管理器重启、
Kasumi 激活失败或普通功能状态变化不会清除它们；重启设备或替换宿主模块后不保留。
相同 `(kind, target)` 共用一个槽位，每次拒绝都会更新序号。队列满时，新的事件组合
替换最旧的记录。

GET 返回一致快照，不清除记录。用户手动确认已显示的告警后，使用快照中的最大序号
执行 ACK，仅清除当前序号不大于该值的记录。读取后新发生或再次发生的冲突会保留。
ACK 零成功且不做修改；尚未发出的未来序号返回 `EINVAL`，不修改队列；无效用户缓冲区
返回 `EFAULT`。确认作用于宿主模块的全局队列，不会启用 Kasumi、停用其他组件或重试
被拒绝的操作。

Manager 保留已显示的快照，直到用户确认且 ACK 成功。返回键和点击弹窗外部不能关闭
警告；确认失败保留警告并允许重试。未知类型显示通用冲突提示。

## SELinux Guard

SELinux Guard 沿用独立 Kasumi 的能力位 10（`KSM_FEATURE_SELINUX_FIX`，
名称 `selinux_fix`）和命令 29（`KSM_IOC_SELINUX_FIX`，`_IOW('S', 29, int)`）。
传入零关闭，非零开启；proc 属性写入代理可用时才能开启，
不可用时返回 `EOPNOTSUPP`。

Kagami 通过 `enable_selinux_fix` 保存配置，默认关闭。管理器提供独立的
SELinux Guard 开关，`ksud kagami kasumi selinux-fix on|off` 可修改运行状态。
旧内核不支持该能力时不能开启，保持关闭则仍可正常应用其他配置。

Android 的 `android_vh_check_file_open` hook 为新打开的可写 proc
`attr/current` 文件安装共享 `.write` 代理。开启 Guard 后，对 SPOOF 范围
内的调用者（包括 isolated app 进程），仅为零长度或首字节为 NUL、LF 的
输入补时。非空输入通过无缺页预读读取一个字节；预读失败或偏移非零时
跳过补时。原写入函数始终以原始参数调用一次，仅在它返回 `EINVAL` 后
补时，成功写入和其他错误不补时；权限检查和返回结果仍由它负责。
selinuxfs 的 `access`、`context` 入口继续透传。Kasumi 不替换 `status`
的文件操作表，其 `.open` 回调生命周期由 SELinux Hide 自身管理。

模块参数 `kasumi_guard_pad_ns` 默认 1000 ns，有效预算最高 2000 ns，
设为零可关闭补时。每次等待使用单调时钟截止点，最多循环 256 次，出现
调度请求时退出。保留中断和抢占，因此该预算不是整次调用墙钟耗时的上限。
`GET_HOOKS` 显示代理状态、有效预算、
补时位置和 `spins`（至少执行过一次循环的等待次数）。

代理安装与 Guard 开关独立，已代理 FD 的后续写入会读取最新开关状态；
hook 注册前已打开的 FD 不覆盖。预读和原函数复制用户缓冲区是两次读取，
其间内容仍可能变化。本功能属于实验性补偿，不能保证所有设备上的
时序差异都消失。

## 挂载隐藏与保留编号

挂载隐藏仅保留原普通模式。`KSM_IOC_SET_MOUNT_HIDE` 的命令编号 25 和结构保持不变；
命名空间链接伪装 hook、等级选择器和激进模式能力标志已删除。Kagami 接受
`mount-hide off|on|normal`，Manager 仅显示一个开关。

旧配置中的 `mount_hide_mode` 在读取时被忽略，`enable_mount_hide` 保持原值。
配置查询不再输出旧字段，下次成功保存配置时清除它。新写入该字段会被视为未知配置并拒绝。

命令 38 和能力位 12 保留，不得重新分配。命令 45 留给独立版实例监视接口，本集成不实现。
API 18 没有重新编号现存命令或能力位，也没有改变现存结构大小。扩展或改变已冻结的
结构必须采用新的 ioctl 编码并重新审查协议。
