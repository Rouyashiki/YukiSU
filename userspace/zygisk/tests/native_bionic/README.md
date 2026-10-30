# Native Bionic regression tests

These runners exercise production helpers with local fixtures. Build outputs
must stay outside the checkout. None of the runners accesses a device.

| Runner | Coverage |
| --- | --- |
| `run_native_bionic_tests.py` | Module identity, local dependencies, duplicate/invalid loads, PLT callbacks and handle lifetime |
| `run_caller_anchor_tests.py` | Linker export validation, bound libc selection, four-argument FD loading and failure without fallback |
| `run_soinfo_layout_tests.py` | Runtime ELF layout matching, ambiguous/unreadable fields, constructor-flag writes and real loader retirement |
| `run_loader_handoff_tests.py` | Anonymous Core loading, Hook/Unhook, private runtime cleanup and self-unmapping |

Run the first three runners from the repository root:

```sh
runner=userspace/zygisk/tests/run_caller_anchor_tests.py

# ASan/UBSan on the host, plus ARM64 and ARM32 Linux QEMU.
python3 "$runner" --build-dir /tmp/yz-handoff-host --qemu

# Build both Android ABIs with NDK 30, then run them on a local Bionic runtime.
python3 "$runner" --build-dir /tmp/yz-handoff-android --ndk /path/to/ndk
python3 "$runner" --build-dir /tmp/yz-handoff-android \
  --android-sysroot /path/to/aosp-runtime
```

The anonymous handoff runner supports only the NDK build and Android sysroot
commands above. Build the production YukiPLT archives with `scripts/build.ps1`
first. It retains cold and exception-runtime paths, without simulated handoff
or system-loaded Core variants.

Use a different output directory for each runner. The Android sysroot needs
`system/bin/linker{,64}` and matching
`apex/com.android.runtime/lib{,64}/bionic/{libc,libdl,libm}.so`.
Older ARM32 Bionic requires PIDs below 65536; on a busy host, run QEMU in a
fresh local PID namespace with `unshare --pid --fork --mount-proc`.

The host fixtures substitute platform inputs and extract selected production
functions; they do not emulate Bionic namespaces. Android native module
fixtures use the real Bionic loader. The handoff fixture loads its Core with
YukiLinker and checks actual unmapping after Hook/Unhook.

Native modules keep Bionic identities and file mappings while the Core may
remain anonymous. Successful loading does not prove module initialization or
OEM namespace compatibility. Device acceptance, including Sui/ADB Root, is
separate from these offline regressions.
