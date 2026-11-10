#pragma once

#include <cstdint>
#include <string>

namespace ksud::magica {

int run(uint16_t port, const std::string& package_name, bool allow_shell = false);
int disable_adb_root();

}  // namespace ksud::magica
