#include "test_files.h"
#include "kv_store.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
    const std::string log_path = "compaction-test.log";
    const std::string wal_path = log_path + ".wal";
    const std::string temporary_path =
        log_path + ".compact.tmp";

    reset_test_database(log_path);
std::remove(log_path.c_str());
    std::remove(wal_path.c_str());
    std::remove(temporary_path.c_str());

    std::uintmax_t size_before = 0;
    std::uintmax_t size_after = 0;

    {
        KVStore store(log_path);

        for (int version = 0; version < 100; ++version) {
            store.put(
                "updated",
                "value-" + std::to_string(version)
            );
        }

        store.put("deleted", "old-value");
        store.remove("deleted");
        store.put("stable", "keep-me");
        store.sync();

        size_before =
            test_data_size(log_path);

        store.compact();

        size_after =
            test_data_size(log_path);

        if (std::filesystem::file_size(wal_path) != 8) {
            std::cerr << "WAL was not reset after compaction\n";
            return 1;
        }

        const auto updated = store.get("updated");
        const auto stable = store.get("stable");

        if (!updated.has_value()
            || *updated != "value-99") {
            std::cerr << "Latest value changed during compaction\n";
            return 1;
        }

        if (!stable.has_value() || *stable != "keep-me") {
            std::cerr << "Stable value changed during compaction\n";
            return 1;
        }

        if (store.get("deleted").has_value()) {
            std::cerr << "Deleted key returned after compaction\n";
            return 1;
        }

        store.put("after", "new-write");
        store.sync();
    }

    if (size_after >= size_before) {
        std::cerr << "Compaction did not reduce log size\n";
        return 1;
    }

    if (std::filesystem::exists(temporary_path)) {
        std::cerr << "Temporary compaction file remains\n";
        return 1;
    }

    {
        KVStore reopened(log_path);

        const auto updated = reopened.get("updated");
        const auto stable = reopened.get("stable");
        const auto after = reopened.get("after");

        if (!updated.has_value()
            || *updated != "value-99") {
            std::cerr << "Latest value was not restored\n";
            return 1;
        }

        if (!stable.has_value() || *stable != "keep-me") {
            std::cerr << "Stable value was not restored\n";
            return 1;
        }

        if (!after.has_value() || *after != "new-write") {
            std::cerr << "Write after compaction was not restored\n";
            return 1;
        }

        if (reopened.get("deleted").has_value()) {
            std::cerr << "Deleted key returned after restart\n";
            return 1;
        }
    }

    reset_test_database(log_path);
std::remove(log_path.c_str());
    std::remove(wal_path.c_str());
    std::cout
        << "Compaction reduced log from "
        << size_before
        << " to "
        << size_after
        << " bytes\n";
    return 0;
}
