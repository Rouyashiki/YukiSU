#ifndef YUKIZYGISK_EXIT_CLEANUP_HPP
#define YUKIZYGISK_EXIT_CLEANUP_HPP

#include <cstddef>
#include <cstdint>

namespace yukizygisk::exit_cleanup {

enum class Status : uint8_t { Complete, Unavailable, RegistrationFailed };
struct Result {
  Status status = Status::Unavailable;
  size_t requested = 0;
  size_t registered = 0;
  int error = 0;
  uint64_t elapsed_us = 0;
};

Result compact() noexcept;

namespace detail {
using Callback = void (*)(void *);
struct Services {
  int (*append)(Callback, void *, void *) = nullptr;
  void (*finalize)(void *) = nullptr;
  Callback release = nullptr;
};

constexpr size_t batch_size(size_t page, size_t pointer_size) noexcept {
  if ((pointer_size != 4 && pointer_size != 8) || page == 0 ||
      (page & (page - 1)) != 0 || page < 3 * pointer_size)
    return 0;
  const size_t entry = 3 * pointer_size;
  return (page / entry) + (page % entry != 0);
}

// The production caller validates all three services against the same libc.
Result run(const Services &services, size_t page) noexcept;
} // namespace detail
} // namespace yukizygisk::exit_cleanup
#endif
