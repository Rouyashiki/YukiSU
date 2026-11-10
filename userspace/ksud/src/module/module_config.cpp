#include "module_config.hpp"
#include "../cli_args.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../terminal.hpp"
#include "../utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace ksud {
namespace module_config {
namespace {
constexpr uint32_t MAGIC = 0x4b53554d;
constexpr uint32_t VERSION = 1;
constexpr size_t MAX_FILE_LEN = 12 + (MAX_COUNT * (8 + MAX_KEY_LEN + MAX_VALUE_LEN));

std::string config_path(const std::string& id, Type type) {
    return std::string(MODULE_CONFIG_DIR) + id + "/" +
           (type == Type::Temp ? TEMP_CONFIG_NAME : PERSIST_CONFIG_NAME);
}

bool valid_utf8(std::string_view value) {
    size_t i = 0;
    while (i < value.size()) {
        const auto lead = static_cast<unsigned char>(value[i++]);
        if (lead < 0x80)
            continue;
        unsigned count;
        uint32_t code;
        uint32_t minimum;
        if (lead >= 0xc2 && lead <= 0xdf) {
            count = 1;
            code = lead & 0x1f;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            count = 2;
            code = lead & 0x0f;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            count = 3;
            code = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (value.size() - i < count)
            return false;
        while (count-- != 0) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}

bool read_u32(std::string_view& input, uint32_t& value) {
    if (input.size() < 4)
        return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << (i * 8);
    input.remove_prefix(4);
    return true;
}

void append_u32(std::string& output, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        output.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

std::optional<Config> decode(std::string_view input) {
    Config config;
    std::string_view rest = input;
    uint32_t magic = 0;
    if (read_u32(rest, magic) && magic == MAGIC) {
        uint32_t version = 0;
        uint32_t count = 0;
        if (!read_u32(rest, version) || version != VERSION || !read_u32(rest, count) ||
            count > MAX_COUNT)
            return std::nullopt;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t length = 0;
            if (!read_u32(rest, length) || length > MAX_KEY_LEN || length > rest.size())
                return std::nullopt;
            std::string key(rest.substr(0, length));
            rest.remove_prefix(length);
            if (!read_u32(rest, length) || length > MAX_VALUE_LEN || length > rest.size())
                return std::nullopt;
            std::string value(rest.substr(0, length));
            rest.remove_prefix(length);
            if (!valid_utf8(key) || !valid_utf8(value))
                return std::nullopt;
            config[std::move(key)] = std::move(value);
        }
        return config;
    }

    // Older C++ releases wrote unescaped key=value lines at these same paths.
    if (input.find('\0') != std::string_view::npos || !valid_utf8(input))
        return std::nullopt;
    bool valid = true;
    for_each_line(input, [&](std::string_view line) {
        if (line.empty())
            return true;
        const size_t eq = line.find('=');
        if (eq > MAX_KEY_LEN || line.size() - eq - 1 > MAX_VALUE_LEN) {
            valid = false;
            return false;
        }
        config[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
        valid = config.size() <= MAX_COUNT;
        return valid;
    });
    return valid ? std::optional(std::move(config)) : std::nullopt;
}
}  // namespace

bool valid_id(std::string_view id) {
    const auto alpha = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    };
    return id.size() >= 2 && alpha(id.front()) &&
           std::all_of(id.begin(), id.end(), [&](unsigned char ch) {
               return alpha(ch) || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
           });
}

bool valid_key(std::string_view key) {
    return key.size() <= MAX_KEY_LEN && valid_id(key);
}

std::optional<Config> load(const std::string& module_id, Type type) {
    if (!valid_id(module_id)) {
        terminal::error("Invalid module configuration ID");
        return std::nullopt;
    }
    const std::string path = config_path(module_id, type);
    const std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "re"), fclose);
    if (!file) {
        if (errno == ENOENT)
            return Config{};
        terminal::file_error("cannot read configuration", path, errno);
        return std::nullopt;
    }
    std::string content;
    std::array<char, 8192> buffer{};
    for (;;) {
        const size_t count = fread(buffer.data(), 1, buffer.size(), file.get());
        if (count > MAX_FILE_LEN - content.size()) {
            terminal::error("Module configuration exceeds size limit");
            return std::nullopt;
        }
        content.append(buffer.data(), count);
        if (count < buffer.size()) {
            if (ferror(file.get())) {
                terminal::file_error("cannot read configuration", path, errno);
                return std::nullopt;
            }
            break;
        }
    }
    auto config = decode(content);
    if (!config)
        terminal::error("Invalid module configuration: " + path);
    return config;
}

bool save(const std::string& module_id, Type type, const Config& config) {
    if (!valid_id(module_id) || config.size() > MAX_COUNT) {
        terminal::error("Invalid module ID or too many configuration entries (maximum 32)");
        return false;
    }
    std::string output;
    append_u32(output, MAGIC);
    append_u32(output, VERSION);
    append_u32(output, static_cast<uint32_t>(config.size()));
    for (const auto& [key, value] : config) {
        if (!valid_key(key) || value.size() > MAX_VALUE_LEN || !valid_utf8(value)) {
            terminal::error(
                "Invalid configuration key or value (maximum 256-byte key, 1 MiB UTF-8 value)");
            return false;
        }
        append_u32(output, static_cast<uint32_t>(key.size()));
        output += key;
        append_u32(output, static_cast<uint32_t>(value.size()));
        output += value;
    }
    const std::string dir = std::string(MODULE_CONFIG_DIR) + module_id;
    const std::string path = config_path(module_id, type);
    if (!ensure_dir_exists(dir) || !write_file_atomic(path, output)) {
        terminal::file_error("cannot save configuration", path, errno);
        return false;
    }
    return true;
}

Config merge(const std::string& module_id) {
    auto config = load(module_id, Type::Persist).value_or(Config{});
    if (auto temp = load(module_id, Type::Temp)) {
        for (auto& [key, value] : *temp)
            config[key] = std::move(value);
    }
    return config;
}
}  // namespace module_config

