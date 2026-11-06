"""Build and verify the actual freestanding native injection images."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def run(*args):
    return subprocess.check_output(list(map(str, args)), stderr=subprocess.STDOUT).decode()


def main():
    root = Path(__file__).resolve().parents[3]
    source = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument('--ndk', default=os.environ.get('ANDROID_NDK_HOME', 'D:/Softwares/AndroidSDK/ndk/30.0.16248370'))
    parser.add_argument('--out', type=Path, default=root / 'out/native-inject')
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    tools = Path(args.ndk) / 'toolchains/llvm/prebuilt' / ('windows-x86_64' if os.name == 'nt' else 'linux-x86_64') / 'bin'
    suffix = '.exe' if os.name == 'nt' else ''
    clang = tools / ('clang' + suffix)
    readelf = tools / ('llvm-readelf' + suffix)
    objcopy = tools / ('llvm-objcopy' + suffix)
    args.out.mkdir(parents=True, exist_ok=True)
    evidence = {}
    for bits, target in [(64, 'aarch64-linux-android31'), (32, 'armv7a-linux-androideabi31')]:
        arrays = []
        for kind, filename in [('resolver', 'native_resolver.c'), ('start', 'native_start.S')]:
            elf = args.out / f'{kind}{bits}.elf'
            binary = elf.with_suffix('.bin')
            command = [clang, '--target=' + target, '-Os', '-ffreestanding', '-fno-builtin',
                       '-fPIC', '-fvisibility=hidden', '-fno-stack-protector', '-fno-unwind-tables',
                       '-fno-asynchronous-unwind-tables', '-fno-ident', '-nostdlib', '-static',
                       '-Wl,--no-undefined', '-Wl,--build-id=none', '-Wl,-T,' + str(source / 'blob.ld'),
                       '-Wl,-e,' + ('yz_native_resolve' if kind == 'resolver' else 'yz_native_start'),
                       '-I' + str(source.parent / 'third_party/yukilinker/include'),
                       source / filename, '-o', elf]
            if bits == 32:
                command.insert(2, '-marm')
            try:
                run(*command)
            except subprocess.CalledProcessError as error:
                raise RuntimeError(error.output.decode()) from error
            info = run(readelf, '-r', '-l', '-s', elf)
            if 'R_AARCH64_' in info or 'R_ARM_' in info or ' TLS ' in info:
                raise RuntimeError(f'{elf}: unexpected relocations/TLS')
            for line in info.splitlines():
                if ' UND ' in line and len(line.split()) > 7:
                    raise RuntimeError(f'{elf}: unresolved symbol: {line}')
            run(objcopy, '-O', 'binary', '--only-section=.text', elf, binary)
            data = binary.read_bytes()
            if kind == 'start' and len(data) > 0x400:
                raise RuntimeError('native launcher overlaps descriptor')
            name = f'yz_native_{kind}{bits}'
            rows = ['\t' + ', '.join(f'0x{b:02x}' for b in data[i:i+12]) + ',' for i in range(0, len(data), 12)]
            arrays.append(f'static const unsigned char {name}[] = {{\n' + '\n'.join(rows) + '\n};\n')
            evidence[name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            (args.out / f'{kind}{bits}.elf.txt').write_text(info)
        text = '/* Generated from userspace/zygisk/inject; run generate.py --write. */\n/* clang-format off */\n' + '\n'.join(arrays) + '/* clang-format on */\n'
        header = root / f'kernel/feature/yukizygisk/native_blob{bits}.h'
        if args.write:
            header.write_text(text, newline='\n')
        elif not header.exists() or header.read_text() != text:
            raise RuntimeError(f'{header}: stale generated injection image')
    (args.out / 'manifest.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()
