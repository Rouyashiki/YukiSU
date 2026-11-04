#pragma once

#include <cstdint>
#include <string>

namespace ksud {

enum class PluginRunResult {
    Success,
    MissingCallback,
    Failed,
};

PluginRunResult run_plugin_callback_isolated(const std::string& plugin_id,
                                             const std::string& callback);
bool exec_plugin_stage(const std::string& stage, bool block);
bool stop_plugin_daemons(const std::string& plugin_id, std::string* error);
bool launch_plugin_daemon(const std::string& plugin_id, const std::string& callback,
                          int64_t interval_seconds);
bool start_plugin_daemon(const std::string& plugin_id, const std::string& callback,
                         int64_t interval_seconds, int ready_fd);

}  // namespace ksud
