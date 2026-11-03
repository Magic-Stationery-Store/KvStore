#include "test_files.h"
#include "kv_store.h"

#include <cstdio>
#include <iostream>
#include <string>

int main() {
    const std::string log_path = "disk-index-test.log";
    reset_test_database(log_path);
std::remove(log_path.c_str());
    std::remove((log_path + ".wal").c_str());

    const std::string binary_value("ab\0cd\n", 6);

    {
        KVStore store(log_path);
        store.put("empty", "");
        store.put("binary", binary_value);
        store.put("overwrite", "old");
        store.put("overwrite", "new");
        store.compact();
    }

    {
        KVStore store(log_path);

        const auto empty = store.get("empty");
        const auto binary = store.get("binary");
        const auto overwritten = store.get("overwrite");

        if (!empty.has_value() || !empty->empty()) {
            std::cerr << "Empty value was not restored\n";
            return 1;
        }

        if (!binary.has_value() || *binary != binary_value) {
            std::cerr << "Binary value was not restored\n";
            return 1;
        }

        if (!overwritten.has_value() || *overwritten != "new") {
            std::cerr << "Index does not point to newest value\n";
            return 1;
        }

        if (!store.remove("binary")) {
            std::cerr << "Failed to remove indexed value\n";
            return 1;
        }

        store.sync();
    }

    {
        KVStore store(log_path);

        if (store.get("binary").has_value()) {
            std::cerr << "Deleted value returned after restart\n";
            return 1;
        }

        const auto overwritten = store.get("overwrite");
        if (!overwritten.has_value() || *overwritten != "new") {
            std::cerr << "Newest value was not retained\n";
            return 1;
        }
    }

    reset_test_database(log_path);
std::remove(log_path.c_str());
    std::remove((log_path + ".wal").c_str());
    std::cout << "Disk index test passed\n";
    return 0;
}
