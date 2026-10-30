"""Exercise native host ownership with private static YukiPLT copies."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile


def native_source(core):
    source = (core / 'yukizncore.cpp').read_text()
    return source[source.index('struct NativePltBinding {'):source.index('int api_inline_hook(')]


def build_archive(root, build, compiler, flags):
    core = root / 'userspace/zygisk/core/src'
    vendor = root / 'userspace/zygisk/third_party/yukiplt'
    common = [*compiler, '-std=c++17', '-O1', '-g', '-fPIC', '-fexceptions',
              '-pthread', '-DYUKIPLT_STATIC=1', *flags,
              '-I' + str(core), '-I' + str(vendor / 'include')]
    sources = [*sorted((vendor / 'src').glob('*.cpp')), core / 'plt_lifecycle.cpp', core / 'process_maps.cpp']
    objects = [build / (source.stem + '.o') for source in sources]
    def compile_one(pair):
        source, output = pair
        subprocess.run([*common, '-fvisibility=hidden', '-fvisibility-inlines-hidden',
                        '-c', source, '-o', output], check=True)
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(compile_one, zip(sources, objects)))
    return common, objects


def build_backend(root, build, compiler, flags):
    common, objects = build_archive(root, build, compiler, flags)
    source = """#include <yukiplt.hpp>
#include <algorithm>
#include <memory>
#include "plt_lifecycle.hpp"
namespace yukizygisk::hyos { bool original_output_allowed(void **) { return true; } }
constexpr int kSuccess = 0, kFailed = 1;
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
""" + native_source(root / 'userspace/zygisk/core/src') + """
extern "C" int fixture_plt_hook(void *base, const char *symbol, void *callback, void **original) {
  return api_plt_hook(base, symbol, callback, original);
}
extern "C" void fixture_recover_plt() { recover_native_plt(); }
extern "C" bool fixture_retire_plt() {
  yukizygisk::plt_lifecycle::Operation operation(yukizygisk::plt_lifecycle::Access::Recover);
  if (!operation) return false;
  bool ok = true;
  for (auto &binding : g_native_plt)
    if (binding->handle.valid()) ok &= yukiplt::unhook(binding->handle).ok();
  return ok;
}
"""
    (build / 'native_host.cpp').write_text(source)
    subprocess.run([*common, '-shared', build / 'native_host.cpp', *objects, '-ldl',
                    '-Wl,-z,now', '-Wl,-Bsymbolic-functions',
                    '-o', build / 'libyz_plt_backend.so'], check=True)


def build_tests(root, build, compiler, flags, runner):
    build.mkdir(parents=True, exist_ok=True)
    core = root / 'userspace/zygisk/core/src'
    tests = Path(__file__).parent
    module = (core / 'core.cpp').read_text()
    declarations = module[module.index('struct ModulePltRequest {'):module.index('struct Module {')]
    methods = module[module.index('bool same_module_image('):module.index('bool api_exempt_fd(')]
    (build / 'module_plt_types.inc').write_text(declarations)
    (build / 'module_plt_methods.inc').write_text(methods)
    common, objects = build_archive(root, build, compiler, flags)
    links = ['-L' + str(build), '-Wl,-rpath,$ORIGIN', '-Wl,-z,relro', '-Wl,-z,now']
    subprocess.run([*common, '-shared', tests / 'plt_provider.cpp',
                    '-o', build / 'libplt_provider.so'], check=True)
    subprocess.run([*common, '-shared', tests / 'plt_target.cpp', *links,
                    '-lplt_provider', '-o', build / 'libplt_target.so'], check=True)
    (build / 'entry.cpp').write_text('int fixture_main(int, char **); extern "C" int run_test(int n, char **v) { return fixture_main(n,v); }')
    (build / 'driver.cpp').write_text('#include <dlfcn.h>\n#include <cassert>\nint main(int n,char **v) { auto h=dlopen(v[1],RTLD_NOW); assert(h); auto f=(int(*)(int,char **))dlsym(h,"run_test"); assert(f); return f(n-1,v+1); }')
    subprocess.run([*common, build / 'driver.cpp', '-ldl', '-o', build / 'driver'], check=True)
    library = build / 'plt_state_test.so'
    wrapping = ['-Wl,--wrap=mprotect', '-Wl,--wrap=open',
                '-Wl,--wrap=' + ('_Znwj' if 'arm-linux' in str(compiler) else '_Znwm')]
    subprocess.run([*common, *wrapping, '-shared', '-Dmain=fixture_main', '-I' + str(build),
                    tests / 'plt_state_test.cpp', build / 'entry.cpp', *objects, *links,
                    '-Wl,-Bsymbolic-functions', '-ldl', '-o', library], check=True)
    subprocess.run([*runner, build / 'driver', library, build / 'libplt_target.so'], check=True, timeout=60)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    with tempfile.TemporaryDirectory(prefix='yz-yukiplt-') as temporary:
        build_tests(root, Path(temporary) / 'host', [os.environ.get('CXX', 'clang++')],
                    ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'], [])
        if args.qemu:
            for arch, target in [('aarch64', 'aarch64-linux-gnu'), ('arm', 'arm-linux-gnueabihf')]:
                build_tests(root, Path(temporary) / arch, [target + '-g++'], [],
                            ['qemu-' + arch, '-L', '/usr/' + target])


if __name__ == '__main__':
    main()
