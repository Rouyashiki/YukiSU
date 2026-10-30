/* Copyright 2022-2023 John "topjohnwu" Wu
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

// This is the public API for Zygisk modules. It is the cross-implementation
// ABI contract: a module is compiled against this header and must run on any
// conforming Zygisk implementation. Our core
// (libzygisk64.so/libzygisk32.so) provides the matching api_table defined at
// the bottom. Distributed under its original
// permissive (ISC) terms above -- this is the one file we reuse verbatim,
// precisely because it is the shared interface and must stay byte-compatible.
//
// DO NOT MODIFY THE PUBLIC CONTRACT IN THIS HEADER.

#pragma once

#include <cstdint>
#include <jni.h>
#include <sys/types.h> // dev_t, ino_t (used by pltHookRegister)

#define ZYGISK_API_VERSION 5

namespace zygisk {

// Read a live PLT original after the replacement becomes reachable.
template <typename Function>
inline Function loadPltOriginal(const Function &original) {
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return __atomic_load_n(&original, __ATOMIC_ACQUIRE);
}

struct Api;
struct AppSpecializeArgs;
struct ServerSpecializeArgs;

class ModuleBase {
public:
  // Called as soon as the module is loaded into the target process.
  virtual void onLoad([[maybe_unused]] Api *api, [[maybe_unused]] JNIEnv *env) {
  }

  // Called before the app process is specialized. The process just got forked
  // from zygote; no sandbox restrictions are applied yet and it still runs
  // with zygote's privilege. Read/overwrite args to change specialization.
  virtual void preAppSpecialize([[maybe_unused]] AppSpecializeArgs *args) {}

  // Called after the app process is specialized -- runs with the app's own
  // (sandboxed) privilege.
  virtual void
  postAppSpecialize([[maybe_unused]] const AppSpecializeArgs *args) {}

  // Called before system_server is specialized. See preAppSpecialize.
  virtual void
  preServerSpecialize([[maybe_unused]] ServerSpecializeArgs *args) {}

  // Called after system_server is specialized -- runs with system_server's
  // privilege.
  virtual void
  postServerSpecialize([[maybe_unused]] const ServerSpecializeArgs *args) {}
};

struct AppSpecializeArgs {
  // Required arguments, present on all Android versions.
  jint &uid;
  jint &gid;
  jintArray &gids;
  jint &runtime_flags;
  jobjectArray &rlimits;
  jint &mount_external;
  jstring &se_info;
  jstring &nice_name;
  jstring &instruction_set;
  jstring &app_data_dir;

  // Optional arguments. Null-check the pointer before dereferencing.
  jintArray *fds_to_ignore = nullptr;
  jboolean *is_child_zygote = nullptr;
  jboolean *is_top_app = nullptr;
  jobjectArray *pkg_data_info_list = nullptr;
  jobjectArray *whitelisted_data_info_list = nullptr;
  jboolean *mount_data_dirs = nullptr;
  jboolean *mount_storage_dirs = nullptr;
  jboolean *mount_sysprop_overrides = nullptr;

  AppSpecializeArgs() = delete;
  // Core-internal: bind the required fields to the live JNI arguments. Optional
  // pointers are filled in by the caller per the method signature. Modules
  // never construct this -- they only read it.
  AppSpecializeArgs(jint &uid_, jint &gid_, jintArray &gids_, jint &rf_,
                    jobjectArray &rl_, jint &me_, jstring &si_, jstring &nn_,
                    jstring &is_, jstring &ad_)
      : uid(uid_), gid(gid_), gids(gids_), runtime_flags(rf_), rlimits(rl_),
        mount_external(me_), se_info(si_), nice_name(nn_), instruction_set(is_),
        app_data_dir(ad_) {}
};

struct ServerSpecializeArgs {
  jint &uid;
  jint &gid;
  jintArray &gids;
  jint &runtime_flags;
  jlong &permitted_capabilities;
  jlong &effective_capabilities;

  ServerSpecializeArgs() = delete;
  // Core-internal: bind to the live JNI arguments. Modules only read it.
  ServerSpecializeArgs(jint &uid_, jint &gid_, jintArray &gids_, jint &rf_,
                       jlong &pc_, jlong &ec_)
      : uid(uid_), gid(gid_), gids(gids_), runtime_flags(rf_),
        permitted_capabilities(pc_), effective_capabilities(ec_) {}
};

namespace internal {
struct api_table;
template <class T> void entry_impl(api_table *, JNIEnv *);
} // namespace internal

// Values used in Api::setOption(Option)
enum Option : int {
  // Force the denylist unmount routine to run on this process, regardless of
  // enforcement status. Only meaningful in preAppSpecialize.
  FORCE_DENYLIST_UNMOUNT = 0,

  // dlclose the module library after post[XXX]Specialize. Do NOT enable this
  // after hooking any functions in the process.
  DLCLOSE_MODULE_LIBRARY = 1,
};

// Bit masks for the return value of Api::getFlags()
enum StateFlag : uint32_t {
  // The user has granted root access to the current process.
  PROCESS_GRANTED_ROOT = (1u << 0),
  // The current process is on the denylist.
  PROCESS_ON_DENYLIST = (1u << 1),
};

// All API methods stop working after post[XXX]Specialize, as Zygisk is unloaded
// from the specialized process afterwards.
struct Api {
  // Connect to a root companion process; returns an IPC socket fd, or -1.
  // Only works in pre[XXX]Specialize (SELinux). ABI-aware (32<->32, 64<->64).
  int connectCompanion();

  // fd of the current module's root folder. Only works in pre[XXX]Specialize.
  int getModuleDir();

  // Set a single module option. See zygisk::Option.
  void setOption(Option opt);

  // Bitwise-or'd zygisk::StateFlag values for the current process.
  uint32_t getFlags();

  // Exempt an fd from zygote's automatic close. Only meaningful in
  // preAppSpecialize; elsewhere a no-op (true) or error (false).
  bool exemptFd(int fd);

  // Replace registered JNI native methods of a class with your own; the
  // originals are written back into each JNINativeMethod::fnPtr (or nullptr
  // if no match).
  void hookJniNativeMethods(JNIEnv *env, const char *className,
                            JNINativeMethod *methods, int numMethods);

  // Register a PLT hook: for the ELF identified by (dev, inode), replace
  // `symbol` with `newFunc`; if `oldFunc` != nullptr, save the original.
  // The YukiPLT host requires a unique, non-null JUMP_SLOT and one owner.
  // Keep original storage alive and read it with loadPltOriginal; do not reuse
  // it.
  void pltHookRegister(dev_t dev, ino_t inode, const char *symbol,
                       void *newFunc, void **oldFunc);

  // Commit all previously registered PLT hooks. Returns false on error.
  bool pltHookCommit();

private:
  internal::api_table *tbl;
  template <class T>
  friend void internal::entry_impl(internal::api_table *, JNIEnv *);
};

// Register a class as a Zygisk module.
#define REGISTER_ZYGISK_MODULE(clazz)                                          \
  void zygisk_module_entry(zygisk::internal::api_table *table, JNIEnv *env) {  \
    zygisk::internal::entry_impl<clazz>(table, env);                           \
  }

// Register a root companion request handler. It runs in a superuser daemon
// process and is given a Unix domain socket connected to the requesting target
// process. May run concurrently on multiple threads.
#define REGISTER_ZYGISK_COMPANION(func)                                        \
  void zygisk_companion_entry(int client) { func(client); }

/*********************************************************
 * Internal ABI implementation detail. Layout is fixed by
 * the contract; modules need not understand it.
 *********************************************************/

