#pragma once

#include "uapi/yukizygisk.h"

namespace ksud {

bool prepare_yukizygisk_diagnostics(bool create_if_missing);
bool prepare_yukizygisk_recovery_diagnostics();
void update_yukizygisk_boot_diagnostics(bool safe_mode, bool feature_supported,
                                        bool feature_enabled, const char* phase);
void record_yukizygisk_early_linker(const yz_early_native_snapshot_header& header);

}  // namespace ksud
