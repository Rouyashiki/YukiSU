#pragma once

#include "zygisk_next_api.h"

#include <cstddef>

namespace yukizygisk::hyos {

using OpenControlSessionFn = int (*)();
using ReportCallbackFn = bool (*)(uint32_t module_index);

bool initialize(OpenControlSessionFn open_control_session,
                ReportCallbackFn report_callback, void (*recover_plt)());
bool available();
const ZygiskNextRuntime *runtime();
size_t registered_module_count();
bool install_registered_hooks();
bool original_output_allowed(void **output);
int child_control_session();
bool in_specialized_child();
void lock_child_control_session();
void unlock_child_control_session();
void invalidate_child_control_session();
void begin_module_registration(const uint32_t *module_index);
void end_module_registration();

} // namespace yukizygisk::hyos
