#include "sstable.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

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
        expect(reopened.might_contain_user("apple"),
               "Bloom filter is restored from the footer");
        const auto range = reopened.user_key_range();
        expect(range == std::make_pair(
                   std::string("apple"), std::string("zebra")),
               "user-key range is restored from the footer");
    }

    {
        const std::string corrupt = path + ".corrupt";
        std::filesystem::remove(corrupt);
        std::filesystem::copy_file(path, corrupt);
        {
            std::fstream file(
                corrupt,
                std::ios::binary | std::ios::in | std::ios::out);
            const auto size = std::filesystem::file_size(corrupt);
            file.seekg(static_cast<std::streamoff>(size - 1));
            char byte = 0;
            file.get(byte);
            byte ^= 0x01;
            file.seekp(static_cast<std::streamoff>(size - 1));
            file.put(byte);
        }
        bool rejected = false;
        try {
            SSTable damaged(corrupt);
        } catch (const std::exception&) {
            rejected = true;
        }
        expect(rejected, "corrupt footer rejects SSTable open");
        std::filesystem::remove(corrupt);
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
