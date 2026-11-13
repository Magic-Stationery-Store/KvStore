#include "kv_store.h"
#include "test_files.h"

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

void cleanup(const std::string& path) {
    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
}

} // namespace

int main() {
    const std::string path = "write-batch-test.log";
    cleanup(path);

    {
        KVStore store(path, 64);
        store.put("a", "old");
        store.put("b", "old");

        std::optional<KVStore::Snapshot> before = store.snapshot();

        KVStore::WriteBatch batch;
        batch.put("a", "new");
        batch.put("b", "new");
        batch.put("c", "new");
        expect(batch.size() == 3, "batch size");
        store.write(batch);

        expect(store.get("a") == "new"
                   && store.get("b") == "new"
                   && store.get("c") == "new",
               "batch applies all writes");
        expect(store.get("a", *before) == "old"
                   && store.get("b", *before) == "old"
                   && !store.get("c", *before).has_value(),
               "batch is invisible to an older snapshot");

        KVStore::WriteBatch duplicate;
        duplicate.put("x", "1");
        duplicate.erase("x");
        store.write(duplicate);
        expect(!store.get("x").has_value(),
               "last operation for a duplicated key wins");

        KVStore::WriteBatch erase_batch;
        erase_batch.erase("a");
        store.write(erase_batch);
        expect(!store.get("a").has_value(),
               "batch delete is visible to latest read");
        expect(store.get("a", *before) == "old",
               "batch delete is invisible to an older snapshot");
    }

    {
        KVStore reopened(path, 64);
        expect(!reopened.get("a").has_value()
                   && reopened.get("b") == "new"
                   && reopened.get("c") == "new",
               "batch survives reopen");
    }

    // A batch is one WAL record. Truncating its CRC must roll back the whole
    // batch, not just the last operation.
    const std::string crash_path = "write-batch-crash-test.log";
    cleanup(crash_path);
    {
        KVStore store(crash_path, 64);
        store.put("a", "old");
        store.put("b", "old");
    }
    {
        KVStore store(crash_path, 64);
        KVStore::WriteBatch batch;
        batch.put("a", "new");
        batch.put("b", "new");
        store.write(batch);
    }

    const auto wal_path = crash_path + ".wal";
    const auto wal_size = std::filesystem::file_size(wal_path);
    expect(wal_size > 4, "batch WAL exists");
    std::filesystem::resize_file(wal_path, wal_size - 1);

    {
        KVStore store(crash_path, 64);
        expect(store.get("a") == "old" && store.get("b") == "old",
               "truncated batch record rolls back all keys");
    }

    {
        KVStore store(crash_path, 64);
        KVStore::WriteBatch batch;
        batch.put("a", "new");
        batch.put("b", "new");
        store.write(batch);
    }
    {
        KVStore store(crash_path, 64);
        expect(store.get("a") == "new" && store.get("b") == "new",
               "retried batch commits atomically after truncation repair");
    }

    cleanup(path);
    cleanup(crash_path);

    if (failures != 0) {
        std::cerr << failures << " batch checks failed\n";
        return 1;
    }
    std::cout << "PASS: atomic WriteBatch\n";
    return 0;
}
