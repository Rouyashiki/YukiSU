#include "check.hpp"
#include "native_bionic.hpp"

#include <fcntl.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstring>

using namespace yukizygisk::native_bionic;

#include "caller_linker_under_test.inc"
#include "caller_maps_under_test.inc"

bool yukizygisk::native_bionic::resolve_loader(const Maps &maps, LoaderApi &api,
                                               Failure &failure) {
  return checked_resolve(maps, api, failure);
}

#include "caller_native_under_test.inc"

int main(int argc, char **argv) {
  CHECK(argc == 2);
  const auto maps = real_maps();
  LoaderApi api;
  Failure failure;
  if (!resolve_loader(maps, api, failure))
    (void)fprintf(stderr, "resolver: %s errno=%d AT_BASE=%p\n",
                  error_name(failure.error), failure.system_error,
                  reinterpret_cast<void *>(getauxval(AT_BASE)));
  CHECK(failure.error == Error::None);
  CallerContext context;
  const bool found = find_caller(
      api, maps, reinterpret_cast<const void *>(getpid), context, failure);
  if (!found) {
    Dl_info provider{};
    api.address_info(reinterpret_cast<const void *>(getpid), &provider);
    (void)fprintf(stderr,
                  "anchor: %s errno=%d reference=%p provider=%s base=%p\n",
                  error_name(failure.error), failure.system_error,
                  reinterpret_cast<const void *>(getpid),
                  provider.dli_fname != nullptr ? provider.dli_fname : "",
                  provider.dli_fbase);
  }
  CHECK(found);
  CHECK(validate_context(context, real_maps(), failure));
  const void *caller = reinterpret_cast<const void *>(context.anchor);
  CHECK(api.lookup(RTLD_DEFAULT, "getpid", caller) ==
        reinterpret_cast<void *>(getpid));
  CHECK(api.lookup_version(RTLD_DEFAULT, "getpid", "LIBC", caller) ==
        reinterpret_cast<void *>(getpid));
  const int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
  CHECK(fd >= 0);
  android_dlextinfo ext{};
  ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
  ext.library_fd = fd;
  const auto loaded =
      load("/nonexistent/caller-anchor-payload.so", RTLD_NOW | RTLD_LOCAL, ext);
  void *handle = loaded.handle;
  if (handle == nullptr)
    (void)fprintf(stderr, "loader: stage=%s reason=%s %s\n",
                  stage_name(loaded.failure.stage),
                  error_name(loaded.failure.error),
                  loaded.loader_error.c_str());
  CHECK(handle != nullptr);
  CHECK(fcntl(fd, F_GETFD) >= 0);
  close(fd);
  const auto answer =
      reinterpret_cast<int (*)()>(dlsym(handle, "caller_anchor_answer"));
  CHECK(answer != nullptr && answer() == 42);
  Dl_info provider{};
  CHECK(api.address_info(reinterpret_cast<const void *>(answer), &provider) !=
        0);
  CHECK(provider.dli_fbase != nullptr);
  CHECK(dlclose(handle) == 0);
  printf(
      "PASS: real Bionic four-argument FD load caller=%p linker=%s libc=%s\n",
      reinterpret_cast<void *>(context.anchor), api.linker.path.c_str(),
      context.libc.path.c_str());
}
