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
    std::remove(path.c_str());
    std::remove(wal.c_str());

    // Phase 1: The base file has two keys and the WAL is empty.
    {
        KVStore store(path);
        store.put("a", "old");
        store.put("b", "keep");
        store.compact();
    }
    const auto base_size = std::filesystem::file_size(path);
    expect(base_size > 8, "compaction moved live data into the base file");
    expect(std::filesystem::file_size(wal) == 8,
           "compaction resets the WAL to its 8-byte header");

    // TODO Phase 2: Open a new KVStore in a separate scope.
    // Put a=new and remove b. Check both Get results.
    // Check that the base file size is still base_size and WAL is larger than 8.
    expect(false, "TODO: Phase 2 - WAL shadows the base file");

    // TODO Phase 3: Open it again WITHOUT Compact. Check that a=new
    // and b is absent. This tests WAL replay after process restart.
    expect(false, "TODO: Phase 3 - WAL replay restores latest state");

    // TODO Phase 4: Open it, Compact, check WAL size is 8, then close.
    // Open it once more and check the final values again.
    expect(false, "TODO: Phase 4 - compaction preserves latest state");

    std::remove(path.c_str());
    std::remove(wal.c_str());
    std::remove((path + ".lock").c_str());
    return failures == 0 ? 0 : 1;
}
