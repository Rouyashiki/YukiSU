#include "userspace/zygisk/daemon/crash_monitor.hpp"
#include "userspace/zygisk/settings.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/wait.h>
#include <time.h>

using namespace yukizygisk::crash;
namespace fs = std::filesystem;

static int config_lock_signal = -1;
static bool fail_config_sync;
static bool stop_before_policy_retire;

extern "C" int __real_unlinkat(int, const char *, int);
extern "C" int __wrap_unlinkat(int dir, const char *name, int flags) {
  if (stop_before_policy_retire &&
      std::string_view(name) == "yzconfig.json.policy-pending")
    _exit(75);
  return __real_unlinkat(dir, name, flags);
}

extern "C" int __real_flock(int fd, int operation);
extern "C" int __wrap_flock(int fd, int operation) {
  if (config_lock_signal >= 0 && operation == LOCK_EX) {
    assert(write(config_lock_signal, "L", 1) == 1);
    config_lock_signal = -1;
  }
  return __real_flock(fd, operation);
}

extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
  if (fail_config_sync) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

static void put(const fs::path &path, const std::string &text) {
  std::ofstream output(path);
  output << text;
  output.close();
  assert(output.good());
}

static std::string get(const fs::path &path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), {}};
}

static yz_zygote_exit_event exit_event() {
  timespec now{};
  assert(clock_gettime(CLOCK_BOOTTIME, &now) == 0);
  yz_zygote_exit_event event{};
  event.event.type = YZ_EV_ZYGOTE_EXIT;
  event.event.pid = 2137;
  event.event.appid = 9;
  event.generation = 11;
  event.abi = 2;
  event.start_boottime = 1;
  event.observed_boottime = uint64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
  return event;
}

static void publish(const fs::path &directory, const std::string &dump,
                    uid_t owner = 1058) {
  const int fd =
      open(directory.c_str(), O_WRONLY | O_TMPFILE | O_CLOEXEC, 0640);
  assert(fd >= 0);
  assert(fchown(fd, owner, 0) == 0);
  assert(write(fd, dump.data(), dump.size()) ==
         static_cast<ssize_t>(dump.size()));
  const auto source = "/proc/self/fd/" + std::to_string(fd);
  const auto destination = directory / "tombstone_17";
  assert(linkat(AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                AT_SYMLINK_FOLLOW) == 0);
  close(fd);
}

static std::string replace(std::string text, const std::string &from,
                           const std::string &to) {
  const auto position = text.find(from);
  assert(position != std::string::npos);
  text.replace(position, from.size(), to);
  return text;
}

static void replay(const fs::path &base, const std::string &dump, bool enabled,
                   bool zygisk, bool file_first, bool expected,
                   yz_zygote_exit_event event, bool change_image = false,
                   uid_t owner = 1058) {
  fs::create_directories(base / "state");
  fs::create_directories(base / "dumps");
  put(base / "state/boot_id", boot_id());
  const auto parsed = parse_tombstone(dump);
  assert(parsed.frames.size() == 1);
  const auto &frame = parsed.frames[0];
  fs::create_directories(fs::path(frame.path).parent_path());
  put(frame.path, "module fixture");
  std::cerr << "CASE: " << base.filename() << " expected=" << expected << '\n';
  Monitor monitor((base / "dumps").string(),
                  [](const char *message) { std::cerr << message << '\n'; });
  monitor.set_modules({{frame.module, frame.path, {}, zygisk}});
  monitor.set_protection_enabled(enabled);
  monitor.start((base / "state").string(), 2);
  Monitor abi32((base / "dumps").string());
  abi32.set_protection_enabled(true);
  abi32.start((base / "state").string(), 1);
  assert(!monitor.suspended(frame.module));
  assert(!abi32.suspended(frame.module));
  if (change_image)
    put(frame.path, "replaced module fixture with another size");
  if (!file_first)
    monitor.on_exit(event);
  publish(base / "dumps", dump, owner);
  monitor.drain();
  if (file_first) {
    assert(!monitor.suspended(frame.module));
    monitor.on_exit(event);
  }
  assert(monitor.suspended(frame.module) == expected);
  assert(abi32.suspended(frame.module) == expected);
  assert(!monitor.suspended("another_module"));
  monitor.on_exit(event);
  monitor.drain();
  monitor.tick();
  const auto saved = suspended_modules((base / "state").string());
  assert(saved.a.size() == (expected ? 1 : 0));
  if (expected) {
    const auto state = get(base / "state/protection64.json");
    monitor.on_exit(event);
    assert(get(base / "state/protection64.json") == state);
    Monitor restarted((base / "dumps").string());
    restarted.start((base / "state").string(), 2);
    restarted.set_protection_enabled(true);
    assert(restarted.suspended(frame.module));
    restarted.set_protection_enabled(false);
    assert(!restarted.suspended(frame.module));
    restarted.set_protection_enabled(true);
    assert(restarted.suspended(frame.module));
  } else if (!enabled) {
    monitor.set_protection_enabled(true);
    monitor.on_exit(event);
    assert(!monitor.suspended(frame.module));
  }
}

