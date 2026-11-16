#include "kv_store.h"
#include "test_files.h"

#include <algorithm>
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

std::vector<std::uint64_t> table_ids(
    const KVStore& store,
    const std::string& kind
) {
    std::vector<std::uint64_t> ids;
    for (const auto& table : store.inspect_tables()) {
        if (table.kind == kind) {
            ids.push_back(table.generation);
        }
    }
    return ids;
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

    const std::string auto_l2_path = "level-auto-l2-test.log";
    reset_test_database(auto_l2_path);
    std::remove(auto_l2_path.c_str());
    std::remove((auto_l2_path + ".wal").c_str());
    {
        KVStore store(auto_l2_path, 2);
        store.set_level0_compaction_trigger(2);
        store.set_level1_to_level2_trigger_bytes(1);
        store.set_level2_to_level3_trigger_bytes(0);

        for (int batch = 0; batch < 2; ++batch) {
            store.put("auto-l2-" + std::to_string(batch) + "-a", "a");
            store.put("auto-l2-" + std::to_string(batch) + "-b", "b");
            store.flush();
        }

        expect(count_tables(store, "L0") == 0
                   && count_tables(store, "L1") == 0,
               "automatic L0->L1->L2 clears upper levels");
        expect(count_tables(store, "L2") >= 1,
               "automatic L1->L2 creates L2");
        expect(store.get("auto-l2-0-a") == "a"
                   && store.get("auto-l2-1-b") == "b",
               "automatic L2 keeps all values");
    }
    reset_test_database(auto_l2_path);
    std::remove(auto_l2_path.c_str());
    std::remove((auto_l2_path + ".wal").c_str());

    const std::string auto_l3_path = "level-auto-l3-test.log";
    reset_test_database(auto_l3_path);
    std::remove(auto_l3_path.c_str());
    std::remove((auto_l3_path + ".wal").c_str());
    {
        KVStore store(auto_l3_path, 2);
        store.set_level0_compaction_trigger(2);
        store.set_level1_to_level2_trigger_bytes(1);
        store.set_level2_to_level3_trigger_bytes(1);

        for (int batch = 0; batch < 2; ++batch) {
            store.put("auto-l3-" + std::to_string(batch) + "-a", "a");
            store.put("auto-l3-" + std::to_string(batch) + "-b", "b");
            store.flush();
        }

        expect(count_tables(store, "L0") == 0
                   && count_tables(store, "L1") == 0
                   && count_tables(store, "L2") == 0,
               "automatic L0->L1->L2->L3 clears upper levels");
        expect(count_tables(store, "L3") >= 1,
               "automatic L2->L3 creates L3");
        expect(store.get("auto-l3-0-a") == "a"
                   && store.get("auto-l3-1-b") == "b",
               "automatic L3 keeps all values");
    }
    reset_test_database(auto_l3_path);
    std::remove(auto_l3_path.c_str());
    std::remove((auto_l3_path + ".wal").c_str());

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

        const auto before = table_ids(store, "L1");
        expect(before.size() >= 2, "capture split L1 generations");
        const std::uint64_t first = before.front();
        const std::uint64_t second = before.back();

        // Only the L1 table overlapping the new L0 range should be rebuilt.
        store.put(key(0), "partial");
        store.flush();
        store.compact_levels();
        const auto after = table_ids(store, "L1");
        expect(std::find(after.begin(), after.end(), first) == after.end(),
               "overlapping L1 table is replaced");
        expect(std::find(after.begin(), after.end(), second) != after.end(),
               "non-overlapping L1 table is retained");
        expect(store.get(key(0)) == "partial"
                   && store.get(key(5999)) == value,
               "partial L1 compaction preserves both ranges");

        store.compact_l1_to_l2();
        expect(count_tables(store, "L1") == 0,
               "L1 to L2 compaction clears L1");
        expect(count_tables(store, "L2") >= 2,
               "L1 to L2 compaction splits L2");
        expect(store.get(key(0)) == "partial"
                   && store.get(key(5999)) == value,
               "values survive L1 to L2 compaction");

        const auto second_manifest = store.manifest_snapshot();
        expect(second_manifest.level1.empty()
                   && second_manifest.level2.size() >= 2,
               "MANIFEST records L2 and clears L1");

        store.compact_l2_to_l3();
        expect(count_tables(store, "L2") == 0,
               "L2 to L3 compaction clears L2");
        expect(count_tables(store, "L3") >= 2,
               "L2 to L3 compaction splits L3");
        expect(store.get(key(0)) == "partial"
                   && store.get(key(5999)) == value,
               "values survive L2 to L3 compaction");

        const auto third_manifest = store.manifest_snapshot();
        expect(third_manifest.level2.empty()
                   && third_manifest.level3.size() >= 2,
               "MANIFEST records L3 and clears L2");
    }

    {
        KVStore reopened(split_path, 10000);
        expect(count_tables(reopened, "L3") >= 2,
               "reopen loads all split L3 tables");
        expect(reopened.get(key(0)) == "partial"
                   && reopened.get(key(5999)) == std::string(1024, 'v'),
               "values survive reopening split L3 tables");

        reopened.compact();
        expect(count_tables(reopened, "L3") == 0,
               "full compaction consumes L3");
        expect(reopened.get(key(0)) == "partial"
                   && reopened.get(key(5999)) == std::string(1024, 'v'),
               "full compaction preserves L3 values");
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
