#include "check.hpp"
#include "yukilinker.hpp"

#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv) {
  CHECK(argc == 4);
  void *stage = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (stage == nullptr)
    (void)fprintf(stderr, "stage load: %s\n", dlerror());
  CHECK(stage != nullptr);
  auto load = reinterpret_cast<void *(*)(int, const char *)>(
      dlsym(stage, "yuki_dlopen_memfd"));
  auto symbol = reinterpret_cast<void *(*)(void *, const char *)>(
      dlsym(stage, "yuki_dlsym"));
  auto retire =
      reinterpret_cast<bool (*)(void *)>(dlsym(stage, "retire_metadata"));
  CHECK(load != nullptr && symbol != nullptr && retire != nullptr);
  CHECK(strcmp(argv[3], "anonymous") == 0 ||
        strcmp(argv[3], "anonymous-clean") == 0);
  char target_path[4096];
  const char *slash = strrchr(argv[2], '/');
  CHECK(slash != nullptr);
  CHECK(snprintf(target_path, sizeof(target_path), "%.*s/libanon_target.so",
                 static_cast<int>(slash - argv[2]), argv[2]) > 0);
  void *target = dlopen(target_path, RTLD_NOW | RTLD_LOCAL);
  CHECK(target != nullptr);
  auto call = reinterpret_cast<int (*)()>(dlsym(target, "fixture_call"));
  CHECK(call != nullptr && call() == 17);
  int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
  CHECK(fd >= 0);
  auto *core = static_cast<yukilinker::SoHandle *>(load(fd, ""));
  close(fd);
  CHECK(core != nullptr);
  const uintptr_t base = reinterpret_cast<uintptr_t>(core->load_bias);
  const size_t span = core->map_size;
  auto adopt = reinterpret_cast<bool (*)(uintptr_t, size_t, void *)>(
      symbol(core, "zygisk_adopt_anonymous_image"));
  auto run = reinterpret_cast<int (*)(int (*)(), bool)>(
      symbol(core, "anonymous_hook_check"));
  auto entry = reinterpret_cast<void *(*)(uintptr_t, size_t)>(
      symbol(core, "anonymous_close_entry"));
  auto close_stage =
      reinterpret_cast<bool (*)()>(symbol(core, "anonymous_close_stage"));
  CHECK(adopt != nullptr && run != nullptr && entry != nullptr &&
        close_stage != nullptr);
  CHECK(adopt(base, span, stage));
  auto close_core = reinterpret_cast<int (*)(void *)>(entry(base, span));
  CHECK(close_core != nullptr);
  CHECK(retire(core));
  CHECK(close_stage());
  CHECK(run(call, strcmp(argv[3], "anonymous") == 0) == 0);
  CHECK(close_core(nullptr) == 0);
  unsigned char residency = 0;
  CHECK(mincore(reinterpret_cast<void *>(base),
                static_cast<size_t>(getpagesize()), &residency) == -1);
  CHECK(call() == 17);
  CHECK(dlclose(target) == 0);
  puts("Anonymous YukiLinker: hook, restore, private runtime cleanup and "
       "self-unmap passed");
  return 0;
}
