# Kasumi UAPI 18

[简体中文](zh/kasumi-uapi.md)

The API 18 freeze candidate is defined by [`uapi/kasumi.h`](../uapi/kasumi.h).
Integrated Kasumi, Kagami C++, and Manager JNI include this header directly.
Kernel include paths resolve it through `kernel/include/uapi`, a symlink to the
same directory. There is no separate Kagami or kernel copy of the definitions.

## Transport and versioning

Use the existing KernelSU control fd with ioctl type `'S'`. The caller must be
an authorized manager or root. Compat syscalls return `EOPNOTSUPP`; this
integration supports native 64-bit clients. The fixed-width conflict records
do not change that transport restriction.

`KSM_PROTOCOL_VERSION` is 18. Kagami requires an exact protocol match for normal
operations. Manager obtains its expected version from JNI, which reads the
shared header. API 17 kernel/userspace combinations must be updated together.
The KernelSU supercall UAPI version and command definitions are unchanged.

Only the two conflict ioctls below bypass Kasumi's VFS initialization gate. They
remain available before activation and after initialization is refused, even
when the ordinary `GET_VERSION` ioctl fails. Reading them does not enable Kasumi.

## Rule policy bypass

API 18 uses bit 0 of `kasumi_syscall_arg.type` as `KSM_RULE_BYPASS_POLICY`:
`type = 0` preserves the existing policy, and `type = 1` explicitly opts a rule
into global visibility or hiding. API 17 and existing controllers initialize
this field to zero. The structure size and ioctl encodings do not change.
This field is a flag mask, not a directory-entry file type.

The flag applies to `ADD_RULE`, `ADD_MERGE_RULE`, and `HIDE_RULE`; other bits
are rejected with `EINVAL`. Non-rule commands require zero. Materialized merge
children inherit the parent flag. Managed user hides use the same bit in
`kasumi_user_hide_arg.flags` on UPSERT, and QUERY returns the saved flags.
Other managed-hide requests still require zero input flags. RETRY preserves
the saved flag, including across storage rebinding.

Check `KSM_FEATURE_RULE_BYPASS` (feature bit 15, name `rule_bypass`) before
requesting bypass. Kagami rejects requests when this capability is unavailable
instead of silently treating them as ordinary rules.

Bypass rules apply even to processes excluded by policy, including privileged
and storage-service observers. They still obey Kasumi enablement, teardown,
and internal control/resolution recursion guards. Ordinary sibling rules keep
their existing policy. Bypass does not change mount hiding, maps spoofing, or
other unrelated features.

Manager and Kagami WebUI expose module and individual-rule switches, initially
off. The module flag applies to every Kasumi rule owned by that module; a
single path rule uses the existing longest-prefix selection. Effective bypass
is the module flag OR the selected path-rule flag. OverlayFS and Magic Mount
do not consume this setting. Module edits take effect when the module rules
are rebuilt, such as a Kasumi hot mount or the next boot. After saving and
verifying the stored settings, YukiSU Manager immediately replays rules for
modules already owned by Kasumi in the current boot. Mount backend changes
still require a reboot. If application fails, the saved settings remain and
Manager reports the failure; a successful save alone does not mean the
runtime rules have changed.

Saved user hides are submitted immediately and retain their existing
binding-status reporting. Expand Manager's bottom "Rule options" to reveal
"Make the new rule global", which configures the next rule to add. Its path
and option survive tab changes, but are saved and submitted only after
tapping Add. Tap an existing rule's path row to expand its switch. Enabled
global rules retain a "Global" label while collapsed.

CLI examples (`ksud kagami` in YukiSU, `kagamid` standalone):

```sh
ksud kagami module set-bypass example on
ksud kagami module add-rule example /system/etc/example kasumi --bypass
ksud kagami hide add /data/local/tmp/example --bypass
ksud kagami hide list --details
```

Omitting `--bypass` writes zero; `set-bypass example off` disables the module
flag. Module rules persist a boolean `bypass`. Module-wide and user-hide
overrides are stored as `"on"` entries in `module_bypass.json` and
`hide_bypass.json`; absence means off. `hide list` retains the legacy string
array, while `--details` returns `{path, bypass}` objects. `LIST_RULES` reports
the numeric flags after ADD, MERGE, and HIDE records.

After saving module settings through the integrated CLI, run
`ksud kagami module apply example`. This replays only modules owned by Kasumi
in the current boot and does not migrate OverlayFS or Magic Mount mounts.
`{"applied":false}` means the settings await the next boot. Application
failure returns a nonzero status; resolve the reported error and retry the
command.

## Conflict diagnostics

| Command | Number | Encoded ioctl | Payload | Direction |
| --- | --- | --- | --- | --- |
| `KSM_IOC_GET_CONFLICTS` | 46 | `0x8508532e` | `kasumi_conflicts_arg`, 1288 bytes | kernel to user |
| `KSM_IOC_ACK_CONFLICTS` | 47 | `0x4008532f` | `kasumi_conflict_ack_arg`, 8 bytes | user to kernel |

`kasumi_conflict` is 80 bytes, aligned to 8 bytes:

| Field | Offset | Size | Meaning |
| --- | --- | --- | --- |
| `sequence` | 0 | 8 | Nonzero monotonically increasing event sequence |
| `kind` | 8 | 4 | Conflict kind below |
| `reserved` | 12 | 4 | Zero |
| `target` | 16 | 64 | NUL-terminated module name or configuration assignment |

