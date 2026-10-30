#include "plt_lifecycle.hpp"
#include "process_maps.hpp"
#include "userspace/zygisk/endpoint.hpp"
#include <yukiplt.hpp>

#include "core_bindings.hpp"
#include "hook.hpp"
#include "load_config.hpp"
#include "loader_handoff.hpp"
#include "log.hpp"
#include "solist.hpp"
#include "userspace/zygisk/daemon/zygiskd.hpp"
#include "yukilinker.hpp"
#include "zygisk.hpp"

#include "uapi/supercall.h"
#include "uapi/yukizygisk.h"

#include <android/dlext.h>
#include <dlfcn.h>
#include <link.h>
#include <regex.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using zygisk::Option;
using zygisk::internal::api_table;
using zygisk::internal::module_abi;

namespace {

#define LOGE(...) ZLOGE(__VA_ARGS__)
#define LOGI(...) ZLOGI(__VA_ARGS__)

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif // #ifndef MFD_CLOEXEC

/* Module-facing API table. */
struct CoreApiTable {
  void *impl;
  bool (*registerModule)(CoreApiTable *, module_abi *);
  void *slot[8];
};

struct ModulePltRequest {
  yukiplt::NamedImage image;
  std::string symbol;
  void *replacement = nullptr;
  void **original = nullptr;
};
struct ModulePltGroup {
  yukiplt::NamedImage image;
  std::vector<ModulePltRequest> requests;
  std::vector<yukiplt::HookRequest> native;
  std::vector<yukiplt::SlotReport> slots;
  yukiplt::HookHandle handle;
  yukiplt::OperationDiagnostic install;
  yukiplt::OperationDiagnostic restore;
  bool recovery = false;
  bool published = false;
};

struct Module {
  module_abi *abi = nullptr;
  int id = -1; // zygiskd module index
  long version = 0;
  void *handle = nullptr;
  uintptr_t linker_anchor = 0;
  bool yuki_loaded = false;
  std::vector<ModulePltRequest> plt_pending;
  std::vector<std::unique_ptr<ModulePltGroup>> plt_groups;
  bool plt_registration_failed = false;
  uint32_t option = 0; // zygisk::Option bits set via setOption
  CoreApiTable api{};  // per-module, filled by RegisterModuleImpl
};

// Reserved to the zygiskd module count before the first emplace_back, so
// g_cur/g_loading stay valid for the whole load and the per-specialize walks
// below get contiguous storage. Never push past that reserve.
std::vector<Module> g_modules;
std::atomic<pid_t> g_plt_callback_tid{0};
Module *g_cur = nullptr;     // module currently in onLoad/pre/post
Module *g_loading = nullptr; // module currently being registered
int g_loading_id = -1;       // zygiskd index of the module being loaded

// zygiskd-backed helpers.
int zd_module_dir(int id);
int zd_connect_companion(int id);
uint32_t zd_get_flags(int uid);
bool zd_get_module_info(int id, char *module_id, size_t module_id_size);
void zd_report_zygote_module(const char *module_id, uint8_t state);
int g_app_uid = -1; // uid of the process currently being specialized
bool g_module_policy_armed = false;
uint32_t g_monitor_zygote_pid = 0;
uint32_t g_monitor_zygote_generation = 0;

void api_hook_jni_native_methods(JNIEnv *env, const char *cls,
                                 JNINativeMethod *methods, int n) {
  if (g_cur != nullptr)
    zygisk_hook_jni_methods(env, cls, methods, n, g_cur->id);
}

bool same_module_image(const yukiplt::NamedImage &left,
                       const yukiplt::NamedImage &right) {
  return left.image.load_bias == right.image.load_bias &&
         left.image.phdr_table == right.image.phdr_table &&
         left.image.phdr_count == right.image.phdr_count &&
         left.image.instance_token == right.image.instance_token &&
         left.path == right.path;
}

Module *plt_module() {
  return g_plt_callback_tid.load(std::memory_order_acquire) == gettid()
             ? g_cur
             : nullptr;
}

yukiplt::Result<yukiplt::NamedImage> module_plt_image(dev_t dev, ino_t inode) {
  int map_error = 0;
  const auto maps = yukizygisk::MapInfo::Scan(&map_error);
  if (map_error != 0)
    return yukiplt::Error::Sys(yukiplt::ErrorCode::kVmaUnavailable, map_error);
  std::unique_ptr<yukiplt::NamedImage> found;
  for (const auto &map : maps) {
    if (map.dev != dev || map.inode != inode)
      continue;
    auto image =
        yukiplt::find_image_containing(reinterpret_cast<void *>(map.start));
    if (!image) {
      if (image.error().code == yukiplt::ErrorCode::kImageNotFound)
        continue;
      return image.error();
    }
    if (found && !same_module_image(*found, image.value()))
      return yukiplt::Error::Make(yukiplt::ErrorCode::kAmbiguousImage);
    if (!found)
      found = std::make_unique<yukiplt::NamedImage>(std::move(image.value()));
  }
  if (!found)
    return yukiplt::Error::Make(yukiplt::ErrorCode::kImageNotFound);
  return std::move(*found);
}

void api_plt_hook_register(dev_t dev, ino_t inode, const char *symbol,
                           void *new_func, void **old_func) noexcept {
  Module *module = plt_module();
  if (module == nullptr)
    return;
  yukizygisk::plt_lifecycle::Operation operation;
  if (!operation || inode == 0 || symbol == nullptr || *symbol == '\0' ||
      new_func == nullptr ||
      (old_func != nullptr &&
       reinterpret_cast<uintptr_t>(old_func) % alignof(void *) != 0)) {
    module->plt_registration_failed = true;
    return;
  }
  try {
    auto image = module_plt_image(dev, inode);
    if (!image) {
      module->plt_registration_failed = true;
      LOGE("module %d PLT image: %s errno=%d", module->id,
           yukiplt::error_name(image.error().code), image.error().sys_errno);
      return;
    }
    module->plt_pending.push_back(
        {std::move(image.value()), symbol, new_func, old_func});
  } catch (...) {
    module->plt_registration_failed = true;
    LOGE("module %d PLT registration failed", module->id);
  }
}

void api_plt_hook_register_byname(const char *path_regex, const char *symbol,
                                  void *new_func, void **old_func) noexcept {
  Module *module = plt_module();
  if (module == nullptr)
    return;
  regex_t expression;
  if (path_regex == nullptr || symbol == nullptr || new_func == nullptr ||
      regcomp(&expression, path_regex, REG_NOSUB) != 0) {
    module->plt_registration_failed = true;
    return;
  }
  int error = 0;
  const auto maps = yukizygisk::MapInfo::Scan(&error);
  if (error != 0)
    module->plt_registration_failed = true;
  for (const auto &map : maps) {
    if (map.offset == 0 && (map.perms & PROT_READ) && map.inode != 0 &&
        regexec(&expression, map.path.c_str(), 0, nullptr, 0) == 0)
      api_plt_hook_register(map.dev, map.inode, symbol, new_func, old_func);
  }
  regfree(&expression);
}

void api_plt_hook_exclude(const char *, const char *) {}

bool api_plt_hook_commit() noexcept {
  Module *module = plt_module();
  if (module == nullptr)
    return false;
  yukizygisk::plt_lifecycle::Operation operation;
  if (!operation)
    return false;
  if (module->plt_registration_failed) {
    module->plt_registration_failed = false;
    module->plt_pending.clear();
    return false;
  }
  auto pending = std::move(module->plt_pending);
  if (pending.empty())
    return true;
  try {
    for (size_t i = 0; i < pending.size(); ++i) {
      const auto &request = pending[i];
      if (!zygisk_original_output_allowed(request.original))
        return false;
      for (size_t j = 0; j < i; ++j)
        if ((request.original != nullptr &&
             request.original == pending[j].original) ||
            (same_module_image(request.image, pending[j].image) &&
             request.symbol == pending[j].symbol))
          return false;
      if (request.original != nullptr)
        for (const auto &owner : g_modules)
          for (const auto &group : owner.plt_groups)
            if (group->published)
              for (const auto &previous : group->requests)
                if (request.original == previous.original)
                  return false;
    }
    std::vector<std::unique_ptr<ModulePltGroup>> groups;
    for (auto &request : pending) {
      ModulePltGroup *group = nullptr;
      for (auto &candidate : groups)
        if (same_module_image(candidate->image, request.image)) {
          group = candidate.get();
          break;
        }
      if (group == nullptr) {
        auto prepared = std::make_unique<ModulePltGroup>();
        prepared->image = request.image;
        group = prepared.get();
        groups.push_back(std::move(prepared));
      }
      group->requests.push_back(std::move(request));
      if (group->requests.size() > 64)
        return false;
    }
    for (auto &group : groups) {
      group->native.reserve(group->requests.size());
      group->slots.resize(group->requests.size());
      for (const auto &request : group->requests)
        group->native.push_back(
            {request.symbol.c_str(), request.replacement, request.original});
    }
    module->plt_groups.reserve(module->plt_groups.size() + groups.size());
    const size_t first = module->plt_groups.size();
    for (auto &group : groups)
      module->plt_groups.push_back(std::move(group));
    bool success = true;
    for (size_t i = first; i < module->plt_groups.size(); ++i) {
      auto &group = *module->plt_groups[i];
      auto result = yukiplt::hook_batch_nonnull_diagnosed_named(
          group.image, group.native.data(), group.native.size(),
          group.slots.data(), &group.install);
      group.handle = std::move(result.handle);
      group.published = group.install.stage >= yukiplt::OperationStage::kCommit;
      yukizygisk::plt_lifecycle::set_recovery(
          group.recovery, !result.ok() && group.handle.valid());
      success &= result.ok();
      LOGI("module %d PLT: path=%s result=%s cause=%s errno=%d recovery=%d",
           module->id, group.image.path.c_str(),
           yukiplt::error_name(result.error.code),
           yukiplt::error_cause_name(group.install.cause),
           result.error.sys_errno, group.recovery);
      if (group.recovery)
        break;
    }
    return success;
  } catch (...) {
    LOGE("module %d PLT preparation failed", module->id);
    return false;
  }
}

void recover_module_plt(Module &module) {
  yukizygisk::plt_lifecycle::Operation operation(
      yukizygisk::plt_lifecycle::Access::Recover);
  if (!operation)
    return;
  for (auto &group : module.plt_groups) {
    if (!group->recovery)
      continue;
    const auto error =
        yukiplt::unhook_diagnosed(group->handle, group->slots.data(),
                                  group->slots.size(), &group->restore);
    yukizygisk::plt_lifecycle::set_recovery(group->recovery,
                                            group->handle.valid());
    LOGI("module %d PLT recovery: %s errno=%d consumed=%d", module.id,
         yukiplt::error_name(error.code), error.sys_errno,
         group->restore.handle_consumed);
  }
}

bool module_plt_can_unload(Module &module) {
  yukizygisk::plt_lifecycle::Operation operation(
      yukizygisk::plt_lifecycle::Access::Recover);
  if (!operation)
    return false;
  module.plt_pending.clear();
  return std::none_of(module.plt_groups.begin(), module.plt_groups.end(),
                      [](const auto &group) {
                        return group->handle.valid() || group->published;
                      });
}

class ModuleCallbackScope {
public:
  explicit ModuleCallbackScope(Module &module) : module_(module) {
    g_cur = &module;
    g_plt_callback_tid.store(gettid(), std::memory_order_release);
  }
  ~ModuleCallbackScope() {
    g_plt_callback_tid.store(0, std::memory_order_release);
    g_cur = nullptr;
    recover_module_plt(module_);
  }
  ModuleCallbackScope(const ModuleCallbackScope &) = delete;
  ModuleCallbackScope &operator=(const ModuleCallbackScope &) = delete;
  ModuleCallbackScope(ModuleCallbackScope &&) = delete;
  ModuleCallbackScope &operator=(ModuleCallbackScope &&) = delete;

private:
  Module &module_;
};

bool api_exempt_fd(int fd) { return zygisk_exempt_fd(fd); }

int api_connect_companion(void * /*impl*/) {
  return g_cur != nullptr ? zd_connect_companion(g_cur->id) : -1;
}

void api_set_option(void * /*impl*/, Option opt) {
  if (g_cur != nullptr)
    g_cur->option |= (1u << static_cast<int>(opt));
}

int api_get_module_dir(void * /*impl*/) {
  // Fresh fd; fork sanitization closes it unless exempted.
  int fd = g_cur != nullptr ? zd_module_dir(g_cur->id) : -1;
  if (fd >= 0)
    g_module_policy_armed = true;
  return fd;
}

uint32_t api_get_flags(void * /*impl*/) { return zd_get_flags(g_app_uid); }

bool RegisterModuleImpl(CoreApiTable *tbl, module_abi *abi) {
  long v = abi == nullptr ? 0 : abi->api_version;
  if (v < 1 || v > ZYGISK_API_VERSION) {
    LOGE("module api_version %ld unsupported (need 1..%d), rejecting", v,
         ZYGISK_API_VERSION);
    return false;
  }
  if (g_loading == nullptr)
    return false;
  g_loading->abi = abi;
  g_loading->version = v;
  g_cur = g_loading;
  g_plt_callback_tid.store(
      gettid(),
      std::memory_order_release); // current module for onLoad's api calls

  tbl->slot[0] = reinterpret_cast<void *>(api_hook_jni_native_methods);
  tbl->slot[3] = reinterpret_cast<void *>(api_plt_hook_commit);
  tbl->slot[4] = reinterpret_cast<void *>(api_connect_companion);
  tbl->slot[5] = reinterpret_cast<void *>(api_set_option);
  if (v >= 4) {
    tbl->slot[1] = reinterpret_cast<void *>(api_plt_hook_register); // dev/inode
    tbl->slot[2] = reinterpret_cast<void *>(api_exempt_fd);
  } else {
    tbl->slot[1] = reinterpret_cast<void *>(api_plt_hook_register_byname);
    tbl->slot[2] = reinterpret_cast<void *>(api_plt_hook_exclude);
  }
  if (v >= 2) {
    tbl->slot[6] = reinterpret_cast<void *>(api_get_module_dir);
    tbl->slot[7] = reinterpret_cast<void *>(api_get_flags);
  }
  LOGI("registered module %d (api v%ld)", g_loading_id, v);
  return true;
}

/* Constructors may not run at AT_ENTRY. */
volatile int g_ctors_done = 0;
__attribute__((constructor)) void mark_ctors_done() { g_ctors_done = 1; }

using ZdRequest = zygiskd::Request;

bool read_all(int fd, void *buf, size_t n) {
  auto *p = static_cast<uint8_t *>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0)
      return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

bool write_all(int fd, const void *buf, size_t n) {
  const auto *p = static_cast<const uint8_t *>(buf);
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w <= 0)
      return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

int recv_fd(int sock) {
  char data = 0;
  char cbuf[CMSG_SPACE(sizeof(int))] = {};
  iovec io{&data, 1};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof(cbuf);
  if (recvmsg(sock, &msg, 0) <= 0)
    return -1;
  for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int fd = -1;
      memcpy(&fd, CMSG_DATA(c), sizeof(fd));
      return fd;
    }
  return -1;
}