int module_config_handle(const CliArguments& cli) {
    using namespace module_config;
    const auto& args = cli.args;
    if (args.size() < 2)
        return terminal::error("A module config subcommand is required");
    const char* environment_id = getenv("KSU_MODULE");
    std::string module_id = environment_id ? environment_id : "";
    if (cli.has("--internal"))
        module_id = "internal." + cli.value("--internal");
    if (!valid_id(module_id))
        return terminal::error("Invalid or missing KSU_MODULE",
                               "Run in a module context or pass --internal <name>.");
    const auto type = cli.has("--temp") ? Type::Temp : Type::Persist;
    const std::string& command = args[1];
    if (command == "get" || command == "list") {
        const auto config = merge(module_id);
        if (command == "get") {
            const auto found = config.find(args[2]);
            if (found == config.end())
                return terminal::error("Key '" + args[2] + "' not found");
            if (fwrite(found->second.data(), 1, found->second.size(), stdout) !=
                found->second.size())
                return 1;
            putchar('\n');
        } else if (config.empty()) {
            puts("No config entries found");
        } else {
            for (const auto& [key, value] : config) {
                if (fwrite(key.data(), 1, key.size(), stdout) != key.size())
                    return 1;
                putchar('=');
                if (fwrite(value.data(), 1, value.size(), stdout) != value.size())
                    return 1;
                putchar('\n');
            }
        }
        return ferror(stdout) ? 1 : 0;
    }
    if (command == "clear") {
        const std::string path = config_path(module_id, type);
        if (unlink(path.c_str()) != 0 && errno != ENOENT)
            return terminal::file_error("cannot clear configuration", path, errno);
        return 0;
    }
    if (command != "set" && command != "delete")
        return terminal::error("Unknown module config command: " + command);
    auto config = load(module_id, type);
    if (!config)
        return 1;
    const std::string& key = args[2];
    if (command == "delete") {
        if (config->erase(key) == 0)
            return terminal::error("Key '" + key + "' not found in config");
    } else {
        if (!valid_key(key))
            return terminal::error("Invalid configuration key");
        std::string value;
        if (args.size() > 3 && !cli.has("--stdin")) {
            value = args[3];
        } else {
            std::array<char, 8192> buffer{};
            for (;;) {
                const size_t count = fread(buffer.data(), 1, buffer.size(), stdin);
                if (count > MAX_VALUE_LEN - value.size())
                    return terminal::error("Configuration value exceeds 1 MiB");
                value.append(buffer.data(), count);
                if (count < buffer.size()) {
                    if (ferror(stdin))
                        return terminal::error("Failed to read configuration value from stdin");
                    break;
                }
            }
        }
        (*config)[key] = std::move(value);
    }
    return save(module_id, type, *config) ? 0 : 1;
}

void clear_all_temp_configs() {
    DIR* dir = opendir(MODULE_CONFIG_DIR);
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        const std::string config_dir = std::string(MODULE_CONFIG_DIR) + entry->d_name;
        struct stat status{};
        if (stat(config_dir.c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
            continue;

        const std::string temp_config = config_dir + "/" + TEMP_CONFIG_NAME;
        if (access(temp_config.c_str(), F_OK) == 0) {
            unlink(temp_config.c_str());
        }
    }

    closedir(dir);
}

}  // namespace ksud
