#pragma once

#include <filesystem>
#include <string>

namespace ghostfs {

inline constexpr const char* kEngineName = "ghostfs_engine";
inline constexpr const char* kEngineVersion = "0.1.0";
inline constexpr const char* kFileSystemFormat = "GFS1";

/// Resolves `relative_or_name` inside `workspace_root` and throws std::runtime_error
/// if the canonical path escapes `workspace_root`.
std::filesystem::path resolve_workspace_path(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& relative_or_name);

/// Processes a single JSON command payload and returns a structured JSON string.
std::string process_request_json(const std::string& request_json);

}  // namespace ghostfs