int connect_zygiskd() { return yukizygisk::endpoint::connect(); }

/* One-shot fd request to zygiskd. */
int zd_request_fd(ZdRequest req, uint32_t arg) {
  int sock = connect_zygiskd();
  if (sock < 0)
    return -1;
  auto r = static_cast<uint8_t>(req);
  if (write(sock, &r, 1) != 1 ||
      write(sock, &arg, sizeof(arg)) != static_cast<ssize_t>(sizeof(arg))) {
    close(sock);
    return -1;
  }
  int fd = recv_fd(sock);
  close(sock);
  return fd;
}

int zd_module_dir(int id) {
  return zd_request_fd(ZdRequest::GetModuleDir, static_cast<uint32_t>(id));
}

int zd_connect_companion(int id) {
  return zd_request_fd(ZdRequest::ConnectCompanion, static_cast<uint32_t>(id));
}

uint32_t zd_get_flags(int uid) {
  int sock = connect_zygiskd();
  if (sock < 0)
    return 0;
  auto r = static_cast<uint8_t>(ZdRequest::GetProcessFlags);
  auto u = static_cast<uint32_t>(uid);
  if (write(sock, &r, 1) != 1 ||
      write(sock, &u, sizeof(u)) != static_cast<ssize_t>(sizeof(u))) {
    close(sock);
    return 0;
  }
  uint32_t flags = 0;
  read_all(sock, &flags, sizeof(flags));
  close(sock);
  return flags;
}