The snapshot contains `count` at offset 0, a zero reserved word at offset 4,
and 16 event slots at offset 8. Only the first `count` entries are valid;
unused slots are zero. Entries have no ordering guarantee. The ACK payload
contains only an 8-byte aligned sequence at offset 0.

| Kind | Value | Refused operation |
| --- | --- | --- |
| `KSM_CONFLICT_LOADED_MODULE` | 1 | Kasumi activation while a conflicting module is present |
| `KSM_CONFLICT_BUILTIN_CONFIG` | 2 | Kasumi activation after detecting a conflicting built-in configuration |
| `KSM_CONFLICT_MODULE_LOAD` | 3 | Conflicting module load while the Kasumi VFS guard is active |

The blacklist currently contains `kasumi_lkm`, `pathmask`, `procguard`, `nomount`,
`susfs_guard_lkm`, and `hybridmount`. Built-in checks recognize `CONFIG_KSU_SUSFS=y`,
`CONFIG_NOMOUNT=y`, and `CONFIG_HYBRIDMOUNT=y` in the running kernel's IKCONFIG.
If that configuration cannot be read, the existing guard logs the failure and
permits initialization; an
empty conflict snapshot is not proof that the running kernel contains no
conflicting functionality.

Events live in kernel memory for the lifetime of the hosting KernelSU module.
They survive Manager restarts, failed Kasumi activation, and ordinary feature
state changes. They do not persist across a reboot or host module replacement.
Repeated `(kind, target)` pairs occupy one slot and receive a new sequence for
each refused attempt. At capacity, a new pair replaces the oldest entry.

GET returns a coherent snapshot without clearing it. After the user confirms
the displayed warning, ACK its maximum sequence. ACK clears events whose
current sequence is at most that value. A repeated or new conflict occurring
after the snapshot therefore survives an older ACK. Zero is a successful no-op;
a future sequence fails with `EINVAL` without changing the queue. Invalid user
buffers fail with `EFAULT`. Acknowledgement is global to the host module and
does not enable Kasumi, disable another component, or retry a refused operation.

Manager keeps the displayed snapshot until explicit confirmation succeeds.
Back and outside clicks cannot dismiss it. An acknowledgement failure leaves
the warning available for retry. Unknown kinds receive a generic warning.

## SELinux Guard

SELinux Guard retains standalone Kasumi's feature bit 10
(`KSM_FEATURE_SELINUX_FIX`, name `selinux_fix`) and command 29
(`KSM_IOC_SELINUX_FIX`, `_IOW('S', 29, int)`). Zero disables the guard; nonzero
enables it when the proc-attribute write proxy is available.
An unavailable guard rejects enable requests with `EOPNOTSUPP`.

Kagami persists `enable_selinux_fix`, defaulting to false. Manager exposes an
independent SELinux Guard switch; `ksud kagami kasumi selinux-fix on|off`
changes the runtime state. On older kernels without the capability, leaving
the guard disabled remains compatible with other feature settings.

The Android `android_vh_check_file_open` hook installs a shared `.write`
proxy on newly opened writable proc `attr/current` files. With Guard enabled,
SPOOF callers, including isolated app processes, receive bounded padding for
zero-length writes or inputs whose first byte is NUL or LF. The proxy peeks
one byte without faulting, then calls the original writer once with the
original arguments. A failed peek or a nonzero offset bypasses padding.
Padding runs only after the original writer returns `EINVAL`; successful
writes and other errors receive no padding. The original handlers retain
all permission checks and return values. The selinuxfs `access` and `context`
hooks remain passthrough. Kasumi leaves `status` file operations untouched
so SELinux Hide retains control of its `.open` callback lifetime.

The module parameter `kasumi_guard_pad_ns` defaults to 1000 ns and is capped
at 2000 ns; zero disables padding. Each wait uses a monotonic deadline and
at most 256 iterations, and stops on a rescheduling request. Interrupts and
preemption remain enabled, so this is not an upper bound on total wall time.
`GET_HOOKS` reports proxy availability, effective budget, placement, and
`spins`, the number of waits that executed at least one loop iteration.

The proxy is installed independently of the feature switch, so writes on an
already proxied FD observe later switch changes. FDs opened before hook
registration are not covered. The nofault peek and the original user copy
are separate reads, so their contents may differ. This is experimental
compensation, not a guarantee that timing differences disappear on every device.

## Mount hiding and reserved slots

Mount hiding now has one behavior, the former normal mode. Its existing
`KSM_IOC_SET_MOUNT_HIDE` command 25 and payload are unchanged. The namespace-link
projection hook, mode selector, and aggressive capability have been removed.
Kagami accepts `mount-hide off|on|normal`; Manager exposes one switch.

Existing configuration files may contain `mount_hide_mode`. Loading ignores
that field while preserving `enable_mount_hide`. Configuration output omits it,
and the next successful configuration save removes it. New writes to the
removed key are rejected as unknown.

Command 38 and feature bit 12 remain reserved and must not be reassigned.
Command 45 remains reserved for the standalone instance-watch interface and
is not implemented by this integration. No retained command, feature bit, or
pre-existing structure has been renumbered or resized for API 18. Extending or
changing a frozen payload requires a new ioctl encoding and protocol review.
