#include <jni.h>

#include "kagami/embedded_paths.hpp"
#include "kagami/kasumi_client.hpp"
#include "uapi/kasumi.h"
#include "userspace/common/su_path.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/klog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" int ksu_grant_root(void);
extern "C" int ksu_kasumi_ioctl(unsigned long cmd, void *arg);

namespace {
namespace ksm = kagami::kasumi;
static_assert(sizeof(kasumi_conflict) == 80);
static_assert(sizeof(kasumi_conflicts_arg) == 1288);
static_assert(sizeof(kasumi_conflict_ack_arg) == 8);
static_assert(offsetof(kasumi_conflict, sequence) == 0);
static_assert(offsetof(kasumi_conflict, kind) == 8);
static_assert(offsetof(kasumi_conflict, target) == 16);
static_assert(offsetof(kasumi_conflicts_arg, events) == 8);
static_assert(KSM_IOC_GET_CONFLICTS == 0x8508532eU);
static_assert(KSM_IOC_ACK_CONFLICTS == 0x4008532fU);
constexpr const char *socket_path = "/data/adb/ksu/kagami/kagamid.sock";
constexpr const char *request_lock_path =
    "/data/adb/ksu/kagami/kagamid.requests.lock";
constexpr const char *log_path = kagami::embedded_log_file;
constexpr size_t response_limit = 1024UL * 1024;
std::mutex worker_mutex;

class Fd {
public:
  explicit Fd(int value) : value_(value) {}
  ~Fd() {
    if (value_ >= 0)
      close(value_);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&) = delete;
  Fd &operator=(Fd &&) = delete;
  [[nodiscard]] int get() const { return value_; }
  int release() {
    const int value = value_;
    value_ = -1;
    return value;
  }

private:
  int value_;
};

[[noreturn]] void failure(const char *operation, int error = errno) {
  throw std::runtime_error(std::string(operation) + ": " +
                           strerror(error ? error : EIO));
}

std::string quote(const std::string &input) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (const unsigned char c : input) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += static_cast<char>(c);
  }
  return out + '"';
}

struct RootTask {
  std::function<std::string()> action;
  std::string output;
  std::string error;
};

void *root_entry(void *opaque) {
  const std::lock_guard<std::mutex> lock(worker_mutex);
  const std::unique_ptr<std::shared_ptr<RootTask>> holder(
      static_cast<std::shared_ptr<RootTask> *>(opaque));
  const auto &task = *holder;
  try {
    if (ksu_grant_root() != 0 || geteuid() != 0)
      failure("Manager root", EPERM);
    task->output = task->action();
  } catch (const std::exception &error) {
    task->error = error.what();
  } catch (...) {
    task->error = "Native Kasumi operation failed";
  }
  return nullptr;
}

std::string root_call(std::function<std::string()> action) {
  // Serialize root-worker operations against the shared KSU driver fd.
  auto task = std::make_shared<RootTask>();
  task->action = std::move(action);
  auto *holder = new std::shared_ptr<RootTask>(task);
  pthread_t thread;
  int error = pthread_create(&thread, nullptr, root_entry, holder);
  if (error) {
    delete holder;
    failure("pthread_create", error);
  }
  error = pthread_join(thread, nullptr);
  if (error) {
    pthread_detach(thread);
    failure("pthread_join", error);
  }
  if (!task->error.empty())
    throw std::runtime_error(task->error);
  return task->output;
}

void start_controller(const std::string &path) {
  if (path.empty() || path.front() != '/' ||
      path.find('\0') != std::string::npos)
    failure("Invalid ksud path", EINVAL);
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error)
    failure("spawn actions", error);
  for (int fd = 0; fd <= 2 && !error; ++fd)
    error =
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
  char name[] = "ksud";
  char kagami[] = "kagami";
  char daemon[] = "daemon";
  char start[] = "start";
  char *const argv[] = {name, kagami, daemon, start, nullptr};
  pid_t pid = -1;
  if (!error)
    error = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (error)
    failure("Start Kagami", error);
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0)
    failure("Wait for Kagami");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    failure("Kagami startup failed", EIO);
}