using module_entry_fn = void (*)(api_table *, JNIEnv *);
constexpr char kSystemModuleName[] = "libzygiskmodule.so";

bool image_has_tls(int fd) {
  ElfW(Ehdr) ehdr{};
  if (fd < 0 || pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr) ||
      memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
      ehdr.e_phentsize != sizeof(ElfW(Phdr)) || ehdr.e_phnum == PN_XNUM ||
      ehdr.e_phnum == 0 || ehdr.e_phnum > 256)
    return false;

  std::vector<ElfW(Phdr)> phdrs(ehdr.e_phnum);
  const size_t size = phdrs.size() * sizeof(phdrs[0]);
  if (pread(fd, phdrs.data(), size, static_cast<off_t>(ehdr.e_phoff)) !=
      static_cast<ssize_t>(size))
    return false;
  for (const auto &phdr : phdrs)
    if (phdr.p_type == PT_TLS)
      return true;
  return false;
}

/* Copy a daemon-staged image into a zygote-owned executable memfd. */
int make_app_memfd(int src_fd) {
  struct stat st{};
  if (fstat(src_fd, &st) != 0 || st.st_size <= 0)
    return -1;
  size_t sz = static_cast<size_t>(st.st_size);
  void *src = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, src_fd, 0);
  if (src == MAP_FAILED)
    return -1;
  int mfd = static_cast<int>(syscall(__NR_memfd_create, "", MFD_CLOEXEC));
  bool ok = mfd >= 0 && ftruncate(mfd, static_cast<off_t>(sz)) == 0;
  if (ok) {
    void *dst = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (dst != MAP_FAILED) {
      memcpy(dst, src, sz);
      munmap(dst, sz);
    } else {
      ok = false;
    }
  }
  munmap(src, sz);
  if (!ok) {
    if (mfd >= 0)
      close(mfd);
    return -1;
  }
  lseek(mfd, 0, SEEK_SET);
  return mfd;
}

