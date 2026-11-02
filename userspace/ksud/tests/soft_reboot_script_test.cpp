#include "../src/core/ksucalls.hpp"
#include "../src/module/module.hpp"
#include "../src/utils.hpp"

#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
bool fail_exec = false;
bool fail_fork = false;
bool fail_wait = false;
bool detached = false;
bool cgroups_switched = false;
}  // namespace

namespace ksud {
ModuleVersions module_script_versions() {
    return {};
}
void apply_module_version_env(const ModuleVersions&) {}
const char* runtime_mode() {
    return "LKM";
}
bool is_late_load() {
    return false;
}
std::pair<uint64_t, bool> get_feature(uint32_t) {
    return {1, true};
}
void detach_process_group(bool init_group) {
    assert(init_group);
    detached = true;
}
void switch_cgroups() {
    cgroups_switched = true;
}
bool switch_mnt_ns(pid_t) {
    return true;
}
void log_i(const char*, ...) {}
void log_w(const char*, ...) {}
void log_e(const char*, ...) {}
}  // namespace ksud

extern "C" pid_t __real_fork();
extern "C" pid_t __wrap_fork() {
    if (fail_fork) {
        errno = EAGAIN;
        return -1;
    }
    return __real_fork();
}

extern "C" pid_t __real_waitpid(pid_t, int*, int);
extern "C" pid_t __wrap_waitpid(pid_t pid, int* status, int options) {
    if (fail_wait) {
        while (__real_waitpid(pid, status, 0) < 0 && errno == EINTR) {
        }
        errno = ECHILD;
        return -1;
    }
    return __real_waitpid(pid, status, options);
}

extern "C" int __wrap_execl(const char*, const char* argv0, ...) {
    assert(detached && cgroups_switched);
    sigset_t mask;
    assert(sigprocmask(SIG_SETMASK, nullptr, &mask) == 0);
    assert(sigismember(&mask, SIGCHLD) == 0);
    if (fail_exec) {
        errno = ENOENT;
        return -1;
    }
    va_list args;
    va_start(args, argv0);
    const char* script = va_arg(args, const char*);
    va_end(args);
    char* argv[] = {const_cast<char*>(argv0), const_cast<char*>(script), nullptr};
    return execv("/bin/sh", argv);
}

int main() {
    using namespace ksud;
    char directory[] = "/tmp/yukisu-soft-reboot-script-XXXXXX";
    assert(mkdtemp(directory));
    const auto root = std::filesystem::path(directory);
    const auto script = (root / "post-fs-data.sh").string();
    std::ofstream(script) << "[ \"$KSU_MODULE\" = hostsredirect ] || exit 20\n"
                             "[ \"$ZYGISK_ENABLED\" = 1 ] || exit 21\n"
                             "[ ! -d . ] && mkdir -p .\n";
    const auto execute = [&](ScriptWait wait, ScriptExitPolicy policy, std::string& error) {
        error.clear();
        return run_script(script, wait, "hostsredirect", nullptr, nullptr, false, policy, &error);
    };
    for (const auto wait :
         {ScriptWait::forever(),
          ScriptWait::until(std::chrono::steady_clock::now() + std::chrono::seconds(10))}) {
        std::string error;
        assert(execute(wait, ScriptExitPolicy::RequireSuccess, error) == 1);
        assert(error.find(script) != std::string::npos);
        assert(execute(wait, ScriptExitPolicy::ObserveExit, error) == 0 && error.empty());
        fail_wait = true;
        assert(execute(wait, ScriptExitPolicy::ObserveExit, error) == -1);
        assert(error.find("Cannot wait for " + script) != std::string::npos);
        fail_wait = false;
    }
    for (const auto wait : {ScriptWait::no_wait(), ScriptWait::forever(),
                            ScriptWait::until(std::chrono::steady_clock::now())}) {
        for (const auto policy :
             {ScriptExitPolicy::ObserveExit, ScriptExitPolicy::RequireSuccess}) {
            std::string error;
            fail_exec = true;
            assert(execute(wait, policy, error) == -1);
            assert(error.find("Cannot exec " + script) != std::string::npos);
            fail_exec = false;
            fail_fork = true;
            assert(execute(wait, policy, error) == -1);
            assert(error.find("Cannot fork for " + script) != std::string::npos);
            fail_fork = false;
        }
    }
    std::ofstream(script) << "kill -TERM $$\n";
    std::string error;
    assert(execute(ScriptWait::forever(), ScriptExitPolicy::RequireSuccess, error) == -1);
    assert(execute(ScriptWait::forever(), ScriptExitPolicy::ObserveExit, error) == 0);
    assert(error.empty());
    std::filesystem::remove_all(root);
    std::cout << "boot scripts: exit status, signal, fork/exec/wait errors, environment passed\n";
}
