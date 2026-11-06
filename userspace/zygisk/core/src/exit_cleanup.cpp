#include "exit_cleanup.hpp"

#include "native_bionic.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace yukizygisk::exit_cleanup {
namespace {
class File {
public:
  explicit File(const char *path) : fd_(open(path, O_RDONLY | O_CLOEXEC)) {}
  ~File() {
    if (fd_ >= 0)
      close(fd_);
  }
  File(const File &) = delete;
  File &operator=(const File &) = delete;
  File(File &&) = delete;
  File &operator=(File &&) = delete;
  [[nodiscard]] int get() const { return fd_; }

private:
  int fd_;
};

class Batch {
public:
  explicit Batch(const detail::Services &services) : services_(services) {}
  ~Batch() {
    if (registered_)
      services_.finalize(&cookie_);
  }
  Batch(const Batch &) = delete;
  Batch &operator=(const Batch &) = delete;
  Batch(Batch &&) = delete;
  Batch &operator=(Batch &&) = delete;
  bool append() {
    if (services_.append(services_.release, nullptr, &cookie_) != 0)
      return false;
    ++registered_;
    return true;
  }
  [[nodiscard]] size_t registered() const { return registered_; }

private:
  size_t registered_ = 0;
  const detail::Services &services_;
  // libc compares this token; neither it nor free(NULL) dereferences it.
  unsigned char cookie_ = 0;
};

uint64_t monotonic_us() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (static_cast<uint64_t>(now.tv_sec) * 1000000) + (now.tv_nsec / 1000);
}

bool matches_file(const native_bionic::CallerContext &context,
                  const native_bionic::Maps &maps, int fd, const void *entry) {
  const auto code =
      native_bionic::code_address(reinterpret_cast<uintptr_t>(entry));
  constexpr size_t bytes = 16;
  if (!native_bionic::mapped_range(maps, code, bytes, PROT_READ | PROT_EXEC,
                                   &context.libc.identity))
    return false;
  for (const auto &map : maps) {
    if (code < map.start || code >= map.end)
      continue;
    const uint64_t relative = code - map.start;
    if (map.offset > INT64_MAX || relative > INT64_MAX - map.offset)
      return false;
    uint8_t original[bytes];
    return pread64(fd, original, bytes,
                   static_cast<off64_t>(map.offset + relative)) == bytes &&
           memcmp(original, reinterpret_cast<const void *>(code), bytes) == 0;
  }
  return false;
}

bool resolve(detail::Services &services, int &error) {
  native_bionic::CallerContext context;
  native_bionic::Failure failure;
  if (!native_bionic::prepare_caller(context, failure)) {
    error = failure.system_error ? failure.system_error : ENOTSUP;
    return false;
  }
  const auto maps = native_bionic::read_maps();
  const File file(context.libc.path.c_str());
  struct stat identity{};
  if (file.get() < 0 || fstat(file.get(), &identity) != 0) {
    error = errno;
    return false;
  }
  if (static_cast<uint64_t>(identity.st_dev) != context.libc.identity.device ||
      static_cast<uint64_t>(identity.st_ino) != context.libc.identity.inode) {
    error = ESTALE;
    return false;
  }
  void *entries[3]{};
  const char *names[] = {"__cxa_atexit", "__cxa_finalize", "free"};
  for (size_t i = 0; i != 3; ++i) {
    entries[i] = context.api.lookup(RTLD_DEFAULT, names[i],
                                    reinterpret_cast<void *>(context.anchor));
    if (!matches_file(context, maps, file.get(), entries[i])) {
      error = ENOTSUP;
      return false;
    }
  }
  services = {reinterpret_cast<decltype(services.append)>(entries[0]),
              reinterpret_cast<decltype(services.finalize)>(entries[1]),
              reinterpret_cast<detail::Callback>(entries[2])};
  return true;
}
} // namespace

Result detail::run(const Services &services, size_t page) noexcept {
  const int saved_errno = errno;
  Result result;
  result.requested = batch_size(page, sizeof(void *));
  if (!result.requested || !services.append || !services.finalize ||
      !services.release) {
    result.error = EINVAL;
    return result;
  }
  result.status = Status::Complete;
  {
    Batch batch(services);
    // Bionic compacts when extraction saves at least one page. A full page of
    // temporary entries reaches that threshold regardless of the live suffix.
    for (size_t i = 0; i != result.requested; ++i) {
      errno = 0;
      if (!batch.append()) {
        result.status = Status::RegistrationFailed;
        result.error = errno ? errno : ENOMEM;
        break;
      }
    }
    result.registered = batch.registered();
  }
  errno = saved_errno;
  return result;
}

Result compact() noexcept {
  const int saved_errno = errno;
  const uint64_t start = monotonic_us();
  Result result;
  try {
    detail::Services services;
    if (resolve(services, result.error)) {
      const int page = getpagesize();
      result = detail::run(services, page > 0 ? static_cast<size_t>(page) : 0);
    }
  } catch (...) {
    result.error = ENOMEM;
  }
  const uint64_t finish = monotonic_us();
  if (start && finish >= start)
    result.elapsed_us = finish - start;
  errno = saved_errno;
  return result;
}
} // namespace yukizygisk::exit_cleanup