/* Runtime config from zygiskd. */
yz_config g_yz_config = yukizygisk::config::defaults;

void zd_load_config() { (void)yukizygisk::config::read_runtime(&g_yz_config); }

uint32_t zd_get_runtime_generation(uint8_t kind) {
  int s = connect_zygiskd();
  if (s < 0)
    return 0;
  const uint8_t req = static_cast<uint8_t>(ZdRequest::GetRuntimeGeneration);
  uint32_t generation = 0;
  if (!write_all(s, &req, sizeof(req)) || !write_all(s, &kind, sizeof(kind)) ||
      !read_all(s, &generation, sizeof(generation)))
    generation = 0;
  close(s);
  return generation;
}

void zd_report_zygote(uint32_t generation) {
  if (generation == 0)
    return;
  int s = connect_zygiskd();
  if (s < 0)
    return;
  const uint8_t req = static_cast<uint8_t>(ZdRequest::ReportZygote);
  uint8_t ack = 0;
  if (write_all(s, &req, sizeof(req)) &&
      write_all(s, &generation, sizeof(generation)))
    (void)read_all(s, &ack, sizeof(ack));
  close(s);
}

bool zd_get_module_info(int id, char *module_id, size_t module_id_size) {
  if (module_id == nullptr || module_id_size == 0)
    return false;
  int s = connect_zygiskd();
  if (s < 0)
    return false;
  const uint8_t req = static_cast<uint8_t>(ZdRequest::GetModuleInfo);
  uint32_t index = static_cast<uint32_t>(id);
  zygiskd::ModuleInfo info{};
  const bool ok = write_all(s, &req, sizeof(req)) &&
                  write_all(s, &index, sizeof(index)) &&
                  read_all(s, &info, sizeof(info));
  close(s);
  info.module_id[sizeof(info.module_id) - 1] = '\0';
  if (!ok || info.module_id[0] == '\0')
    return false;
  (void)std::snprintf(module_id, module_id_size, "%s", info.module_id);
  return true;
}

bool zd_module_suspended(uint32_t index) {
  const int socket = connect_zygiskd();
  if (socket < 0)
    return false;
  const uint8_t request = static_cast<uint8_t>(ZdRequest::GetModuleLoadState);
  uint8_t state = 0;
  uint8_t packet[sizeof(request) + sizeof(index)]{request};
  memcpy(packet + sizeof(request), &index, sizeof(index));
  const bool ok = send(socket, packet, sizeof(packet), MSG_NOSIGNAL) ==
                      static_cast<ssize_t>(sizeof(packet)) &&
                  read_all(socket, &state, sizeof(state));
  close(socket);
  return ok && state == 1;
}

void zd_report_zygote_module(const char *module_id, uint8_t state) {
  if (g_monitor_zygote_generation == 0 || module_id == nullptr ||
      module_id[0] == '\0')
    return;
  int s = connect_zygiskd();
  if (s < 0)
    return;
  const uint8_t req = static_cast<uint8_t>(ZdRequest::ReportZygoteModule);
  zygiskd::ZygoteModuleReport report{};
  report.zygote_pid = g_monitor_zygote_pid;
  report.generation = g_monitor_zygote_generation;
  report.state = state;
  (void)std::snprintf(report.module_id, sizeof(report.module_id), "%s",
                      module_id);
  uint8_t ack = 0;
  if (write_all(s, &req, sizeof(req)) && write_all(s, &report, sizeof(report)))
    (void)read_all(s, &ack, sizeof(ack));
  close(s);
}

void zd_restore_load_policy() {
  int s = connect_zygiskd();
  if (s < 0)
    return;
  uint8_t req = static_cast<uint8_t>(ZdRequest::RestoreLoadPolicy);
  uint8_t ack = 0;
  if (write(s, &req, 1) == 1 && read_all(s, &ack, sizeof(ack)))
    LOGI("zygote load policy restore: ok=%u", ack ? 1U : 0U);
  else
    LOGE("zygote load policy restore request failed");
  close(s);
}

void zd_restore_module_load_policy() {
  if (!g_module_policy_armed)
    return;
  zd_restore_load_policy();
  g_module_policy_armed = false;
}

bool arm_module_load_policy(int id) {
  if (g_module_policy_armed)
    return true;
  int dir_fd = zd_module_dir(id);
  if (dir_fd < 0) {
    LOGE("module %d load policy unavailable", id);
    return false;
  }
  close(dir_fd);
  g_module_policy_armed = true;
  return true;
}

/* Built-in yukilinker symbols. */
extern "C" {
__attribute__((visibility("hidden"))) void *
yuki_core_dlopen_memfd(int memfd, const char *vma_name, bool file_backed);
__attribute__((visibility("hidden"))) void *yuki_core_dlsym(void *handle,
                                                            const char *name);
__attribute__((visibility("hidden"))) void yuki_core_dlclose(void *handle);
}
using yuki_dlopen_fn = void *(*)(int, const char *, bool);
using yuki_dlsym_fn = void *(*)(void *, const char *);
using yuki_dlclose_fn = void (*)(void *);
yuki_dlopen_fn g_yuki_dlopen = nullptr;
yuki_dlsym_fn g_yuki_dlsym = nullptr;
yuki_dlclose_fn g_yuki_dlclose = nullptr;
/* libzygisk mapping range from yukilinker. */
uintptr_t g_self_base = 0;
size_t g_self_size = 0;

