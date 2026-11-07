#pragma once

#include <link.h>
#include <yukilinker.h>
#include <yukiplt.hpp>

#include <memory>
#include <string>
#include <vector>

namespace yukizygisk::linker {

bool initialize() noexcept;
bool ready() noexcept;
bool mapping_changed() noexcept;
bool shutdown() noexcept;
void prepare_fork() noexcept;
void after_fork(bool child) noexcept;
// End Core dependency-directory access after the pre-specialize callbacks.
void close_dependency_directories() noexcept;
bool handoff_ready(uintptr_t base, size_t size) noexcept;
bool fallback_allowed(const yukilinker_c_error &error) noexcept;
bool bind_native(const yukilinker_c_native_services *incoming) noexcept;
bool native_window_open() noexcept;
bool revoke_native() noexcept;

struct LibraryState;
struct LeaseState;

// The Core retains failed operations until explicit cleanup succeeds.
class Library {
public:
  Library() noexcept = default;
  Library(Library &&other) noexcept;
  Library(const Library &) = delete;
  Library &operator=(const Library &) = delete;
  Library &operator=(Library &&) = delete;
  ~Library();

  // Borrow both fds; the directory dup lasts through pre-specialize.
  bool open(int fd, const char *name, int directory, bool use_private,
            bool file_backed, bool force_system_load) noexcept;
  void *symbol(const char *name) const noexcept;
  bool close() noexcept;
  bool retained() const noexcept { return state_ != nullptr; }
  bool loaded() const noexcept;
  bool is_private() const noexcept;
  const char *origin() const noexcept;

private:
  friend class Execution;
  LibraryState *state_ = nullptr;
};

class Execution {
public:
  explicit Execution(const Library &library, bool admitted = true) noexcept;
  ~Execution();
  Execution(const Execution &) = delete;
  Execution &operator=(const Execution &) = delete;
  explicit operator bool() const noexcept { return active_; }

private:
  LeaseState *lease_ = nullptr;
  bool active_ = false;
};

// A stable PHDR copy and an image lease outlive every custom YukiPLT handle.
struct PrivateImage {
  yukilinker_c_image_id id{};
  uint64_t device = 0;
  uint64_t inode = 0;
  yukiplt::NamedImage named;
  std::vector<ElfW(Phdr)> headers;
  LeaseState *lease = nullptr;
  ~PrivateImage();
};
std::shared_ptr<PrivateImage> private_image(const void *address);
std::shared_ptr<PrivateImage> private_image(dev_t device, ino_t inode,
                                            bool *ambiguous = nullptr);
std::vector<std::shared_ptr<PrivateImage>> private_images();
yukiplt::HookResult hook_batch(yukiplt::MapsCache &cache,
                               const yukiplt::NamedImage &image,
                               const yukiplt::HookRequest *requests,
                               size_t count, yukiplt::SlotReport *reports,
                               yukiplt::OperationDiagnostic *diagnostic);

} // namespace yukizygisk::linker
