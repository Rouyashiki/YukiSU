#include <lsplt.hpp>

#include "hook.hpp"
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

struct Module {
  module_abi *abi = nullptr;
  int id = -1; // zygiskd module index
  long version = 0;
  void *handle = nullptr;
  uintptr_t linker_anchor = 0;
  bool yuki_loaded = false;
  bool has_plt_hooks = false;
  uint32_t option = 0; // zygisk::Option bits set via setOption
  CoreApiTable api{};  // per-module, filled by RegisterModuleImpl
};

// Reserved to the zygiskd module count before the first emplace_back, so
// g_cur/g_loading stay valid for the whole load and the per-specialize walks
// below get contiguous storage. Never push past that reserve.
std::vector<Module> g_modules;
Module *g_cur = nullptr;     // module currently in onLoad/pre/post
Module *g_loading = nullptr; // module currently being registered
int g_loading_id = -1;       // zygiskd index of the module being loaded

// zygiskd-backed helpers.
int zd_module_dir(int id);
int zd_connect_companion(int id);
uint32_t zd_get_flags(int uid);
int g_app_uid = -1; // uid of the process currently being specialized
bool g_module_policy_armed = false;

void api_hook_jni_native_methods(JNIEnv *env, const char *cls,
                                 JNINativeMethod *methods, int n) {
  if (g_cur != nullptr)
    zygisk_hook_jni_methods(env, cls, methods, n, g_cur->id);
}

void api_plt_hook_register(dev_t dev, ino_t inode, const char *symbol,
                           void *new_func, void **old_func) {
  if (zygisk_plt_hook_register(dev, inode, symbol, new_func, old_func) &&
      g_cur != nullptr)
    g_cur->has_plt_hooks = true;
}

/* v1/v2 path-regex PLT hook. */
void api_plt_hook_register_byname(const char *path_regex, const char *symbol,
                                  void *new_func, void **old_func) {
  if (path_regex == nullptr || symbol == nullptr || new_func == nullptr)
    return;
  regex_t re;
  if (regcomp(&re, path_regex, REG_NOSUB) != 0)
    return;
  for (const auto &map : lsplt::MapInfo::ScanCached()) {
    if (map.offset != 0 || !(map.perms & PROT_READ) || map.inode == 0)
      continue;
    if (regexec(&re, map.path.c_str(), 0, nullptr, 0) == 0)
      api_plt_hook_register(map.dev, map.inode, symbol, new_func, old_func);
  }
  regfree(&re);
}

void api_plt_hook_exclude(const char * /*lib*/, const char * /*symbol*/) {}

bool api_plt_hook_commit() { return zygisk_plt_hook_commit(); }

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
  g_cur = g_loading; // current module for onLoad's api calls

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

int connect_zygiskd() {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  size_t len = std::strlen(zygiskd::kSocketName);
  memcpy(addr.sun_path + 1, zygiskd::kSocketName,
         len); // abstract: leading NUL
  auto alen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + len);
  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), alen) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

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
yz_config g_yz_config{};

void zd_load_config() {
  int s = connect_zygiskd();
  if (s < 0)
    return;
  uint8_t req = static_cast<uint8_t>(ZdRequest::GetConfig);
  yz_config cfg{};
  if (write(s, &req, 1) == 1 && read_all(s, &cfg, sizeof(cfg)))
    g_yz_config = cfg;
  close(s);
}

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
yuki_core_dlopen_memfd(int memfd, const char *vma_name);
__attribute__((visibility("hidden"))) void *yuki_core_dlsym(void *handle,
                                                            const char *name);
