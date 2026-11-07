#include "linker_runtime.hpp"

#include "core_bindings.hpp"
#include "log.hpp"
#include "native_bionic.hpp"
#include "plt_lifecycle.hpp"
#include "uapi/yukizygisk.h"

#include <android/dlext.h>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

static_assert(YUKILINKER_C_ABI_VERSION == 1);
static_assert(YUKILINKER_C_HOST_SERVICES_VERSION == 1);
static_assert(YUKILINKER_C_EARLY_PACKET_MAGIC == YZ_BOOTSTRAP_PACKET_MAGIC);
static_assert(YUKILINKER_C_EARLY_PACKET_VERSION == YZ_BOOTSTRAP_PACKET_VERSION);
static_assert(YUKILINKER_C_EARLY_PACKET_MAX_ENTRIES == YZ_NATIVE_TARGET_MAX);
static_assert(YUKILINKER_C_TANGO_BOOTSTRAP_SIZE == YZ_TANGO_BOOTSTRAP_SIZE);
static_assert(sizeof(yukilinker_c_early_packet_header) ==
              sizeof(yz_bootstrap_packet_header));
static_assert(sizeof(yukilinker_c_early_packet_entry) ==
              sizeof(yz_early_native_packet_entry));
#define CHECK_PACKET_FIELD(sdk, kernel, field)                                 \
  static_assert(offsetof(sdk, field) == offsetof(kernel, field))
CHECK_PACKET_FIELD(yukilinker_c_early_packet_header, yz_bootstrap_packet_header,
                   endpoint_fd);
CHECK_PACKET_FIELD(yukilinker_c_early_packet_header, yz_bootstrap_packet_header,
                   magic);
CHECK_PACKET_FIELD(yukilinker_c_early_packet_header, yz_bootstrap_packet_header,
                   version);
CHECK_PACKET_FIELD(yukilinker_c_early_packet_header, yz_bootstrap_packet_header,
                   header_size);
CHECK_PACKET_FIELD(yukilinker_c_early_packet_header, yz_bootstrap_packet_header,
                   entry_size);
CHECK_PACKET_FIELD(yukilinker_c_early_packet_header, yz_bootstrap_packet_header,
                   reserved);
CHECK_PACKET_FIELD(yukilinker_c_early_packet_header, yz_bootstrap_packet_header,
                   load_flags);
CHECK_PACKET_FIELD(yukilinker_c_early_packet_header, yz_bootstrap_packet_header,
                   count);
static_assert(offsetof(yukilinker_c_early_packet_entry, module_id) ==
              offsetof(yz_early_native_packet_entry, module) +
                  offsetof(yz_early_native_entry, module_id));
static_assert(offsetof(yukilinker_c_early_packet_entry, lib_path) ==
              offsetof(yz_early_native_packet_entry, module) +
                  offsetof(yz_early_native_entry, lib_path));
CHECK_PACKET_FIELD(yukilinker_c_early_packet_entry,
                   yz_early_native_packet_entry, fd);
static_assert(offsetof(yukilinker_c_early_packet_entry, tail_reserved) ==
              offsetof(yz_early_native_packet_entry, reserved));
#define CHECK_MODULE_FIELD(field)                                              \
  static_assert(offsetof(yukilinker_c_early_packet_entry, field) ==            \
                offsetof(yz_early_native_packet_entry, module) +               \
                    offsetof(yz_early_native_entry, field));                   \
  static_assert(sizeof(yukilinker_c_early_packet_entry::field) ==              \
                sizeof(yz_early_native_entry::field))
CHECK_MODULE_FIELD(target_type);
CHECK_MODULE_FIELD(flags);
CHECK_MODULE_FIELD(reserved);
CHECK_MODULE_FIELD(module_id);
CHECK_MODULE_FIELD(target);
CHECK_MODULE_FIELD(lib_path);
#undef CHECK_MODULE_FIELD
#undef CHECK_PACKET_FIELD

