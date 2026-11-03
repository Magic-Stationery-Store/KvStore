#include "test_files.h"
#include "kv_store.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::size_t count_level0_files(const std::string& path) {
    const std::filesystem::path base =
        std::filesystem::absolute(path).lexically_normal();
    const std::string prefix =
        base.filename().string() + ".L0.";
    const std::string suffix = ".sst";
    std::size_t count = 0;

    for (const auto& item :
         std::filesystem::directory_iterator(base.parent_path())) {
        const std::string name = item.path().filename().string();
        if (name.size() > prefix.size() + suffix.size()
            && name.compare(0, prefix.size(), prefix) == 0
            && name.compare(
                   name.size() - suffix.size(),
                   suffix.size(), suffix) == 0) {
            ++count;
        }
    }
    return count;
}

void cleanup(const std::string& path) {
    reset_test_database(path);
    const std::filesystem::path base =
        std::filesystem::absolute(path).lexically_normal();
    const std::string prefix =
        base.filename().string() + ".L0.";
    const std::string immutable_prefix =
        base.filename().string() + ".wal.L0.";
    for (const auto& item :
         std::filesystem::directory_iterator(base.parent_path())) {
        const std::string name = item.path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0
            || name.compare(
                   0, immutable_prefix.size(), immutable_prefix) == 0) {
            std::error_code ignored;
            std::filesystem::remove(item.path(), ignored);
        }
    }
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
    std::remove((path + ".lock").c_str());
    std::remove((path + ".compact.tmp").c_str());
}

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    const std::string path = "level0-flush-test.log";
    cleanup(path);

    // Three distinct MemTable keys reach the configured threshold.
    {
        KVStore store(path, 3);
        store.put("a", "old");
        store.put("b", "keep");
        store.put("c", "stable");
        store.flush();

        if (count_level0_files(path) != 1
            || std::filesystem::file_size(path + ".wal") != 8) {
            cleanup(path);
            return fail("threshold did not create the first L0 SSTable");
        }

        // These newer WAL entries must shadow the first SSTable.
        store.put("a", "new");
        store.remove("b");
    }

    // Startup discovers L0 files, then replays newer WAL entries.
    {
        KVStore store(path, 3);
        if (store.get("a") != "new"
            || store.get("b").has_value()
            || store.get("c") != "stable") {
            cleanup(path);
            return fail("WAL did not shadow the first L0 table");
        }

        // Replayed a/b plus d reach three distinct keys and create L0 #2.
        store.put("d", "four");
        store.flush();
        if (count_level0_files(path) != 2
            || std::filesystem::file_size(path + ".wal") != 8) {
            cleanup(path);
            return fail("second automatic flush was not created");
        }
    }

    // Reads must search newest L0 to oldest L0.
    {
        KVStore store(path, 3);
        if (store.get("a") != "new"
            || store.get("b").has_value()
            || store.get("c") != "stable"
            || store.get("d") != "four") {
            cleanup(path);
            return fail("newest-to-oldest L0 lookup is incorrect");
        }

        store.compact();
        if (count_level0_files(path) != 0
            || std::filesystem::file_size(path + ".wal") != 8) {
            cleanup(path);
            return fail("full compaction did not consume L0 tables");
        }
    }

    // Final values now live in the base file.
    {
        KVStore store(path, 3);
        if (store.get("a") != "new"
            || store.get("b").has_value()
            || store.get("c") != "stable"
            || store.get("d") != "four") {
            cleanup(path);
            return fail("compacted L0 values did not survive restart");
        }
    }

    cleanup(path);
    std::cout << "PASS: automatic MemTable flush and L0 lookup\n";
    return 0;
}
