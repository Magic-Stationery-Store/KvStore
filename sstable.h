#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct SSTableLookupResult {
    // found=false: this table has no such key.
    // found=true, value=nullopt: this table contains a tombstone.
    // found=true, value has data: this table contains a value.
    bool found = false;
    std::optional<std::string> value;
};

struct SSTableSeekResult {
    // found=false: no key >= target exists in this table.
    bool found = false;
    std::string key;
    std::optional<std::string> value;
};

class BlockCache {
public:
    using Block =
        std::map<std::string, std::optional<std::string>>;

    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::size_t entries = 0;
        std::size_t bytes = 0;
    };

    explicit BlockCache(
        std::size_t capacity_bytes = 8 * 1024 * 1024);

    std::shared_ptr<const Block> get(const std::string& id);

    void put(
        const std::string& id,
        std::shared_ptr<const Block> block,
        std::size_t bytes);

    Stats stats() const;

private:
    struct Entry {
        std::shared_ptr<const Block> block;
        std::size_t bytes = 0;
        std::list<std::string>::iterator position;
    };

    mutable std::mutex mutex_;
    std::size_t capacity_bytes_ = 0;
    std::size_t current_bytes_ = 0;
    std::list<std::string> lru_;
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t evictions_ = 0;
};

class SSTable {
public:
    using Entries =
        std::map<std::string, std::optional<std::string>>;

    struct EntryRef {
        std::string key;
        std::uint64_t value_offset = 0;
        std::uint32_t value_size = 0;
        bool tombstone = false;
    };

    // Build a new immutable table. Existing tables are never overwritten.
    static void build(
        const std::string& path,
        const Entries& entries
    );

    explicit SSTable(
        std::string path,
        std::shared_ptr<BlockCache> cache = nullptr);

    SSTableLookupResult get(const std::string& key) const;

    // First entry with internal key >= target. Used by MVCC snapshot reads.
    SSTableSeekResult seek(const std::string& key) const;

    Entries read_all() const;

    // Metadata-only snapshot for a streaming iterator (values are not copied).
    std::vector<EntryRef> index_snapshot() const;

    const std::string& path() const noexcept;

    std::size_t entry_count() const noexcept;

    // Bloom filter on user keys (safe negative: false means no version exists).
    bool might_contain_user(const std::string& user_key) const;

    // Inclusive user-key range. Used to select overlapping tables.
    std::pair<std::string, std::string> user_key_range() const;

private:
    struct IndexEntry {
        std::string key;
        bool tombstone;
        std::uint64_t value_offset;
        std::uint32_t value_size;
        std::uint64_t record_offset;
    };

    struct BlockRef {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::size_t first_index = 0;
        std::size_t last_index = 0;
    };

    std::shared_ptr<const BlockCache::Block> load_block(
        std::size_t block_index) const;

    void load_footer();
    void build_block_index();

    void bloom_add(const std::string& key);
    bool bloom_maybe(const std::string& key) const;

    std::string path_;
    std::vector<IndexEntry> index_;
    std::vector<BlockRef> blocks_;
    std::vector<std::size_t> block_for_index_;
    std::uint64_t data_end_ = 0;
    std::shared_ptr<BlockCache> cache_;
    std::vector<std::uint64_t> bloom_bits_;
    std::uint32_t bloom_hash_count_ = 3;
};