namespace yukizygisk::linker {
struct LibraryState {
  LibraryState *next = nullptr;
  yukilinker_c_handle handle{};
  yukilinker_c_context *context = nullptr;
  yukilinker_c_image_id image{};
  char origin[PATH_MAX]{};
  int directory = -1;
  bool private_backend = false;
  bool closing = false;
  bool detached = false;
  bool uncertain = false;
};
struct LeaseState {
  LeaseState *next = nullptr;
  yukilinker_c_lease token{};
  yukilinker_c_snapshot snapshot{};
};

namespace {
yukilinker_c_runtime *owner = nullptr;
LibraryState *libraries = nullptr;
pthread_mutex_t directory_mutex = PTHREAD_MUTEX_INITIALIZER;
LeaseState *pending_leases = nullptr;
pthread_mutex_t lease_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t error_key{};
bool error_key_valid = false;
bool runtime_stopped = false;
uint64_t event_cursor = 0;
native_bionic::Lookup host_lookup = nullptr;
native_bionic::LookupVersion host_lookup_version = nullptr;
const void *host_caller = nullptr;
enum class NativeWindow { Unbound, Open, Draining, Revoked };
pthread_mutex_t native_mutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<NativeWindow> native_window{NativeWindow::Unbound};
yukilinker_c_native_services native_services{};
unsigned native_calls = 0;
struct ErrorState {
  char message[512];
  bool pending;
  int force_fd;
};

ErrorState *thread_error() noexcept {
  if (!error_key_valid)
    return nullptr;
  auto *state = static_cast<ErrorState *>(pthread_getspecific(error_key));
  if (!state) {
    state = static_cast<ErrorState *>(calloc(1, sizeof(ErrorState)));
    if (state)
      state->force_fd = -1;
    if (state && pthread_setspecific(error_key, state) != 0) {
      free(state);
      state = nullptr;
    }
  }
  return state;
}

void save_error(const char *message) noexcept {
  if (auto *state = thread_error()) {
    (void)snprintf(state->message, sizeof(state->message), "%s",
                   message ? message : "system linker failure");
    state->pending = true;
  }
}

// The wrappers and the explicit Bionic caller anchor belong to this Core.
#define HOST_CALL __attribute__((noinline, disable_tail_calls))
HOST_CALL void *system_open(void *,
                            const yukilinker_c_host_dlopen_request *request) {
  plt_lifecycle::invalidate_fork_cache();
  try {
    android_dlextinfo extension{};
    if (request->extension)
      extension = *static_cast<const android_dlextinfo *>(request->extension);
    auto *error = thread_error();
    if (error && request->requester.generation == 0 &&
        (extension.flags & ANDROID_DLEXT_USE_LIBRARY_FD) &&
        extension.library_fd == error->force_fd)
      extension.flags |= ANDROID_DLEXT_FORCE_LOAD;
    if (native_window != NativeWindow::Unbound) {
      if (native_services.origin.process != static_cast<uint32_t>(getpid())) {
        save_error("native bootstrap service inherited across fork");
        return nullptr;
      }
      pthread_mutex_lock(&native_mutex);
      const bool admitted =
          native_window == NativeWindow::Open && native_services.open;
      const auto callback = admitted ? native_services.open : nullptr;
      void *context = admitted ? native_services.context : nullptr;
      if (admitted)
        ++native_calls;
      pthread_mutex_unlock(&native_mutex);
      if (!admitted) {
        save_error("native bootstrap loading window is closed");
        return nullptr;
      }
      auto borrowed = *request;
      borrowed.extension = &extension;
      char message[512]{};
      void *result = callback(context, &borrowed, message, sizeof(message));
      if (native_services.origin.process != static_cast<uint32_t>(getpid())) {
        save_error("fork interrupted native bootstrap load; retaining "
                   "uncertain reference");
        return nullptr;
      }
      pthread_mutex_lock(&native_mutex);
      --native_calls;
      pthread_mutex_unlock(&native_mutex);
      ZLOGI("native open: module=%s backend=SYSTEM policy=libc-loader pid=%u "
            "generation=%u "
            "libc=%p result=%p error=%s",
            request->name, native_services.origin.process,
            native_services.origin.generation,
            reinterpret_cast<void *>(native_services.origin.libc_base), result,
            message);
      if (!result)
        save_error(message);
      return result;
    }
    auto result = native_bionic::load(request->name, request->flags, extension);
    if (!result.handle) {
      ZLOGE("linker host open: %s stage=%s code=%s errno=%d %s", request->name,
            native_bionic::stage_name(result.failure.stage),
            native_bionic::error_name(result.failure.error),
            result.failure.system_error, result.loader_error.c_str());
      save_error(result.loader_error.empty()
                     ? native_bionic::error_name(result.failure.error)
                     : result.loader_error.c_str());
    }
    return result.handle;
  } catch (...) {
    save_error("system linker adapter allocation failed");
    return nullptr;
  }
}
HOST_CALL int system_symbol(void *, void *handle, const char *name,
                            const char *version, void **value) {
  dlerror();
  // Anonymous Core code has no Bionic caller identity. Global lookup uses the
  // same resident-libc namespace as system loads; concrete handles keep theirs.
  if (handle == RTLD_DEFAULT)
    *value = version ? host_lookup_version(handle, name, version, host_caller)
                     : host_lookup(handle, name, host_caller);
  else
    *value = version ? dlvsym(handle, name, version) : dlsym(handle, name);
  const char *error = dlerror();
  if (error)
    save_error(error);
  return error == nullptr;
}
HOST_CALL int system_close(void *, void *handle) {
  plt_lifecycle::invalidate_fork_cache();
  return dlclose(handle);
}
HOST_CALL int system_address(void *, const void *address, Dl_info *info) {
  return dladdr(address, info);
}
HOST_CALL char *system_error(void *) {
  auto *state = error_key_valid
                    ? static_cast<ErrorState *>(pthread_getspecific(error_key))
                    : nullptr;
  if (state && state->pending) {
    state->pending = false;
    return state->message;
  }
  return dlerror();
}
HOST_CALL int system_iterate(void *,
                             int (*callback)(dl_phdr_info *, size_t, void *),
                             void *data) {
  return core_system_iterate_phdr(callback, data);
}
#undef HOST_CALL
void runtime_log(void *, uint32_t, const char *message, size_t length) {
  ZLOGI("linker: %.*s", static_cast<int>(length > 1024 ? 1024 : length),
        message);
}
const yukilinker_c_host_services services{
    sizeof(services), YUKILINKER_C_HOST_SERVICES_VERSION,
    nullptr,          system_open,
    system_symbol,    system_close,
    system_address,   system_error,
    system_iterate,   runtime_log,
    nullptr,          nullptr,
    nullptr};
std::vector<std::weak_ptr<PrivateImage>> image_cache;

void log_error(const char *operation, const char *name,
               const yukilinker_c_error &error) {
  ZLOGE("linker %s: %s code=%u stage=%u execution=%u cleanup=%u errno=%d "
        "object=%s symbol=%s %s",
        operation, name, error.detail.code, error.detail.stage, error.execution,
        error.detail.cleanup_incomplete, error.detail.system_errno,
        error.detail.object, error.detail.symbol, error.system_message);
}

int dependency(void *opaque, const char *, const char *name) {
  auto &state = *static_cast<LibraryState *>(opaque);
  if (!name || !*name)
    return -1;
  pthread_mutex_lock(&directory_mutex);
  const int fd =
      state.directory >= 0
          ? openat(state.directory, name, O_RDONLY | O_CLOEXEC | O_NONBLOCK)
          : -1;
  pthread_mutex_unlock(&directory_mutex);
  struct stat status{};
  if (fd >= 0 && (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode))) {
    close(fd);
    return -1;
  }
  return fd;
}

void forget_state(LibraryState *state) {
  pthread_mutex_lock(&directory_mutex);
  auto **link = &libraries;
  while (*link && *link != state)
    link = &(*link)->next;
  if (*link)
    *link = state->next;
  if (state->directory >= 0)
    close(state->directory);
  pthread_mutex_unlock(&directory_mutex);
  delete state;
}

bool close_state(LibraryState *state) {
  state->closing = true;
  if (state->uncertain)
    return false;
  if (state->handle.generation) {
    auto result = yukilinker_c_close(owner, state->handle);
    ZLOGI("linker close: %s backend=%u valid=%u reference=%u retention=%u "
          "unmapped=%u code=%u",
          state->origin, result.info.backend, result.handle_valid,
          result.info.reference, result.info.retention_reasons,
          result.info.private_mapping_unmapped, result.error.detail.code);
    if (result.handle_valid) {
      // Releasing the root may leave callback-return holds in its context.
      // Only explicit retirement can clear them after the caller drains users.
      if (!state->context ||
          result.info.reference != YUKILINKER_C_REFERENCE_RELEASED ||
          !yukilinker_c_context_destroy(owner, state->context))
        return false;
      state->context = nullptr;
      result = yukilinker_c_close(owner, state->handle);
      if (result.handle_valid || result.error.detail.code != YUKILINKER_C_OK)
        return false;
    }
    if (result.error.detail.code != YUKILINKER_C_OK &&
        result.info.reference != YUKILINKER_C_REFERENCE_RELEASED)
      return false;
    state->handle = {};
  }
  if (state->context) {
    if (!yukilinker_c_context_destroy(owner, state->context))
      return false;
    state->context = nullptr;
  }
  forget_state(state);
  return true;
}

LeaseState *acquire(yukilinker_c_image_id image, uint32_t kind) {
  auto *state = new (std::nothrow) LeaseState();
  if (!state)
    return nullptr;
  if (yukilinker_c_image_acquire(owner, image, kind, &state->token) !=
      YUKILINKER_C_OK) {
    delete state;
    return nullptr;
  }
  return state;
}
void release(LeaseState *state) {
  if (!state)
    return;
  if (!state->snapshot.value && !state->token.value) {
    delete state;
    return;
  }
  const auto result =
      state->snapshot.value
          ? yukilinker_c_snapshot_release(owner, &state->snapshot)
          : yukilinker_c_image_release(owner, &state->token);
  if (result != YUKILINKER_C_OK)
    ZLOGE("linker lease release: code=%u retained=%d", result,
          state->token.value != 0 || state->snapshot.value != 0);
  if (!state->token.value && !state->snapshot.value) {
    delete state;
    return;
  }
  pthread_mutex_lock(&lease_mutex);
  state->next = pending_leases;
  pending_leases = state;
  pthread_mutex_unlock(&lease_mutex);
}

std::shared_ptr<PrivateImage>
pin_image(const yukilinker_c_image_record &record) {
  for (auto it = image_cache.begin(); it != image_cache.end();) {
    if (auto cached = it->lock()) {
      if (cached->id.generation == record.id.generation &&
          cached->id.namespace_id == record.id.namespace_id)
        return cached;
      ++it;
    } else {
      it = image_cache.erase(it);
    }
  }
  auto image = std::make_shared<PrivateImage>();
  image->device = record.device;
  image->inode = record.inode;
  image->id = record.id;
  image->lease = acquire(record.id, YUKILINKER_C_LEASE_HOST_BRIDGE);
  if (!image->lease)
    return {};
  image->headers.resize(record.program_header_count);
  size_t required = 0;
  const auto result = yukilinker_c_image_program_headers(
      owner, record.id, image->headers.data(),
      image->headers.size() * sizeof(ElfW(Phdr)), &required);
  if (result != YUKILINKER_C_OK ||
      required != image->headers.size() * sizeof(ElfW(Phdr)))
    return {};
  image->named = {{record.load_bias, image->headers.data(),
                   image->headers.size(), record.id.generation},
                  record.origin};
  image_cache.push_back(image);
  return image;
}
} // namespace

