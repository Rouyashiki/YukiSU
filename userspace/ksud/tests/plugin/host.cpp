#include "../../kagami/include/kagami/config.hpp"
#include "cli_args.hpp"
#include "defs.hpp"
#include "plugin/lua_engine.hpp"
#include "plugin/plugin.hpp"
#include "utils.hpp"

#include <sys/stat.h>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace kagami {
bool validate_config_patch([[maybe_unused]] const json::Value& patch, std::string& error) {
    error = "Kagami commands are unavailable in the plugin fixture";
    return false;
}
}  // namespace kagami

namespace ksud {
const char* const VERSION_CODE = "1";
const char* const VERSION_NAME = "plugin-test";

bool switch_mnt_ns([[maybe_unused]] pid_t pid) {
    return true;
}
void switch_cgroups() {}
void log_i([[maybe_unused]] const char* fmt, ...) {}
void log_w([[maybe_unused]] const char* fmt, ...) {}
bool lsetfilecon([[maybe_unused]] const std::filesystem::path& path,
                 [[maybe_unused]] const std::string& con) {
    return true;
}
std::optional<std::string> getprop(const std::string& prop) {
    return prop == "ro.build.version.sdk" ? "36" : "";
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

bool copy_file_data(const std::filesystem::path& source, const std::filesystem::path& target,
                    mode_t mode, bool overwrite) {
    std::error_code error;
    const auto options = overwrite ? std::filesystem::copy_options::overwrite_existing
                                   : std::filesystem::copy_options::none;
    return std::filesystem::copy_file(source, target, options, error) &&
           chmod(target.c_str(), mode) == 0;
}

bool write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.close();
    return !stream.fail();
}

}  // namespace ksud

#ifndef KSUD_PLUGIN_TEST_SUPPORT_ONLY
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "stage")
        return ksud::exec_plugin_stage(argv[2], true) ? 0 : 1;
    if (argc >= 2 && std::string(argv[1]) == "plugin") {
        ksud::CliArguments parsed;
        const int result = ksud::parse_cli(std::vector<std::string>(argv + 1, argv + argc), parsed);
        return result >= 0 ? result : ksud::plugin_handle(parsed.args);
    }
    return 2;
}
#endif
