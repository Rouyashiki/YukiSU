"""Test the production Native load function; never deploy to a device."""

import argparse
import itertools
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


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


def generate(core, build):
    generate_linker(core, build)
    (build / 'caller_native_under_test.inc').write_text(
        (core / 'native_bionic.cpp').read_text().replace('LoadResult load(', 'LoadResult checked_load(', 1))
    native = (core / 'native_bionic.cpp').read_text()
    (build / 'caller_names_under_test.inc').write_text('\n'.join(
        function(native, signature) for signature in (
            'const char *stage_name(', 'const char *error_name(')))
    source = (core / 'yukizncore.cpp').read_text()
    handle = source.index('struct ModuleHandle {')
    (build / 'native_handle_under_test.inc').write_text(
        source[handle:source.index('\n};', handle) + 3])
    (build / 'native_load_under_test.inc').write_text('\n'.join(
        function(source, signature) for signature in (
            'ModuleHandle *find_loaded_module(', 'std::string bounded_cstr(',
            'std::string module_id_of(', 'bool load_native_module_from_fd(')))
    (build / 'native_map_under_test.inc').write_text(
        function(source, 'bool map_from_base('))


def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def build_tests(root, build, compiler, flags, runner=None):
    core = root / 'userspace/zygisk/core/src'
    sources = Path(__file__).with_name('native_bionic')
    vendor = root / 'userspace/zygisk/third_party/yukiplt'
    build.mkdir(parents=True, exist_ok=True)
    generate(core, build)
    common = [*compiler, '-std=c++17', '-O1', '-g', '-fPIC', '-fno-exceptions', '-fno-rtti', *flags,
              '-I' + str(root), '-I' + str(core), '-I' + str(build),
              '-I' + str(core.parents[1] / 'include'),
              '-I' + str(vendor / 'include'),
              '-DLOG_DISABLED', '-DDT_ANDROID_REL=0x6000000f',
              '-DDT_ANDROID_RELSZ=0x60000010', '-DDT_ANDROID_RELA=0x60000011',
              '-DDT_ANDROID_RELASZ=0x60000012']
    android = any('--target=' in flag for flag in flags)
    if not android:
        common += ['-I' + str(sources / 'host')]
    from run_plt_tests import build_backend
    build_backend(root, build, compiler, flags)

    def library(name, source, extra=()):
        run([*common, '-shared', sources / source, *extra,
             '-Wl,-soname,libyz_native_' + name + '.so',
             '-Wl,-rpath,$ORIGIN', '-ldl', '-o', build / ('libyz_native_' + name + '.so')])

    library('private', 'private.cpp')
    library('target', 'target.cpp')
    library('module', 'module.cpp', ['-L' + str(build), '-lyz_native_private'])
    (build / 'alternate').mkdir(exist_ok=True)
    shutil.copyfile(build / 'libyz_native_module.so', build / 'alternate/libyz_native_module.so')
    library('missing', 'invalid.cpp')
    library('invalid', 'invalid.cpp', ['-DINVALID_VERSION'])
    library('null', 'invalid.cpp', ['-DNULL_ENTRY'])
    (build / 'malformed.so').write_bytes(b'not an ELF shared object\n')
    run([*common, '-fPIE', '-pie', sources / 'runner.cpp',
         '-lyz_plt_backend', core / 'process_maps.cpp', '-fexceptions',
         '-L' + str(build), '-lyz_native_target', '-ldl',
         '-Wl,--export-dynamic', '-Wl,-z,now', '-Wl,-rpath,$ORIGIN',
         '-o', build / 'native_bionic_test'])
    if runner is None:
        print('BUILT Android fixture (not deployed):', build, flush=True)
        return
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = str(build)
    env['ASAN_OPTIONS'] = 'detect_leaks=1'
    for private, anonymous, early in itertools.product((0, 1), repeat=3):
        run([*runner, build / 'native_bionic_test', build,
             str(private), str(anonymous), str(early)], env=env)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', action='store_true', help='Also run Linux ARM64 and ARM32 fixtures')
    parser.add_argument('--ndk', type=Path, help='Build Android fixtures only; does not deploy')
    parser.add_argument('--android-sysroot', type=Path,
                        help='Run existing Android fixtures using a local Bionic sysroot and QEMU')
    parser.add_argument('--build-dir', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    if args.ndk and not args.build_dir:
        parser.error('--ndk requires an external --build-dir')
    if args.android_sysroot and (args.ndk or not args.build_dir):
        parser.error('--android-sysroot requires existing --build-dir and cannot be combined with --ndk')
    if args.build_dir and args.build_dir.resolve().is_relative_to(root):
        parser.error('Build artifacts must be outside the repository')
    with tempfile.TemporaryDirectory(prefix='native-bionic-') as temporary:
        out = (args.build_dir or Path(temporary)).resolve()
        if args.android_sysroot:
            sysroot = args.android_sysroot.resolve()
            for abi, emulator, lib in [('arm64-v8a', 'qemu-aarch64', 'lib64'),
                                      ('armeabi-v7a', 'qemu-arm', 'lib')]:
                directory = out / abi
                libraries = str(directory) + ':' + str(sysroot / ('apex/com.android.runtime/' + lib + '/bionic'))
                for private, anonymous, early in itertools.product((0, 1), repeat=3):
                    run([emulator, '-L', sysroot, '-E', 'LD_LIBRARY_PATH=' + libraries,
                         directory / 'native_bionic_test', directory,
                         str(private), str(anonymous), str(early)])
            return
        if args.ndk:
            prebuilt = args.ndk / 'toolchains/llvm/prebuilt'
            tag = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
            toolchain = prebuilt / tag
            compiler = [toolchain / 'bin' / ('clang++.exe' if os.name == 'nt' else 'clang++')]
            for abi, target in [('arm64-v8a', 'aarch64-linux-android31'),
                                ('armeabi-v7a', 'armv7a-linux-androideabi31')]:
                build_tests(root, out / abi, compiler,
                            ['--target=' + target, '--sysroot=' + str(toolchain / 'sysroot'),
                             '-lc++_shared'])
        else:
            build_tests(root, out / 'host', [os.environ.get('CXX', 'clang++')],
                        ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'], [])
            if args.qemu:
                for arch, prefix, sysroot in [
                    ('arm64', 'aarch64', '/usr/aarch64-linux-gnu'),
                    ('arm32', 'arm', '/usr/arm-linux-gnueabihf')]:
                    compiler = sysroot.rsplit('/', 1)[1] + '-g++'
                    build_tests(root, out / arch, [compiler], [],
                                ['qemu-' + prefix, '-L', sysroot])


if __name__ == '__main__':
    main()