bool bind_native(const yukilinker_c_native_services *incoming) noexcept {
  if (!incoming || incoming->size != sizeof(*incoming) ||
      incoming->version != YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION ||
      incoming->origin.size != sizeof(incoming->origin) ||
      incoming->origin.version != YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION ||
      incoming->origin.process != static_cast<uint32_t>(getpid()) ||
      !incoming->origin.generation || !incoming->open ||
      !incoming->origin.libc_base || !incoming->origin.libc_anchor)
    return false;
  pthread_mutex_lock(&native_mutex);
  const bool accepted = native_window == NativeWindow::Unbound && !owner;
  if (accepted) {
    native_services = *incoming;
    native_window = NativeWindow::Open;
  }
  pthread_mutex_unlock(&native_mutex);
  return accepted;
}

bool native_window_open() noexcept {
  return native_window == NativeWindow::Open &&
         native_services.origin.process == static_cast<uint32_t>(getpid());
}

bool revoke_native() noexcept {
  if (native_services.origin.process != static_cast<uint32_t>(getpid()))
    return false;
  pthread_mutex_lock(&native_mutex);
  if (native_window == NativeWindow::Open)
    native_window = NativeWindow::Draining;
  const bool drained = (native_window == NativeWindow::Draining ||
                        native_window == NativeWindow::Revoked) &&
                       !native_calls;
  if (drained) {
    native_services.open = nullptr;
    native_services.context = nullptr;
    native_window = NativeWindow::Revoked;
  }
  pthread_mutex_unlock(&native_mutex);
  return drained;
}