void load_modules_impl(JNIEnv *env) {
  if (!g_modules.empty())
    return; // already loaded in this process (called per-specialize)
  zd_load_config();
  int sock = connect_zygiskd();
  if (sock < 0) {
    LOGE("cannot connect zygiskd");
    return;
  }
  auto req = static_cast<uint8_t>(ZdRequest::GetModuleCount);
  uint32_t count = 0;
  if (write(sock, &req, 1) != 1 || !read_all(sock, &count, sizeof(count))) {
    close(sock);
    return;
  }
  close(sock);
  LOGI("zygiskd reports %u module(s)", count);

  // Must precede every emplace_back below: g_loading/g_cur hold Module* into
  // this storage, so a reallocation mid-load would dangle them. The loop below
  // emplaces at most once per iteration and runs `count` times, so reserving
  // `count` is sufficient -- but `count` arrives over a socket, and reserving
  // an absurd value here would abort inside Zygote (this target is built
  // -fno-exceptions) and take the boot with it. The deque this replaced grew
  // only as modules actually arrived, so it had no equivalent exposure.
  constexpr uint32_t kMaxModules = 1024;
  if (count > kMaxModules) {
    LOGE("zygiskd reported %u modules, clamping to %u", count, kMaxModules);
    count = kMaxModules;
  }
  g_modules.reserve(count);

  // Arm the temporary module-load policy before receiving the first module
  // image. On policies without the memfd_file class, SCM_RIGHTS reception of
  // zygiskd's read-only memfd requires temporary tmpfs:file access for the
  // SCM_RIGHTS handoff plus fstat() and the read-only source mapping.
  if (count > 0)
    (void)arm_module_load_policy(0);

  for (uint32_t i = 0; i < count; ++i) {
    if (zd_module_suspended(i)) {
      LOGI("module %u injection suspended for this boot", i);
      continue;
    }
    char module_id[YZ_NATIVE_MODULE_ID_MAX]{};
    if (g_monitor_zygote_generation != 0)
      (void)zd_get_module_info(static_cast<int>(i), module_id,
                               sizeof(module_id));
    const auto report_module = [&](uint8_t state) {
      zd_report_zygote_module(module_id, state);
    };
    int s = connect_zygiskd();
    if (s < 0) {
      report_module(YZ_RUNTIME_STATE_FAILED);
      continue;
    }
    req = static_cast<uint8_t>(ZdRequest::GetModuleFd);
    if (write(s, &req, 1) != 1 || write(s, &i, sizeof(i)) != sizeof(i)) {
      close(s);
      report_module(YZ_RUNTIME_STATE_FAILED);
      continue;
    }
    int lib_fd = recv_fd(s);
    close(s);
    if (lib_fd < 0) {
      LOGE("no fd for module %u", i);
      report_module(YZ_RUNTIME_STATE_FAILED);
      continue;
    }

    void *handle = nullptr;
    module_entry_fn entry = nullptr;
    bool yuki_loaded = false;
    // zygiskd sends a sealed anonymous image. Copy it once more into a
    // zygote-owned memfd so executable mappings use the local tmpfs label.
    int mfd = make_app_memfd(lib_fd);
    close(lib_fd);
    if (mfd >= 0) {
      const bool use_system_tls = image_has_tls(mfd);
      if (g_yz_config.yukilinker && !use_system_tls &&
          g_yuki_dlopen != nullptr && g_yuki_dlsym != nullptr &&
          g_yuki_dlclose != nullptr) {
        handle =
            g_yuki_dlopen(mfd, "", !yukizygisk::config::anonymous(g_yz_config));
        if (handle != nullptr) {
          yuki_loaded = true;
          entry = reinterpret_cast<module_entry_fn>(
              g_yuki_dlsym(handle, "zygisk_module_entry"));
        }
      }
      if (handle == nullptr) {
        if (use_system_tls)
          LOGI("module %u has PT_TLS; using system linker", i);
        android_dlextinfo ext{};
        ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD | ANDROID_DLEXT_FORCE_LOAD;
        ext.library_fd = mfd;
        handle = android_dlopen_ext(kSystemModuleName, RTLD_NOW, &ext);
        if (handle != nullptr) {
          LOGI("module %u using system linker", i);
          entry = reinterpret_cast<module_entry_fn>(
              dlsym(handle, "zygisk_module_entry"));
          if (entry != nullptr && yukizygisk::config::anonymous(g_yz_config)) {
            int anonymized = yuki::solist::spoof_loaded_object_maps(
                reinterpret_cast<uintptr_t>(entry), true);
            LOGI("module %u system load anonymized %d segment(s)", i,
                 anonymized);
          }
        }
      }
      close(mfd);
    }
    if (handle == nullptr) {
      LOGE("dlopen module %u failed", i);
      report_module(YZ_RUNTIME_STATE_FAILED);
      continue;
    }
    if (entry == nullptr) {
      LOGE("module %u has no zygisk_module_entry", i);
      if (yuki_loaded)
        g_yuki_dlclose(handle);
      else
        dlclose(handle);
      report_module(YZ_RUNTIME_STATE_FAILED);
      continue;
    }
    Module &m = g_modules.emplace_back();
    m.id = static_cast<int>(i);
    m.handle = handle;
    m.linker_anchor = yuki_loaded ? 0 : reinterpret_cast<uintptr_t>(entry);
    m.yuki_loaded = yuki_loaded;
    m.api.impl = nullptr; // api callbacks resolve the module via g_cur
    m.api.registerModule = RegisterModuleImpl;
    g_loading = &m;
    g_loading_id = static_cast<int>(i);
    {
      ModuleCallbackScope callback(m);
      entry(reinterpret_cast<api_table *>(&m.api), env);
    }
    g_plt_callback_tid.store(0, std::memory_order_release);
    g_cur = nullptr;
    g_loading = nullptr;
    if (m.version == 0) {
      if (yuki_loaded)
        g_yuki_dlclose(handle);
      else
        dlclose(handle);
      g_modules.pop_back();
      report_module(YZ_RUNTIME_STATE_FAILED);
      continue;
    }
    report_module(YZ_RUNTIME_STATE_INJECTED);
  }
  g_loading = nullptr;
  g_plt_callback_tid.store(0, std::memory_order_release);
  g_cur = nullptr;
  LOGI("loaded %zu module(s)", g_modules.size());
}

