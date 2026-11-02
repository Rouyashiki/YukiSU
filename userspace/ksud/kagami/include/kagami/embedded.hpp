#pragma once
#include <string>
#include <vector>

namespace kagami {
int embedded_command(const std::vector<std::string>& args);
int embedded_mount();
void embedded_post_fs_data();
void embedded_mount_skipped(const std::string& reason);
void embedded_boot_completed();
bool embedded_verify_hide_rules(int& pending, std::string& error);
bool embedded_register_umount(const std::string& path);
bool embedded_unregister_umount(const std::string& path);
std::string embedded_external_mount_owner();
}  // namespace kagami