bool fallback_allowed(const yukilinker_c_error &error) noexcept {
  if (error.execution != YUKILINKER_C_EXECUTION_NOT_STARTED ||
      error.detail.execution_started || error.detail.cleanup_incomplete)
    return false;
  return error.detail.code == YUKILINKER_C_UNSUPPORTED ||
         error.detail.code == YUKILINKER_C_DEPENDENCY_MISSING ||
         error.detail.code == YUKILINKER_C_SYMBOL_MISSING ||
         error.detail.code == YUKILINKER_C_PERMISSION_DENIED;
}

bool initialize() noexcept {
  if (owner)
    return !runtime_stopped;
  if (runtime_stopped)
    return false;
  try {
    native_bionic::CallerContext context;
    native_bionic::Failure failure;
    if (!native_bionic::prepare_caller(context, failure)) {
      ZLOGE("linker host caller: stage=%s code=%s errno=%d",
            native_bionic::stage_name(failure.stage),
            native_bionic::error_name(failure.error), failure.system_error);
      return false;
    }
    // Linker and libc remain mapped while the Core owns its bootstrap deps.
    host_lookup = context.api.lookup;
    host_lookup_version = context.api.lookup_version;
    host_caller = reinterpret_cast<const void *>(context.anchor);
    if (native_window != NativeWindow::Unbound &&
        (native_services.origin.libc_base != context.libc.base ||
         !native_window_open())) {
      ZLOGE("native runtime libc identity differs from bootstrap binding");
      return false;
    }
    ZLOGI("linker host caller: libc=%s anchor=%p", context.libc.path.c_str(),
          host_caller);
  } catch (...) {
    ZLOGE("linker host caller allocation failed");
    return false;
  }
  if (!error_key_valid) {
    if (pthread_key_create(&error_key, free) != 0)
      return false;
    error_key_valid = true;
  }
  owner = yukilinker_c_runtime_create(&services);
  return owner != nullptr;
}
bool ready() noexcept { return owner && !runtime_stopped; }

