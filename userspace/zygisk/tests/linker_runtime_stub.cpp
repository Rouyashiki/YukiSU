// Linux YukiPLT unit tests isolate the dispatcher from Android loader
// operations. The Native integration runner separately links the production SDK
// adapter.
#include "linker_runtime.hpp"
#include <unistd.h>

namespace yukizygisk::linker {
void prepare_fork() noexcept {}
void after_fork(bool) noexcept {}
bool mapping_changed() noexcept { return false; }
Library::~Library() = default;
Library::Library(Library &&other) noexcept : state_(other.state_) {
  other.state_ = nullptr;
}
Execution::Execution(const Library &, bool admitted) noexcept
    : active_(admitted) {}
Execution::~Execution() = default;
PrivateImage::~PrivateImage() = default;
std::shared_ptr<PrivateImage> private_image(const void *) { return {}; }
std::shared_ptr<PrivateImage> private_image(dev_t, ino_t, bool *ambiguous) {
  if (ambiguous)
    *ambiguous = false;
  return {};
}
std::vector<std::shared_ptr<PrivateImage>> private_images() { return {}; }
#include "plt_batch.inc"
} // namespace yukizygisk::linker
extern "C" pid_t __real_fork() { return fork(); }
extern "C" int core_system_iterate_phdr(int (*callback)(dl_phdr_info *, size_t,
                                                        void *),
                                        void *data) {
  return dl_iterate_phdr(callback, data);
}