std::string save_su_path(const std::string &ksud, const std::string &path) {
  if (ksud.empty() || ksud.front() != '/' ||
      ksud.find('\0') != std::string::npos)
    failure("Invalid ksud path", EINVAL);
  const int validation = ksud::validate_su_path(path);
  if (validation)
    failure("Invalid su path", -validation);

  std::array<int, 2> output{};
  if (pipe2(output.data(), O_CLOEXEC) != 0)
    failure("su path pipe");
  const Fd reader(output[0]);
  Fd writer(output[1]);
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error)
    failure("spawn actions", error);
  error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                           O_RDONLY, 0);
  if (!error)
    error =
        posix_spawn_file_actions_adddup2(&actions, writer.get(), STDOUT_FILENO);
  if (!error)
    error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                             "/dev/null", O_WRONLY, 0);
  std::array<std::string, 5> arguments{ksud, "su-path", "set", "--json", path};
  std::array<char *, 6> argv{arguments[0].data(), arguments[1].data(),
                             arguments[2].data(), arguments[3].data(),
                             arguments[4].data(), nullptr};
  pid_t pid = -1;
  if (!error)
    error = posix_spawn(&pid, ksud.c_str(), &actions, nullptr, argv.data(),
                        environ);
  posix_spawn_file_actions_destroy(&actions);
  if (error)
    failure("Start su path controller", error);
  close(writer.release());

  std::string text;
  std::array<char, 1024> buffer{};
  for (;;) {
    const auto count = read(reader.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR)
        continue;
      error = errno;
      break;
    }
    if (!count)
      break;
    if (text.size() < 16384)
      text.append(buffer.data(), static_cast<size_t>(count));
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0)
    failure("Wait for su path controller");
  if (error)
    failure("Read su path result", error);
  if (!WIFEXITED(status) || WEXITSTATUS(status) > 1 || text.empty())
    failure("Invalid su path response", EIO);
  return text;
}

int connect_controller(const char *path) {
  Fd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (fd.get() < 0)
    failure("socket");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path))
    failure("socket path", ENAMETOOLONG);
  memcpy(address.sun_path, path, strlen(path) + 1);
  if (connect(fd.get(), reinterpret_cast<sockaddr *>(&address),
              sizeof(address)) != 0)
    return -errno;
  ucred peer{};
  socklen_t size = sizeof(peer);
  if (getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
      peer.uid != 0)
    failure("Untrusted Kagami peer", EPERM);
  return fd.release();
}

using Deadline = std::chrono::steady_clock::time_point;
void wait_io(int fd, short events, Deadline deadline) {
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0)
      failure("Kagami response deadline", ETIMEDOUT);
    pollfd descriptor{fd, events, 0};
    const int result = poll(&descriptor, 1, static_cast<int>(left));
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0)
      failure("poll Kagami");
    if (!result)
      failure("Kagami response deadline", ETIMEDOUT);
    if (descriptor.revents & POLLNVAL)
      failure("Kagami socket", EBADF);
    return;
  }
}

