#pragma once

#include "manifest.h"
#include <filesystem>
#include <string>

// Only call before opening a test DB or after all its handles are destroyed.
inline void reset_test_database(const std::string& path) {
    const auto base = std::filesystem::absolute(path).lexically_normal();
    const std::string name = base.filename().string();
    for (const auto& item : std::filesystem::directory_iterator(base.parent_path())) {
        const std::string file = item.path().filename().string();
        const bool sidecar = file == name + ".wal" || file == name + ".lock"
            || file == name + ".manifest" || file == name + ".manifest.tmp"
            || file == name + ".compact.tmp" || file == name + ".wal.compact.tmp"
            || file.compare(0, name.size() + 4, name + ".L0.") == 0
            || file.compare(0, name.size() + 6, name + ".base.") == 0
            || file.compare(0, name.size() + 8, name + ".wal.L0.") == 0;
        if (file == name || sidecar) { std::filesystem::remove(item.path()); }
    }
}

inline std::string current_test_base(const std::string& path) {
    return base_file_path(path, read_manifest(path + ".manifest").base_generation);
}

inline std::uintmax_t test_data_size(const std::string& path) {
    const auto state = read_manifest(path + ".manifest");
    std::uintmax_t size = std::filesystem::file_size(base_file_path(path, state.base_generation))
        + std::filesystem::file_size(path + ".wal");
    const auto database = std::filesystem::absolute(path);
    for (const auto& item : std::filesystem::directory_iterator(database.parent_path())) {
        const auto name = item.path().filename().string();
        const auto prefix = database.filename().string() + ".L0.";
        if (name.compare(0, prefix.size(), prefix) == 0 && item.path().extension() == ".sst") {
            size += item.file_size();
        }
    }
    return size;
}
