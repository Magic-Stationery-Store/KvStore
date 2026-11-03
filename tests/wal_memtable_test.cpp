#include "test_files.h"
#include "kv_store.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
    const std::string path = "wal-memtable-test.log";
    const std::string wal = path + ".wal";
    reset_test_database(path);
std::remove(path.c_str());
    std::remove(wal.c_str());

    {
        KVStore store(path);
        store.put("overwrite", "old");
        store.put("deleted", "old");
        store.compact();
    }

    // A pre-WAL database has only the base log. Opening it creates a WAL.
    std::remove(wal.c_str());

    std::uintmax_t base_size = 0;
    {
        KVStore store(path);
        base_size = std::filesystem::file_size(path);
        store.put("overwrite", "new");
        store.remove("deleted");
        store.put("pending", "wal-only");
        store.sync();

        if (std::filesystem::file_size(path) != base_size
            || std::filesystem::file_size(wal) <= 8) {
            std::cerr << "Writes did not stay in the WAL\n";
            return 1;
        }
    }

    {
        KVStore store(path);
        if (store.get("overwrite") != "new"
            || store.get("deleted").has_value()
            || store.get("pending") != "wal-only") {
            std::cerr << "WAL did not override base data\n";
            return 1;
        }

        store.compact();

        if (std::filesystem::file_size(wal) != 8) {
            std::cerr << "Compaction did not reset the WAL\n";
            return 1;
        }
    }

    {
        KVStore store(path);
        if (store.get("overwrite") != "new"
            || store.get("deleted").has_value()
            || store.get("pending") != "wal-only") {
            std::cerr << "Compacted data did not survive restart\n";
            return 1;
        }
    }

    reset_test_database(path);
std::remove(path.c_str());
    std::remove(wal.c_str());
    std::remove((path + ".lock").c_str());
    std::cout << "WAL and MemTable test passed\n";
    return 0;
}
