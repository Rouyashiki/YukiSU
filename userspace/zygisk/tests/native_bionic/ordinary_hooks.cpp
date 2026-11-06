#include "hook.cpp"
#include <cassert>
#include <sys/stat.h>

namespace {
ZygiskContext fd_context;
}

void fixture_snapshot_fds() {
  fd_context.allowed_fds.assign(fd_table_size(), false);
  DIR *directory = opendir("/proc/self/fd");
  assert(directory);
  while (const dirent *entry = readdir(directory)) {
    const int fd = atoi(entry->d_name);
    if (fd != dirfd(directory))
      mark_allowed(&fd_context, fd);
  }
  closedir(directory);
}

int fixture_dependency_directory(const char *path) {
  struct stat expected{};
  assert(stat(path, &expected) == 0);
  DIR *directory = opendir("/proc/self/fd");
  assert(directory);
  int found = -1;
  while (const dirent *entry = readdir(directory)) {
    const int fd = atoi(entry->d_name);
    if (fd == dirfd(directory) || fd_context.allowed_fds.at(fd))
      continue;
    struct stat actual{};
    if (fstat(fd, &actual) == 0 && actual.st_dev == expected.st_dev &&
        actual.st_ino == expected.st_ino) {
      assert(found == -1);
      found = fd;
    }
  }
  closedir(directory);
  assert(found >= 0);
  return found;
}

void fixture_sanitize_fds(bool app) {
  jintArray ignored = nullptr;
  fd_context.pid = 0;
  fd_context.fds_to_ignore = app ? &ignored : nullptr;
  ctx_sanitize_fds(&fd_context);
  fd_context.fds_to_ignore = nullptr;
}

void fixture_quiesce_unused_hooks() {
  // This driver has no ART runtime. Establish the same quiescent state that
  // the device reaches after retiring its child hooks, before testing Core TSD.
  assert(!g_runtime_ready && !g_jni_hooked && g_jni_hooks.empty());
  assert(!g_fork_handle.valid() && !g_strdup_handle.valid() &&
         !g_art_handle.valid());
  assert(!g_orig_fork.load() && !g_strdup.load() &&
         !g_art_post_fork_child.load());
  assert(g_plt_callbacks.load(std::memory_order_acquire) == 0);
  g_fork_quiet = g_strdup_quiet = g_art_quiet = true;
}
