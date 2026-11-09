#pragma once

#include <filesystem>
#include <string>

namespace kagami {

std::filesystem::path runtime_data_dir();
std::filesystem::path runtime_modules_dir();
std::filesystem::path runtime_config_file();
std::filesystem::path runtime_log_file();
std::filesystem::path runtime_socket_file();
std::filesystem::path runtime_pid_file();
std::filesystem::path runtime_daemon_lock_file();
std::string runtime_boot_id();
bool marker_matches_current_boot(const std::filesystem::path& path);
bool write_boot_marker(const std::filesystem::path& path, const std::string& detail = {});
bool prepare_private_directory(const std::filesystem::path& path, std::string& error);
bool prepare_private_file(const std::filesystem::path& path, std::string& error);
bool prepare_loop_image(const std::filesystem::path& path, std::string& error);
bool prepare_runtime(std::string& error);

}  // namespace kagami
