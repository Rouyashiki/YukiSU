#pragma once

#include <sys/types.h>
#include <string>
#include <utility>
#include <vector>

namespace ksud {

class SoftRebootWaiter {
public:
    SoftRebootWaiter() = default;
    ~SoftRebootWaiter();
    SoftRebootWaiter(const SoftRebootWaiter&) = delete;
    SoftRebootWaiter& operator=(const SoftRebootWaiter&) = delete;
    SoftRebootWaiter(SoftRebootWaiter&&) = delete;
    SoftRebootWaiter& operator=(SoftRebootWaiter&&) = delete;

    bool prepare();
    bool wait();

private:
    pid_t server_pid_ = -1;
    int server_pidfd_ = -1;
    std::vector<std::string> services_;
    std::vector<std::pair<pid_t, int>> zygotes_;
};

}  // namespace ksud
