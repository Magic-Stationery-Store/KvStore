#include "kv_store.h"
#include "test_files.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
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

std::size_t count_files(
    const std::string& database,
    const std::string& suffix
) {
    const auto base =
        std::filesystem::absolute(database).lexically_normal();
    std::size_t count = 0;
    for (const auto& item :
         std::filesystem::directory_iterator(base.parent_path())) {
        const std::string name = item.path().filename().string();
        const bool is_sstable =
            name.size() > 4
            && name.compare(name.size() - 4, 4, ".sst") == 0;
        if (is_sstable && name.find(suffix) != std::string::npos) {
            ++count;
        }
    }
    return count;
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
    const std::string path = "leveled-compaction-test.log";
    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    {
        KVStore store(path, 2);
        store.set_level0_compaction_trigger(0);

        store.put("k", "v1");
        std::optional<KVStore::Snapshot> old = store.snapshot();
        store.put("k", "v2");
        store.flush();

        // Four more flushes create enough L0 tables to compact.
        for (int batch = 0; batch < 4; ++batch) {
            store.put("key-" + std::to_string(batch) + "-a", "a");
            store.put("key-" + std::to_string(batch) + "-b", "b");
            store.flush();
        }

        expect(count_tables(store, "L0") >= 4,
               "test setup creates multiple L0 tables");
        expect(count_tables(store, "L1") == 0,
               "test starts without L1 tables");

        store.compact_levels();

        expect(count_tables(store, "L0") == 0,
               "L0 compaction consumes all L0 tables");
        expect(count_tables(store, "L1") == 1,
               "L0 compaction creates one L1 table");
        expect(store.get("k") == "v2",
               "latest value survives L0 to L1 compaction");
        expect(store.get("k", *old) == "v1",
               "older snapshot survives L0 to L1 compaction");
        expect(store.get("key-0-a") == "a"
                   && store.get("key-3-b") == "b",
               "keys from all L0 tables survive L1 compaction");

        const auto manifest = store.manifest_snapshot();
        expect(manifest.level0.empty() && manifest.level1.size() == 1,
               "MANIFEST commits the new L1 and clears L0");

        expect(count_files(path, ".L0.") == 0,
               "old L0 files are reclaimed");
        expect(count_files(path, ".L1.") == 1,
               "the new L1 file exists");
    }

    {
        KVStore reopened(path, 2);
        reopened.set_level0_compaction_trigger(0);
        expect(reopened.get("k") == "v2"
                   && reopened.get("key-2-a") == "a",
               "L1 table is loaded after reopen");
        expect(count_tables(reopened, "L1") == 1,
               "reopen keeps one L1 table");

        reopened.put("k", "v3");
        reopened.put("new", "value");
        reopened.flush();
        expect(count_tables(reopened, "L0") == 1,
               "new writes create a new L0 table above L1");

        reopened.compact_levels();
        expect(reopened.get("k") == "v3"
                   && reopened.get("new") == "value",
               "second L0 to L1 compaction preserves new values");
        expect(count_tables(reopened, "L1") == 1,
               "second compaction still keeps one L1 table");

        reopened.remove("k");
        reopened.flush();
        reopened.compact_levels();
        expect(!reopened.get("k").has_value(),
               "tombstone survives L0 to L1 compaction");
        expect(reopened.get("new") == "value",
               "unrelated key survives tombstone compaction");

        reopened.compact();
        expect(count_tables(reopened, "L1") == 0,
               "full compaction consumes L1");
        expect(!reopened.get("k").has_value()
                   && reopened.get("new") == "value",
               "full compaction preserves logical values");
    }

    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    if (failures != 0) {
        std::cerr << failures << " leveled compaction checks failed\n";
        return 1;
    }
    std::cout << "PASS: L0 to L1 leveled compaction\n";
    return 0;
}