static void protection_state(const fs::path &base) {
  fs::create_directories(base);
  put(base / "boot_id", boot_id());
  Protection a, b;
  a.start(base.string(), 2);
  b.start(base.string(), 1);
  assert(!a.suspend("disabled"));
  a.set_enabled(true);
  b.set_enabled(true);
  const auto stale =
      base / ("protection64.json.tmp." + std::to_string(getpid()));
  put(stale, "interrupted write");
  assert(a.suspend("module64"));
  assert(!a.dirty());
  assert(get(stale) == "interrupted write");
  assert(b.blocked("module64"));
  assert(b.suspend("module32"));
  assert(a.blocked("module32"));
  assert(suspended_modules(base.string()).a.size() == 2);
  assert(!a.suspend("module64"));
  assert(!a.suspend("../escape"));
  for (int i = 0; i < 70; ++i)
    assert(a.suspend("m" + std::to_string(i)));
  Protection restarted;
  restarted.start(base.string(), 2);
  restarted.set_enabled(true);
  assert(restarted.blocked("m0") && restarted.blocked("m69"));
  auto old = json::Value::object();
  old["boot_id"] = "previous-boot";
  old["modules"] = json::Value::array();
  old["modules"].push_back("old");
  put(base / "protection64.json", json::dump(old));
  put(base / "protection32.json", json::dump(old));
  Protection rebooted;
  rebooted.start(base.string(), 2);
  rebooted.set_enabled(true);
  assert(!rebooted.blocked("old") && !rebooted.blocked("m0"));
  assert(suspended_modules(base.string()).a.empty());
  old["boot_id"] = boot_id();
  put(base / "protection64.json", json::dump(old));
  assert(chown((base / "protection64.json").c_str(), 1058, 0) == 0);
  assert(suspended_modules(base.string()).a.empty());
}

static bool stage_test_profile(const json::Value &root) {
  if (!root.at("fixed_sepolicy").as_bool())
    return yukizygisk::settings::detail::withdraw_fixed_policy();
  return yukizygisk::fixed_policy::write_boot(
      std::string(ksud::PREINIT_DIR_DEFAULT) + "yukizygisk_policy.bin", root);
}

