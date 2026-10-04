#pragma once

#include "core/json.hpp"

#include <cstdint>

namespace ksud {

json::Value yukizygisk_kernel_health(uint32_t capabilities);
json::Value yukizygisk_daemon_health();
int ensure_yukizygisk_daemons(uint32_t abi = 0);

}  // namespace ksud