bool mapping_changed() noexcept {
  if (!ready())
    return false;
  size_t count = 0;
  uint64_t next = event_cursor;
  const auto result =
      yukilinker_c_read_events(owner, event_cursor, nullptr, 0, &count, &next);
  const bool changed = result != YUKILINKER_C_OK || next != event_cursor;
  event_cursor = next;
  return changed;
}

bool shutdown() noexcept {
  if (owner) {
    // The caller has stopped module callbacks and joined their participating
    // threads.
    pthread_mutex_lock(&lease_mutex);
    LeaseState *retry = pending_leases;
    pending_leases = nullptr;
    pthread_mutex_unlock(&lease_mutex);
    while (retry) {
      auto *next = retry->next;
      release(retry);
      retry = next;
    }
    for (auto *state = libraries; state;) {
      auto *next = state->next;
      if (!state->detached || !close_state(state))
        return false;
      state = next;
    }
    yukilinker_c_error_detail error{};
    if (!runtime_stopped) {
      if (!yukilinker_c_try_shutdown(owner, &error)) {
        ZLOGE("linker shutdown: code=%u cleanup=%u", error.code,
              error.cleanup_incomplete);
        return false;
      }
      runtime_stopped = true;
      if (error.cleanup_incomplete)
        ZLOGE("linker shutdown completed with retained system dependency "
              "references");
    }
    if (!yukilinker_c_runtime_destroy(owner))
      return false;
    owner = nullptr;
    std::vector<std::weak_ptr<PrivateImage>>().swap(image_cache);
  }
  if (error_key_valid) {
    void *value = pthread_getspecific(error_key);
    if (pthread_setspecific(error_key, nullptr) != 0)
      return false;
    free(value);
    if (pthread_key_delete(error_key) != 0)
      return false;
    error_key_valid = false;
  }
  host_lookup = nullptr;
  host_lookup_version = nullptr;
  host_caller = nullptr;
  return true;
}
void prepare_fork() noexcept { yukilinker_c_prepare_fork(owner); }
void after_fork(bool child) noexcept {
  yukilinker_c_after_fork(owner, child);
  // Core serializes Library mutations with fork; a provider only reads the fd.
  if (child)
    directory_mutex = PTHREAD_MUTEX_INITIALIZER;
  if (child && native_window != NativeWindow::Unbound) {
    // An interrupted bootstrap remains non-retirable in this child. Never
    // inherit a callback or a mutex owned by a vanished bootstrap thread.
    native_mutex = PTHREAD_MUTEX_INITIALIZER;
    native_services.open = nullptr;
    native_services.context = nullptr;
    native_calls = 0;
    native_window = NativeWindow::Revoked;
  }
}