static void policy_configuration(const fs::path &base) {
  namespace settings = yukizygisk::settings;
  namespace policy = yukizygisk::fixed_policy;
  fs::create_directories(base);
  const pid_t isolated = fork();
  assert(isolated >= 0);
  if (isolated) {
    int status = 0;
    assert(waitpid(isolated, &status, 0) == isolated && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0);
    return;
  }
  assert(chroot(base.c_str()) == 0 && chdir("/") == 0);
  fs::create_directories("/dev");
  const std::string path = ksud::YUKIZYGISK_CONFIG_PATH;
  fs::create_directories(fs::path(path).parent_path());
  fs::create_directories(ksud::PREINIT_DIR_DEFAULT);
  const auto pending = path + settings::detail::kPolicyPending;
  const auto profile =
      std::string(ksud::PREINIT_DIR_DEFAULT) + "yukizygisk_policy.bin";
  const std::vector<std::string> enable = {"fixed_sepolicy", "true"};
  const std::vector<std::string> disable = {"fixed_sepolicy", "false"};
  const auto reject = +[](const json::Value &) {
    errno = EROFS;
    return false;
  };
  put(path, R"({"fixed_sepolicy":false,"custom":7})");
  const auto original = get(path);
  json::Value recovered;
  const std::string opaque =
      R"({"id":9007199254740993,"list":[-9007199254740995,0.1250]})";
  put(path, R"({"fixed_sepolicy":"invalid","custom":)" + opaque + "}");
  assert(settings::read(path, &recovered) &&
         !recovered.at("fixed_sepolicy").as_bool());
  assert(get(path).find(opaque) != std::string::npos);
  put(path, R"({"denylist_mode":1.0000000000000001,"custom":)" + opaque + "}");
  assert(settings::update(path, {"denylist_mode", "1"}));
  assert(get(path).find(opaque) != std::string::npos &&
         get(path).find("1.0000000000000001") == std::string::npos);
  std::string dense =
      R"({"fixed_sepolicy":"invalid","custom":[9007199254740993)";
  dense.append(yukizygisk::crash::kMaxDocument - dense.size() - 2, ' ');
  dense += "]}";
  put(path, dense);
  assert(settings::read(path, &recovered) &&
         !recovered.at("fixed_sepolicy").as_bool());
  assert(get(path).find("9007199254740993") != std::string::npos);
  put(path, original);
  for (const bool symlink : {false, true}) {
    const auto candidate = R"({"fixed_sepolicy":true})";
    assert(stage_test_profile(json::parse(candidate)));
    const auto backup = path + ".backup";
    put(backup, candidate);
    if (symlink)
      fs::create_symlink(backup, pending);
    else {
      put(pending, candidate);
      assert(chmod(pending.c_str(), 0664) == 0);
    }
    assert(settings::read(path, &recovered) &&
           !recovered.at("fixed_sepolicy").as_bool());
    assert(get(path) == original && !fs::exists(pending) &&
           get(backup) == candidate);
    assert(!fs::exists(profile) && fs::remove(policy::kRebootRequired));
    fs::remove(backup);
  }
  assert(chmod(path.c_str(), 0664) == 0);
  assert(!settings::update(path, enable, stage_test_profile) && errno == EPERM);
  assert(chmod(path.c_str(), 0600) == 0);
  assert(!settings::update(path, enable, reject) && errno == EROFS);
  assert(get(path) == original && !fs::exists(pending) && !fs::exists(profile));
  const auto restore_after_stage = +[](const json::Value &candidate) {
    assert(stage_test_profile(candidate));
    put(ksud::YUKIZYGISK_CONFIG_PATH, R"({"fixed_sepolicy":false,"custom":8})");
    return true;
  };
  assert(!settings::update(path, enable, restore_after_stage) &&
         errno == ESTALE);
  assert(get(path) == R"({"fixed_sepolicy":false,"custom":8})");
  assert(!fs::exists(pending) && !fs::exists(profile));
  assert(fs::remove(policy::kRebootRequired));
  put(path, original);
  assert(settings::update(path, enable, stage_test_profile));
  const auto enabled = get(path);
  const auto old_profile = get(profile);
  assert(!settings::update(path, disable, reject) && errno == EROFS);
  assert(get(path) == enabled && !fs::exists(pending) &&
         get(profile) == old_profile);

  json::Value root;
  assert(chmod(path.c_str(), 0664) == 0);
  for (const std::string contents : {R"({"fixed_sepolicy":false})", "{"}) {
    put(path, contents);
    assert(!settings::read(path, &root) && errno == EPERM);
    assert(!settings::update(path, disable, stage_test_profile) &&
           errno == EPERM);
    assert(get(path) == contents && get(profile) == old_profile);
  }
  assert(chmod(path.c_str(), 0600) == 0);
  put(path, enabled);
  assert(settings::read(path, &root));
  root["fixed_sepolicy"] = false;
  assert(!policy::write_boot(profile, root, false, [](const std::string &) {
    errno = EPERM;
    return false;
  }));
  assert(get(profile) == old_profile);
  for (const bool next_fixed : {false, true}) {
    for (const bool published : {false, true}) {
      const auto before_config = get(path);
      const pid_t child = fork();
      assert(child >= 0);
      if (child == 0) {
        const auto before = +[](const json::Value &) -> bool { _exit(75); };
        const auto after = +[](const json::Value &candidate) -> bool {
          assert(stage_test_profile(candidate));
          _exit(75);
        };
        settings::update(path, next_fixed ? enable : disable,
                         published ? after : before);
        _exit(1);
      }
      int status = 0;
      assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
             WEXITSTATUS(status) == 75);
      assert(get(path) == before_config && fs::exists(pending));
      const bool expected = published ? next_fixed : !next_fixed;
      assert(settings::read(path, &root) &&
             root.at("fixed_sepolicy").as_bool() == expected);
      assert(root.at("custom").as_number() == 7 && !fs::exists(pending));
      assert(fs::exists(profile) == expected);
    }
  }
  for (const bool legacy : {false, true}) {
    for (const bool keep_fixed : {false, true}) {
      put(path, original);
      assert(settings::detail::withdraw_fixed_policy());
      const pid_t child = fork();
      assert(child >= 0);
      if (!child) {
        settings::update(
            path, enable, +[](const json::Value &candidate) -> bool {
              assert(stage_test_profile(candidate));
              _exit(75);
            });
        _exit(1);
      }
      int status = 0;
      assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
             WEXITSTATUS(status) == 75);
      if (legacy)
        put(pending, enabled);
      struct stat before{}, after{};
      assert(stat(path.c_str(), &before) == 0);
      const auto restored = keep_fixed
                                ? R"({"fixed_sepolicy":true,"custom":8})"
                                : R"({"fixed_sepolicy":false,"custom":8})";
      put(path, restored);
      assert(stat(path.c_str(), &after) == 0 && before.st_ino == after.st_ino);
      assert(settings::read(path, &root) && get(path) == restored);
      assert(root.at("custom").as_number() == 8 && !fs::exists(pending));
      assert(fs::exists(profile) == keep_fixed);
      assert(fs::remove(policy::kRebootRequired) == !keep_fixed);
    }
  }
  const auto restore_journal = +[](const json::Value &candidate) {
    assert(stage_test_profile(candidate));
    const std::string config = ksud::YUKIZYGISK_CONFIG_PATH;
    const auto current = get(config);
    auto journal = current;
    journal.append(2, '\0').append(current);
    put(config + settings::detail::kPolicyPending, journal);
    return true;
  };
  put(path, original);
  assert(stage_test_profile(json::parse(original)));
  assert(!settings::update(path, enable, restore_journal));
  assert(errno == ESTALE && get(path) == original && !fs::exists(pending));
  assert(!fs::exists(profile) && fs::remove(policy::kRebootRequired));

  const std::string legacy_domains =
      R"({"fixed_sepolicy":true,"fixed_zn_domains":["first"],"custom":8})";
  put(path, legacy_domains);
  assert(settings::read(path, &root));
  assert(root.at("fixed_sepolicy").as_bool() &&
         root.at("custom").as_number() == 8 &&
         root.at("fixed_zn_domains").is_null());
  assert(get(path).find("fixed_zn_domains") == std::string::npos);
  assert(!settings::update(path, {"fixed_zn_domains", R"(["first"])"},
                           stage_test_profile) &&
         errno == EINVAL);

  const std::string fixed_saved = R"({"fixed_sepolicy":true,"custom":8})";
  put(path, fixed_saved);
  assert(stage_test_profile(json::parse(fixed_saved)));
  assert(!settings::update(path, enable, reject) && errno == EROFS);
  assert(get(path) == fixed_saved && !fs::exists(pending));
  for (const bool legacy_baseline : {false, true}) {
    put(path, original);
    assert(stage_test_profile(json::parse(original)));
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      stop_before_policy_retire = true;
      settings::update(path, enable, stage_test_profile);
      _exit(1);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 75);
    auto published = get(path);
    assert(json::parse(published).at("fixed_sepolicy").as_bool());
    if (legacy_baseline) {
      published.push_back('\0');
      published += original;
      put(pending, published);
    }
    put(path, original);
    assert(settings::read(path, &root) && get(path) == original);
    assert(!fs::exists(profile) && !fs::exists(pending));
    assert(fs::remove(policy::kRebootRequired));
  }
  _exit(0);
}