__attribute__((visibility("hidden"))) void yuki_core_dlclose(void *handle);
}
using yuki_dlopen_fn = void *(*)(int, const char *);
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
    int s = connect_zygiskd();
    if (s < 0)
      continue;
    req = static_cast<uint8_t>(ZdRequest::GetModuleFd);
    if (write(s, &req, 1) != 1 || write(s, &i, sizeof(i)) != sizeof(i)) {
      close(s);
      continue;
    }
    int lib_fd = recv_fd(s);
    close(s);
    if (lib_fd < 0) {
      LOGE("no fd for module %u", i);
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
      if (!use_system_tls && g_yuki_dlopen != nullptr &&
          g_yuki_dlsym != nullptr && g_yuki_dlclose != nullptr) {
        handle = g_yuki_dlopen(mfd, "");
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
          LOGI("module %u using system linker fallback", i);
          entry = reinterpret_cast<module_entry_fn>(
              dlsym(handle, "zygisk_module_entry"));
          int anonymized = yuki::solist::spoof_loaded_object_maps(
              reinterpret_cast<uintptr_t>(entry), true);
          LOGI("module %u system fallback anonymized %d segment(s)", i,
               anonymized);
        }
      }
      close(mfd);
    }
    if (handle == nullptr) {
      LOGE("dlopen module %u failed", i);
      continue;
    }
    if (entry == nullptr) {
      LOGE("module %u has no zygisk_module_entry", i);
      if (yuki_loaded)
        g_yuki_dlclose(handle);
      else
        dlclose(handle);
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
    entry(reinterpret_cast<api_table *>(&m.api), env);
    if (m.version == 0) {
      if (yuki_loaded)
        g_yuki_dlclose(handle);
      else
        dlclose(handle);
      g_modules.pop_back();
      continue;
    }
  }
  g_loading = nullptr;
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
    if (m.has_plt_hooks || !zygisk_restore_module_hooks(env, m.id)) {
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
      g_cur = &m;
      m.abi->preAppSpecialize(m.abi->impl,
                              reinterpret_cast<zygisk::AppSpecializeArgs *>(
                                  app_args_for(m, args, &v1args)));
    }
  g_cur = nullptr;
  zd_restore_module_load_policy();
}

/* Hide injected linker entries. */
void hide_injection() {
  yuki::solist::hide_from_solist("libzygisk");
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
      g_cur = &m;
      m.abi->postAppSpecialize(
          m.abi->impl, reinterpret_cast<const zygisk::AppSpecializeArgs *>(
                           app_args_for(m, mut, &v1args)));
    }
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
      g_cur = &m;
      m.abi->preServerSpecialize(m.abi->impl, args);
    }
  }
  g_cur = nullptr;
  zd_restore_module_load_policy();
}

void run_server_post_impl(JNIEnv *env,
                          const zygisk::ServerSpecializeArgs *args) {
  LOGI("run_server_post: %zu module(s)", g_modules.size());
  for (auto &m : g_modules)
    if (m.abi != nullptr && m.abi->postServerSpecialize != nullptr) {
      LOGI("  module %d postServerSpecialize", m.id);
      g_cur = &m;
      m.abi->postServerSpecialize(m.abi->impl, args);
    }
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
  const uint32_t runtime_generation =
      zd_get_runtime_generation(YZ_RUNTIME_KIND_ZYGOTE);
  LOGI("core start, self=%s", self_path ? self_path : "(null)");
  if (!yuki::solist::prepare_linker())
    LOGE("linker internals unavailable; solist cleanup disabled");
  if (zygisk_hook_bootstrap(self_path))
    zd_report_zygote(runtime_generation);
  zd_restore_load_policy();
}

/* Address inside the first-stage loader mapping. */
uintptr_t g_loader_base = 0;

extern "C" const ElfW(Dyn) _DYNAMIC[];

static const ElfW(Dyn) * self_dynamic_table(uintptr_t load_bias) {
  uintptr_t dyn = reinterpret_cast<uintptr_t>(_DYNAMIC);
  if (load_bias != 0 && g_self_size != 0 && dyn < g_self_size)
    dyn += load_bias;
  return reinterpret_cast<const ElfW(Dyn) *>(dyn);
}

using SystemIterateFunction = int (*)(int (*)(struct dl_phdr_info *, size_t,
                                              void *),
                                      void *);

static SystemIterateFunction resolve_system_dl_iterate_phdr() {
  volatile char vn[] = "dl_iterate_phdr";
  char nm[sizeof(vn)];
  for (size_t i = 0; i < sizeof(vn); ++i)
    nm[i] = vn[i];
  return reinterpret_cast<SystemIterateFunction>(dlsym(RTLD_DEFAULT, nm));
}