/* zygisk API v1/v2 AppSpecializeArgs layout. */
struct AppSpecializeArgs_v1 {
  jint &uid;
  jint &gid;
  jintArray &gids;
  jint &runtime_flags;
  jint &mount_external;
  jstring &se_info;
  jstring &nice_name;
  jstring &instruction_set;
  jstring &app_data_dir;
  jboolean *const is_child_zygote;
  jboolean *const is_top_app;
  jobjectArray *const pkg_data_info_list;
  jobjectArray *const whitelisted_data_info_list;
  jboolean *const mount_data_dirs;
  jboolean *const mount_storage_dirs;

  explicit AppSpecializeArgs_v1(zygisk::AppSpecializeArgs *a)
      : uid(a->uid), gid(a->gid), gids(a->gids),
        runtime_flags(a->runtime_flags), mount_external(a->mount_external),
        se_info(a->se_info), nice_name(a->nice_name),
        instruction_set(a->instruction_set), app_data_dir(a->app_data_dir),
        is_child_zygote(a->is_child_zygote), is_top_app(a->is_top_app),
        pkg_data_info_list(a->pkg_data_info_list),
        whitelisted_data_info_list(a->whitelisted_data_info_list),
        mount_data_dirs(a->mount_data_dirs),
        mount_storage_dirs(a->mount_storage_dirs) {}
};

// Module ABI layout bridge.
void *app_args_for(const Module &m, zygisk::AppSpecializeArgs *v5,
                   AppSpecializeArgs_v1 *v1) {
  return m.version <= 2 ? static_cast<void *>(v1) : static_cast<void *>(v5);
}

void unload_requested_modules(JNIEnv *env) {
  for (auto it = g_modules.rbegin(); it != g_modules.rend(); ++it) {
    auto &m = *it;
    if (m.handle == nullptr ||
        !(m.option & (1u << zygisk::DLCLOSE_MODULE_LIBRARY)))
      continue;
    if (!module_plt_can_unload(m) || !zygisk_restore_module_hooks(env, m.id)) {
      LOGE("module %d unload blocked by active hooks", m.id);
      continue;
    }
    if (m.yuki_loaded) {
      g_yuki_dlclose(m.handle);
      if (static_cast<yukilinker::SoHandle *>(m.handle)->private_state !=
          nullptr) {
        LOGE("module %d mapping release failed", m.id);
        continue;
      }
    } else if (dlclose(m.handle) != 0) {
      LOGE("module %d dlclose failed", m.id);
      continue;
    }
    m.handle = nullptr;
    m.abi = nullptr;
    LOGI("module %d DLCLOSE'd after post-specialize", m.id);
  }
}

void run_app_pre_impl(zygisk::AppSpecializeArgs *args) {
  g_app_uid = args->uid;
  AppSpecializeArgs_v1 v1args(args);
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->preAppSpecialize != nullptr) {
      ModuleCallbackScope callback(m);
      m.abi->preAppSpecialize(m.abi->impl,
                              reinterpret_cast<zygisk::AppSpecializeArgs *>(
                                  app_args_for(m, args, &v1args)));
    }
  g_plt_callback_tid.store(0, std::memory_order_release);
  g_cur = nullptr;
  zd_restore_module_load_policy();
}

/* Hide injected linker entries. */
void hide_injection() {
  yuki::solist::hide_from_solist("libyukilinker"); // split-out loader .so
  for (auto &m : g_modules) {
    if (m.handle != nullptr && !m.yuki_loaded && m.linker_anchor != 0) {
      yuki::solist::drop_lib_containing(m.linker_anchor,
                                        /*keep_mapped=*/true);
      m.linker_anchor = 0;
    }
  }
}

void run_app_post_impl(JNIEnv *env, const zygisk::AppSpecializeArgs *args) {
  auto *mut = const_cast<zygisk::AppSpecializeArgs *>(args);
  AppSpecializeArgs_v1 v1args(mut);
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->postAppSpecialize != nullptr) {
      ModuleCallbackScope callback(m);
      m.abi->postAppSpecialize(
          m.abi->impl, reinterpret_cast<const zygisk::AppSpecializeArgs *>(
                           app_args_for(m, mut, &v1args)));
    }
  g_plt_callback_tid.store(0, std::memory_order_release);
  g_cur = nullptr;
  unload_requested_modules(env);
  hide_injection();
  yz_drop_runtime_header_pages();
}

void run_server_pre_impl(zygisk::ServerSpecializeArgs *args) {
  g_app_uid = args->uid;
  LOGI("run_server_pre: uid=%d, %zu module(s)", args->uid, g_modules.size());
  for (auto &m : g_modules) {
    LOGI("  module %d: preServer=%p postServer=%p", m.id,
         m.abi ? reinterpret_cast<void *>(m.abi->preServerSpecialize) : nullptr,
         m.abi ? reinterpret_cast<void *>(m.abi->postServerSpecialize)
               : nullptr);
    if (m.abi != nullptr && m.abi->preServerSpecialize != nullptr) {
      ModuleCallbackScope callback(m);
      m.abi->preServerSpecialize(m.abi->impl, args);
    }
  }
  g_plt_callback_tid.store(0, std::memory_order_release);
  g_cur = nullptr;
  zd_restore_module_load_policy();
}

void run_server_post_impl(JNIEnv *env,
                          const zygisk::ServerSpecializeArgs *args) {
  LOGI("run_server_post: %zu module(s)", g_modules.size());
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->postServerSpecialize != nullptr) {
      LOGI("  module %d postServerSpecialize", m.id);
      ModuleCallbackScope callback(m);
      m.abi->postServerSpecialize(m.abi->impl, args);
    }
  g_plt_callback_tid.store(0, std::memory_order_release);
  g_cur = nullptr;
  unload_requested_modules(env);
  hide_injection();
}

} // namespace

/* Patch our own text through zygiskd/kernel. */
extern "C" bool yz_patch_text(uintptr_t addr, const void *bytes,
                              unsigned int len) {
  if (bytes == nullptr || len == 0 || len > 64)
    return false;
  int s = connect_zygiskd();
  if (s < 0)
    return false;
  uint8_t req = static_cast<uint8_t>(ZdRequest::PatchText);
  uint64_t a64 = addr;
  uint32_t l32 = len;
  uint8_t ack = 0;
  bool ok = write(s, &req, 1) == 1 &&
            write(s, &a64, sizeof(a64)) == static_cast<ssize_t>(sizeof(a64)) &&
            write(s, &l32, sizeof(l32)) == static_cast<ssize_t>(sizeof(l32)) &&
            write(s, bytes, len) == static_cast<ssize_t>(len);
  if (ok)
    ok = read_all(s, &ack, 1) && ack != 0;
  close(s);
  return ok;
}

