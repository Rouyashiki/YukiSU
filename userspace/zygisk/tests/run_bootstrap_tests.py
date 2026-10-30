import argparse
from pathlib import Path
import re
import subprocess
import tempfile


root = Path(__file__).resolve().parents[3]
kernel = root / 'kernel/feature/yukizygisk'
if not kernel.exists():
    kernel = root / 'kernel/feature'
injector = (kernel / 'injector.c').read_text()
tango = (kernel / 'tango.c').read_text()


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


arrays = section(injector, 'static const u32 tmpl[]', '\n\t\tu32 code[')
patch = section(injector, '\t\tif (yuki)\n\t\t\tcleanup_fds', '\n\t\tif (yuki) {\n\t\t\tlib_str')
patch_imm = section(injector, 'static void __maybe_unused yz_patch_imm64', '\n/* bionic').replace('__maybe_unused ', '')
tango_function = section(tango, 'static void yz_tango_bootstrap', '\nstatic bool yz_tango_claim')
tango_size = re.search(r'#define YZ_TANGO_BOOTSTRAP_SIZE (\d+)',
                       (root / 'uapi/yukizygisk.h').read_text()).group(1)
loader = (root / 'userspace/zygisk/core/src/yukilinker.cpp').read_text()
tango_restore = section(loader, 'struct TangoProtection {', 'static BootstrapTarget prepare_bootstrap(')


fixture = ('''
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
struct yz_early_packet_state {
  int packet_fd, endpoint_fd;
  unsigned module_fd_count;
  int module_fds[YZ_NATIVE_TARGET_MAX];
};
''' + patch_imm + '''
static void prepare(void *stub, uintptr_t saved, uintptr_t dlopen_addr,
                    uintptr_t dlsym_addr, int loader_fd, int core_fd, bool yuki,
                    const yz_early_packet_state &early_packet) {
''' + arrays + '''
  u32 code[ARRAY_SIZE(tmpl)];
  u8 compat_code[ARRAY_SIZE(compat_tmpl)];
  int stub_core_fd;
  const int early_packet_arg = early_packet.packet_fd + 1;
  s32 cleanup_fds[YZ_NATIVE_TARGET_MAX + 4];
  u32 cleanup_count = 0, cleanup_index;
''' + patch + '''
  auto *bytes = static_cast<u8 *>(stub);
#if defined(__aarch64__)
  memcpy(bytes, code, sizeof(code));
  memcpy(bytes + 0x100, cleanup64, sizeof(cleanup64));
  memcpy(bytes + 0xa00 + 28, &loader_fd, sizeof(loader_fd));
#else
  memcpy(bytes, compat_code, sizeof(compat_code));
  memcpy(bytes + 0x100, cleanup32, sizeof(cleanup32));
  memcpy(bytes + 0xa00 + 20, &loader_fd, sizeof(loader_fd));
#endif
  memcpy(bytes + 0x500, cleanup_fds, (cleanup_count + 1) * sizeof(s32));
  strcpy(reinterpret_cast<char *>(bytes + 0xc00), "test-loader.so");
  strcpy(reinterpret_cast<char *>(bytes + 0xd00), "test-entry");
}
''' + f'#define YZ_TANGO_STUB_SIZE {tango_size}\n' + tango_function + '\n' +
    'struct TangoBootstrap { uint32_t got, original, stub; };\n' + tango_restore)

parser = argparse.ArgumentParser()
parser.add_argument('--abi', choices=('32', '64', 'all'), default='all')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='yz-bootstrap-') as directory:
    build = Path(directory)
    (build / 'bootstrap_fixture.hpp').write_text(fixture)
    for abi, arch in [('64', 'aarch64'), ('32', 'arm')]:
        if args.abi not in ('all', abi):
            continue
        compiler = arch + '-linux-' + ('gnu' if abi == '64' else 'gnueabihf') + '-g++'
        binary = build / ('bootstrap' + abi)
        command = [compiler, '-std=c++17', '-O2', '-static', '-pthread',
                   '-Wl,--wrap=mprotect', '-I' + str(build), '-I' + str(root), '-I' + str(root / 'kernel'),
                   str(root / 'userspace/zygisk/tests/bootstrap_stub_test.cpp'),
                   str(root / 'userspace/zygisk/endpoint.cpp'), '-o', str(binary)]
        if abi == '32':
            command.append('-mthumb')
        subprocess.run(command, check=True)
        subprocess.run(['qemu-' + arch, str(binary)], check=True, timeout=60)
