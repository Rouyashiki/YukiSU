#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include "json.hpp"

namespace kagami::reboot {

inline constexpr const char* token_env = "KSU_SOFT_REBOOT_TOKEN";
JsonValue state();
bool record(const std::string& phase, const std::string& result, const std::string& error = {},
            int pending = 0);
bool acquire(std::string& error);
void release();
bool active();
bool authorized();
int claim_services(std::string& error);
bool check_support(std::string& error);
bool recover_abandoned(
    const std::function<bool(const std::string&, std::string&)>& recover_android);
void set_request_token(const std::string& token);
std::string client_token();
std::string cycle_id();
bool advance_cycle(std::string& error);
bool marker_matches_cycle(const std::filesystem::path& path);
bool write_cycle_marker(const std::filesystem::path& path, const std::string& detail = {});

}  // namespace kagami::reboot