// These are core-local bindings. They prevent the first-stage relocation pass
// from leaving callbacks into its own mapping in the core's GOT. The core is
// built with -fno-c++-static-destructors, while modules retain their normal
// yukilinker-owned destructor registry through the module relocation path.
extern "C" __attribute__((visibility("hidden"))) int
__cxa_atexit(void (*)(void *), void *, void *) {
  return 0;
}

extern "C" __attribute__((visibility("hidden"))) void __cxa_finalize(void *) {}

extern "C" __attribute__((visibility("hidden"))) int
dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *),
                void *data) {
  static SystemIterateFunction system_iterator =
      resolve_system_dl_iterate_phdr();
  return system_iterator == nullptr ? 0 : system_iterator(callback, data);
}

// The bootstrap loader must remain mapped until every capability borrowed by
// the core no longer resolves into it. The core itself is custom-mapped, so
// map identity is more dependable here than dladdr()/system-linker metadata.
struct LoaderMapIdentity {
  unsigned int dev_major = 0;
  unsigned int dev_minor = 0;
  unsigned long inode = 0;
};

static bool parse_proc_map_line(const char *line, uintptr_t *start,
                                uintptr_t *end, unsigned int *dev_major,
                                unsigned int *dev_minor, unsigned long *inode) {
  unsigned long map_start = 0;
  unsigned long map_end = 0;
  unsigned long map_inode = 0;
  unsigned int map_dev_major = 0;
  unsigned int map_dev_minor = 0;
  char perms[5] = {};
  if (sscanf(line, "%lx-%lx %4s %*lx %x:%x %lu", &map_start, &map_end, perms,
             &map_dev_major, &map_dev_minor, &map_inode) != 6) {
    return false;
  }
  *start = map_start;
  *end = map_end;
  *dev_major = map_dev_major;
  *dev_minor = map_dev_minor;
  *inode = map_inode;
  return true;
}

static bool get_loader_map_identity(LoaderMapIdentity *identity) {
  FILE *maps = fopen("/proc/self/maps", "re");
  if (maps == nullptr) {
    LOGE("loader handoff: cannot open /proc/self/maps");
    return false;
  }

  bool found = false;
  char line[512];
  while (fgets(line, sizeof(line), maps) != nullptr) {
    uintptr_t start = 0;
    uintptr_t end = 0;
    unsigned int dev_major = 0;
    unsigned int dev_minor = 0;
    unsigned long inode = 0;
    if (!parse_proc_map_line(line, &start, &end, &dev_major, &dev_minor,
                             &inode) ||
        g_loader_base < start || g_loader_base >= end) {
      continue;
    }
    if (inode == 0) {
      LOGE("loader handoff: first-stage mapping has no stable identity");
      break;
    }
    *identity = {dev_major, dev_minor, inode};
    found = true;
    break;
  }
  (void)fclose(maps);
  if (!found) {
    LOGE("loader handoff: cannot identify first-stage mapping at %p",
         reinterpret_cast<void *>(g_loader_base));
  }
  return found;
}

enum class PointerOwner {
  FirstStage,
  Other,
  Unknown,
};

static PointerOwner pointer_owner(uintptr_t address,
                                  const LoaderMapIdentity &loader) {
  if (address == 0)
    return PointerOwner::Other;

  FILE *maps = fopen("/proc/self/maps", "re");
  if (maps == nullptr)
    return PointerOwner::Unknown;

  PointerOwner owner = PointerOwner::Unknown;
  char line[512];
  while (fgets(line, sizeof(line), maps) != nullptr) {
    uintptr_t start = 0;
    uintptr_t end = 0;
    unsigned int dev_major = 0;
    unsigned int dev_minor = 0;
    unsigned long inode = 0;
    if (!parse_proc_map_line(line, &start, &end, &dev_major, &dev_minor,
                             &inode) ||
        address < start || address >= end) {
      continue;
    }
    owner = dev_major == loader.dev_major && dev_minor == loader.dev_minor &&
                    inode == loader.inode
                ? PointerOwner::FirstStage
                : PointerOwner::Other;
    break;
  }
  (void)fclose(maps);
  return owner;
}

