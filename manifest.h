#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A complete metadata snapshot; paths are derived from the database name and IDs.
struct ManifestState {
    std::uint64_t next_generation = 1;
    std::uint64_t base_generation = 0; // 0 selects the legacy base log.
    std::uint64_t retired_through = 0;
    std::vector<std::uint64_t> level0;
};

ManifestState read_manifest(const std::string& path);
void write_manifest(const std::string& path, const ManifestState& state);

std::string base_file_path(const std::string& database, std::uint64_t generation);
