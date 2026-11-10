#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ksud::kernelsu_loader {

bool load_module(const char* path, const std::string& param_values = "",
                 bool kernel_lookup = false);
bool load_module(std::vector<uint8_t> buffer, const std::string& param_values = "",
                 bool kernel_lookup = false);

}  // namespace ksud::kernelsu_loader
