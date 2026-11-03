#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct SSTableLookupResult {
    // found=false: this table has no such key.
    // found=true, value=nullopt: this table contains a tombstone.
    // found=true, value has data: this table contains a value.
    bool found = false;
    std::optional<std::string> value;
};

class SSTable {
public:
    using Entries =
        std::map<std::string, std::optional<std::string>>;

    // Build a new immutable table. Existing tables are never overwritten.
    static void build(
        const std::string& path,
        const Entries& entries
    );

    explicit SSTable(std::string path);

    SSTableLookupResult get(const std::string& key) const;

    Entries read_all() const;

    std::size_t entry_count() const noexcept;

private:
    struct IndexEntry {
        std::string key;
        bool tombstone;
        std::uint64_t value_offset;
        std::uint32_t value_size;
    };

    std::string path_;
    std::vector<IndexEntry> index_;
};
