#include "handoff_control.hpp"
#include "kernel/feature/yukizygisk/native_stub.h"
#include "uapi/yukizygisk.h"
#include <android/dlext.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yukilinker.h>
#if defined(__aarch64__)
#include "kernel/feature/yukizygisk/native_blob64.h"
#define NATIVE_RESOLVER yz_native_resolver64
#define NATIVE_START yz_native_start64
#else
#include "kernel/feature/yukizygisk/native_blob32.h"
#define NATIVE_RESOLVER yz_native_resolver32
#define NATIVE_START yz_native_start32
#endif

namespace {
extern "C" int native_register_probe(void *);
extern "C" void native_test_resume();
unsigned resumed;
void native_bootstrap(const char *loader_path, int fd, int packet_fd,
                      unsigned fault = 0,
                      void (*invoke)(void (*)(void *), void *) = nullptr) {
  const size_t page = getpagesize();
  const size_t length = (sizeof(NATIVE_RESOLVER) + page - 1) & ~(page - 1);
  void *parser = mmap(nullptr, length, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  auto *launch = static_cast<unsigned char *>(
      mmap(nullptr, page, PROT_READ | PROT_WRITE | PROT_EXEC,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(parser != MAP_FAILED && launch != MAP_FAILED);
  memcpy(parser, NATIVE_RESOLVER, sizeof(NATIVE_RESOLVER));
  memcpy(launch, NATIVE_START, sizeof(NATIVE_START));
  auto *data = reinterpret_cast<yz_native_stub *>(launch + YZ_NATIVE_STUB_DATA);
  data->magic = YZ_NATIVE_STUB_MAGIC;
  data->version = YZ_NATIVE_STUB_VERSION;
  data->entry = reinterpret_cast<uintptr_t>(native_test_resume);
  data->linker = getauxval(AT_BASE);
  if (fault == 1)
    data->linker = reinterpret_cast<uintptr_t>(parser);
  if (fault == 2)
    data->version = 99;
  if (fault == 5) {
    Dl_info libc{};
    assert(dladdr(reinterpret_cast<void *>(getpid), &libc));
    data->linker = reinterpret_cast<uintptr_t>(libc.dli_fbase);
  }
  data->resolver = reinterpret_cast<uintptr_t>(parser);
  data->resolver_size = length;
  data->process = getpid();
  data->generation = 1;
  data->report_fd = memfd_create("bootstrap-result", MFD_CLOEXEC);
  const int report = dup(data->report_fd);
  assert(report >= 0);
  data->loader_fd = open(loader_path, O_RDONLY | O_CLOEXEC);
  if (fault == 6) {
    assert(close(data->loader_fd) == 0);
    data->loader_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  }
  data->core_fd = fd;
  data->packet_argument = packet_fd + 1;
  data->cleanup_fds[data->cleanup_count++] = fd;
  data->cleanup_fds[data->cleanup_count++] = packet_fd;
  yz_bootstrap_packet_header header{};
  assert(pread(packet_fd, &header, sizeof(header), 0) == sizeof(header));
  data->cleanup_fds[data->cleanup_count++] = header.endpoint_fd;
  for (unsigned i = 0; i < header.count; ++i) {
    yz_early_native_packet_entry entry{};
    assert(pread(packet_fd, &entry, sizeof(entry),
                 sizeof(header) + i * sizeof(entry)) == sizeof(entry));
    data->cleanup_fds[data->cleanup_count++] = entry.fd;
  }
  __builtin___clear_cache(static_cast<char *>(parser),
                          static_cast<char *>(parser) + length);
  __builtin___clear_cache(reinterpret_cast<char *>(launch),
                          reinterpret_cast<char *>(launch) + page);
  auto probe = [](void *code) {
    assert(native_register_probe(code) == 1);
    ++resumed;
  };
  if (invoke)
    invoke(probe, launch);
  else
    probe(launch);
  fprintf(stderr,
          "native generated stub: status=%d stage=%u object=%llx resumed=%u "
          "message=%s\n",
          data->status, data->stage, (unsigned long long)data->loader, resumed,
          data->message);
  const int expected = fault == 1                 ? 2
                       : fault == 2               ? 1
                       : fault == 3               ? 5
                       : fault == 4               ? 4
                       : fault == 5               ? 3
                       : fault == 6               ? 6
                       : fault == 9               ? 8
                       : fault == 7 || fault == 8 ? data->completion.error
                                                  : 0;
  assert(data->status == expected && resumed == 1 && !data->resolver &&
         !data->resolver_size);
  if (fault == 7 || fault == 8)
    assert(data->completion.error &&
           data->completion.ownership == YUKILINKER_C_NATIVE_LOADER_RETURNED &&
           !data->loader);
  assert(fcntl(data->loader_fd, F_GETFD) < 0);
  assert(fcntl(data->report_fd, F_GETFD) < 0);
  int reported_status = -1;
  assert(pread(report, &reported_status, sizeof(reported_status), 0) ==
         sizeof(reported_status));
  assert(reported_status == data->status && close(report) == 0);
  unsigned char resident;
  assert(mincore(parser, length, &resident) == -1);
  assert(munmap(launch, page) == 0);
}

void *foreign_library(const char *directory, const char *file,
                      const char *domain, bool link_platform) {
  void *android = dlopen("libdl_android.so", RTLD_NOW);
  auto create = reinterpret_cast<
      android_namespace_t *(*)(const char *, const char *, const char *,
                               uint64_t, const char *, android_namespace_t *)>(
      dlsym(android, "android_create_namespace"));
  auto link = reinterpret_cast<bool (*)(android_namespace_t *,
                                        android_namespace_t *, const char *)>(
      dlsym(android, "android_link_namespaces"));
  assert(create && link);
  auto *space = create(domain, directory, directory, 1, directory, nullptr);
  assert(space &&
         (!link_platform || link(space, nullptr, "libc.so:libdl.so:libm.so")));
  char path[4096];
  snprintf(path, sizeof(path), "%s/%s", directory, file);
  android_dlextinfo ext{};
  ext.flags = ANDROID_DLEXT_USE_NAMESPACE;
  ext.library_namespace = space;
  void *handle = android_dlopen_ext(path, RTLD_NOW | RTLD_GLOBAL, &ext);
  if (!handle)
    fprintf(stderr, "foreign: %s\n", dlerror());
  assert(handle);
  assert(dlclose(android) == 0);
  return handle;
}
struct HostThread {
  pthread_t thread{};
  int channel[2]{-1, -1};

  static void *wait_for_stop(void *opaque) {
    auto &worker = *static_cast<HostThread *>(opaque);
    char byte = 1;
    assert(write(worker.channel[1], &byte, 1) == 1);
    assert(read(worker.channel[1], &byte, 1) == 1);
    return nullptr;
  }
  void start() {
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel) == 0);
    assert(pthread_create(&thread, nullptr, wait_for_stop, this) == 0);
    char ready;
    assert(read(channel[0], &ready, 1) == 1);
  }
  void finish() {
    if (channel[0] < 0)
      return;
    const char stop = 1;
    assert(write(channel[0], &stop, 1) == 1);
    assert(pthread_join(thread, nullptr) == 0);
    close(channel[0]);
    close(channel[1]);
  }
};

int loader_visit(dl_phdr_info *info, size_t, void *) {
  return info->dlpi_name && strstr(info->dlpi_name, "libyloader") ? 1 : 0;
}
unsigned anonymous_core_count(int core_fd) {
  unsigned char expected[4096], actual[4096];
  ElfW(Ehdr) header{};
  assert(pread(core_fd, &header, sizeof(header), 0) == sizeof(header));
  const size_t bytes = header.e_phoff + header.e_phnum * sizeof(ElfW(Phdr));
  assert(bytes <= sizeof(expected) &&
         pread(core_fd, expected, bytes, 0) == bytes);
  FILE *maps = fopen("/proc/self/maps", "re");
  int memory = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
  assert(maps && memory >= 0);
  char line[8192], permissions[5];
  unsigned long long begin, end;
  unsigned matches = 0;
  while (fgets(line, sizeof(line), maps)) {
    if (sscanf(line, "%llx-%llx %4s", &begin, &end, permissions) != 3 ||
        permissions[0] != 'r' || end - begin < bytes)
      continue;
    if (pread64(memory, actual, bytes, static_cast<off64_t>(begin)) == bytes &&
        memcmp(actual, expected, bytes) == 0)
      ++matches;
  }
  close(memory);
  fclose(maps);
  return matches;
}
int packet(const char *module = nullptr, unsigned count = 1,
           const char *module_id = "integration") {
  int endpoint = memfd_create("endpoint", MFD_CLOEXEC);
  assert(endpoint >= 0 && ftruncate(endpoint, getpagesize()) == 0);
  yz_endpoint_state state{};
  state.size = sizeof(state);
  state.version = YZ_ENDPOINT_VERSION;
  state.abi = sizeof(void *) == 8 ? YZ_RUNTIME_ABI_64 : YZ_RUNTIME_ABI_32;
  assert(pwrite(endpoint, &state, sizeof(state), 0) == sizeof(state));
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", endpoint);
  int readonly = open(path, O_RDONLY | O_CLOEXEC);
  assert(readonly >= 0);
  close(endpoint);
  int fd = memfd_create("packet", MFD_CLOEXEC);
  yz_bootstrap_packet_header header{YZ_BOOTSTRAP_PACKET_MAGIC,
                                    YZ_BOOTSTRAP_PACKET_VERSION,
                                    sizeof(header),
                                    sizeof(yz_early_native_packet_entry),
                                    0,
                                    module ? count : 0U,
                                    readonly,
                                    0};
  assert(fd >= 0 && write(fd, &header, sizeof(header)) == sizeof(header));
  for (unsigned i = 0; module && i < count; ++i) {
    yz_early_native_packet_entry entry{};
    entry.module.target_type = YZ_NATIVE_TARGET_NAME;
    snprintf(entry.module.module_id, sizeof(entry.module.module_id), "%s",
             module_id);
    strcpy(entry.module.target, "handoff_bionic");
    snprintf(entry.module.lib_path, sizeof(entry.module.lib_path), "%s",
             module);
    entry.fd = open(module, O_RDONLY | O_CLOEXEC);
    assert(entry.fd >= 0 && write(fd, &entry, sizeof(entry)) == sizeof(entry));
  }
  return fd;
}
void run(const char *directory, const char *core_name, bool native,
         const char *module_name, unsigned count = 1, bool rejected = false,
         bool tango = false, bool foreign = false, bool adapter = false) {
  const bool module = module_name != nullptr;
  IntegrationControl control{};
  control.native_policy = native;
  control.adapter_only = adapter;
  char pointer[32], path[4096], loader_path[4096], module_path[4096];
  void *android = dlopen("libdl_android.so", RTLD_NOW | RTLD_LOCAL);
  assert(android);
  auto anonymous_namespace =
      reinterpret_cast<bool (*)(const char *, const char *)>(
          dlsym(android, "android_init_anonymous_namespace"));
  assert(anonymous_namespace &&
         anonymous_namespace("libc.so:libdl.so:libm.so:libdl_android.so", ""));
  snprintf(path, sizeof(path), "%s/libintegration_host.so", directory);
  void *host_library = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  assert(host_library && dlsym(RTLD_DEFAULT, "integration_system_probe"));
  snprintf(
      pointer, sizeof(pointer), "%llx",
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&control)));
  assert(setenv("YUKISU_INTEGRATION_CONTROL", pointer, 1) == 0);
  snprintf(loader_path, sizeof(loader_path), "%s/core/libyloader%zu.so",
           directory, sizeof(void *) * 8);
  void *loader = native ? nullptr : dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
  if (!native && !loader)
    fprintf(stderr, "loader: %s\n", dlerror());
  assert(native || loader);
  auto bootstrap = native ? nullptr
                          : reinterpret_cast<void (*)(int, int)>(
                                dlsym(loader, "yuki_bootstrap"));
  assert(native || bootstrap);
  snprintf(path, sizeof(path), "%s/%s", directory, core_name);
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  assert(fd >= 0);
  const int inspection_fd = dup(fd);
  assert(inspection_fd >= 0 && anonymous_core_count(inspection_fd) == 0);
  snprintf(module_path, sizeof(module_path), "%s/libintegration_%s.so",
           directory, module_name ? module_name : "private");
  const int packet_fd =
      packet(module ? module_path : nullptr, count,
             foreign ? "different_native_module" : "integration");
  void *foreign_host = nullptr;
  void (*invoke)(void (*)(void *), void *) = nullptr;
  if (foreign) {
    assert(prctl(PR_SET_NAME, "scope-renamed", 0, 0, 0) == 0);
    foreign_host = foreign_library(directory, "libintegration_foreign.so",
                                   "renamed_domain", true);
    auto query = reinterpret_cast<int (*)()>(
        dlsym(foreign_host, "integration_scope_probe"));
    invoke = reinterpret_cast<decltype(invoke)>(
        dlsym(foreign_host, "integration_invoke"));
    assert(query && query() == 999 && invoke);
  }
  HostThread unrelated;
  if (native)
    unrelated.start();
#if defined(__arm__)
  if (tango) {
    auto call =
        reinterpret_cast<void (*)(uint32_t, uint32_t, uint32_t, int, int)>(
            dlsym(loader, "yuki_bootstrap_tango"));
    auto *slot = static_cast<uint32_t *>(
        mmap(nullptr, getpagesize(), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(call && slot != MAP_FAILED);
    const auto stub = reinterpret_cast<uintptr_t>(slot) + 64;
    *slot = stub | 1U;
    assert(mprotect(slot, getpagesize(), PROT_READ) == 0);
    call(reinterpret_cast<uintptr_t>(slot), 0x12345678U, stub, fd,
         packet_fd + 1);
    assert(*slot == 0x12345678U);
    // The synthetic trampoline page lives until this short-lived host exits.
  } else
#else
  assert(!tango);
#endif
      if (native)
    native_bootstrap(loader_path, fd, packet_fd, 0, invoke);
  else
    bootstrap(fd, packet_fd + 1);
  fprintf(stderr, "bootstrap core=%s errno=%d\n", core_name, errno);
  assert(fcntl(fd, F_GETFD) < 0 && fcntl(packet_fd, F_GETFD) < 0);
  assert(dl_iterate_phdr(loader_visit, nullptr) == 0);
  assert(anonymous_core_count(inspection_fd) ==
         (native && (!module || rejected) ? 0U : 1U));
  close(inspection_fd);
  if (control.fork_process) {
    const pid_t child = control.fork_process();
    assert(child >= 0);
    if (child) {
      int status = 0;
      assert(waitpid(child, &status, 0) == child && status == 0);
      return;
    }
  }
  if (control.exercise) {
    unrelated.start();
    assert(control.exercise(directory) == 0);
    const uintptr_t base = control.base;
    const uintptr_t bridge =
        control.caller_bridge & ~(getpagesize() - uintptr_t{1});
    unsigned char residency;
    assert(bridge && mincore(reinterpret_cast<void *>(bridge), getpagesize(),
                             &residency) == 0);
    assert(control.retire(nullptr) == 0);
    assert(mincore(reinterpret_cast<void *>(base), getpagesize(), &residency) ==
           -1);
    assert(mincore(reinterpret_cast<void *>(bridge), getpagesize(),
                   &residency) == -1);
    puts("PASS Core and caller-bridge unmapping with an unrelated host thread");
  }
  unrelated.finish();
  if (rejected)
    assert(control.module_constructors == 1 && control.module_finalizers == 1 &&
           control.native_entries == 0);
  if (module && !rejected) {
    assert(control.module_constructors == 1 && control.native_entries == 1);
    assert(control.module_probe && control.module_probe());
    if (native) {
      unsigned active = 1;
      pthread_t thread;
      assert(control.native_thread && control.native_fork);
      assert(pthread_create(&thread, nullptr, control.native_thread, &active) ==
             0);
      fprintf(stderr, "native resident: worker started\n");
      for (unsigned i = 0; i < 2000; ++i)
        assert(control.module_probe());
      fprintf(stderr, "native resident: concurrent queries complete\n");
      __atomic_store_n(&active, 0U, __ATOMIC_RELEASE);
      assert(pthread_join(thread, nullptr) == 0 &&
             control.thread_destructors == 1);
      // Bionic's linker mutex is not reset by fork. Quiesce system libdl calls
      // before exercising libdl in the child; post-fork concurrent libdl is not
      // an async-signal-safe contract, even without this Core.
      assert(control.native_fork() == 0);
      fprintf(stderr,
              "native resident: module fork after query drain complete\n");
    }
  }
  if (foreign_host)
    assert(dlclose(foreign_host) == 0);
  assert(dlclose(android) == 0 && dlclose(host_library) == 0);
  fflush(stdout);
  const pid_t child = fork();
  assert(child >= 0);
  if (!child)
    exit(0);
  int status = 0;
  assert(waitpid(child, &status, 0) == child && status == 0);
  printf("PASS integrated %s module=%d\n", core_name, module);
}
} // namespace
int main(int argc, char **argv) {
  assert(argc == 2 || argc == 3);
  const int modes = 12;
  for (int mode = 0; mode < modes; ++mode) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      alarm(60);
      if (mode == 9 && sizeof(void *) != 4)
        _exit(0);
      char core[128];
      if (mode == 0 || mode == 4 || mode == 9)
        strcpy(core, "libintegration_ordinary_core.so");
      else
        snprintf(core, sizeof(core), "lib%s%zu.so",
                 mode == 1 ? "zygisk" : "yukizncore", sizeof(void *) * 8);
      const char *module_name = mode == 11 ? "private_ifunc"
                                : mode == 3 || mode == 8 || mode == 10
                                    ? "private"
                                : mode == 5 ? "missing"
                                : mode == 6 ? "invalid"
                                : mode == 7 ? "null_entry"
                                            : nullptr;
      run(argv[1], core, mode != 0 && mode != 1 && mode != 4 && mode != 9,
          module_name, mode == 8 ? 2 : 1, mode >= 5 && mode <= 7, mode == 9,
          mode == 10, mode == 0);
      fflush(stdout);
      _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    if (status)
      fprintf(stderr, "mode=%d status=%d\n", mode, status);
    assert(status == 0);
  }
  for (unsigned fault = 1; fault <= (argc == 3 ? 9U : 8U); ++fault) {
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      char loader[4096], core[4096];
      snprintf(loader, sizeof(loader), "%s/core/libyloader%zu.so", argv[1],
               sizeof(void *) * 8);
      if (fault == 9)
        snprintf(loader, sizeof(loader), "%s", argv[2]);
      snprintf(core, sizeof(core), "%s/libyukizncore%zu.so", argv[1],
               sizeof(void *) * 8);
      if (fault == 8)
        snprintf(core, sizeof(core), "%s/libintegration_ordinary_core.so",
                 argv[1]);
      void *existing = fault == 3 ? dlopen(loader, RTLD_NOW) : nullptr;
      if (fault == 3)
        assert(existing);
      if (fault == 4)
        existing = foreign_library(argv[1], "libintegration_duplicate_libc.so",
                                   "duplicate_candidate", false);
      const int fd =
          open(fault == 7 ? "/dev/null" : core, O_RDONLY | O_CLOEXEC);
      const int inspection = open(core, O_RDONLY | O_CLOEXEC);
      const int packet_fd = packet();
      assert(fd >= 0 && inspection >= 0);
      native_bootstrap(loader, fd, packet_fd, fault);
      assert(fcntl(fd, F_GETFD) < 0 && fcntl(packet_fd, F_GETFD) < 0);
      assert(anonymous_core_count(inspection) == 0 && close(inspection) == 0);
      if (existing)
        assert(dlclose(existing) == 0);
      puts("PASS generated native refusal, descriptor cleanup and original "
           "entry");
      fflush(stdout);
      _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && status == 0);
  }
}