std::string exchange(int fd, const std::string &request) {
  if (request.empty() || request.size() > 60UL * 1024 ||
      request.find_first_of("\r\n") != std::string::npos ||
      request.find('\0') != std::string::npos)
    failure("Invalid Kagami request", EINVAL);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  const auto wire = request + '\n';
  size_t sent = 0;
  while (sent < wire.size()) {
    wait_io(fd, POLLOUT, deadline);
    const auto count =
        send(fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
    if (count < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (count <= 0)
      failure("Send Kagami request");
    sent += static_cast<size_t>(count);
  }
  shutdown(fd, SHUT_WR);
  std::string response;
  std::array<char, 4096> buffer{};
  while (response.size() < response_limit) {
    wait_io(fd, POLLIN, deadline);
    const auto count =
        recv(fd, buffer.data(),
             std::min(buffer.size(), response_limit - response.size()), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (count < 0)
      failure("Read Kagami response");
    if (count == 0)
      failure("Incomplete Kagami response", EPROTO);
    response.append(buffer.data(), static_cast<size_t>(count));
    if (response.back() == '\n')
      return response;
  }
  failure("Kagami response too large", EOVERFLOW);
}

std::string request_controller(const std::string &ksud,
                               const std::string &request) {
  for (bool started = false;; started = true) {
    int connected;
    {
      const Fd request_lock(open(
          request_lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
      if (request_lock.get() < 0) {
        connected = -errno;
      } else {
        int locked;
        do {
          locked = flock(request_lock.get(), LOCK_SH);
        } while (locked < 0 && errno == EINTR);
        if (locked < 0)
          failure("Lock Kagami request");
        connected = connect_controller(socket_path);
        if (connected >= 0) {
          const Fd fd(connected);
          // Never replay a mutation after any request bytes might have been
          // delivered.
          return exchange(fd.get(), request);
        }
      }
    }
    if (started || (connected != -ENOENT && connected != -ECONNREFUSED))
      failure("Connect Kagami", -connected);
    // Startup needs the exclusive request lock; release ours before spawning
    // it.
    start_controller(ksud);
  }
}

std::string read_tail(const char *path, size_t limit, size_t lines) {
  const Fd fd(open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0) {
    if (errno == ENOENT)
      return {};
    failure("Open log");
  }
  struct stat st{};
  if (fstat(fd.get(), &st) || !S_ISREG(st.st_mode))
    failure("Log is not a regular file", EINVAL);
  const auto start = st.st_size > static_cast<off_t>(limit)
                         ? st.st_size - static_cast<off_t>(limit)
                         : 0;
  if (lseek(fd.get(), start, SEEK_SET) < 0)
    failure("Seek log");
  std::string text(limit, '\0');
  size_t used = 0;
  while (used < limit) {
    const auto count = read(fd.get(), text.data() + used, limit - used);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      failure("Read log");
    if (!count)
      break;
    used += static_cast<size_t>(count);
  }
  text.resize(used);
  if (start > 0) {
    const auto newline = text.find('\n');
    text.erase(0, newline == std::string::npos ? text.size() : newline + 1);
  }
  size_t seen = 0;
  for (size_t i = text.size(); i > 0; --i) {
    if (text[i - 1] == '\n' && i != text.size() && ++seen == lines)
      return text.substr(i);
  }
  return text;
}

std::string kernel_snapshot() {
  const auto version = ksm::version_info();
  const bool available = version.status == ksm::Status::Available;
  utsname uts{};
  if (uname(&uts))
    failure("uname");
  auto out =
      std::string("{\"kernel\":") + quote(uts.release) +
      ",\"kasumi_available\":" + (available ? "true" : "false") +
      ",\"protocol_version\":" + std::to_string(version.expected_protocol) +
      ",\"kernel_version\":" + std::to_string(version.kernel_protocol);
  if (!available)
    return out + ",\"features\":{\"bitmask\":0,\"names\":[]},\"hooks\":\"\","
                 "\"rules\":\"\"}";
  const auto caps = ksm::feature_capabilities();
  if (!caps.ok)
    failure("Kasumi features", caps.last_errno);
  const int enabled = ksm::enabled_state();
  if (enabled < 0)
    failure("Kasumi state");
  out += ",\"features\":{\"bitmask\":" + std::to_string(caps.bitmask) +
         ",\"names\":[";
  bool first = true;
  for (const auto &name : ksm::feature_names(caps.bitmask)) {
    if (!first)
      out += ',';
    first = false;
    out += quote(name);
  }
  out += "]},\"enabled\":";
  out += enabled ? "true" : "false";
  std::vector<ksm::UserHideRule> user_rules;
  const bool queried = ksm::user_hide_rules(user_rules);
  if (queried &&
      ((caps.bitmask & KSM_FEATURE_MANAGED_HIDE) || !user_rules.empty())) {
    out += ",\"user_hide\":[";
    bool first_rule = true;
    for (const auto &rule : user_rules) {
      if (!first_rule)
        out += ',';
      first_rule = false;
      out += "{\"path\":" + quote(rule.path) +
             ",\"id\":" + std::to_string(rule.id) +
             ",\"management\":" + std::to_string(rule.management) +
             ",\"binding\":" + std::to_string(rule.binding) +
             ",\"error\":" + std::to_string(rule.error) +
             ",\"bypass\":" + (rule.bypass ? "true}" : "false}");
    }
    out += ']';
  } else if (caps.bitmask & KSM_FEATURE_MANAGED_HIDE) {
    failure("Kasumi user hide state");
  }
  out += ",\"hooks\":" + quote(ksm::hooks()) +
         ",\"rules\":" + quote(ksm::active_rules()) + "}";
  return out;
}

void clear_log(const char *path) {
  const std::string lock_path = std::string(path) + ".lock";
  const Fd lock(
      open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
  struct stat lock_info{};
  if (lock.get() < 0 || fstat(lock.get(), &lock_info) ||
      !S_ISREG(lock_info.st_mode) || lock_info.st_nlink != 1)
    failure("Invalid daemon log lock", EINVAL);
  if (flock(lock.get(), LOCK_EX))
    failure("Lock daemon log");
  const Fd fd(open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0) {
    if (errno == ENOENT)
      return;
    failure("Open daemon log");
  }
  struct stat info{};
  if (fstat(fd.get(), &info) || !S_ISREG(info.st_mode))
    failure("Invalid daemon log", EINVAL);
  if (ftruncate(fd.get(), 0))
    failure("Clear daemon log");
}

std::string bytes(JNIEnv *env, jbyteArray array) {
  if (!array)
    failure("Missing data", EINVAL);
  const jsize size = env->GetArrayLength(array);
  if (size > static_cast<jsize>(response_limit))
    failure("JNI input too large", EOVERFLOW);
  std::string result(static_cast<size_t>(size), '\0');
  env->GetByteArrayRegion(array, 0, size,
                          reinterpret_cast<jbyte *>(result.data()));
  if (env->ExceptionCheck())
    failure("Read JNI input", EINVAL);
  return result;
}

jbyteArray result(JNIEnv *env, std::function<std::string()> action) {
  try {
    const auto output = root_call(std::move(action));
    if (output.size() > response_limit)
      failure("JNI result too large", EOVERFLOW);
    auto *array = env->NewByteArray(static_cast<jsize>(output.size()));
    if (array)
      env->SetByteArrayRegion(array, 0, static_cast<jsize>(output.size()),
                              reinterpret_cast<const jbyte *>(output.data()));
    return array;
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}
} // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_anatdx_yukisu_Natives_kagamiRequest(JNIEnv *env, jobject /* thiz */,
                                             jbyteArray ksud,
                                             jbyteArray request) {
  try {
    const auto input =
        std::make_shared<const std::pair<std::string, std::string>>(
            bytes(env, ksud), bytes(env, request));
    return result(env, [input] {
      return request_controller(input->first, input->second);
    });
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_anatdx_yukisu_Natives_kasumiKernelSnapshot(JNIEnv *env,
                                                    jobject /* thiz */) {
  return result(env, kernel_snapshot);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_anatdx_yukisu_Natives_kasumiRetryUserHide(JNIEnv *env,
                                                   jobject /* thiz */,
                                                   jbyteArray path) {
  try {
    const auto input = std::make_shared<const std::string>(bytes(env, path));
    return result(env, [input] {
      if (!ksm::retry_user_hide(*input))
        failure("Retry user hide rule");
      return std::string("{}");
    });
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_anatdx_yukisu_Natives_saveSuPath(JNIEnv *env, jobject /* thiz */,
                                          jbyteArray ksud, jbyteArray path) {
  try {
    const auto input =
        std::make_shared<const std::pair<std::string, std::string>>(
            bytes(env, ksud), bytes(env, path));
    return result(
        env, [input] { return save_su_path(input->first, input->second); });
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anatdx_yukisu_Natives_kasumiIsInitialized(JNIEnv * /*env*/,
                                                   jobject /*thiz*/) {
  return ksm::is_available() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_anatdx_yukisu_Natives_kasumiRuntimeState(JNIEnv * /*env*/,
                                                  jobject /*thiz*/) {
  return ksm::enabled_state();
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_anatdx_yukisu_Natives_getKasumiConflicts(JNIEnv *env,
                                                  jobject /*thiz*/) {
  kasumi_conflicts_arg cmd{};
  if (ksu_kasumi_ioctl(KSM_IOC_GET_CONFLICTS, &cmd) != 0 ||
      cmd.count > KSM_CONFLICT_MAX)
    return nullptr;

  jclass cls = env->FindClass("com/anatdx/yukisu/ui/util/KasumiConflict");
  if (!cls)
    return nullptr;
  jmethodID constructor =
      env->GetMethodID(cls, "<init>", "(JILjava/lang/String;)V");
  if (!constructor) {
    env->DeleteLocalRef(cls);
    return nullptr;
  }
  jobjectArray array =
      env->NewObjectArray(static_cast<jsize>(cmd.count), cls, nullptr);
  if (array) {
    for (__u32 i = 0; i < cmd.count; i++) {
      auto &event = cmd.events[i];
      event.target[KSM_CONFLICT_NAME_LEN - 1] = '\0';
      jstring target = env->NewStringUTF(event.target);
      if (!target)
        break;
      jobject entry =
          env->NewObject(cls, constructor, static_cast<jlong>(event.sequence),
                         static_cast<jint>(event.kind), target);
      env->DeleteLocalRef(target);
      if (!entry)
        break;
      env->SetObjectArrayElement(array, static_cast<jsize>(i), entry);
      env->DeleteLocalRef(entry);
      if (env->ExceptionCheck())
        break;
    }
  }
  env->DeleteLocalRef(cls);
  return array;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_anatdx_yukisu_Natives_ackKasumiConflicts(JNIEnv * /*env*/,
                                                  jobject /*thiz*/,
                                                  jlong sequence) {
  kasumi_conflict_ack_arg ack{};
  ack.sequence = static_cast<std::uint64_t>(sequence);
  return ksu_kasumi_ioctl(KSM_IOC_ACK_CONFLICTS, &ack) == 0 ? JNI_TRUE
                                                            : JNI_FALSE;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_anatdx_yukisu_Natives_kasumiReadLog(JNIEnv *env, jobject /* thiz */,
                                             jboolean kernel) {
  return result(env, [kernel] {
    if (!kernel)
      return read_tail(log_path, 256UL * 1024, 1000);
    std::string buffer(response_limit, '\0');
    const int count =
        klogctl(3, buffer.data(), static_cast<int>(buffer.size()));
    if (count < 0)
      failure("Read kernel log");
    buffer.resize(static_cast<size_t>(count));
    return buffer;
  });
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_anatdx_yukisu_Natives_kasumiStorageInfo(JNIEnv *env,
                                                 jobject /* thiz */,
                                                 jbyteArray path) {
  try {
    const auto target = std::make_shared<const std::string>(bytes(env, path));
    return result(env, [target] {
      if (target->empty() || target->front() != '/' ||
          target->find('\0') != std::string::npos)
        failure("Invalid storage path", EINVAL);
      struct statfs stats{};
      if (statfs(target->c_str(), &stats))
        failure("statfs");
      const auto size = stats.f_blocks * stats.f_bsize;
      const auto avail = stats.f_bavail * stats.f_bsize;
      std::string type = "unknown";
      if (stats.f_type == 0x01021994)
        type = "tmpfs";
      else if (stats.f_type == 0xef53)
        type = "ext4";
      else if (stats.f_type == 0xe0f5e1e2)
        type = "erofs";
      return "{\"size\":" + std::to_string(size) +
             ",\"avail\":" + std::to_string(avail) +
             ",\"type\":" + quote(type) + "}";
    });
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_anatdx_yukisu_Natives_kasumiClearLog(JNIEnv *env, jobject /* thiz */) {
  auto *unused = result(env, [] {
    clear_log(log_path);
    return std::string{};
  });
  if (unused)
    env->DeleteLocalRef(unused);
}

extern "C" JNIEXPORT void JNICALL
Java_com_anatdx_yukisu_Natives_kasumiClearMapsRules(JNIEnv *env,
                                                    jobject /* thiz */) {
  auto *unused = result(env, [] {
    if (!ksm::clear_maps_rules())
      failure("Clear Maps rules");
    return std::string{};
  });
  if (unused)
    env->DeleteLocalRef(unused);
}

extern "C" JNIEXPORT void JNICALL
Java_com_anatdx_yukisu_Natives_kasumiAddMapsRule(JNIEnv *env,
                                                 jobject /* thiz */,
                                                 jlongArray numbers,
                                                 jbyteArray path) {
  try {
    if (!numbers || env->GetArrayLength(numbers) != 4)
      failure("Invalid Maps fields", EINVAL);
    std::array<jlong, 4> values{};
    env->GetLongArrayRegion(numbers, 0, 4, values.data());
    const auto target = std::make_shared<const std::string>(bytes(env, path));
    if (target->empty() || target->front() != '/' ||
        target->find('\0') != std::string::npos)
      failure("Invalid Maps path", EINVAL);
    auto *unused = result(env, [values, target] {
      if (!ksm::add_maps_rule(static_cast<unsigned long>(values[0]),
                              static_cast<unsigned long>(values[1]),
                              static_cast<unsigned long>(values[2]),
                              static_cast<unsigned long>(values[3]), *target))
        failure("Add Maps rule");
      return std::string{};
    });
    if (unused)
      env->DeleteLocalRef(unused);
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
  }
}
