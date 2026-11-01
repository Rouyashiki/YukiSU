# ksud terminal interface

Use `ksud --help`, `ksud module --help`, or `ksud help feature set` to inspect
commands. Help is handled before the selected command executes. The core command
parser rejects unknown options, missing values, and extra operands. Use `--`
before positional values that begin with `-`.

Global options work before the command, between subcommands, or after operands:

```sh
ksud --color=auto module --help
ksud --color=never feature list
ksud module install example.zip --verbose
ksud kagami module set-mode --help
```

Color modes are `auto`, `always`, and `never`. Automatic color requires a terminal
on the relevant output stream and a nonempty `TERM` other than `dumb`. A nonempty
`NO_COLOR` disables automatic color. Explicit color options take precedence.
Diagnostics go to stderr; existing command results and JSON remain on stdout.
Interactive diagnostics omit routine internal logs unless `--verbose` is used.

Core argument errors exit with status 2; help exits with status 0. Command-specific
runtime statuses and the existing `su` error status remain unchanged. Embedded
third-party tools retain their own argument grammars. First-party commands,
including boot patching, plugin configuration, UTS templates, and Kagami, use the
shared help and validation layer. Kagami arguments are checked before contacting
or starting its daemon; `daemon call` also validates the nested command. New
configuration patches validate field names, JSON types, enum values, paths, and
numeric ranges before taking the configuration lock. Existing stored configuration
keeps its legacy loading behavior.

Arguments after the module path in `ksud insmod` are passed to the loader verbatim;
use `ksud insmod --help` to inspect that command. Use `--option=-value` when an
option's value starts with a dash. Empty UTS field values are accepted and restore
that field; repeated `--inherit` options may restore several distinct fields.
Module configuration uses a single-line key/value format: keys cannot contain
`=` or line breaks, and values cannot contain line breaks. Plugin configuration
uses JSON and continues to support multiline values.

```sh
ksud plugin config --id example set -- key --literal-value
ksud uts-view set-global --release=-custom --inherit version --inherit nodename
```

Missing or unsuitable input files are rejected before module installation or
boot-image work begins. Input files must also be readable; raw block-device input
is reserved for boot images and partition flashing. Optional installer files are
staged and renamed over their destination entries, so existing symlink or hardlink
targets are not truncated. Runtime diagnostics include the operation and path where
available; common filesystem failures include a short recovery hint. Flashing
progress and success messages go to stderr. JSON and daemon response envelopes
are not decorated.

## Interactive su shells

For an interactive system mksh or the bundled BusyBox ash, `su` loads the shipped
`/data/adb/ksu/bin/shellrc.sh` when available. This asset is extracted with the
other userspace assets during installation. Without it, the system shell's normal
initialization is used.

The mksh setup reads `/system/etc/mkshrc` first. Both mksh and the bundled BusyBox
ash show the device codename and full current directory, for example
`popsicle:/data/data/com.termux/files/home # ` or `popsicle:/ # `. The codename is
read once at shell startup from `ro.product.device`, falling back to `HOSTNAME`
and then `android` if unavailable. The prompt omits the username and preserves
the full path without HOME abbreviation or long-path shortening. A failed
command's status appears before the next prompt without changing `$?`.
mksh expands its prompt in-process; ash uses a command substitution containing
shell builtins only. Color follows terminal capabilities and `NO_COLOR`.
Interactive `ksud debug su` uses the same startup setup; its noninteractive
execution path is unchanged. Prompt setup checks
access after the requested identity/context change and falls back to the system
startup when the target user cannot read the shipped rc. Exported generated
prompts remain usable in nested shells. Control characters in displayed paths are
replaced, and ash prompt escapes cannot be injected through directory names.

Put personal shell settings in `/data/adb/ksu/.shellrc`, which is sourced last and
is not overwritten by feature saves or asset updates. For example:

```sh
PS1='[${PWD}] # '
```

An explicitly supplied `ENV`, exported `PS1`, `su -p`, or a custom shell keeps its
own configuration. `su -p` preserves the identity variables and does not select a new startup file;
the existing PATH and ASH_STANDALONE setup still applies. An exported PS1 is also
retained when `su -p` inherits the shipped rc. `su -c` and directly executed commands do not receive the new
interactive setup. An inherited `ENV=/data/adb/ksu/.ksurc` from older ksud versions
is recognized and replaced for ordinary interactive shells.

`/data/adb/ksu/.ksurc` remains the existing feature configuration file. It is no
longer used as a shell startup script.
