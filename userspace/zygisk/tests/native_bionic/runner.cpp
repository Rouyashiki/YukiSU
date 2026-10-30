#include "check.hpp"
#include "process_maps.hpp"
#include "userspace/zygisk/daemon/zygiskd.hpp"
#include "userspace/zygisk/load_policy.hpp"
#include "zygisk_next_api.h"

#include <algorithm>
#include <climits>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "native_bionic.hpp"
#include <android/dlext.h>

namespace {
int events[6]{};
int loads;
int reports;
std::vector<std::string> logs;

void log_message(const char *format, ...) {
  char message[2048];
  va_list args;
  va_start(args, format);
  (void)vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  logs.emplace_back(message);
  puts(message);
}
#define LOGI(...) log_message(__VA_ARGS__)
#define LOGE(...) log_message(__VA_ARGS__)

bool logged(std::string_view text) {
  for (const auto &line : logs)
    if (line.find(text) != std::string::npos)
      return true;
  return false;
}

void observe_loaded(void *so) {
  if (so != nullptr) {
    const auto probe =
        reinterpret_cast<void (*)()>(dlsym(so, "native_identity_probe"));
    if (probe != nullptr)
      probe();
  }
}

constexpr int kApiVersion3 = 3;
constexpr int kApiVersion4 = ZYGISK_NEXT_API_VERSION;
constexpr int kSuccess = ZN_SUCCESS;
constexpr int kFailed = ZN_FAILED;
constexpr uint32_t kHandleMagic = 0x595a4e31u;

#include "native_handle_under_test.inc"

static_assert(sizeof(ModuleHandle{}.device) >=
              sizeof(std::declval<struct stat>().st_dev));
static_assert(sizeof(ModuleHandle{}.inode) >=
              sizeof(std::declval<struct stat>().st_ino));

std::vector<ModuleHandle *> g_loaded_modules;
yz_config g_yz_config = yukizygisk::config::defaults;
bool report_native_injection(uint32_t) {
  ++reports;
  return true;
}

std::string self_exe_path() {
  char path[4096];
  const ssize_t size = readlink("/proc/self/exe", path, sizeof(path));
  CHECK(size > 0 && size < static_cast<ssize_t>(sizeof(path)));
  return {path, static_cast<size_t>(size)};
}
std::string clean_map_path(const std::string &path) { return path; }
} // namespace

#if defined(__ANDROID__)
#include "caller_linker_under_test.inc"
namespace yukizygisk::native_bionic {
bool resolve_loader(const Maps &maps, LoaderApi &api, Failure &failure) {
  return checked_resolve(maps, api, failure);
}
} // namespace yukizygisk::native_bionic
#include "caller_native_under_test.inc"
#endif

namespace yukizygisk::native_bionic {
#if !defined(__ANDROID__)
#include "caller_names_under_test.inc"
#endif
LoadResult load(const char *path, int flags, const android_dlextinfo &ext) {
  ++loads;
  CHECK(path[0] == '/');
  CHECK(flags == (RTLD_NOW | RTLD_LOCAL));
  CHECK(ext.flags == ANDROID_DLEXT_USE_LIBRARY_FD);
  CHECK(fcntl(ext.library_fd, F_GETFD) >= 0);
#if defined(__ANDROID__)
  auto result = checked_load(path, flags, ext);
#else
  // The Android fixture runs the production Bionic resolver and FD loader.
  LoadResult result;
  result.failure.stage = Stage::Load;
  dlerror();
  errno = 0;
  result.handle = dlopen(path, flags);
  if (result.handle == nullptr) {
    const int saved_errno = errno;
    const char *error = dlerror();
    result.loader_error = error != nullptr ? error : "no linker error";
    result.failure = {Stage::Load, Error::LoaderFailure, saved_errno};
  }
#endif
  observe_loaded(result.handle);
  return result;
}
} // namespace yukizygisk::native_bionic

namespace yukizygisk::hyos {
bool available() { return false; }
bool in_specialized_child() { return false; }
size_t registered_module_count() { return 0; }
void begin_module_registration(uint32_t *) {}
void end_module_registration() {}
} // namespace yukizygisk::hyos

namespace {
#include "native_map_under_test.inc"
extern "C" int fixture_plt_hook(void *, const char *, void *, void **);
extern "C" void fixture_recover_plt();
extern "C" bool fixture_retire_plt();
void recover_native_plt() { fixture_recover_plt(); }

const ZygiskNextAPI g_api{fixture_plt_hook};

#include "native_load_under_test.inc"

zygiskd::NativeModuleInfo info(const char *id, const std::string &path) {
  zygiskd::NativeModuleInfo result{};
  CHECK(strlen(id) < sizeof(result.module_id));
  CHECK(path.size() < sizeof(result.lib_path));
  strcpy(result.module_id, id);
  strcpy(result.lib_path, path.c_str());
  return result;
}

bool load(const zygiskd::NativeModuleInfo &module, bool early = false) {
  const int fd = open(module.lib_path, O_RDONLY | O_CLOEXEC);
  CHECK(fd >= 0);
  const bool result = load_native_module_from_fd(module, 0, fd, early);
  CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
  return result;
}

void expect_file_mapping(void *address) {
  for (const auto &map : yukizygisk::MapInfo::Scan()) {
    const auto value = reinterpret_cast<uintptr_t>(address);
    if (value >= map.start && value < map.end) {
      CHECK(map.inode != 0 && map.is_private);
      CHECK((map.perms & (PROT_READ | PROT_EXEC)) == (PROT_READ | PROT_EXEC));
      CHECK((map.perms & PROT_WRITE) == 0);
      return;
    }
  }
  std::abort();
}
} // namespace

