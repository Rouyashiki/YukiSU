"""Exercise production layout discovery and Bionic loader retirement offline."""

import argparse
import os
from pathlib import Path

from run_native_bionic_tests import function, generate_linker, run


def generate(root, build):
    core = root / 'userspace/zygisk/core/src'
    source = (core / 'solist.cpp').read_text()
    (build / 'ctor_writer_under_test.inc').write_text(function(source, 'inline void u_set_ctor('))
    generate_linker(core, build)
    begin = source.index('/* Runtime soinfo unload glue. */')
    end = source.index('\n} // namespace', begin)
    runtime = source[begin:end]
    (build / 'layout_runtime_under_test.inc').write_text(
        'namespace yuki::solist { namespace {\n'
        '#define SLOGE(...) ((void)fprintf(stderr, __VA_ARGS__))\n'
        '#define SLOGI(...) ((void)printf(__VA_ARGS__))\n'
        'using realpath_fn = const char *(*)(void *);\n'
        'using guard_fn = void (*)(void *);\n'
        'constexpr int kMaxWalk = 2000;\n' + function(source, 'size_t page_size(') +
        '\n' + runtime + '\n}\n' + function(source, 'int drop_lib_containing(') + '\n}\n')


def build_one(root, build, compiler, flags, runner=None):
    build.mkdir(parents=True, exist_ok=True)
    generate(root, build)
    fixture = root / 'userspace/zygisk/tests/native_bionic'
    command = [str(compiler), '-std=c++17', '-O1', '-g', '-fno-exceptions', '-fno-rtti',
               *flags, '-I' + str(build), '-I' + str(root / 'userspace/zygisk/core/src'),
               '-I' + str(root / 'userspace/zygisk/core/src')]
    maps = root / 'userspace/zygisk/core/src/process_maps.cpp'
    run([*command, fixture / 'soinfo_layout.cpp', '-o', build / 'soinfo_layout'])
    if runner is not None:
        run([*runner, build / 'soinfo_layout'], cwd=build)
    if any('--target=' in flag for flag in flags):
        run([*command, fixture / 'soinfo_layout_bionic.cpp', maps, '-fexceptions', '-ldl',
             '-o', build / 'soinfo_layout_bionic'])
        run([*command, '-shared', '-fPIC', fixture / 'caller_anchor_payload.cpp',
             '-o', build / 'payload.so'])


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
            prefix = [emulator, '-L', args.android_sysroot, '-E', 'LD_LIBRARY_PATH=' + str(libraries)]
            run([*prefix, out / abi / 'soinfo_layout'], cwd=out / abi)
            for mode in ('unmap', 'keep'):
                run([*prefix, out / abi / 'soinfo_layout_bionic', out / abi / 'payload.so', mode], cwd=out / abi)
    elif args.ndk:
        tag = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
        compiler = args.ndk / 'toolchains/llvm/prebuilt' / tag / 'bin' / ('clang++.exe' if os.name == 'nt' else 'clang++')
        for abi, target in [('arm64-v8a', 'aarch64-linux-android31'),
                            ('armeabi-v7a', 'armv7a-linux-androideabi31')]:
            build_one(root, out / abi, compiler, ['--target=' + target, '-static-libstdc++'])
    else:
        build_one(root, out / 'host', os.environ.get('CXX', 'clang++'),
                  ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'], [])
        if args.qemu:
            for arch, target in [('aarch64', 'aarch64-linux-gnu'), ('arm', 'arm-linux-gnueabihf')]:
                build_one(root, out / arch, target + '-g++', [], ['qemu-' + arch, '-L', '/usr/' + target])


if __name__ == '__main__':
    main()