extern "C" uint8_t yz_patch_text_checked(uintptr_t addr, const void *expected,
                                         const void *replacement,
                                         unsigned int len) {
  if (expected == nullptr || replacement == nullptr || len == 0 || len > 64)
    return 0;
  int s = connect_zygiskd();
  if (s < 0) {
    return 0;
  }
  const uint8_t req = static_cast<uint8_t>(ZdRequest::PatchTextV2);
  const uint64_t a64 = addr;
  const uint32_t l32 = len;
  uint8_t result = 2;
  const bool sent = write_all(s, &req, 1) && write_all(s, &a64, sizeof(a64)) &&
                    write_all(s, &l32, sizeof(l32)) &&
                    write_all(s, expected, len) &&
                    write_all(s, replacement, len);
  const bool received = sent && read_all(s, &result, 1);
  close(s);
  return received && result <= 2 ? result : 2;
}

extern "C" {
extern void (*__init_array_start[])(void) __attribute__((visibility("hidden")));
extern void (*__init_array_end[])(void) __attribute__((visibility("hidden")));
}

/* Shared core startup. */
static void core_start(const char *self_path) {
  if (!g_ctors_done) {
    for (void (**p)(void) = __init_array_start; p < __init_array_end; ++p)
      if (*p)
        (*p)();
  }
  g_yuki_dlopen = yuki_core_dlopen_memfd;
  g_yuki_dlsym = yuki_core_dlsym;
  g_yuki_dlclose = yuki_core_dlclose;
  zd_load_config();
  const uint32_t runtime_generation =
      zd_get_runtime_generation(YZ_RUNTIME_KIND_ZYGOTE);
  g_monitor_zygote_pid = static_cast<uint32_t>(getpid());
  g_monitor_zygote_generation = runtime_generation;
  LOGI("core start, self=%s", self_path ? self_path : "(null)");
  if (!yuki::solist::prepare_linker())
    LOGE("linker internals unavailable; solist cleanup disabled");
  if (zygisk_hook_bootstrap(self_path))
    zd_report_zygote(runtime_generation);
  zd_restore_load_policy();
}

/* Address inside the first-stage loader mapping. */
uintptr_t g_loader_base = 0;

extern "C" void __cxa_finalize(void *);

static bool first_stage_handoff_complete() {
  if (!yukizygisk::handoff::anonymous_image_matches(g_self_base, g_self_size) ||
      !core_runtime_profile_ready(g_self_base, g_self_size))
    return false;
  const uintptr_t capabilities[] = {
      reinterpret_cast<uintptr_t>(g_yuki_dlopen),
      reinterpret_cast<uintptr_t>(g_yuki_dlsym),
      reinterpret_cast<uintptr_t>(g_yuki_dlclose)};
  const auto result =
      g_self_base == 0
          ? yukizygisk::handoff::verify_loaded(
                reinterpret_cast<uintptr_t>(core_start), g_loader_base,
                capabilities, 3)
          : yukizygisk::handoff::verify(g_self_base, g_self_size, g_loader_base,
                                        capabilities, 3);
  LOGI("loader handoff: %s; slot=%p target=%p", result.reason,
       reinterpret_cast<void *>(result.slot),
       reinterpret_cast<void *>(result.target));
  if (!result.complete && !result.tls_retained)
    LOGE("loader verification failed: %s", result.reason);
  return result.complete;
}

bool g_loader_unmap_safe = false;

extern "C" [[gnu::visibility("default")]] void
zygisk_set_load_config(const yz_config *config) {
  if (config != nullptr)
    g_yz_config = *config;
}

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry(const char *self_path, void *loader_self, void *core_base,
                  void *core_size, int early_packet_fd_plus1) {
  const int packet_fd =
      early_packet_fd_plus1 > 0 ? early_packet_fd_plus1 - 1 : -1;
  const bool endpoint_ready = yukizygisk::endpoint::initialize(packet_fd, true);
  yukizygisk::endpoint::close_packet(packet_fd, false);
  if (!endpoint_ready)
    return;
  g_loader_base = reinterpret_cast<uintptr_t>(loader_self);
  g_self_base = reinterpret_cast<uintptr_t>(core_base);
  g_self_size = reinterpret_cast<size_t>(core_size);
  core_start(self_path);
  // Never turn the first stage into an unmapped trampoline. A partial handoff
  // is safe only while its mapping remains pinned.
  const bool all_pointers_handed_off = first_stage_handoff_complete();
  g_loader_unmap_safe = all_pointers_handed_off;
  if (!g_loader_unmap_safe)
    LOGI("loader handoff is incomplete; retaining first-stage mapping");
  else
    LOGI("loader handoff is complete; first-stage mapping may be unmapped");
}

extern "C" [[gnu::visibility("default")]] bool
zygisk_bootstrap_handoff_ready() {
  return g_loader_unmap_safe;
}

extern "C" [[gnu::visibility("default")]] bool
zygisk_bootstrap_handoff_complete() {
  return g_loader_unmap_safe && yukizygisk::handoff::retirement_ready();
}

namespace {
uintptr_t g_tango_stub = 0;
uint32_t g_tango_stub_size = 0;
} // namespace

extern "C" [[gnu::visibility("default")]] bool
zygisk_set_tango_stub(uint32_t stub, uint32_t size) {
  if (g_tango_stub != 0 || stub == 0 || size != YZ_TANGO_BOOTSTRAP_SIZE ||
      stub > UINT32_MAX - size)
    return false;
  g_tango_stub = stub;
  g_tango_stub_size = size;
  return true;
}

