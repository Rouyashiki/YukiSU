# Lua 插件兼容说明

兼容基线：YukiSU `cb73bef9`、[FolkPatch `0f3f42ba`](https://github.com/LyraVoid/FolkPatch/tree/0f3f42ba02d6441dbbeb247f34c0f5eabc14e393)、[FinaSU `53fcde65`](https://github.com/LyraVoid/FinaSU/tree/53fcde6595a077f7ab2554277c33d08a6e1185a8)。
YukiSU 使用一套 Lua 5.5 行为，尽量兼容两家的源码插件和共同 API，继续保留自己的扩展。

## 清单、配置和管理器

- `min_version` 可以省略、为 `null`，或为合法非负 32 位整数。保留并在列表中输出，但不限制安装、启用或运行。
- `entry` 省略或为 `null` 时使用 `main.lua`。入口仍须为包根下的安全文件名。
- 配置键是非空 UTF-8 文本，最多 64 字节，不含 `/`、`\`。空格、中文、冒号均保留，不自动 trim。
- `callbacks` 保留声明状态并去重，列表始终输出规范化数组。未声明时，`service` 自动发现并启动 `main`；显式声明时仅在包含 `main` 才自动启动。空数组不启动 `main`。普通阶段仍查实际 Lua 回调表。
- `has_action`、`callbacks`、`quick_action` 沿用显式元数据语义。无操作元数据的旧插件使用 `action_unknown`，管理器提供“运行”入口；点击后执行 `action` 并报告缺失或失败。列表刷新不执行 Lua 入口。
- `quick_action` 接受字符串或对象，快捷按钮调用其中的 `function`；`plugin action <id>` 始终调用 `action`。`has_action` 表达 `action` 能力，其他名称的快捷操作单独显示。
- 管理器的“恢复默认”仅修改编辑草稿；取消不会写配置。保存时先删除恢复项，再写变化项。部分失败列出全部失败键，并重新读取已保存值；读取失败时关闭编辑器并报告错误。
- `config[].default` 仅作界面展示，未保存或删除后的键在 Lua 中仍返回 `""`。损坏配置不会被静默覆盖。

ZIP 支持平铺根目录和单层包裹目录。事务发布、已有配置和日志迁移、执行权限、路径和解压限制保持原有行为。

## API 参数与返回值

共同接口包括 `info`、`warn`、`getprop`、`setprop`、`exec`、`read_file`、`write_file`、`sysctl`、`chmod`、`mkdir`、`rm`、`file_exists`、`list_dir`、`get_config`、`set_config`、`json_decode`、`json_encode`、`start_daemon`。

- 字符串参数接受 Lua 自身的数字到字符串转换，例如 `set_config("count", 3)`、`exec("sleep", 1)`。不对布尔、table、function 自动调用 `tostring`。
- `mkdir(path, recursive)` 的第二个参数遵循 Lua 真值：缺省、`nil`、`false` 为非递归，其余值包括 `0` 和空字符串为递归。
- `chmod` 的字符串模式按八进制解析，允许首尾 ASCII 空白；整数仍按数值解释，范围为 `0..07777`。
- `start_daemon` 接受数字和 Lua 可转换的数字字符串；有限小数向零截断，小于 1 的结果归一为 1 秒。拒绝非有限值和超出有符号 64 位范围的值。内部按短段等待，大间隔不会溢出或阻碍停用。
- 回调名为 1–64 字节的 `[A-Za-z0-9_.:-]+`，按 Lua table 的完整键查找，不解释为 Lua 表达式。
- 回调名以 `-` 开头时，手动 CLI 调用应在位置参数前加 `--`，例如 `ksud plugin run -- example --help`。管理器和 daemon 内部转发自动添加分隔符，确保这些名称按回调键执行。

## 命令和 daemon

```sh
ksud plugin daemon example worker.tick 60
ksud plugin daemon example worker.tick -- -2
ksud plugin run example worker.tick
ksud plugin config --id example delete '配置: key'
```

公共 `daemon` 命令的秒数为有符号 64 位十进制整数，零和负数归一为 1。它与 Lua `start_daemon` 使用相同的单实例锁和就绪握手。`--ready-fd` 形式仅供内部重执行使用。

daemon 锁按插件 ID、回调名分别编码，避免 `a.b/tick` 与 `a/b.tick` 混淆。仍兼容旧的 `<id>.<callback>.daemon.lock`：旧实例启动检查、停用、更新和卸载均保留；可表达的回调同时持有新旧锁，防止旧宿主重复启动。更新在原子替换前停止等待中的新旧实例；正在执行回调时仍拒绝更新。停止实例继续使用锁状态、进程组和 pidfd 核验，不使用文件名前缀或裸 PID 批量终止。

`exec` 用关闭于 exec 的状态管道区分程序启动失败与真实退出 127。管道、fork、初始化、exec、输出读取和等待失败抛 Lua 错误；成功启动后的普通非零退出仍返回结果表。stdin 为 `/dev/null`，继承环境、工作目录和回调进程组。stdout/stderr 分批读取，保持原有每路 4 MiB 截断策略；错误路径关闭自有描述符并回收自有子进程。

## 固定的 YukiSU 行为

| 项目 | 行为 |
| --- | --- |
| `set_config` 持久化失败 | 返回 `false` 并记录原因；参数错误抛错 |
| 属性设置内部失败 | 保持布尔返回契约 |
| `exec` 信号退出 | `code = 128 + signal` |
| 文件和命令输出 | 保留原始字节，不替换非法 UTF-8 |
| 覆盖已有文件 | `write_file` 保留原有权限 |
| 输出超限 | 截断并继续排空，不因超限终止进程 |
| JSON 编码 | 保留完整键检查和混合键处理，不静默丢字段 |
| CLI 缺失回调 | 退出码 `1`，提供明确错误信息 |
| 插件依赖 | 保留安装、启用、运行时的依赖检查 |
| Lua 状态 | 每次回调、每轮 daemon 新建，持久状态请使用文件或配置 |

继续支持 `post_mount`、完整 Lua 标准库、包内模块搜索路径、`PLUGIN_CODE_DIR` 和多仓库。没有移植 FinaSU 包指纹放行或每次开机阶段去重策略，也没有添加 FolkPatch 旧模块 Lua 路径的 `modules`、`install_module`、`getConfig/setConfig`。

## Lua 版本边界

不增加 Lua 5.4 运行时，不承诺 Lua 5.4 原生 `.so` ABI 或预编译字节码兼容。
源码也必须符合 Lua 5.5 语法：FolkPatch 基线中的 `boot-script` 示例在循环内重新赋值 `for` 控制变量 `line`，Lua 5.5 会报告 `attempt to assign to const variable 'line'`。该未修改示例作为已知语言边界回归保留；不通过改写插件源码或放宽运行时隐藏此差异。

## 验证入口

Linux 宿主需要 CMake、C/C++ 编译器、Python 3 和 zlib 开发包；Lua、libzip 等复用仓库内构建方式。

```sh
cmake -S userspace/ksud/tests/plugin -B build/plugin-compat
cmake --build build/plugin-compat -j
ctest --test-dir build/plugin-compat --output-on-failure

cmake -S userspace/ksud/tests/soft_reboot -B build/soft-reboot
cmake --build build/soft-reboot -j
ctest --test-dir build/soft-reboot --output-on-failure
```

测试直接编译生产插件实现和顶层 CLI 解析器，覆盖命令解析、转发和插件处理的完整路径。插件、暂存、锁和 daemon 路径仅在非 Android 测试编译中指向构建目录；系统属性、SELinux、namespace/cgroup 交互由夹具提供。命令故障测试通过链接包装注入系统调用失败，检查描述符和子进程回收。

验收包括清单和配置边界、参数转换、阶段调度、旧插件操作、锁碰撞和旧锁迁移、启动超时、更新/卸载、二进制输出和输出截断。未修改的 FolkPatch `cache-cleaner` 在模拟命令下运行，FinaSU 最小插件验证配置与 greeting 文件；来源及原始哈希见[样例说明](../userspace/ksud/tests/plugin/fixtures/README.md)。

管理器测试：

```sh
cd manager
./gradlew :app:testDebugUnitTest \
  --tests com.anatdx.yukisu.ui.viewmodel.PluginCompatibilityTest \
  --tests com.anatdx.yukisu.ui.screen.PluginConfigDialogTest \
  :app:lintRelease
```

宿主测试、Android ARM64 构建和静态检查不能替代设备运行验证；本次兼容修改不包含设备部署。