void close_dependency_directories() noexcept {
  pthread_mutex_lock(&directory_mutex);
  for (auto *state = libraries; state; state = state->next)
    if (state->directory >= 0)
      close(std::exchange(state->directory, -1));
  pthread_mutex_unlock(&directory_mutex);
}

bool handoff_ready(uintptr_t base, size_t size) noexcept {
  if (native_window != NativeWindow::Unbound &&
      (native_window != NativeWindow::Revoked || native_services.open ||
       native_services.context || native_calls ||
       native_services.origin.process != static_cast<uint32_t>(getpid())))
    return false;
  if (!ready() || !host_lookup || !host_lookup_version || !host_caller ||
      !base || size > UINTPTR_MAX - base)
    return false;
  const uintptr_t pointers[] = {
      reinterpret_cast<uintptr_t>(&services),
      reinterpret_cast<uintptr_t>(system_open),
      reinterpret_cast<uintptr_t>(system_symbol),
      reinterpret_cast<uintptr_t>(system_close),
      reinterpret_cast<uintptr_t>(system_address),
      reinterpret_cast<uintptr_t>(system_error),
      reinterpret_cast<uintptr_t>(system_iterate),
      reinterpret_cast<uintptr_t>(runtime_log),
      reinterpret_cast<uintptr_t>(dependency),
      reinterpret_cast<uintptr_t>(yukilinker_c_open_fd),
      reinterpret_cast<uintptr_t>(yukilinker_c_runtime_destroy)};
  for (const uintptr_t address : pointers)
    if (address < base || address - base >= size)
      return false;
  return true;
}

Library::Library(Library &&other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}
Library::~Library() {
  if (state_) {
    state_->detached = true;
    ZLOGE("linker owner retained for explicit shutdown: %s", state_->origin);
  }
}