static bool verify_handoff_pointer(const char *name, uintptr_t address,
                                   const LoaderMapIdentity &loader) {
  switch (pointer_owner(address, loader)) {
  case PointerOwner::Other:
    return true;
  case PointerOwner::FirstStage:
    LOGE("loader handoff incomplete: %s still points into first stage (%p)",
         name, reinterpret_cast<void *>(address));
    return false;
  case PointerOwner::Unknown:
    LOGE("loader handoff incomplete: cannot classify %s (%p)", name,
         reinterpret_cast<void *>(address));
    return false;
  }
  return false;
}

#if defined(__LP64__)
using SelfRelocation = ElfW(Rela);
#else
using SelfRelocation = ElfW(Rel);
#endif // #if defined(__LP64__)

struct SelfDynamicInfo {
  const ElfW(Sym) *symtab = nullptr;
  const char *strtab = nullptr;
  const SelfRelocation *jmprel = nullptr;
  size_t pltrelsz = 0;
  const SelfRelocation *rel = nullptr;
  size_t relsz = 0;
};

static bool get_self_dynamic_info(uintptr_t load_bias, SelfDynamicInfo *info) {
  const ElfW(Dyn) *dynamic = self_dynamic_table(load_bias);
  if (dynamic == nullptr)
    return false;

  for (const ElfW(Dyn) *entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
    switch (entry->d_tag) {
    case DT_SYMTAB:
      info->symtab =
          reinterpret_cast<const ElfW(Sym) *>(load_bias + entry->d_un.d_ptr);
      break;
    case DT_STRTAB:
      info->strtab =
          reinterpret_cast<const char *>(load_bias + entry->d_un.d_ptr);
      break;
    case DT_JMPREL:
      info->jmprel = reinterpret_cast<const SelfRelocation *>(
          load_bias + entry->d_un.d_ptr);
      break;
    case DT_PLTRELSZ:
      info->pltrelsz = entry->d_un.d_val;
      break;
#if defined(__LP64__)
    case DT_RELA:
#else
    case DT_REL:
#endif // #if defined(__LP64__)
      info->rel = reinterpret_cast<const SelfRelocation *>(load_bias +
                                                           entry->d_un.d_ptr);
      break;
#if defined(__LP64__)
    case DT_RELASZ:
#else
    case DT_RELSZ:
#endif // #if defined(__LP64__)
      info->relsz = entry->d_un.d_val;
      break;
    default:
      break;
    }
  }

  if (info->symtab == nullptr || info->strtab == nullptr ||
      info->pltrelsz % sizeof(SelfRelocation) != 0 ||
      info->relsz % sizeof(SelfRelocation) != 0) {
    LOGE("loader handoff: malformed core dynamic relocation metadata");
    return false;
  }
  return true;
}

static bool verify_relocation_table(const SelfRelocation *table,
                                    size_t table_size,
                                    const SelfDynamicInfo &info,
                                    const LoaderMapIdentity &loader) {
  if (table == nullptr && table_size != 0) {
    LOGE("loader handoff: missing core relocation table");
    return false;
  }

  bool complete = true;
  const size_t count = table_size / sizeof(SelfRelocation);
  for (size_t i = 0; i < count; ++i) {
    const auto &reloc = table[i];
    if (reloc.r_offset > g_self_size ||
        sizeof(uintptr_t) > g_self_size - reloc.r_offset) {
      LOGE("loader handoff: relocation slot lies outside the core image");
      complete = false;
      continue;
    }

#if defined(__LP64__)
    const auto symbol_index = ELF64_R_SYM(reloc.r_info);
#else
    const auto symbol_index = ELF32_R_SYM(reloc.r_info);
#endif // #if defined(__LP64__)
    const char *name = symbol_index == 0
                           ? "<relative>"
                           : info.strtab + info.symtab[symbol_index].st_name;
    const auto *slot =
        reinterpret_cast<const uintptr_t *>(g_self_base + reloc.r_offset);
    if (!verify_handoff_pointer(name, *slot, loader))
      complete = false;
  }
  return complete;
}

