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

std::string collect(KVStore::Iterator iterator) {
    std::string result;
    for (iterator.seek_to_first(); iterator.valid(); iterator.next()) {
        if (!result.empty()) {
            result += ",";
        }
        result += iterator.key() + "=" + iterator.value();
    }
    return result;
}

} // namespace

int main() {
    const std::string path = "mvcc-test.log";
    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    {
        KVStore store(path, 64);

        store.put("k", "v1");
        std::optional<KVStore::Snapshot> s1 = store.snapshot();

        store.put("k", "v2");
        std::optional<KVStore::Snapshot> s2 = store.snapshot();

        store.remove("k");
        std::optional<KVStore::Snapshot> s3 = store.snapshot();

        store.put("k", "v4");
        std::optional<KVStore::Snapshot> s4 = store.snapshot();

        expect(store.get("k", *s1) == "v1",
               "snapshot 1 sees v1");
        expect(store.get("k", *s2) == "v2",
               "snapshot 2 sees v2");
        expect(!store.get("k", *s3).has_value(),
               "snapshot 3 sees the tombstone");
        expect(store.get("k", *s4) == "v4",
               "snapshot 4 sees v4");
        expect(store.get("k") == "v4",
               "latest read sees v4");

        expect(collect(store.new_iterator(*s1)) == "k=v1",
               "iterator(snapshot 1) sees v1");
        expect(collect(store.new_iterator(*s3)).empty(),
               "iterator(snapshot 3) hides the tombstone");
        expect(collect(store.new_iterator(*s4)) == "k=v4",
               "iterator(snapshot 4) sees v4");

        // A full compaction must retain versions still visible to s1.
        store.flush();
        store.compact();

        expect(store.get("k", *s1) == "v1",
               "compaction keeps the version visible to snapshot 1");
        expect(store.get("k", *s2) == "v2",
               "compaction keeps the version visible to snapshot 2");
        expect(!store.get("k", *s3).has_value(),
               "compaction keeps the tombstone visible to snapshot 3");
        expect(store.get("k") == "v4",
               "latest read still sees v4 after compaction");

        s1.reset();
        s2.reset();
        s3.reset();
        s4.reset();

        // With no live snapshots, compaction may collapse to one version.
        store.compact();
        expect(store.get("k") == "v4",
               "latest value survives snapshot-free compaction");

        const auto tables = store.inspect_tables();
        expect(!tables.empty() && tables.back().kind == "BASE"
                   && tables.back().entries == 1,
               "snapshot-free compaction collapses old versions");
    }

    {
        KVStore reopened(path, 64);
        expect(reopened.get("k") == "v4",
               "compacted MVCC data survives reopen");
    }

    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    if (failures != 0) {
        std::cerr << failures << " MVCC checks failed\n";
        return 1;
    }
    std::cout << "PASS: MVCC snapshots and compaction\n";
    return 0;
}
