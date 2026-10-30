"""Exercise anonymous YukiLinker loading, hook recovery and Core self-unmapping."""

import argparse
import os
from pathlib import Path

from run_native_bionic_tests import run


def build_one(root, build, compiler, flags):
    build.mkdir(parents=True, exist_ok=True)
    core = root / 'userspace/zygisk/core/src'
    fixture = root / 'userspace/zygisk/tests/native_bionic'
    (build / 'bootstrap_loader_under_test.inc').write_text(
        (core / 'yukilinker.cpp').read_text())
    command = [str(compiler), '-std=c++17', '-O1', '-g', '-fno-exceptions', '-fno-rtti',
               *flags, '-I' + str(build), '-I' + str(core), '-I' + str(root)]
    shared = [*command, '-shared', '-fPIC', '-fvisibility=hidden',
              '-fno-c++-static-destructors', '-ffunction-sections', '-fdata-sections',
              '-Wl,--gc-sections', '-Wl,-z,now', '-Wl,-z,defs', '-ldl']
    run([*shared, '-Wl,--wrap=pthread_key_create', '-Wl,--wrap=pthread_key_delete', '-DYUKILINKER_BOOTSTRAP=1', fixture / 'handoff_loader.cpp',
         root / 'userspace/zygisk/endpoint.cpp', root / 'userspace/common/cxx_slim.cpp', core / 'core_bindings.cpp',
         '-o', build / 'loader.so'])
    abi64 = any('aarch64' in flag for flag in flags)
    archive = core.parent / ('build' if abi64 else 'build-armv7') / 'yukiplt/src/libyukiplt.a'
    if not archive.is_file():
        raise RuntimeError('Build the production archive with scripts/build.ps1 first')
    run([*shared, '-fvisibility=default', '-Wl,-z,max-page-size=16384', fixture.parent / 'plt_provider.cpp', '-Wl,-soname,libanon_provider.so',
         '-o', build / 'libanon_provider.so'])
    run([*shared, '-fvisibility=default', '-Wl,-z,max-page-size=16384', fixture.parent / 'plt_target.cpp', '-L' + str(build), '-lanon_provider',
         '-Wl,-soname,libanon_target.so', '-o', build / 'libanon_target.so'])
    run([*shared, '-fexceptions', '-DYUKIPLT_STATIC=1',
         '-I' + str(core.parent.parent / 'third_party/yukiplt/include'),
         '-Wl,--wrap=pthread_key_create', '-Wl,--wrap=pthread_key_delete',
         '-Wl,--wrap=dl_iterate_phdr', '-Wl,--wrap=dl_unwind_find_exidx',
         '-Wl,--wrap=' + ('_Znwm' if abi64 else '_Znwj'),
         '-Wl,--exclude-libs,ALL', '-Wl,-Bsymbolic-functions',
         '-Wl,-z,max-page-size=16384', '-Wl,-Map,' + str(build / 'anonymous.map'),
         fixture / 'handoff_payload.cpp', core / 'core_bindings.cpp', core / 'loader_handoff.cpp',
         core / 'self_unmap.S', core / 'jni_entry.S', archive,
         root / 'userspace/common/cxx_slim.cpp',
         '-Wl,-soname,libanonymous_retire.so', '-o', build / 'libanonymous_retire.so'])
    run([*command, fixture / 'handoff_bionic.cpp', '-ldl',
         '-o', build / 'handoff_bionic'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', required=True, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--ndk', type=Path)
    mode.add_argument('--android-sysroot', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    out = args.build_dir.resolve()
    if out.is_relative_to(root):
        parser.error('Build outputs must be outside the checkout')
    if args.ndk:
        tag = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
        compiler = args.ndk / 'toolchains/llvm/prebuilt' / tag / 'bin' / ('clang++.exe' if os.name == 'nt' else 'clang++')
        for abi, target in [('arm64-v8a', 'aarch64-linux-android31'),
                            ('armeabi-v7a', 'armv7a-linux-androideabi31')]:
            build_one(root, out / abi, compiler, ['--target=' + target, '-static-libstdc++'])
    else:
        for abi, emulator, lib in [('arm64-v8a', 'qemu-aarch64', 'lib64'),
                                  ('armeabi-v7a', 'qemu-arm', 'lib')]:
            directory = out / abi
            libraries = str(directory) + ':' + str(args.android_sysroot / ('apex/com.android.runtime/' + lib + '/bionic'))
            prefix = [emulator, '-L', args.android_sysroot, '-E', 'LD_LIBRARY_PATH=' + libraries]
            for mode in ('anonymous', 'anonymous-clean'):
                run([*prefix, directory / 'handoff_bionic', directory / 'loader.so',
                     directory / 'libanonymous_retire.so', mode])


if __name__ == '__main__':
    main()
