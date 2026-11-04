#pragma once

#include "json.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ksud {

class PluginCodeSnapshot {
public:
    explicit PluginCodeSnapshot(int directory_fd);
    ~PluginCodeSnapshot();

    PluginCodeSnapshot(const PluginCodeSnapshot&) = delete;
    PluginCodeSnapshot& operator=(const PluginCodeSnapshot&) = delete;

    [[nodiscard]] const std::string& directory() const { return directory_; }

private:
    int directory_fd_;
    std::string directory_;
};

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string license;
    std::string entry;
    std::vector<std::string> depends;
    std::optional<std::vector<std::string>> callbacks;
    plugin_json::Value descriptions = plugin_json::Value::object();
    plugin_json::Value config = plugin_json::Value::array();
    plugin_json::Value quick_action;
    bool has_action = false;
    bool action_unknown = false;
    uint32_t min_version = 0;
};

struct PluginRecord {
    std::string id;
    std::string directory;
    std::shared_ptr<PluginCodeSnapshot> code_snapshot;
    PluginManifest manifest;
    bool enabled = true;
    bool manifest_valid = false;
    std::string error;
};

bool plugin_id_is_valid(const std::string& id);
bool plugin_callback_is_valid(const std::string& callback);
bool plugin_config_key_is_valid(const std::string& key);

std::optional<PluginManifest> plugin_read_manifest(const std::string& directory,
                                                   const std::string& expected_id,
                                                   std::string* error);
std::vector<PluginRecord> plugin_discover();
std::vector<PluginRecord> plugin_resolve_enabled(std::vector<std::string>* errors);
bool plugin_dependencies_available(const PluginManifest& manifest, std::string* error);

bool plugin_get_config_value(const std::string& id, const std::string& key, std::string* value,
                             std::string* error);
bool plugin_set_config_value(const std::string& id, const std::string& key,
                             const std::string& value, std::string* error);
bool plugin_delete_config_value(const std::string& id, const std::string& key, std::string* error);

void plugin_append_log(const std::string& plugin_dir, const char* level,
                       const std::string& message);

int plugin_handle(const std::vector<std::string>& args);

}  // namespace ksud