bool Library::open(int fd, const char *name, int directory, bool use_private,
                   bool file_backed, bool force_system_load) noexcept {
  if (state_ || !ready() || !name || strnlen(name, PATH_MAX) == PATH_MAX)
    return false;
  state_ = new (std::nothrow) LibraryState();
  if (!state_)
    return false;
  auto &state = *state_;
  (void)snprintf(state.origin, sizeof(state.origin), "%s", name);
  pthread_mutex_lock(&directory_mutex);
  state.next = libraries;
  libraries = &state;
  pthread_mutex_unlock(&directory_mutex);
  yukilinker_c_open_options options{};
  options.size = sizeof(options);
  options.flags = RTLD_NOW | RTLD_LOCAL;
  options.origin = state.origin;
  yukilinker_c_error error{};
  if (use_private) {
    if (directory >= 0) {
      pthread_mutex_lock(&directory_mutex);
      const int duplicate = fcntl(directory, F_DUPFD_CLOEXEC, 0);
      state.directory = duplicate;
      pthread_mutex_unlock(&directory_mutex);
      if (duplicate < 0)
        return false;
    }
    state.context =
        yukilinker_c_context_create(owner, nullptr, dependency, &state);
    if (!state.context)
      return false;
    options.backend = YUKILINKER_C_BACKEND_PRIVATE;
    options.context = state.context;
    options.file_backed = file_backed;
    state.handle = yukilinker_c_open_fd(owner, fd, &options, &error);
    if (state.handle.generation) {
      state.private_backend = true;
      yukilinker_c_image_record record{};
      if (yukilinker_c_handle_image(owner, state.handle, &record) !=
          YUKILINKER_C_OK)
        return false;
      state.image = record.id;
      ZLOGI("linker open: %s backend=PRIVATE generation=%llu", name,
            static_cast<unsigned long long>(record.id.generation));
      return true;
    }
    log_error("private open", name, error);
    state.uncertain = error.execution != YUKILINKER_C_EXECUTION_NOT_STARTED ||
                      error.detail.execution_started;
    if (!fallback_allowed(error) ||
        !yukilinker_c_context_destroy(owner, state.context))
      return false;
    state.context = nullptr;
    ZLOGI("linker fallback: %s code=%u stage=%u", name, error.detail.code,
          error.detail.stage);
  }
  options.backend = YUKILINKER_C_BACKEND_SYSTEM;
  options.context = nullptr;
  options.file_backed = 0;
  auto *thread = thread_error();
  if (!thread)
    return false;
  const int previous_force_fd = thread->force_fd;
  thread->force_fd = force_system_load ? fd : -1;
  state.handle = yukilinker_c_open_fd(owner, fd, &options, &error);
  thread->force_fd = previous_force_fd;
  if (!state.handle.generation) {
    log_error("system open", name, error);
    state.uncertain = error.execution != YUKILINKER_C_EXECUTION_NOT_STARTED ||
                      error.detail.cleanup_incomplete;
    return false;
  }
  ZLOGI("linker open: %s backend=SYSTEM", name);
  return true;
}
bool Library::loaded() const noexcept {
  return state_ && state_->handle.generation && !state_->closing;
}
bool Library::is_private() const noexcept {
  return state_ && state_->private_backend;
}
const char *Library::origin() const noexcept {
  return state_ ? state_->origin : "";
}
void *Library::symbol(const char *name) const noexcept {
  if (!loaded())
    return nullptr;
  const auto result = yukilinker_c_lookup(owner, state_->handle, name, nullptr);
  if (!result.found)
    log_error("symbol", state_->origin, result.error);
  return result.found ? result.address : nullptr;
}
bool Library::close() noexcept {
  if (!state_)
    return true;
  if (!close_state(state_))
    return false;
  state_ = nullptr;
  return true;
}

Execution::Execution(const Library &library, bool admitted) noexcept {
  if (!admitted || !library.loaded())
    return;
  if (!library.is_private()) {
    active_ = true;
    return;
  }
  lease_ = acquire(library.state_->image, YUKILINKER_C_LEASE_EXECUTION);
  active_ = lease_ != nullptr;
}
Execution::~Execution() { release(lease_); }
PrivateImage::~PrivateImage() { release(lease); }