extern "C" void fixture_event(int event) {
  CHECK(event > 0 && event < 6);
  ++events[event];
}

int main(int argc, char **argv) {
  CHECK(setvbuf(stdout, nullptr, _IONBF, 0) == 0);
  CHECK(argc == 5);
  const std::string directory = argv[1];
  g_yz_config.yukilinker = atoi(argv[2]);
  g_yz_config.memory_type =
      atoi(argv[3]) ? YZ_MEMORY_ANONYMOUS : YZ_MEMORY_FILE;
  const bool early = atoi(argv[4]) != 0;
  // The private dependency must be absent from the host global group.
  CHECK(dlsym(RTLD_DEFAULT, "yz_native_private_value") == nullptr);
  CHECK(yz_native_original() == 29);

  auto good = info("fixture", directory + "/libyz_native_module.so");
  auto invalid = good;
  invalid.module_id[0] = '\0';
  CHECK(!load(invalid) && loads == 0);
  CHECK(!load_native_module_from_fd(good, 0, -1, false) && loads == 0);
  const int closed = open(good.lib_path, O_RDONLY | O_CLOEXEC);
  CHECK(closed >= 0 && close(closed) == 0);
  CHECK(!load_native_module_from_fd(good, 0, closed, false) && loads == 0);
  CHECK(logged("library identity failed"));
  auto malformed = info("malformed", directory + "/malformed.so");
  CHECK(!load(malformed) && loads == 1);
  CHECK(logged("Bionic load failed") && !logged("no linker error"));
  CHECK(events[1] == 0 && events[3] == 0);

  for (const char *name : {"missing", "invalid", "null"}) {
    const int before = loads;
    CHECK(!load(info(name, directory + "/libyz_native_" + name + ".so")));
    CHECK(loads == before + 1);
    CHECK(events[1] == events[2] && events[3] == 0);
    CHECK(g_loaded_modules.empty());
  }
  CHECK(logged("zn_module lookup failed") && logged("unsupported module"));
  const int prior_ctors = events[1];
  const int prior_dtors = events[2];
  CHECK(load(good, early));
  CHECK(events[1] == prior_ctors + 1 && events[2] == prior_dtors);
  CHECK(events[3] == 1 && g_loaded_modules.size() == 1);
  CHECK(reports == (early ? 0 : 1));
  const auto probe = reinterpret_cast<void (*)()>(
      dlsym(g_loaded_modules[0]->so, "native_identity_probe"));
  CHECK(probe != nullptr);
  expect_file_mapping(reinterpret_cast<void *>(probe));
  probe();
  CHECK(events[5] == 3); // Load return, initialization, initialization return.
  CHECK(yz_native_original() == 202 && events[4] == 1);
  CHECK(events[5] == 4);
  CHECK(dlsym(RTLD_DEFAULT, "yz_native_private_value") == nullptr);

  const int before = loads;
  CHECK(load(good, early));
  CHECK(loads == before && events[3] == 1);
  auto alias = info("alias", good.lib_path);
  CHECK(!load(alias));
  CHECK(loads == before && events[3] == 1);
  CHECK(events[1] == prior_ctors + 1 && events[2] == prior_dtors);
  CHECK(yz_native_original() == 202 && events[4] == 2);
  probe();
  CHECK(events[2] == prior_dtors);
  const auto other =
      info("different-inode", directory + "/alternate/libyz_native_module.so");
  CHECK(load(other, early));
  CHECK(g_loaded_modules.size() == 2 && events[3] == 2);
  CHECK(events[1] == prior_ctors + 2 && events[2] == prior_dtors);
  CHECK(g_loaded_modules[0]->so != g_loaded_modules[1]->so);
  CHECK(yz_native_original() == 202);
  const auto second_hook_status = reinterpret_cast<int (*)()>(
      dlsym(g_loaded_modules[1]->so, "native_hook_status"));
  CHECK(second_hook_status != nullptr && second_hook_status() == ZN_FAILED);
#if defined(__ANDROID__)
  const int memory_fd = memfd_create("zn-native-fixture", MFD_CLOEXEC);
  const int source_fd = open(good.lib_path, O_RDONLY | O_CLOEXEC);
  CHECK(memory_fd >= 0 && source_fd >= 0);
  char buffer[4096];
  ssize_t count;
  while ((count = read(source_fd, buffer, sizeof(buffer))) > 0)
    CHECK(write(memory_fd, buffer, static_cast<size_t>(count)) == count);
  CHECK(count == 0 && close(source_fd) == 0);
  auto fd_only = info("fd-only", directory + "/does-not-exist.so");
  CHECK(access(fd_only.lib_path, F_OK) == -1 && errno == ENOENT);
  CHECK(load_native_module_from_fd(fd_only, 0, memory_fd, early));
  CHECK(fcntl(memory_fd, F_GETFD) == -1 && errno == EBADF);
  CHECK(g_loaded_modules.size() == 3 && events[3] == 3);
  CHECK(events[1] == prior_ctors + 3 && events[2] == prior_dtors);
  CHECK(yz_native_original() == 202);
#endif
  CHECK(fixture_retire_plt());
  CHECK(yz_native_original() == 29);
  puts("PASS: Native identity, local dependency, runtime PLT callback, "
       "retention, duplicate and failure paths");
  // Deliberately retain module handles until exit, as the production core does.
  for (auto *handle : g_loaded_modules)
    delete handle;
  g_loaded_modules.clear();
  return 0;
}
