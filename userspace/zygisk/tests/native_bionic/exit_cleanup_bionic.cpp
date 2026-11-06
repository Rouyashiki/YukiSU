#include "exit_cleanup.hpp"
#include "native_bionic.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);
extern "C" void __cxa_finalize(void *);
extern "C" pid_t __real_fork();

namespace {
namespace cleanup = yukizygisk::exit_cleanup;
using cleanup::detail::Services;
const Services libc_services{__cxa_atexit, __cxa_finalize, free};

struct Entry {
  cleanup::detail::Callback function;
  void *argument;
  void *cookie;
  bool operator==(const Entry &other) const {
    return function == other.function && argument == other.argument &&
           cookie == other.cookie;
  }
};
struct Snapshot {
  size_t size = 0, cleared = 0;
  std::vector<Entry> live;
};

// Test-only observation: resolve each field independently, including on ARM32.
// Production never reads this private table or writes its contents.
class Observer {
public:
  Observer() {
    using namespace yukizygisk::native_bionic;
    CallerContext context;
    Failure failure;
    assert(prepare_caller(context, failure));
    const int fd = open(context.libc.path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st{};
    assert(fd >= 0 && fstat(fd, &st) == 0);
    std::vector<unsigned char> file(static_cast<size_t>(st.st_size));
    assert(pread(fd, file.data(), file.size(), 0) ==
           static_cast<ssize_t>(file.size()));
    close(fd);
    const auto *elf = reinterpret_cast<const ElfW(Ehdr) *>(file.data());
    const auto *sections =
        reinterpret_cast<const ElfW(Shdr) *>(file.data() + elf->e_shoff);
    const char *names[] = {"_ZL7g_array.0", "_ZL7g_array.1", "_ZL7g_array.2",
                           "_ZL13g_atexit_lock"};
    for (size_t i = 0; i != elf->e_shnum; ++i) {
      const auto &section = sections[i];
      if (section.sh_type != SHT_SYMTAB)
        continue;
      const auto *symbols =
          reinterpret_cast<const ElfW(Sym) *>(file.data() + section.sh_offset);
      const auto *strings = reinterpret_cast<const char *>(
          file.data() + sections[section.sh_link].sh_offset);
      for (size_t j = 0; j != section.sh_size / sizeof(*symbols); ++j) {
        for (size_t k = 0; k != fields_.size(); ++k) {
          if (strcmp(strings + symbols[j].st_name, names[k]) == 0)
            fields_[k] = context.libc.bias + symbols[j].st_value;
        }
      }
    }
    for (const auto address : fields_)
      assert(address);
  }
  Snapshot snapshot() const {
    auto *lock = reinterpret_cast<pthread_mutex_t *>(fields_[3]);
    assert(pthread_mutex_lock(lock) == 0);
    Snapshot result;
    Entry *entries = nullptr;
    memcpy(&entries, reinterpret_cast<const void *>(fields_[0]),
           sizeof(entries));
    memcpy(&result.size, reinterpret_cast<const void *>(fields_[1]),
           sizeof(result.size));
    memcpy(&result.cleared, reinterpret_cast<const void *>(fields_[2]),
           sizeof(result.cleared));
    assert(result.size < 100000 && result.cleared <= result.size);
    for (size_t i = 0; i != result.size; ++i) {
      if (entries[i].function)
        result.live.push_back(entries[i]);
    }
    assert(result.live.size() + result.cleared == result.size);
    assert(pthread_mutex_unlock(lock) == 0);
    return result;
  }

private:
  std::array<uintptr_t, 4> fields_{};
};

struct Ordered {
  unsigned next = 0, calls = 0;
};
struct Argument {
  Ordered *state;
  unsigned index;
};
void ordered(void *opaque) {
  const auto &arg = *static_cast<Argument *>(opaque);
  assert(arg.state->next && --arg.state->next == arg.index);
  ++arg.state->calls;
}
template <size_t N>
void register_ordered(std::array<Argument, N> &args, Ordered &state,
                      void *cookie) {
  state = {N, 0};
  for (size_t i = 0; i != N; ++i) {
    args[i] = {&state, static_cast<unsigned>(i)};
    assert(__cxa_atexit(ordered, &args[i], cookie) == 0);
  }
}

void completed() {
  const void *entries[] = {reinterpret_cast<const void *>(__cxa_atexit),
                           reinterpret_cast<const void *>(__cxa_finalize),
                           reinterpret_cast<const void *>(free)};
  std::array<std::array<unsigned char, 16>, 3> bytes;
  for (size_t i = 0; i != bytes.size(); ++i) {
    entries[i] =
        reinterpret_cast<const void *>(yukizygisk::native_bionic::code_address(
            reinterpret_cast<uintptr_t>(entries[i])));
    memcpy(bytes[i].data(), entries[i], bytes[i].size());
  }
  const auto executable_maps = [] {
    std::vector<std::array<uint64_t, 6>> result;
    for (const auto &map : yukizygisk::native_bionic::read_maps()) {
      if (map.perms & PROT_EXEC)
        result.push_back({map.start, map.end, map.offset, map.dev, map.inode,
                          static_cast<uint64_t>(map.perms)});
    }
    return result;
  };
  const auto maps = executable_maps();
  errno = EDOM;
  const auto result = cleanup::compact();
  if (result.status != cleanup::Status::Complete)
    fprintf(stderr, "cleanup status=%u errno=%d registered=%zu/%zu\n",
            static_cast<unsigned>(result.status), result.error,
            result.registered, result.requested);
  assert(result.status == cleanup::Status::Complete && !result.error &&
         result.registered ==
             cleanup::detail::batch_size(getpagesize(), sizeof(void *)) &&
         errno == EDOM);
  for (size_t i = 0; i != bytes.size(); ++i)
    assert(memcmp(bytes[i].data(), entries[i], bytes[i].size()) == 0);
  assert(executable_maps() == maps);
}

void holes(size_t prefix) {
  const Observer observer;
  completed();
  char padding, victim, survivor;
  auto size = observer.snapshot().size;
  for (; size < prefix; ++size)
    assert(__cxa_atexit(free, nullptr, &padding) == 0);
  for (unsigned round = 0; round != 3; ++round) {
    Ordered removed, retained;
    std::array<Argument, 3> victims;
    std::array<Argument, 207> survivors;
    register_ordered(victims, removed, &victim);
    register_ordered(survivors, retained, &survivor);
    __cxa_finalize(&victim);
    assert(removed.calls == victims.size() && !retained.calls);
    const auto before = observer.snapshot();
    if (prefix == 2526 && !round)
      assert(before.cleared == 3);
    completed();
    const auto after = observer.snapshot();
    assert(!after.cleared && after.live == before.live && !retained.calls);
    __cxa_finalize(&survivor);
    assert(retained.calls == survivors.size() && !retained.next);
    __cxa_finalize(&survivor);
    assert(retained.calls == survivors.size());
    completed();
  }
  __cxa_finalize(&padding);
  completed();
}

size_t budget, accepted, finalized;
void *failed_cookie;
int limited_append(cleanup::detail::Callback callback, void *arg,
                   void *cookie) {
  failed_cookie = cookie;
  if (accepted == budget) {
    errno = ENOMEM;
    return -1;
  }
  const int result = __cxa_atexit(callback, arg, cookie);
  if (!result)
    ++accepted;
  return result;
}
void counted_finalize(void *cookie) {
  assert(cookie == failed_cookie);
  ++finalized;
  __cxa_finalize(cookie);
}
void failures() {
  const Observer observer;
  completed();
  const auto before = observer.snapshot();
  const auto count = cleanup::detail::batch_size(getpagesize(), sizeof(void *));
  for (const auto limit : {size_t{0}, size_t{1}, count / 2, count - 1}) {
    budget = limit;
    accepted = finalized = 0;
    failed_cookie = nullptr;
    errno = EDOM;
    const auto result = cleanup::detail::run(
        {limited_append, counted_finalize, free}, getpagesize());
    assert(result.status == cleanup::Status::RegistrationFailed &&
           result.error == ENOMEM && result.registered == limit &&
           result.requested == count && errno == EDOM &&
           finalized == (limit != 0));
    const auto after = observer.snapshot();
    assert(after.live == before.live);
    for (const auto &entry : after.live)
      assert(entry.cookie != failed_cookie);
    completed();
    assert(!observer.snapshot().cleared);
  }
  for (auto services : {Services{}, Services{nullptr, __cxa_finalize, free},
                        Services{__cxa_atexit, nullptr, free},
                        Services{__cxa_atexit, __cxa_finalize, nullptr}}) {
    assert(cleanup::detail::run(services, getpagesize()).status ==
           cleanup::Status::Unavailable);
  }
  assert(cleanup::detail::run(libc_services, 4095).status ==
         cleanup::Status::Unavailable);
  assert(observer.snapshot().live == before.live);
}

void barrier(pthread_barrier_t &value) {
  const int result = pthread_barrier_wait(&value);
  assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}
struct Concurrent {
  pthread_barrier_t entered, release;
  std::array<Argument, 256> args;
  Ordered state;
  char cookie;
};
void held_finalizer(void *opaque) {
  auto &state = *static_cast<Concurrent *>(opaque);
  barrier(state.entered);
  barrier(state.release);
}
void concurrent() {
  const Observer observer;
  completed();
  const auto before = observer.snapshot();
  Concurrent state{};
  assert(pthread_barrier_init(&state.entered, nullptr, 2) == 0 &&
         pthread_barrier_init(&state.release, nullptr, 2) == 0);
  pthread_t thread;
  assert(pthread_create(
             &thread, nullptr,
             [](void *opaque) -> void * {
               auto &state = *static_cast<Concurrent *>(opaque);
               barrier(state.entered);
               register_ordered(state.args, state.state, &state.cookie);
               return nullptr;
             },
             &state) == 0);
  barrier(state.entered);
  for (unsigned i = 0; i != 8; ++i)
    completed();
  assert(pthread_join(thread, nullptr) == 0);
  auto expected = before.live;
  for (auto &arg : state.args)
    expected.push_back({ordered, &arg, &state.cookie});
  assert(observer.snapshot().live == expected && !state.state.calls);

  char held_cookie;
  assert(__cxa_atexit(held_finalizer, &state, &held_cookie) == 0);
  assert(pthread_create(
             &thread, nullptr,
             [](void *cookie) -> void * {
               __cxa_finalize(cookie);
               return nullptr;
             },
             &held_cookie) == 0);
  barrier(state.entered);
  completed();
  // The outer finalizer is still active, so Bionic must defer compaction.
  const auto nested = observer.snapshot();
  assert(nested.cleared && nested.live == expected);
  barrier(state.release);
  assert(pthread_join(thread, nullptr) == 0);
  const auto after = observer.snapshot();
  assert(!after.cleared && after.live == expected && !state.state.calls);
  __cxa_finalize(&state.cookie);
  assert(state.state.calls == state.args.size() && !state.state.next);
  pthread_barrier_destroy(&state.entered);
  pthread_barrier_destroy(&state.release);
}

template <class Function> void isolated(Function function) {
  const auto child = __real_fork();
  assert(child >= 0);
  if (!child) {
    function();
    _exit(0);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child && status == 0);
}
} // namespace

void exercise_exit_cleanup() {
  using cleanup::detail::batch_size;
  static_assert(batch_size(4096, 8) == 171 && batch_size(4096, 4) == 342 &&
                batch_size(16384, 8) == 683 && batch_size(16384, 4) == 1366);
  for (size_t page : {4096, 16384}) {
    for (size_t pointer : {4, 8}) {
      const auto entry = 3 * pointer;
      const auto count = batch_size(page, pointer);
      for (size_t size = 0; size != 3 * page; ++size)
        assert((size * entry + page - 1) / page <
               ((size + count) * entry + page - 1) / page);
    }
  }
  for (size_t prefix : {1, 170, 171, 172, 341, 342, 683, 2526, 2733})
    isolated([=] { holes(prefix); });
  isolated(failures);
  isolated(concurrent);

  auto *shared = static_cast<Ordered *>(
      mmap(nullptr, getpagesize(), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  assert(shared != MAP_FAILED);
  isolated([&] {
    std::array<Argument, 32> args;
    register_ordered(args, *shared, shared);
    completed();
    exit(0);
  });
  assert(shared->calls == 32 && !shared->next);
  assert(munmap(shared, getpagesize()) == 0);
  puts("PASS libc exit cleanup: stable live entries, three holes, page "
       "boundaries, "
       "repeat, partial failure, concurrent append/finalize and normal exit");
}