static bool first_stage_handoff_complete() {
  if (g_loader_base == 0 || g_self_base == 0 || g_self_size == 0) {
    LOGE("loader handoff: image bounds are unavailable");
    return false;
  }

  LoaderMapIdentity loader;
  if (!get_loader_map_identity(&loader))
    return false;

  bool complete = true;
  complete &= verify_handoff_pointer(
      "yuki_dlopen_memfd", reinterpret_cast<uintptr_t>(g_yuki_dlopen), loader);
  complete &= verify_handoff_pointer(
      "yuki_dlsym", reinterpret_cast<uintptr_t>(g_yuki_dlsym), loader);
  complete &= verify_handoff_pointer(
      "yuki_dlclose", reinterpret_cast<uintptr_t>(g_yuki_dlclose), loader);

  SelfDynamicInfo info;
  if (!get_self_dynamic_info(g_self_base, &info))
    return false;
  complete &= verify_relocation_table(info.jmprel, info.pltrelsz, info, loader);
  complete &= verify_relocation_table(info.rel, info.relsz, info, loader);
  return complete;
}

bool g_loader_unmap_safe = false;

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry(const char *self_path, void *loader_self, void *core_base,
                  void *core_size, int /*early_packet_fd_plus1*/) {
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
zygisk_bootstrap_handoff_complete() {
  return g_loader_unmap_safe;
}

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry_direct(int /*core_fd*/) {
  core_start(nullptr);
}

namespace {
uintptr_t g_tango_stub = 0;
uint32_t g_tango_stub_size = 0;
} // namespace

#if defined(__arm__)
struct TangoCoreImage {
  uintptr_t anchor = 0;
  uintptr_t base = 0;
  size_t size = 0;
};

static int find_tango_core_image(dl_phdr_info *info, size_t, void *data) {
  if (info == nullptr || info->dlpi_phdr == nullptr)
    return 0;
  auto &image = *static_cast<TangoCoreImage *>(data);
  uintptr_t low = UINTPTR_MAX;
  uintptr_t high = 0;
  bool contains_entry = false;
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const auto &ph = info->dlpi_phdr[i];
    if (ph.p_type == PT_TLS && ph.p_memsz != 0)
      return 0;
    if (ph.p_type != PT_LOAD || ph.p_memsz == 0)
      continue;
    if (ph.p_vaddr > UINTPTR_MAX - info->dlpi_addr)
      return 0;
    const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
    if (ph.p_memsz > UINTPTR_MAX - start)
      return 0;
    const uintptr_t end = start + ph.p_memsz;
    low = std::min(start, low);
    high = std::max(end, high);
    if ((ph.p_flags & PF_X) != 0 && image.anchor >= start && image.anchor < end)
      contains_entry = true;
  }
  const auto page_size = static_cast<uintptr_t>(getpagesize());
  if (!contains_entry || page_size == 0 || (page_size & (page_size - 1)) != 0 ||
      high > UINTPTR_MAX - (page_size - 1))
    return 0;
  image.base = low & ~(page_size - 1);
  image.size = ((high + page_size - 1) & ~(page_size - 1)) - image.base;
  return 1;
}

static void prepare_tango_core_unmap(uintptr_t entry) {
  const uintptr_t anchor = entry & ~uintptr_t{1};
  TangoCoreImage image{anchor};
  if (dl_iterate_phdr(find_tango_core_image, &image) != 1 || image.base == 0 ||
      image.size == 0) {
    LOGE("Tango core bounds unavailable; retaining system linker mapping");
    return;
  }
  // Remove linker ownership while keeping this executing image mapped.
  if (yuki::solist::release_lib_containing(anchor) != 1) {
    LOGE("Tango core linker detach failed; retaining mapping");
    return;
  }
  TangoCoreImage remaining{anchor};
  if (dl_iterate_phdr(find_tango_core_image, &remaining) != 0) {
    LOGE("Tango core linker still owns the image; retaining mapping");
    return;
  }
  g_self_base = image.base;
  g_self_size = image.size;
}

