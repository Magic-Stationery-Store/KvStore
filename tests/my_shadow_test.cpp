#include "test_files.h"
#include "kv_store.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& description) {
    if (condition) {
        std::cout << "PASS: " << description << '\n';
    } else {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    const std::string path = "my-shadow-test.log";
    const std::string wal = path + ".wal";
    reset_test_database(path);
std::remove(path.c_str());
    std::remove(wal.c_str());

    // Phase 1: The base file has two keys and the WAL is empty.
    {
        KVStore store(path);
        store.put("a", "old");
        store.put("b", "keep");
        store.compact();
    }
    const auto base_size = std::filesystem::file_size(current_test_base(path));
    expect(base_size > 8, "compaction moved live data into the base file");
    expect(std::filesystem::file_size(wal) == 8,
           "compaction resets the WAL to its 8-byte header");

    // TODO Phase 2: Open a new KVStore in a separate scope.
    // Put a=new and remove b. Check both Get results.
    {
       KVStore store(path);
       store.put("a", "new");
       store.remove("b");
       expect(store.get("a").value_or("") == "new", "Phase 2: a == new in memory");
       expect(!store.get("b").has_value(),          "Phase 2: b is absent in memory");
    }


    // Check that the base file size is still base_size and WAL is larger than 8.
    // TODO Phase 3: Open it again WITHOUT Compact. Check that a=new
    // and b is absent. This tests WAL replay after process restart.
    {
        KVStore store(path);
        expect(store.get("a").value_or("") == "new", "Phase 3: a == new after WAL replay");
        expect(!store.get("b").has_value(),
           "Phase 3: WAL replay restores b as absent");
    }
    // TODO Phase 4: Open it, Compact, check WAL size is 8, then close.
    {
        KVStore store(path);
        store.compact();
        expect(std::filesystem::file_size(wal) == 8,
                "Phase 4: compaction resets the WAL to its 8-byte header");
    }
    // Open it once more and check the final values again.
    {
    KVStore store(path);   // 再次打开
    expect(store.get("a").value_or("") == "new",
           "Phase 4: after compaction a == new");
    expect(!store.get("b").has_value(),
           "Phase 4: after compaction b is absent");
    }

    reset_test_database(path);
std::remove(path.c_str());
    std::remove(wal.c_str());
    std::remove((path + ".lock").c_str());
    return failures == 0 ? 0 : 1;
}
