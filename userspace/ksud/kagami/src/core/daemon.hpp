#pragma once

#include <string>
#include <vector>

namespace kagami {

int run_daemon_command(const std::vector<std::string>& args);
int run_via_daemon(const std::vector<std::string>& args, bool display_errors = false);
bool last_daemon_request_completed();
bool daemon_supports_soft_reboot(std::string& error);

}  // namespace kagami
