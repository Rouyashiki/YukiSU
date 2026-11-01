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


def build_companion_test(root, build, compiler, flags, runner):
    build.mkdir(parents=True, exist_ok=True)
    core = root / 'userspace/zygisk/core/src'
    source = (core / 'yukizncore.cpp').read_text()
    (build / 'native_companion_types.inc').write_text(
        source[source.index('struct ModuleHandle {'):source.index('struct InlineHookRecord {')])
    methods = ''.join(source[source.index(begin):source.index(end)] for begin, end in [
        ('bool has_pending_early_report()', 'bool map_from_base('),
        ('int api_connect_companion(', 'const ZygiskNextRuntime *api_get_runtime()'),
        ('bool sync_early_native_reports_once()', 'void *deferred_native_sync_main(')])
    (build / 'native_companion_methods.inc').write_text(methods)
    binary = build / 'native_companion_test'
    subprocess.run([*compiler, '-std=c++17', '-O1', '-g', '-pthread', *flags,
                    '-I' + str(core), '-I' + str(build),
                    root / 'userspace/zygisk/tests/native_companion_test.cpp',
                    '-o', binary], check=True)
    subprocess.run([*runner, binary], check=True, timeout=15)


def build_archive(root, build, compiler, flags, export_lifecycle=False):
    core = root / 'userspace/zygisk/core/src'
    vendor = root / 'userspace/zygisk/third_party/yukiplt'
    common = [*compiler, '-std=c++17', '-O1', '-g', '-fPIC', '-fexceptions',
              '-pthread', '-DYUKIPLT_STATIC=1', *flags,
              '-I' + str(core), '-I' + str(vendor / 'include')]
    sources = [*sorted((vendor / 'src').glob('*.cpp')), core / 'plt_lifecycle.cpp',
               core / 'process_maps.cpp', core / 'core_bindings.cpp']
    objects = [build / (source.stem + '.o') for source in sources]
    def compile_one(pair):
        source, output = pair
        definitions = ['-DYUKILINKER_BOOTSTRAP=1'] if source.name == 'core_bindings.cpp' else []
        visibility = 'default' if export_lifecycle and source.name == 'plt_lifecycle.cpp' else 'hidden'
        subprocess.run([*common, *definitions, '-fvisibility=' + visibility, '-fvisibility-inlines-hidden',
                        '-c', source, '-o', output], check=True)
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(compile_one, zip(sources, objects)))
    return common, objects


def build_backend(root, build, compiler, flags):
    common, objects = build_archive(root, build, compiler, flags, export_lifecycle=True)
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
                    '-Wl,--wrap=pthread_key_create', '-Wl,--wrap=pthread_key_delete',
                    '-Wl,-z,now', '-Wl,-Bsymbolic-functions',
                    '-o', build / 'libyz_plt_backend.so'], check=True)


def build_tests(root, build, compiler, flags, runner):
    build.mkdir(parents=True, exist_ok=True)
    build_companion_test(root, build, compiler, flags, runner)
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
    (build / 'driver.cpp').write_text('''#include <dlfcn.h>
#include <cassert>
#include <pthread.h>
#include <unistd.h>
void external_handler() {
  pthread_key_t key;
  if (pthread_key_create(&key, nullptr) || pthread_key_delete(key)) _exit(91);
}
int main(int n, char **v) {
  assert(pthread_atfork(external_handler, external_handler, external_handler) == 0);
  auto h = dlopen(v[1], RTLD_NOW);
  assert(h);
  auto f = (int(*)(int, char **))dlsym(h, "run_test");
  assert(f);
  return f(n-1, v+1);
}
''')
    subprocess.run([*common, build / 'driver.cpp', '-ldl', '-o', build / 'driver'], check=True)
    library = build / 'plt_state_test.so'
    wrapping = ['-Wl,--wrap=mprotect', '-Wl,--wrap=open',
                '-Wl,--wrap=pthread_mutex_lock', '-Wl,--wrap=clock_gettime',
                '-Wl,--wrap=pthread_key_create', '-Wl,--wrap=pthread_key_delete',
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