void zygisk_cleanup_tango_stub() {
  if (g_tango_stub == 0)
    return;
  const uint8_t zeros[64]{};
  while (g_tango_stub_size != 0) {
    const auto size = g_tango_stub_size < sizeof(zeros)
                          ? g_tango_stub_size
                          : static_cast<uint32_t>(sizeof(zeros));
    if (!yz_patch_text(g_tango_stub, zeros, size)) {
      LOGE("Tango trampoline cleanup failed");
      return;
    }
    __builtin___clear_cache(reinterpret_cast<char *>(g_tango_stub),
                            reinterpret_cast<char *>(g_tango_stub + size));
    g_tango_stub += size;
    g_tango_stub_size -= size;
  }
  g_tango_stub = 0;
}

/* Retire the first stage only after both references and ownership moved. */
extern "C" [[gnu::visibility("default")]] void zygisk_finalize_loader(int,
                                                                      int) {
  if (!zygisk_bootstrap_handoff_complete()) {
    LOGI("loader retirement not authorized; retaining loader and dependencies");
    return;
  }

  zd_load_config();
  LOGI("finalize_loader: finalizing loader at base=%p munmap=%d",
       reinterpret_cast<void *>(g_loader_base), g_loader_unmap_safe);
  const bool released = yukizygisk::handoff::close_loader_reference();
  LOGI("finalize_loader: bootstrap reference released=%d", released);
  if (released)
    g_loader_base = 0;
}

extern "C" [[gnu::visibility("default")]] void
zygisk_finalize_loader_tango(uint32_t /*got*/, uint32_t /*original*/,
                             uint32_t /*stub*/, int /*core_fd*/,
                             int /*packet_fd*/) {
  zygisk_finalize_loader(0, 0);
}

/* 0=inject, 1=inject+umount, 2=skip+umount. */
int zygisk_inject_decision(int uid) {
  g_app_uid = uid;
  zd_load_config();
  uint32_t flags = zd_get_flags(uid);
  int dec = 0;
  if (flags & zygisk::PROCESS_ON_DENYLIST) {
    if (g_yz_config.denylist_mode == 1)
      dec = 2;
    else if (g_yz_config.denylist_mode == 2)
      dec = 1;
  }
  LOGI("inject_decision: uid=%d flags=0x%x mode=%d -> decision=%d", uid, flags,
       g_yz_config.denylist_mode, dec);
  return dec;
}

extern "C" [[noreturn]] void yz_self_unmap_tail(void *base, size_t span,
                                                CoreUnmapFunction unmap);

namespace {
bool g_core_retiring = false;
}

void zygisk_self_destruct(JNIEnv *env, bool /*isolated*/) {
  if (core_runtime_closed()) {
    if (gettid() != getpid() || !yz_capture_ready() ||
        !yz_capture_outside(g_self_base, g_self_size))
      return;
    const int status = core_finish_runtime_retirement();
    yz_self_unmap_tail(reinterpret_cast<void *>(g_self_base), g_self_size,
                       status == 0 ? core_retirement_unmap() : nullptr);
  }
  if (!g_core_retiring) {
    const auto result = zygisk_self_unhook(env);
    yz_drop_runtime_header_pages();
    if (!result.jni_restored || result.plt_pending || !result.plt_quiescent) {
      LOGE("core retirement incomplete: jni=%d pending=%d quiescent=%d",
           result.jni_restored, result.plt_pending, result.plt_quiescent);
      return;
    }
    if (!zygisk_app_core_unload_safe() ||
        !yukizygisk::handoff::anonymous_image_matches(g_self_base,
                                                      g_self_size) ||
        !core_runtime_profile_ready(g_self_base, g_self_size))
      return;
  }
  // Hook quiescence and module retirement above exclude further Core users.
  // Unrelated ART threads do not own private runtime state when no key exists.
  if (!yukizygisk::plt_lifecycle::retirement_window_ready(
          core_private_tsd_used()) ||
      g_loader_base != 0 || !yz_capture_ready() ||
      !yz_capture_outside(g_self_base, g_self_size) || gettid() != getpid())
    return;
  const auto unmap = core_retirement_unmap();
  if (unmap == nullptr)
    return;
  if (!core_runtime_closed()) {
    {
      yukizygisk::plt_lifecycle::Operation operation(
          yukizygisk::plt_lifecycle::Access::Retire);
      if (!operation || yukizygisk::plt_lifecycle::recovery_pending() ||
          yukilinker::has_active_tls())
        return;
      operation.seal();
      g_core_retiring = true;
      if (!yukilinker::shutdown() || !zygisk_release_hook_storage())
        return;
      std::vector<Module>().swap(g_modules);
      const auto trimmed = yukiplt_c_trim_idle_storage();
      if (trimmed.code != YUKIPLT_C_SUCCESS ||
          !yukizygisk::handoff::release_dependencies())
        return;
    }
    g_cur = nullptr;
    g_loading = nullptr;
    g_plt_callback_tid.store(0, std::memory_order_release);
    LOGI("core retirement prepared; leaving private runtime");
    yukizygisk::endpoint::release();
  }
  // No owning C++ locals or runtime-using calls may follow this boundary.
  const int status = core_finish_runtime_retirement();
  if (!core_runtime_closed())
    return;
  yz_self_unmap_tail(reinterpret_cast<void *>(g_self_base), g_self_size,
                     status == 0 ? unmap : nullptr);
}

bool zygisk_app_core_unload_safe() {
  size_t retained = 0;
  for (const auto &m : g_modules)
    if (m.handle != nullptr ||
        std::any_of(m.plt_groups.begin(), m.plt_groups.end(),
                    [](const auto &group) {
                      return group->handle.valid() || group->published;
                    }))
      retained++;
  if (retained != 0)
    LOGI("core unload deferred: %zu module(s) remain resident", retained);
  return retained == 0;
}

void zygisk_load_modules(JNIEnv *env) { load_modules_impl(env); }
void zygisk_run_app_pre(zygisk::AppSpecializeArgs *args) {
  run_app_pre_impl(args);
}
void zygisk_run_app_post(JNIEnv *env, const zygisk::AppSpecializeArgs *args) {
  run_app_post_impl(env, args);
}
void zygisk_run_server_pre(zygisk::ServerSpecializeArgs *args) {
  run_server_pre_impl(args);
}
void zygisk_run_server_post(JNIEnv *env,
                            const zygisk::ServerSpecializeArgs *args) {
  run_server_post_impl(env, args);
}
