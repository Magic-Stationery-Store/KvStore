#include "kv_store.h"
#include "sstable.h"
#include "test_files.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "PASS: " << message << '\n';
    } else {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string key(std::size_t index) {
    std::string digits = std::to_string(index);
    return "key-" + std::string(3 - digits.size(), '0') + digits;
}

} // namespace

int main() {
    const std::string table_path = "block-cache-table.sst";
    std::remove(table_path.c_str());

    auto cache = std::make_shared<BlockCache>(4 * 1024 * 1024);
    SSTable::Entries entries;
    for (std::size_t i = 0; i < 200; ++i) {
        entries.emplace(key(i), std::string(900, static_cast<char>('a' + i % 26)));
    }
    SSTable::build(table_path, entries);

    {
        SSTable table(table_path, cache);
        expect(table.might_contain_user("key-000"),
               "Bloom filter contains an existing key");
        expect(table.get("key-000").value
                   == std::optional<std::string>(std::string(900, 'a')),
               "block read returns the first value");

        bool all_match = true;
        for (std::size_t i = 0; i < 200; ++i) {
            const auto value = table.get(key(i));
            if (!value.value.has_value()
                || *value.value != std::string(900, static_cast<char>('a' + i % 26))) {
                all_match = false;
                break;
            }
        }
        expect(all_match, "block reads return every value");

        const auto seek = table.seek("key-100");
        expect(seek.found && seek.key == "key-100",
               "block seek lands on the requested key");
    }

    const auto stats = cache->stats();
    expect(stats.misses > 0, "block cache records misses");
    expect(stats.entries > 0 && stats.bytes > 0,
           "block cache stores parsed blocks");

    {
        BlockCache tiny(10);
        auto first = std::make_shared<BlockCache::Block>();
        (*first)["a"] = "1";
        auto second = std::make_shared<BlockCache::Block>();
        (*second)["b"] = "2";
        tiny.put("a", first, 10);
        tiny.put("b", second, 10);
        const auto tiny_stats = tiny.stats();
        expect(tiny_stats.evictions > 0,
               "block cache evicts LRU entries when full");
        expect(tiny_stats.entries == 1,
               "block cache keeps only the newest entry after eviction");
    }

    std::remove(table_path.c_str());

    const std::string db_path = "block-cache-db.log";
    reset_test_database(db_path);
    std::remove(db_path.c_str());
    std::remove((db_path + ".wal").c_str());

    {
        KVStore store(db_path, 3);
        store.set_level0_compaction_trigger(0);
        for (std::size_t i = 0; i < 30; ++i) {
            store.put(key(i), "value-" + key(i));
        }
        store.flush();

        expect(store.get(key(0)) == "value-key-000",
               "KVStore read goes through the block path");
        expect(store.get(key(10)) == "value-key-010",
               "KVStore read hits a later block");
        expect(store.get(key(0)) == "value-key-000",
               "repeated KVStore read reuses the block");

        const auto db_stats = store.block_cache_stats();
        expect(db_stats.misses > 0 && db_stats.hits > 0
                   && db_stats.entries > 0,
               "KVStore exposes block cache hit/miss stats");
    }

    reset_test_database(db_path);
    std::remove(db_path.c_str());
    std::remove((db_path + ".wal").c_str());

    if (failures != 0) {
        std::cerr << failures << " block cache checks failed\n";
        return 1;
    }
    std::cout << "PASS: block SSTable, Bloom filter and block cache\n";
    return 0;
}