static void configuration(const fs::path &base) {
  fs::create_directories(base);
  const auto path = (base / "yzconfig.json").string();
  json::Value config;
  assert(yukizygisk::settings::read(path, &config));
  assert(config.at("crash_protection").as_bool());
  assert(yukizygisk::settings::protection_enabled(path));
  put(path, "{\"custom\":\"preserved\",\"early_load\":true}");
  assert(yukizygisk::settings::protection_enabled(path));
  assert(yukizygisk::settings::update(path, {"crash_protection", "true"}));
  assert(yukizygisk::settings::read(path, &config));
  assert(config.at("custom").string_or("") == "preserved");
  assert(config.at("early_load").as_bool());
  assert(yukizygisk::settings::protection_enabled(path));
  const auto before = get(path);
  for (const auto &invalid : std::vector<std::vector<std::string>>{
           {},
           {"crash_protection"},
           {"crash_protection", "1"},
           {"unknown", "true"},
           {"denylist_mode", "3"},
           {"crash_protection", "false", "denylist_mode", "-1"}}) {
    assert(!yukizygisk::settings::update(path, invalid));
    assert(get(path) == before);
  }
  assert(yukizygisk::settings::update(path, {"crash_protection", "false"}));
  assert(!yukizygisk::settings::protection_enabled(path));
  pid_t children[2]{};
  for (int i = 0; i < 2; ++i) {
    children[i] = fork();
    assert(children[i] >= 0);
    if (children[i] == 0)
      _exit(yukizygisk::settings::update(
                path, {i == 0 ? "crash_protection" : "dmesg_log", "true"})
                ? 0
                : 1);
  }
  for (auto child : children) {
    int status = -1;
    assert(waitpid(child, &status, 0) == child && status == 0);
  }
  assert(yukizygisk::settings::read(path, &config));
  assert(config.at("crash_protection").as_bool() &&
         config.at("dmesg_log").as_bool());
  assert(config.at("custom").string_or("") == "preserved");
  put(path, "not json");
  assert(yukizygisk::settings::update(path, {"crash_protection", "false"}));
  assert(yukizygisk::settings::read(path, &config));
  assert(!config.at("crash_protection").as_bool());
  assert(config.at("anonymous_memory").as_bool());
  fs::remove(path);
  fs::create_symlink(base / "unrelated", path);
  assert(!yukizygisk::settings::read(path, &config));
  assert(!yukizygisk::settings::update(path, {"crash_protection", "true"}));
  assert(!fs::exists(base / "unrelated"));
}

