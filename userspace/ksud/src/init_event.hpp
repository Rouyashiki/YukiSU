#pragma once

#include <string>

namespace ksud {

int on_post_data_fs(std::string* error = nullptr);
int on_services(std::string* error = nullptr);
int on_boot_completed();
int soft_reboot();
int soft_reboot_status();
bool recover_soft_reboot(const std::string& phase, std::string& error);

}  // namespace ksud