std::vector<std::shared_ptr<PrivateImage>> private_images() {
  std::vector<std::shared_ptr<PrivateImage>> images;
  if (!ready())
    return images;
  std::unique_ptr<LeaseState, decltype(&release)> snapshot(
      new (std::nothrow) LeaseState(), release);
  if (!snapshot)
    return images;
  size_t count = 0;
  if (yukilinker_c_snapshot_acquire(owner, nullptr, nullptr,
                                    &snapshot->snapshot,
                                    &count) != YUKILINKER_C_OK)
    return images;
  for (size_t i = 0; i < count; ++i) {
    yukilinker_c_image_record record{};
    if (yukilinker_c_snapshot_image(owner, snapshot->snapshot, i, &record) !=
        YUKILINKER_C_OK)
      continue;
    if (auto image = pin_image(record))
      images.push_back(std::move(image));
  }
  return images;
}
std::shared_ptr<PrivateImage> private_image(const void *address) {
  const auto value = reinterpret_cast<uintptr_t>(address);
  for (auto &image : private_images()) {
    for (const auto &header : image->headers) {
      if (header.p_type != PT_LOAD ||
          header.p_vaddr > UINTPTR_MAX - image->named.image.load_bias)
        continue;
      const uintptr_t begin = image->named.image.load_bias + header.p_vaddr;
      if (value >= begin && value - begin < header.p_memsz)
        return image;
    }
  }
  return {};
}
std::shared_ptr<PrivateImage> private_image(dev_t device, ino_t inode,
                                            bool *ambiguous) {
  if (ambiguous)
    *ambiguous = false;
  std::shared_ptr<PrivateImage> match;
  for (auto &image : private_images()) {
    yukilinker_c_image_record record{};
    if (yukilinker_c_image_query(owner, image->id, &record) !=
            YUKILINKER_C_OK ||
        record.device != static_cast<uint64_t>(device) ||
        record.inode != static_cast<uint64_t>(inode))
      continue;
    if (match) {
      if (ambiguous)
        *ambiguous = true;
      return {};
    }
    match = image;
  }
  return match;
}

yukiplt::HookResult hook_batch(yukiplt::MapsCache &cache,
                               const yukiplt::NamedImage &image,
                               const yukiplt::HookRequest *requests,
                               size_t count, yukiplt::SlotReport *reports,
                               yukiplt::OperationDiagnostic *diagnostic) {
  if (!image.image.instance_token)
    return yukiplt::hook_batch_nonnull_named_cached(cache, image, requests,
                                                    count, reports, diagnostic);
  yukiplt::HookResult failure;
  failure.error = yukiplt::Error::Make(yukiplt::ErrorCode::kInvalidArgument);
  if (!requests || count == 0 || count > 64)
    return failure;
  std::vector<yukiplt::FunctionImport> imports;
  std::vector<yukiplt::FunctionHookRequestEx> selected;
  imports.reserve(count);
  selected.reserve(count);
  bool batch_selected = false;
  if (count > 1) {
    std::vector<yukiplt::FunctionHookTarget> targets;
    targets.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const yukiplt::FunctionImportSelector selector{
          {requests[i].symbol}, yukiplt::kSourceJmprel, yukiplt::kTypeJumpSlot};
      targets.push_back(
          {image.image,
           {selector, requests[i].replacement, requests[i].out_original}});
    }
    auto plan =
        yukiplt::build_function_hook_plan(targets.data(), targets.size());
    if (!plan && plan.error().code == yukiplt::ErrorCode::kOutOfMemory) {
      failure.error = plan.error();
      return failure;
    }
    if (plan) {
      for (const auto &target : plan.value().targets())
        imports.push_back(target.selected);
      batch_selected = true;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    // Failed plans replay the original request order, including GOT
    // preconditions.
    if (!batch_selected) {
      const yukiplt::FunctionImportSelector selector{
          {requests[i].symbol}, yukiplt::kSourceJmprel, yukiplt::kTypeJumpSlot};
      auto found = yukiplt::find_function_import(image.image, selector);
      if (!found) {
        failure.error = found.error();
        return failure;
      }
      imports.push_back(std::move(found.value()));
    }
    const uintptr_t slot = imports[i].base.import.slot_address;
    if (slot % alignof(uintptr_t))
      return failure;
    // The custom-image lease and Core mapping barrier keep this GOT slot
    // readable.
    const uintptr_t original =
        __atomic_load_n(reinterpret_cast<uintptr_t *>(slot), __ATOMIC_ACQUIRE);
    if (!original)
      return failure;
    selected.push_back({yukiplt::exact_function_selector(imports[i]),
                        requests[i].replacement, requests[i].out_original, true,
                        original});
  }
  return yukiplt::hook_function_imports_cached(cache, image.image,
                                               selected.data(), selected.size(),
                                               reports, diagnostic);
}
} // namespace yukizygisk::linker