static void configuration_recovery(const fs::path &base) {
  fs::create_directories(base);
  const auto path = (base / "yzconfig.json").string();
  const auto defaults = json::dump(yukizygisk::settings::defaults());
  json::Value config;
  for (const auto &corrupt : std::vector<std::string>{
           std::string(56, '\0'), "", " \n\t", "{\"yukilinker\":", "not json",
           "{}trailing", "[]", "null", "false", "42", "\"string\""}) {
    put(path, corrupt);
    assert(yukizygisk::settings::read(path, &config));
    assert(json::dump(config) == defaults && get(path) == defaults);
    assert(yukizygisk::settings::protection_enabled(path));
    struct stat status{};
    assert(stat(path.c_str(), &status) == 0);
    assert(status.st_uid == 0 && (status.st_mode & 0777) == 0600);
  }

  // Valid partial settings and unknown fields survive a read byte for byte.
  const std::string custom = "{ \"custom\": 7, \"yukilinker\": false }\n";
  put(path, custom);
  assert(yukizygisk::settings::read(path, &config));
  assert(!config.at("yukilinker").as_bool());
  assert(config.at("custom").u32_or(0) == 7 && get(path) == custom);

  put(path, std::string(56, '\0'));
  fail_config_sync = true;
  assert(!yukizygisk::settings::read(path, &config) && errno == EIO);
  fail_config_sync = false;
  assert(get(path) == std::string(56, '\0'));
  for (const auto &entry : fs::directory_iterator(base))
    assert(entry.path().filename().string().find(".tmp.") == std::string::npos);

  // A reader that saw corruption must re-read after a competing writer commits.
  const int lock = open((path + ".lock").c_str(), O_RDWR | O_CLOEXEC);
  assert(lock >= 0 && flock(lock, LOCK_EX) == 0);
  int signal_pipe[2];
  assert(pipe(signal_pipe) == 0);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    alarm(10);
    close(lock);
    close(signal_pipe[0]);
    config_lock_signal = signal_pipe[1];
    assert(yukizygisk::settings::read(path, &config));
    assert(!config.at("yukilinker").as_bool());
    assert(config.at("custom").u32_or(0) == 7);
    _exit(0);
  }
  close(signal_pipe[1]);
  char ready;
  assert(read(signal_pipe[0], &ready, 1) == 1 && ready == 'L');
  close(signal_pipe[0]);
  put(path, custom);
  assert(flock(lock, LOCK_UN) == 0);
  close(lock);
  int child_status = -1;
  assert(waitpid(child, &child_status, 0) == child && child_status == 0);
  assert(get(path) == custom);

  // A read error or an unsupported file must not be mistaken for bad JSON.
  assert(chown(path.c_str(), 10000, 0) == 0);
  assert(!yukizygisk::settings::read(path, &config));
  assert(get(path) == custom);
  fs::remove(path);
  assert(mkfifo(path.c_str(), 0600) == 0);
  assert(!yukizygisk::settings::read(path, &config));
  assert(fs::is_fifo(path));
  fs::remove(path);
  fs::create_directory(path);
  assert(!yukizygisk::settings::read(path, &config));
  assert(fs::is_directory(path));
}

