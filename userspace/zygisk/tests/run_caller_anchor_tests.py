"""Exercise production caller selection and linker parsing without a device."""

import argparse
import os
import subprocess
from pathlib import Path

def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def generate_linker(core, build):
    source = (core / 'solist.cpp').read_text()
    helpers = '\n'.join(function(source, signature) for signature in (
        'inline uintptr_t elf_runtime_address(', 'inline uintptr_t elf_load_bias('))
    reader = function(source, 'class LinkerSyms {') + ';'
    resolver = function(source, 'bool resolve_loader(').replace(
        'bool resolve_loader(', 'bool checked_resolve(', 1)
    (build / 'caller_linker_under_test.inc').write_text(
        'namespace yuki::solist { namespace {\n' + helpers + '\n' + reader +
        '\n}}\nnamespace yukizygisk::native_bionic {\n' + resolver + '\n}\n')


def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def generate(core, build):
    (build / 'caller_native_under_test.inc').write_text((core / 'native_bionic.cpp').read_text())
    generate_linker(core, build)
    fixture = Path(__file__).with_name('native_bionic')
    (build / 'caller_maps_under_test.inc').write_text(
        function((fixture / 'caller_anchor.cpp').read_text(), 'Maps real_maps('))


def build_one(root, build, compiler, flags, runner=None):
    core = root / 'userspace/zygisk/core/src'
    fixture = Path(__file__).with_name('native_bionic')
    build.mkdir(parents=True, exist_ok=True)
    generate(core, build)
    android = any('--target=' in flag for flag in flags)
    command = [*compiler, '-std=c++17', '-O1', '-g', '-fno-exceptions', '-fno-rtti',
               *flags,
               '-I' + str(core), '-I' + str(build),
               '-I' + str(core)]
    if not android:
        command += ['-I' + str(fixture / 'host')]
    run([*command, fixture / 'caller_anchor.cpp', '-ldl', '-o', build / 'caller_anchor_test'])
    if android:
        run([*command, '-fPIC', '-shared', fixture / 'caller_anchor_payload.cpp',
             '-o', build / 'caller_anchor_payload.so'])
        run([*command, fixture / 'caller_anchor_bionic.cpp', '-ldl',
             '-o', build / 'caller_anchor_bionic'])
    if runner is not None:
        run([*runner, build / 'caller_anchor_test'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--qemu', action='store_true')
    parser.add_argument('--ndk', type=Path)
    parser.add_argument('--android-sysroot', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    out = args.build_dir.resolve()
    if out.is_relative_to(root):
        parser.error('Build outputs must be outside the checkout')
    if args.android_sysroot:
        for abi, emulator, lib in [('arm64-v8a', 'qemu-aarch64', 'lib64'),
                                  ('armeabi-v7a', 'qemu-arm', 'lib')]:
            libraries = args.android_sysroot / ('apex/com.android.runtime/' + lib + '/bionic')
            run([emulator, '-L', args.android_sysroot, '-E', 'LD_LIBRARY_PATH=' + str(libraries),
                 out / abi / 'caller_anchor_test'])
            run([emulator, '-L', args.android_sysroot, '-E', 'LD_LIBRARY_PATH=' + str(libraries),
                 out / abi / 'caller_anchor_bionic', out / abi / 'caller_anchor_payload.so'])
    elif args.ndk:
        tag = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
        compiler = args.ndk / 'toolchains/llvm/prebuilt' / tag / 'bin' / ('clang++.exe' if os.name == 'nt' else 'clang++')
        for abi, target in [('arm64-v8a', 'aarch64-linux-android31'),
                            ('armeabi-v7a', 'armv7a-linux-androideabi31')]:
            build_one(root, out / abi, [compiler], ['--target=' + target, '-static-libstdc++'])
    else:
        build_one(root, out / 'host', [os.environ.get('CXX', 'clang++')],
                  ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'], [])
        if args.qemu:
            for prefix, target in [('aarch64', 'aarch64-linux-gnu'), ('arm', 'arm-linux-gnueabihf')]:
                build_one(root, out / prefix, [target + '-g++'], [],
                          ['qemu-' + prefix, '-L', '/usr/' + target])


if __name__ == '__main__':
    main()
