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
yukiplt::HookResult hook_batch(yukiplt::MapsCache &cache,
                               const yukiplt::NamedImage &image,
                               const yukiplt::HookRequest *requests,
                               size_t count, yukiplt::SlotReport *reports,
                               yukiplt::OperationDiagnostic *diagnostic) {
  return yukiplt::hook_batch_nonnull_named_cached(cache, image, requests, count,
                                                  reports, diagnostic);
}
} // namespace yukizygisk::linker
extern "C" pid_t __real_fork() { return fork(); }