extern "C" [[gnu::visibility("default")]] void
zygisk_core_entry_tango(uint32_t got, uint32_t original, uint32_t stub,
                        uint32_t size) {
  if (got != 0 && original != 0 && stub != 0 && size == 176 &&
      yz_patch_text(got, &original, sizeof(original))) {
    g_tango_stub = stub;
    g_tango_stub_size = size;
  } else {
    LOGE("Tango GOT restore failed; retaining guest trampoline");
  }
  LOGI("Tango guest core start");
  core_start(nullptr);
  if (g_tango_stub != 0)
    prepare_tango_core_unmap(
        reinterpret_cast<uintptr_t>(zygisk_core_entry_tango));
}
#endif

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

/* Remove the first-stage soinfo; retain its mapping if handoff is incomplete.
 */
extern "C" [[gnu::visibility("default")]] void zygisk_finalize_loader(int,
                                                                      int) {
  zd_load_config();
  LOGI("finalize_loader: finalizing loader at base=%p munmap=%d",
       reinterpret_cast<void *>(g_loader_base), g_loader_unmap_safe);
  int n =
      yuki::solist::drop_lib_containing(g_loader_base, !g_loader_unmap_safe);
  LOGI("finalize_loader: unloaded %d soinfo(s)", n);
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

/* Find the core mapping range. */
static bool yz_find_self_range(uintptr_t *base, size_t *size) {
  if (g_self_base == 0 || g_self_size == 0)
    return false;
  const uintptr_t pg = static_cast<uintptr_t>(getpagesize());
  uintptr_t lo = g_self_base & ~(pg - 1);
  uintptr_t hi = (g_self_base + g_self_size + pg - 1) & ~(pg - 1);
  *base = lo;
  *size = hi - lo;
  return true;
}

extern "C" [[noreturn]] void yz_self_unmap_tail(void *base, size_t size);

// Non-weak so __cxa_finalize targets only this DSO.
extern "C" __attribute__((visibility("hidden"))) void *__dso_handle;
static inline void yz_finalize_self_dso() {
  __cxa_finalize(static_cast<void *>(&__dso_handle));
}

void zygisk_self_destruct(JNIEnv *env, bool isolated) {
  const bool fully_inline_hooked = zygisk_specialize_fully_inline_hooked();
  const bool hooks_removed = zygisk_self_unhook(env);
  bool can_unmap = fully_inline_hooked && hooks_removed;
  yz_drop_runtime_header_pages();
  uintptr_t cbase = 0;
  size_t csize = 0;
  bool have_range =
      yz_find_self_range(&cbase, &csize) && cbase != 0 && csize != 0;
  if (!isolated) {
    yuki::solist::hide_from_solist("libzygisk");
    yuki::solist::hide_from_solist("libyukilinker");
  }
  if (!fully_inline_hooked)
    LOGE("self-unmap unavailable: specialize used RegisterNatives fallback");
  if (!hooks_removed)
    LOGE("self-unmap unavailable: hook restore failed");
  if (!have_range)
    LOGE("self-unmap unavailable: core image bounds are missing");
  if (can_unmap && yukilinker::has_active_tls()) {
    LOGE("self-unmap unavailable: a yukilinker TLS resolver is still active");
    can_unmap = false;
  }
  if (have_range && can_unmap) {
    if (!yukilinker::shutdown()) {
      LOGE("self-unmap blocked: loader cleanup incomplete");
      return;
    }
    std::vector<Module>().swap(g_modules);
    g_cur = nullptr;
    g_loading = nullptr;
    yz_finalize_self_dso();
    LOGI("self-unmap handoff: base=%p size=%zu",
         reinterpret_cast<void *>(cbase), csize);
    yz_self_unmap_tail(reinterpret_cast<void *>(cbase), csize);
  }
  LOGE("self-unmap failed: core remains mapped at %p size=%zu",
       reinterpret_cast<void *>(cbase), csize);
  (void)env;
}

bool zygisk_app_core_unload_safe() {
  size_t retained = 0;
  for (const auto &m : g_modules)
    if (m.handle != nullptr)
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
