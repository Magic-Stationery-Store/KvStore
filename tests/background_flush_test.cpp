#include "test_files.h"
#include "kv_store.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::size_t count_matching(
    const std::string& path,
    const std::string& middle,
    const std::string& suffix
) {
    const auto base =
        std::filesystem::absolute(path).lexically_normal();
    const std::string prefix =
        base.filename().string() + middle;
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
    const auto base =
        std::filesystem::absolute(path).lexically_normal();
    const std::string l0_prefix =
        base.filename().string() + ".L0.";
    const std::string immutable_prefix =
        base.filename().string() + ".wal.L0.";
    for (const auto& item :
         std::filesystem::directory_iterator(base.parent_path())) {
        const std::string name = item.path().filename().string();
        if (name.compare(0, l0_prefix.size(), l0_prefix) == 0
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
    const std::string path = "background-flush-test.log";
    cleanup(path);

    // Every two distinct keys freeze the active MemTable. Foreground writes
    // continue in a new MemTable while the worker writes the frozen snapshot.
    {
        KVStore store(path, 2);
        for (int number = 0; number < 20; ++number) {
            const std::string key =
                "key-" + std::to_string(number);
            const std::string value =
                "value-" + std::to_string(number);
            store.put(key, value);
            if (store.get(key) != value) {
                cleanup(path);
                return fail("foreground read lost a value during flush");
            }
        }

        // flush() is a barrier: it waits until active and immutable data are
        // represented by durable SSTables.
        store.flush();
        if (count_matching(path, ".L0.", ".sst") != 10
            || count_matching(path, ".wal.L0.", ".imm") != 0
            || std::filesystem::file_size(path + ".wal") != 8) {
            cleanup(path);
            return fail("flush barrier left unfinished background state");
        }
    }

    {
        KVStore reopened(path, 2);
        for (int number = 0; number < 20; ++number) {
            const std::string key =
                "key-" + std::to_string(number);
            const std::string value =
                "value-" + std::to_string(number);
            if (reopened.get(key) != value) {
                cleanup(path);
                return fail("background-flushed value did not survive restart");
            }
        }
        reopened.compact();
    }
    cleanup(path);

    // Deterministically simulate a crash after active WAL rotation and before
    // the background worker publishes its SSTable.
    const std::string recovery_path =
        "immutable-wal-recovery-test.log";
    cleanup(recovery_path);
    {
        KVStore store(recovery_path, 1000);
        store.put("crash-key", "recovered-value");
        store.sync();
    }

    const auto base = std::filesystem::absolute(recovery_path);
    const std::string immutable_wal =
        (base.parent_path()
         / (base.filename().string()
            + ".wal.L0.00000000000000000001.imm"))
            .string();
    std::filesystem::rename(recovery_path + ".wal", immutable_wal);

    {
        KVStore recovered(recovery_path, 2);
        if (recovered.get("crash-key") != "recovered-value"
            || std::filesystem::exists(immutable_wal)
            || count_matching(
                   recovery_path, ".L0.", ".sst") != 1
            || std::filesystem::file_size(
                   recovery_path + ".wal") != 8) {
            cleanup(recovery_path);
            return fail("immutable WAL recovery is incorrect");
        }
        recovered.compact();
    }
    cleanup(recovery_path);

    std::cout
        << "PASS: background flush and immutable WAL recovery\n";
    return 0;
}