namespace internal {

struct module_abi {
  long api_version;
  ModuleBase *impl;

  void (*preAppSpecialize)(ModuleBase *, AppSpecializeArgs *);
  void (*postAppSpecialize)(ModuleBase *, const AppSpecializeArgs *);
  void (*preServerSpecialize)(ModuleBase *, ServerSpecializeArgs *);
  void (*postServerSpecialize)(ModuleBase *, const ServerSpecializeArgs *);

  module_abi(ModuleBase *module)
      : api_version(ZYGISK_API_VERSION), impl(module) {
    preAppSpecialize = [](auto m, auto args) { m->preAppSpecialize(args); };
    postAppSpecialize = [](auto m, auto args) { m->postAppSpecialize(args); };
    preServerSpecialize = [](auto m, auto args) {
      m->preServerSpecialize(args);
    };
    postServerSpecialize = [](auto m, auto args) {
      m->postServerSpecialize(args);
    };
  }
};

struct api_table {
  // Base
  void *impl;
  bool (*registerModule)(api_table *, module_abi *);

  void (*hookJniNativeMethods)(JNIEnv *, const char *, JNINativeMethod *, int);
  void (*pltHookRegister)(dev_t, ino_t, const char *, void *, void **);
  bool (*exemptFd)(int);
  bool (*pltHookCommit)();
  int (*connectCompanion)(void * /* impl */);
  void (*setOption)(void * /* impl */, Option);
  int (*getModuleDir)(void * /* impl */);
  uint32_t (*getFlags)(void * /* impl */);
};

template <class T> void entry_impl(api_table *table, JNIEnv *env) {
  static Api api;
  api.tbl = table;
  static T module;
  ModuleBase *m = &module;
  static module_abi abi(m);
  if (!table->registerModule(table, &abi))
    return;
  m->onLoad(&api, env);
}

} // namespace internal

inline int Api::connectCompanion() {
  return tbl->connectCompanion ? tbl->connectCompanion(tbl->impl) : -1;
}
inline int Api::getModuleDir() {
  return tbl->getModuleDir ? tbl->getModuleDir(tbl->impl) : -1;
}
inline void Api::setOption(Option opt) {
  if (tbl->setOption)
    tbl->setOption(tbl->impl, opt);
}
inline uint32_t Api::getFlags() {
  return tbl->getFlags ? tbl->getFlags(tbl->impl) : 0;
}
inline bool Api::exemptFd(int fd) {
  return tbl->exemptFd != nullptr && tbl->exemptFd(fd);
}
inline void Api::hookJniNativeMethods(JNIEnv *env, const char *className,
                                      JNINativeMethod *methods,
                                      int numMethods) {
  if (tbl->hookJniNativeMethods)
    tbl->hookJniNativeMethods(env, className, methods, numMethods);
}
inline void Api::pltHookRegister(dev_t dev, ino_t inode, const char *symbol,
                                 void *newFunc, void **oldFunc) {
  if (tbl->pltHookRegister)
    tbl->pltHookRegister(dev, inode, symbol, newFunc, oldFunc);
}
inline bool Api::pltHookCommit() {
  return tbl->pltHookCommit != nullptr && tbl->pltHookCommit();
}

} // namespace zygisk

extern "C" {

[[gnu::visibility("default"), maybe_unused]]
void zygisk_module_entry(zygisk::internal::api_table *, JNIEnv *);

[[gnu::visibility("default"), maybe_unused]]
void zygisk_companion_entry(int);

} // extern "C"
