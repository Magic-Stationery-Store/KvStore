#include "kv_store.h"
#include "test_files.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

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
    return "key-" + std::string(5 - digits.size(), '0') + digits;
}

std::size_t count_tables(const KVStore& store, const std::string& kind) {
    std::size_t count = 0;
    for (const auto& table : store.inspect_tables()) {
        if (table.kind == kind) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main() {
    const std::string auto_path = "level-auto-test.log";
    reset_test_database(auto_path);
    std::remove(auto_path.c_str());
    std::remove((auto_path + ".wal").c_str());

    {
        KVStore store(auto_path, 2);
        store.set_level0_compaction_trigger(3);

        for (int batch = 0; batch < 3; ++batch) {
            store.put("auto-" + std::to_string(batch) + "-a", "a");
            store.put("auto-" + std::to_string(batch) + "-b", "b");
            store.flush();
        }

        expect(count_tables(store, "L0") == 0,
               "automatic trigger consumes L0");
        expect(count_tables(store, "L1") >= 1,
               "automatic trigger creates L1");
        expect(store.get("auto-0-a") == "a"
                   && store.get("auto-2-b") == "b",
               "automatic L1 keeps all values");
    }

    reset_test_database(auto_path);
    std::remove(auto_path.c_str());
    std::remove((auto_path + ".wal").c_str());

    const std::string split_path = "level-split-test.log";
    reset_test_database(split_path);
    std::remove(split_path.c_str());
    std::remove((split_path + ".wal").c_str());

    {
        KVStore store(split_path, 10000);
        store.set_level0_compaction_trigger(0);

        const std::string value(1024, 'v');
        for (std::size_t i = 0; i < 6000; ++i) {
            store.put(key(i), value);
        }
        store.compact_levels();

        expect(count_tables(store, "L0") == 0,
               "split test consumes L0");
        expect(count_tables(store, "L1") >= 2,
               "large L1 is split into multiple tables");
        expect(store.get(key(0)) == value
                   && store.get(key(3000)) == value
                   && store.get(key(5999)) == value,
               "split L1 tables return values from every range");

        const auto manifest = store.manifest_snapshot();
        expect(manifest.level1.size() >= 2,
               "MANIFEST records multiple L1 tables");
    }

    {
        KVStore reopened(split_path, 10000);
        expect(count_tables(reopened, "L1") >= 2,
               "reopen loads all split L1 tables");
        expect(reopened.get(key(0)) == std::string(1024, 'v')
                   && reopened.get(key(5999)) == std::string(1024, 'v'),
               "values survive reopening split L1 tables");
    }

    reset_test_database(split_path);
    std::remove(split_path.c_str());
    std::remove((split_path + ".wal").c_str());

    if (failures != 0) {
        std::cerr << failures << " level split checks failed\n";
        return 1;
    }
    std::cout << "PASS: automatic trigger and split L1\n";
    return 0;
}
