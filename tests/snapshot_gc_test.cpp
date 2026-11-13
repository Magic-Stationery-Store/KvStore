#include "kv_store.h"
#include "test_files.h"

#include <cstdio>
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

} // namespace

int main() {
    const std::string path = "snapshot-gc-test.log";
    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    {
        KVStore store(path, 64);

        store.put("k", "v1");
        std::optional<KVStore::Snapshot> s1 = store.snapshot();
        store.put("k", "v2");
        std::optional<KVStore::Snapshot> s2 = store.snapshot();
        store.put("k", "v3");
        std::optional<KVStore::Snapshot> s3 = store.snapshot();

        s1.reset();
        store.compact();

        expect(store.get("k", *s2) == "v2",
               "snapshot 2 survives snapshot-aware compaction");
        expect(store.get("k", *s3) == "v3",
               "snapshot 3 survives snapshot-aware compaction");
        expect(store.get("k") == "v3",
               "latest value survives snapshot-aware compaction");

        const auto tables = store.inspect_tables();
        expect(!tables.empty() && tables.back().kind == "BASE"
                   && tables.back().entries == 2,
               "GC keeps versions >= oldest snapshot and one fallback");

        // t: v1 -> tombstone -> v3; oldest live snapshot sees the tombstone.
        store.put("t", "t1");
        std::optional<KVStore::Snapshot> t1 = store.snapshot();
        store.remove("t");
        std::optional<KVStore::Snapshot> t2 = store.snapshot();
        store.put("t", "t3");
        std::optional<KVStore::Snapshot> t3 = store.snapshot();

        t1.reset();
        store.compact();

        expect(!store.get("t", *t2).has_value(),
               "old snapshot still sees the tombstone");
        expect(store.get("t", *t3) == "t3",
               "newer snapshot sees the value after the tombstone");
        expect(store.get("t") == "t3",
               "latest value after tombstone stays visible");

        s2.reset();
        s3.reset();
        t2.reset();
        t3.reset();
        store.compact();

        expect(store.get("k") == "v3" && store.get("t") == "t3",
               "snapshot-free GC keeps latest values");
        const auto compacted = store.inspect_tables();
        expect(!compacted.empty()
                   && compacted.back().kind == "BASE"
                   && compacted.back().entries == 2,
               "snapshot-free GC drops all superseded versions");
    }

    {
        KVStore reopened(path, 64);
        expect(reopened.get("k") == "v3" && reopened.get("t") == "t3",
               "snapshot-GC data survives reopen");
    }

    // Oldest snapshot falls between two versions: keep one fallback below it.
    const std::string fallback_path = "snapshot-gc-fallback-test.log";
    reset_test_database(fallback_path);
    std::remove(fallback_path.c_str());
    std::remove((fallback_path + ".wal").c_str());
    {
        KVStore store(fallback_path, 64);
        store.put("f", "f1");
        store.put("g", "g1");
        std::optional<KVStore::Snapshot> old = store.snapshot();
        store.put("f", "f2");

        store.compact();
        expect(store.get("f", *old) == "f1",
               "snapshot between versions sees the fallback version");
        expect(store.get("f") == "f2",
               "latest value still wins after fallback GC");
        auto tables = store.inspect_tables();
        expect(!tables.empty() && tables.back().entries == 3,
               "GC keeps the fallback below the oldest snapshot");

        old.reset();
        store.compact();
        expect(store.get("f") == "f2",
               "fallback is dropped once no snapshot needs it");
        tables = store.inspect_tables();
        expect(!tables.empty() && tables.back().entries == 2,
               "snapshot-free GC keeps one version per key");
    }
    reset_test_database(fallback_path);
    std::remove(fallback_path.c_str());
    std::remove((fallback_path + ".wal").c_str());

    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    if (failures != 0) {
        std::cerr << failures << " snapshot-GC checks failed\n";
        return 1;
    }
    std::cout << "PASS: snapshot-aware compaction GC\n";
    return 0;
}
