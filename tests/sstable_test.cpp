#include "sstable.h"

#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
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

} // namespace

int main() {
    const std::string path = "sstable-test.sst";
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());

    SSTable::Entries entries;
    entries["apple"] = "red";
    entries["banana"] = std::nullopt;
    entries["empty"] = "";
    entries["zebra"] = std::string("ab\0cd", 5);

    SSTable::build(path, entries);

    {
        SSTable table(path);
        expect(table.entry_count() == 4,
               "all sorted entries are indexed");

        const auto apple = table.get("apple");
        expect(apple.found && apple.value == "red",
               "existing value can be found");

        const auto absent = table.get("avocado");
        expect(!absent.found,
               "missing key is different from a tombstone");

        const auto banana = table.get("banana");
        expect(banana.found && !banana.value.has_value(),
               "tombstone is preserved");

        const auto empty = table.get("empty");
        expect(empty.found
                   && empty.value.has_value()
                   && empty.value->empty(),
               "empty value is preserved");

        const auto binary = table.get("zebra");
        expect(binary.found
                   && binary.value == std::string("ab\0cd", 5),
               "binary value is preserved");
    }

    // A new object rebuilds its in-memory index from the immutable file.
    {
        SSTable reopened(path);
        const auto result = reopened.get("apple");
        expect(result.found && result.value == "red",
               "SSTable survives reopen");
    }

    bool overwrite_rejected = false;
    try {
        SSTable::build(path, entries);
    } catch (const std::invalid_argument&) {
        overwrite_rejected = true;
    }
    expect(overwrite_rejected,
           "an immutable SSTable cannot be overwritten");

    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());

    if (failures != 0) {
        std::cerr << failures << " SSTable tests failed\n";
        return 1;
    }
    std::cout << "All SSTable tests passed\n";
    return 0;
}