int main(int argc, char **argv) {
  assert(argc <= 2 && geteuid() == 0);
  std::string dump = argc == 2 ? get(argv[1]) : R"(ABI: 'arm64'
Timestamp: 2026-09-28 00:00:00+0000
Cmdline: zygote64
pid: 4210, tid: 4210, ppid: 2137, name: system_server  >>> zygote64 <<<
uid: 1000
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR)
backtrace:
    #00 pc 0000000000001234 /data/adb/modules/yz_fixture/zygisk/arm64-v8a.so

)";
  const auto parsed = parse_tombstone(dump);
  assert(parsed.pid == 4210 && parsed.ppid == 2137 &&
         parsed.frames.size() == 1);
  assert(parsed.frames[0].index == 0);
  char temporary[] = "/tmp/yz-crash-protection-XXXXXX";
  const char *created = mkdtemp(temporary);
  assert(created != nullptr);
  const fs::path base(created);
  const auto module_directory = fs::path("/data/adb/modules") / base.filename();
  fs::create_directories(module_directory.parent_path());
  assert(fs::create_directory(module_directory));
  dump = replace(dump, parsed.frames[0].path,
                 (module_directory / "zygisk/arm64-v8a.so").string());
  replay(base / "exit-first", dump, true, true, false, true, exit_event());
  replay(base / "file-first", dump, true, true, true, true, exit_event());
  replay(base / "disabled", dump, false, true, true, false, exit_event());
  replay(base / "native", dump, true, false, true, false, exit_event());
  replay(base / "app",
         replace(replace(dump, "Cmdline: zygote64", "Cmdline: example.app"),
                 "uid: 1000", "uid: 10000"),
         true, true, true, false, exit_event());
  replay(base / "server-command",
         replace(dump, "Cmdline: zygote64", "Cmdline: system_server"), true,
         true, true, true, exit_event());
  replay(base / "non-main-thread", replace(dump, "tid: 4210", "tid: 4305"),
         true, true, true, false, exit_event());
  replay(base / "caller", replace(dump, "#00 pc", "#01 pc"), true, true, true,
         false, exit_event());
  replay(base / "replaced", dump, true, true, true, false, exit_event(), true);
  replay(base / "bad-owner", dump, true, true, true, false, exit_event(), false,
         10000);
  auto direct = exit_event();
  direct.event.pid = 4210;
  direct.event.appid = 11;
  replay(base / "zygote", dump, true, true, true, true, direct);
  direct.event.appid = 9;
  replay(base / "wrong-signal", dump, true, true, true, false, direct);
  auto stale = exit_event();
  stale.event.pid = 99999;
  replay(base / "wrong-pid", dump, true, true, true, false, stale);
  protection_state(base / "protection");
  configuration(base / "config");
  policy_configuration(base / "policy-config");
  configuration_recovery(base / "config-recovery");
  Frame malformed;
  assert(!frame_line("#999999999999 pc 1234 " + parsed.frames[0].path,
                     &malformed));
  fs::remove_all(base);
  fs::remove_all(module_directory);
  std::cout << "PASS: tombstone replay, both event orders, per-boot/ABI state, "
               "restart, "
               "toggle, deduplication, scope/identity/owner guards and config "
               "transactions\n";
}
