# YukiLinker integration

## Exit registration

Ordinary and native production Cores use the SDK's immediate exit-registration
path. Specialization can start ART threads before post callbacks; exit cleanup
uses libc's own synchronization and does not require a single-thread window.

After a post-specialize batch successfully closes at least one module, the
ordinary Core asks Bionic to compact its cleared exit entries before Core
retirement. It validates the resident libc identity and the file-backed entry
bytes of `__cxa_atexit`, `__cxa_finalize` and `free`. It then registers
`ceil(page_size / (3 * sizeof(void *)))` temporary `free(NULL)` callbacks under
one operation-local, non-null DSO cookie, and immediately finalizes that cookie.
Bionic owns table locking, page permissions and stable compaction. A concurrent
outer finalizer can postpone compaction until that finalizer returns.

This path does not inspect libc's private table, patch code or allocate executable
memory. Callback code resides in libc even during concurrent process exit.
Partial registration failure finalizes the accepted callbacks once and reports
the accepted/requested counts and error; identity validation failure skips the
operation. Neither path retries indefinitely or prevents Core retirement. Logs
report batch completion and elapsed time, not an observed cleared-entry count.
PRIVATE finalizers, resident TLS/hook leases, SYSTEM fallback and normal exit
retain their existing lifetime rules.

See the SDK [lifetime contract](third_party/yukilinker/docs/lifecycle.md) and
[the native regression fixtures](tests/native_bionic/README.md).

The integrated endpoint, module policy and mount behavior remain owned by this
repository. SDK ABI1, HostServices1 and bootstrap wire v3 are unchanged.
