# Native Bionic regression tests

These runners build the production SDK and Core sources with local fixtures.
Keep build outputs in ignored `out/` directories or external temporary
directories. Every runner operates locally.

| Runner | Coverage |
| --- | --- |
| `run_native_bionic_tests.py` | SDK bootstrap, packet/fd adoption, loader retirement, ordinary module callbacks, ZN early entries, private TLS/dependencies/PHDR, safe SYSTEM fallback, hook leases and Core retirement |
| `run_caller_anchor_tests.py` | Linker export validation, bound libc selection, four-argument FD loading and failure without fallback |
| `run_soinfo_layout_tests.py` | Runtime ELF layout matching, ambiguous/unreadable fields, constructor-flag writes and real loader retirement |

## Integrated SDK suite

Run from the repository root with NDK 30, CMake 3.24 or later and Ninja.
The CMake project builds the pinned SDK bootstrap, both production Cores and
the production loading adapter directly. One Core driver covers adapter lifetime
and ordinary callbacks through the production Core implementation, supplying
module fds locally. Its hook
fixture verifies that no ART/PLT hook was installed before marking those unused
hooks quiescent, matching the prerequisite observed on the device.

```sh
python3 userspace/zygisk/tests/run_native_bionic_tests.py \
  --build-dir out/integration --ndk /path/to/ndk \
  --cmake /path/to/cmake --ninja /path/to/ninja

python3 userspace/zygisk/tests/run_native_bionic_tests.py \
  --build-dir out/integration --android-sysroot out/bionic
```

Both ARM64 and ARM32 compile against API31. `--abi 32` or `--abi 64` selects
one ABI. The runtime needs `system/bin/linker{,64}` and matching
`apex/com.android.runtime/lib{,64}/bionic/{libc,libdl,libm}.so`. Record the runtime
source, API level and SHA-256 values with the results. The runner uses each
executable's PT_INTERP so Bionic receives a valid AT_BASE. ARM32 execution uses
`unshare -Urpf --mount-proc` to isolate fork tests and keep guest PIDs bounded.

Retirement tests keep an unrelated host thread alive across Core shutdown and
use real `/proc/self/task` enumeration. SDK shutdown must drain participating
threads and remove its keys before the remaining Core TSD check. The host also
checks that both the Core and its caller-bridge page become unmapped. Actual
Android thread enumeration and concurrency additionally require device acceptance.
An extra Core-owned key verifies deferred retirement after SDK shutdown and a
successful retry once that key is explicitly released.

The host creates an isolated anonymous Bionic namespace, then publishes a
versioned system symbol outside it. Raw RTLD_DEFAULT lookup from the anonymous
Core must fail, while private modules must resolve both dlsym and dlvsym through
the Core's verified resident-libc caller. The probes also run after bootstrap
retirement, during background enumeration and after fork.

Fixtures check constructor counts, endpoint and module fd transfer, loader
unmapping, callbacks after handoff, root-close retention while dynamic TLS is
live, thread destructors, background PHDR enumeration, module and managed fork,
private dependency ownership, and Core unmapping followed by fork/exit. ARM32
also exercises the SDK Tango entry and continuation slot. Native ZN fixtures
cover duplicate early entries, missing entry symbols, invalid versions and
null callbacks. Ordinary fixtures drive app/server callbacks and requested
unload through the production module lifecycle.

The emulated-TLS fixture exercises a pre-execution compatibility refusal and
one SYSTEM retry. Retained and failed-close states keep their ownership records.
Production module fallbacks must be collected separately during device tests.

Native ZN now takes the generated ARM64/ARM32 AT_ENTRY path. Its fixtures open
the real loader through a verified libc caller and load native modules through
the loader's temporary SYSTEM service. The `registers.S` probe verifies the
caller-saved registers, SP, flags, representative SIMD registers and ARM32
Thumb continuation. The actual resolver mapping must disappear before Core
entry; fd cleanup, completion ownership and loader disappearance are checked.

An isolated domain named `renamed_domain` publishes conflicting versioned symbol
values, and a real call site in that domain invokes the generated stub. Native
modules still observe the libc policy's value, including after loader removal.
The process and module identifiers are changed in this case. Refusal fixtures
cover invalid descriptor/ELF, missing Bionic entries, duplicate libc candidates,
pre-existing loader, Bionic open failure, invalid Core and absent Core native
capability. `native_empty` has no DT_NEEDED libc and tests the same generated
resolver's no-libc refusal. Native IFUNC and constructor counters must stay one.
An optional second host argument points to an older `libyloader` binary; this
adds a real missing-loader-capability refusal without keeping old loader source
in the fixture tree.

Native resident tests drain background libdl/PHDR queries before invoking libdl
in a fork child. Concurrent system-loader activity at fork is outside Bionic's
async-signal-safe contract; a standalone Bionic-only reproducer is kept with the
local report. The Core/YukiPLT coordinated fork cases are tested separately.

## Focused Bionic helpers

The caller-anchor and soinfo runners retain their host and Android modes.
These focused runners require separate output directories outside the checkout.

```sh
python3 userspace/zygisk/tests/run_caller_anchor_tests.py \
  --build-dir /tmp/yz-caller-host --qemu
python3 userspace/zygisk/tests/run_caller_anchor_tests.py \
  --build-dir /tmp/yz-caller-android --ndk /path/to/ndk
python3 userspace/zygisk/tests/run_caller_anchor_tests.py \
  --build-dir /tmp/yz-caller-android --android-sysroot out/bionic
```

The focused host fixtures substitute platform inputs and extract selected
helpers. Full bootstrap and runtime behavior is covered by the integrated
CMake suite and the SDK's three Native hosts and source consumer. Actual
Zygote/ART entry, kernel-triggered Tango, HyOS services, companion transport,
OEM namespaces and long-running resident modules require device acceptance.
