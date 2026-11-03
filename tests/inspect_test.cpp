#include "test_files.h"
#include "kv_store.h"

#include <cstdio>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    const std::string path = "inspect-test.log";
    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    {
        KVStore store(path, 3);
        store.put("a", "old");
        store.put("b", "base");
        store.put("c", "c1");
        store.flush();
        store.compact();

        store.put("a", "new");
        store.remove("b");
        store.put("d", "four");
        store.flush();
    }

    {
        KVStore store(path, 3);

        // One committed L0 table plus the compacted BASE.
        const auto tables = store.inspect_tables();
        int l0Count = 0;
        bool sawBase = false;
        for (const auto& table : tables) {
            if (table.kind == "L0") {
                ++l0Count;
                if (table.entries != 3) {
                    reset_test_database(path);
                    return fail("L0 table should contain a, b, d");
                }
            } else if (table.kind == "BASE") {
                sawBase = true;
                if (table.entries != 3) {
                    reset_test_database(path);
                    return fail("BASE should contain a, b, c");
                }
            }
        }
        if (l0Count != 1 || !sawBase) {
            reset_test_database(path);
            return fail("inspect_tables did not report L0 + BASE");
        }

        // Entries are exposed per layer, including tombstones.
        bool sawTombstone = false;
        bool sawNewValue = false;
        for (const auto& row : store.inspect_entries()) {
            if (row.layer.rfind("L0#", 0) == 0 && row.key == "b"
                && !row.value.has_value()) {
                sawTombstone = true;
            }
            if (row.layer.rfind("L0#", 0) == 0 && row.key == "a"
                && row.value.has_value() && *row.value == "new") {
                sawNewValue = true;
            }
        }
        if (!sawTombstone || !sawNewValue) {
            reset_test_database(path);
            return fail("inspect_entries lost the tombstone or shadowed value");
        }

        // explain_key must walk Active -> L0 (newest) -> BASE and stop at
        // the first layer that contains the key.
        const auto probes = store.explain_key("b");
        if (probes.empty() || probes.front().layer != "Active") {
            reset_test_database(path);
            return fail("explain_key must start at Active");
        }

        bool sawL0Tombstone = false;
        bool sawBaseValue = false;
        for (const auto& probe : probes) {
            if (probe.layer.rfind("L0#", 0) == 0 && probe.found) {
                sawL0Tombstone = !probe.value.has_value();
            }
            if (probe.layer == "BASE" && probe.found && probe.value.has_value()) {
                sawBaseValue = *probe.value == "base";
            }
        }
        if (!sawL0Tombstone || !sawBaseValue) {
            reset_test_database(path);
            return fail("explain_key did not report tombstone + BASE value");
        }

        const auto missing = store.explain_key("missing");
        for (const auto& probe : missing) {
            if (probe.found) {
                reset_test_database(path);
                return fail("missing key should not be found in any layer");
            }
        }

        if (store.memtable_entry_count() != 0) {
            reset_test_database(path);
            return fail("MemTable should be empty after a completed flush");
        }
    }

    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
    std::cout << "Inspection API test passed\n";
    return 0;
}
