#include "ksu.h"
#include "prelude.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define KSUD_PATH "/data/adb/ksud"
#define KSU_WORKING_DIR "/data/adb/ksu"
#define KSU_BINARY_DIR "/data/adb/ksu/bin"
#define KSU_LOG_DIR "/data/adb/ksu/log"
#define KSUD_MAX_SIZE (128ULL * 1024ULL * 1024ULL)

static const char kSelinuxXattr[] = "security.selinux";
static const char kKsuFileContext[] = "u:object_r:ksu_file:s0";

enum ksud_task_type {
  KSUD_TASK_VERIFY,
  KSUD_TASK_INSTALL,
  KSUD_TASK_REPAIR_LINKS,
};

enum ksud_integrity_result {
  KSUD_INTEGRITY_UNAVAILABLE = 0,
  KSUD_INTEGRITY_MATCH = 1,
  KSUD_INTEGRITY_MISMATCH = 2,
};

struct ksud_task {
  enum ksud_task_type type;
  char source[PATH_MAX];
  int result;
};

static bool read_exact_at(int descriptor, void *buffer, size_t size,
                          off_t offset) {
  size_t total = 0;
  while (total < size) {
    ssize_t count = pread(descriptor, (char *)buffer + total, size - total,
                          offset + (off_t)total);
    if (count > 0) {
      total += (size_t)count;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

static bool write_all(int descriptor, const void *buffer, size_t size) {
  size_t total = 0;
  while (total < size) {
    ssize_t count =
        write(descriptor, (const char *)buffer + total, size - total);
    if (count > 0) {
      total += (size_t)count;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

static bool has_daemon_context(int descriptor) {
  char context[128] = {0};
  ssize_t size =
      fgetxattr(descriptor, kSelinuxXattr, context, sizeof(context) - 1);
  if (size <= 0 || (size_t)size >= sizeof(context)) {
    return false;
  }
  context[size] = '\0';
  return strcmp(context, kKsuFileContext) == 0;
}

static int verify_ksud(const char *source_path) {
  int source = open(source_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (source < 0) {
    return -1;
  }

  struct stat source_status = {};
  if (fstat(source, &source_status) != 0 || !S_ISREG(source_status.st_mode) ||
      source_status.st_size <= 0 ||
      (uint64_t)source_status.st_size > KSUD_MAX_SIZE) {
    close(source);
    return -1;
  }

  int installed = open(KSUD_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (installed < 0) {
    close(source);
    return 0;
  }

  struct stat installed_status = {};
  bool matches = fstat(installed, &installed_status) == 0 &&
                 S_ISREG(installed_status.st_mode) &&
                 installed_status.st_uid == 0 && installed_status.st_gid == 0 &&
                 (installed_status.st_mode & 0777) == 0755 &&
                 installed_status.st_size == source_status.st_size &&
                 has_daemon_context(installed);

  uint8_t source_buffer[65536];
  uint8_t installed_buffer[65536];
  off_t offset = 0;
  while (matches && offset < source_status.st_size) {
    size_t count = (size_t)(source_status.st_size - offset);
    if (count > sizeof(source_buffer)) {
      count = sizeof(source_buffer);
    }
    matches = read_exact_at(source, source_buffer, count, offset) &&
              read_exact_at(installed, installed_buffer, count, offset) &&
              memcmp(source_buffer, installed_buffer, count) == 0;
    offset += (off_t)count;
  }

  close(installed);
  close(source);
  return matches ? 1 : 0;
}

static bool ensure_directory(const char *path, mode_t mode) {
  bool created = mkdir(path, mode) == 0;
  if (!created) {
    if (errno != EEXIST) {
      return false;
    }
  }
  struct stat status = {};
  if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
    return false;
  }
  return !created || (chown(path, 0, 0) == 0 && chmod(path, mode) == 0);
}

static bool repair_tool_links(void) {
  if (!ensure_directory(KSU_WORKING_DIR, 0755) ||
      !ensure_directory(KSU_BINARY_DIR, 0755) ||
      !ensure_directory(KSU_LOG_DIR, 0755)) {
    return false;
  }

  int directory =
      open(KSU_BINARY_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) {
    return false;
  }

  static const char *const applets[] = {"ksud", "magiskboot", "bootctl",
                                        "resetprop", "yzctl"};
  const bool yukizygisk_enabled = query_feature(KSU_FEATURE_YUKIZYGISK) > 0;
  bool success = true;
  for (size_t index = 0; index < sizeof(applets) / sizeof(applets[0]);
       ++index) {
    if (strcmp(applets[index], "yzctl") == 0 && !yukizygisk_enabled) {
      continue;
    }
    char temporary[NAME_MAX];
    int length =
        snprintf(temporary, sizeof(temporary), ".%s.yukisu.tmp.%ld.%ld",
                 applets[index], (long)getpid(), syscall(__NR_gettid));
    if (length <= 0 || (size_t)length >= sizeof(temporary)) {
      success = false;
      break;
    }
    unlinkat(directory, temporary, 0);
    if (symlinkat(KSUD_PATH, directory, temporary) != 0 ||
        renameat(directory, temporary, directory, applets[index]) != 0) {
      unlinkat(directory, temporary, 0);
      success = false;
      break;
    }
  }
  if (success) {
    success = fsync(directory) == 0;
  }
  close(directory);
  return success;
}

static bool copy_source_to_temp(int source, int directory,
                                const char *temporary) {
  int output =
      openat(directory, temporary,
             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (output < 0) {
    return false;
  }

  uint8_t buffer[65536];
  bool success = true;
  for (;;) {
    ssize_t count = read(source, buffer, sizeof(buffer));
    if (count > 0) {
      if (!write_all(output, buffer, (size_t)count)) {
        success = false;
        break;
      }
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      success = false;
    }
    break;
  }

  if (success) {
    success = fchown(output, 0, 0) == 0 && fchmod(output, 0755) == 0 &&
              fsetxattr(output, kSelinuxXattr, kKsuFileContext,
                        sizeof(kKsuFileContext), 0) == 0 &&
              fsync(output) == 0;
  }
  close(output);
  return success;
}

static bool install_ksud(const char *source_path) {
  int source = open(source_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (source < 0) {
    return false;
  }
  struct stat status = {};
  if (fstat(source, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || (uint64_t)status.st_size > KSUD_MAX_SIZE) {
    close(source);
    return false;
  }

  int directory =
      open("/data/adb", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) {
    close(source);
    return false;
  }

  char temporary[NAME_MAX] = {0};
  int length =
      snprintf(temporary, sizeof(temporary), ".ksud.yukisu.tmp.%ld.%ld",
               (long)getpid(), syscall(__NR_gettid));
  bool success = length > 0 && (size_t)length < sizeof(temporary);
  if (success) {
    unlinkat(directory, temporary, 0);
    success = copy_source_to_temp(source, directory, temporary);
  }
  close(source);

  if (success) {
    success = renameat(directory, temporary, directory, "ksud") == 0 &&
              fsync(directory) == 0;
  }
  if (!success) {
    unlinkat(directory, temporary, 0);
  }
  close(directory);

  if (!success) {
    return false;
  }
  (void)repair_tool_links();
  return verify_ksud(source_path) == 1;
}

static void *run_ksud_task(void *opaque) {
  struct ksud_task *task = opaque;
  task->result = KSUD_INTEGRITY_UNAVAILABLE;
  if (ksu_grant_root() != 0 || geteuid() != 0) {
    return nullptr;
  }

  switch (task->type) {
  case KSUD_TASK_VERIFY: {
    int verified = verify_ksud(task->source);
    if (verified < 0) {
      task->result = KSUD_INTEGRITY_UNAVAILABLE;
    } else {
      task->result = verified ? KSUD_INTEGRITY_MATCH : KSUD_INTEGRITY_MISMATCH;
    }
    break;
  }
  case KSUD_TASK_INSTALL:
    task->result = install_ksud(task->source) ? 1 : 0;
    break;
  case KSUD_TASK_REPAIR_LINKS:
    task->result = repair_tool_links() ? 1 : 0;
    break;
  }
  return nullptr;
}

static int run_root_task(enum ksud_task_type type, const char *source) {
  struct ksud_task task = {.type = type, .result = KSUD_INTEGRITY_UNAVAILABLE};
  if (source) {
    size_t length = strlen(source);
    if (length == 0 || length >= sizeof(task.source)) {
      return KSUD_INTEGRITY_UNAVAILABLE;
    }
    memcpy(task.source, source, length + 1);
  }

  pthread_t thread;
  if (pthread_create(&thread, nullptr, run_ksud_task, &task) != 0) {
    return KSUD_INTEGRITY_UNAVAILABLE;
  }
  if (pthread_join(thread, nullptr) != 0) {
    return KSUD_INTEGRITY_UNAVAILABLE;
  }
  return task.result;
}

static int with_source_path(JNIEnv *env, jstring source_path,
                            enum ksud_task_type type) {
  if (!source_path) {
    return KSUD_INTEGRITY_UNAVAILABLE;
  }
  const char *source =
      GetEnvironment()->GetStringUTFChars(env, source_path, nullptr);
  if (!source) {
    return KSUD_INTEGRITY_UNAVAILABLE;
  }
  int result = run_root_task(type, source);
  GetEnvironment()->ReleaseStringUTFChars(env, source_path, source);
  return result;
}

NativeBridge(verifyKsudDaemon, jint, jstring source_path) {
  (void)clazz;
  return (jint)with_source_path(env, source_path, KSUD_TASK_VERIFY);
}

NativeBridge(installKsudDaemon, jboolean, jstring source_path) {
  (void)clazz;
  return with_source_path(env, source_path, KSUD_TASK_INSTALL) == 1;
}

NativeBridgeNP(ensureKsudToolLinks, jboolean) {
  (void)env;
  (void)clazz;
  return run_root_task(KSUD_TASK_REPAIR_LINKS, nullptr) == 1;
}
