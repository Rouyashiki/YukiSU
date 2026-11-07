"""Build production SDK/Core targets and exercise them on a supplied Bionic runtime."""

import argparse
from pathlib import Path
import subprocess


def run(command):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, timeout=600)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--abi', choices=('32', '64', 'all'), default='all')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--ndk', type=Path)
    mode.add_argument('--android-sysroot', type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent / 'native_bionic'
    out = args.build_dir.resolve()
    for bits, abi, emulator, lib, linker in [
            ('64', 'arm64-v8a', 'qemu-aarch64', 'lib64', 'linker64'),
            ('32', 'armeabi-v7a', 'qemu-arm', 'lib', 'linker')]:
        if args.abi not in ('all', bits):
            continue
        directory = out / abi
        if args.ndk:
            run([args.cmake, '-S', source, '-B', directory, '-G', 'Ninja',
                 '-DCMAKE_MAKE_PROGRAM=' + args.ninja,
                 '-DCMAKE_TOOLCHAIN_FILE=' + str(args.ndk.resolve() / 'build/cmake/android.toolchain.cmake'),
                 '-DANDROID_ABI=' + abi, '-DANDROID_PLATFORM=android-31',
                 '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DYUKISU_ENABLE_CLANG_TIDY=OFF'])
            run([args.cmake, '--build', directory, '--parallel', '4'])
        else:
            runtime = args.android_sysroot.resolve() / 'apex/com.android.runtime'
            libraries = str(directory) + ':' + str(runtime / lib / 'bionic')
            prefix = ['unshare', '-Urpf', '--mount-proc'] if bits == '32' else []
            run([*prefix, emulator, '-L', args.android_sysroot.resolve(),
                 '-E', 'LD_LIBRARY_PATH=' + libraries,
                 directory / 'handoff_bionic', directory])
            run([*prefix, emulator, '-L', args.android_sysroot.resolve(),
                 directory / 'native_empty'])
            run([*prefix, emulator, '-L', args.android_sysroot.resolve(),
                 '-E', 'LD_LIBRARY_PATH=' + libraries,
                 directory / 'native_phdr',
                 directory / 'libintegration_dependency.so'])


if __name__ == '__main__':
    main()
