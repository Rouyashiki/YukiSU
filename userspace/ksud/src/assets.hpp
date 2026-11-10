#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ksud {

// List all embedded asset names
const std::vector<std::string>& list_assets();

// Returns the compressed storage, not an executable image.
bool get_asset(const std::string& name, const uint8_t*& data, size_t& size);
std::optional<std::vector<uint8_t>> get_asset_data(const std::string& name);

// Copy asset to file
bool copy_asset_to_file(const std::string& name, const std::string& dest_path);

// List supported KMI versions (extracted from embedded LKM names)
std::vector<std::string> list_supported_kmi();

// Ensure binary assets are extracted
int ensure_binaries(bool ignore_if_exist);

// Stage embedded YukiZygisk payloads.
int ensure_yukizygisk(bool ignore_if_exist);

}  // namespace ksud
