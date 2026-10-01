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
  assert(!yukizygisk::settings::update(path, {"crash_protection", "true"}));
  assert(get(path) == "not json");
  fs::remove(path);
  fs::create_symlink(base / "unrelated", path);
  assert(!yukizygisk::settings::update(path, {"crash_protection", "true"}));
  assert(!fs::exists(base / "unrelated"));
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
