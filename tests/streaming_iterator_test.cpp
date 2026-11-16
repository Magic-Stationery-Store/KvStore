#include "kv_store.h"
#include "test_files.h"

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

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

std::vector<std::string> collect(KVStore::StreamingIterator&& iterator) {
    std::vector<std::string> keys;
    for (iterator.seek_to_first(); iterator.valid(); iterator.next()) {
        keys.push_back(iterator.key());
    }
    return keys;
}

std::vector<std::string> scan_keys(
    const KVStore& store,
    const std::string& begin,
    const std::string& end,
    std::size_t limit
) {
    std::vector<std::string> keys;
    for (const auto& [key, value] : store.scan(begin, end, limit)) {
        (void)value;
        keys.push_back(key);
    }
    return keys;
}

std::vector<std::string> scan_keys(
    const KVStore& store,
    const std::string& begin,
    const std::string& end,
    std::size_t limit,
    const KVStore::Snapshot& snapshot
) {
    std::vector<std::string> keys;
    for (const auto& [key, value] :
         store.scan(begin, end, limit, snapshot)) {
        (void)value;
        keys.push_back(key);
    }
    return keys;
}

} // namespace

int main() {
    const std::string path = "streaming-iterator-test.log";
    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    {
        KVStore store(path, 3);

        store.put("a", "old");
        store.put("b", "keep");
        store.put("c", "stable");
        store.flush();
        store.compact();

        store.put("a", "new");
        store.remove("b");
        store.put("d", "four");
        store.flush();

        store.put("e", "five");

        expect(collect(store.new_streaming_iterator())
                   == std::vector<std::string>{"a", "c", "d", "e"},
               "streaming iterator merges layers and skips tombstones");

        KVStore::StreamingIterator iterator = store.new_streaming_iterator();
        iterator.seek("b");
        expect(iterator.valid() && iterator.key() == "c"
                   && iterator.value() == "stable",
               "streaming seek skips a deleted key");

        iterator.seek("d");
        expect(iterator.valid() && iterator.key() == "d",
               "streaming seek lands on an existing key");

        iterator.seek("z");
        expect(!iterator.valid(), "streaming seek past the end is invalid");

        expect(scan_keys(store, "a", "e", 100)
                   == std::vector<std::string>{"a", "c", "d"},
               "scan uses an exclusive end key");
        expect(scan_keys(store, "b", "z", 100)
                   == std::vector<std::string>{"c", "d", "e"},
               "scan starts at an inclusive begin key");
        expect(scan_keys(store, "a", "z", 2)
                   == std::vector<std::string>{"a", "c"},
               "scan honours the limit");
        expect(scan_keys(store, "a", "z", 0).empty(),
               "zero limit returns no rows");
        expect(scan_keys(store, "z", "a", 100).empty(),
               "reversed range returns no rows");

        std::optional<KVStore::Snapshot> snapshot = store.snapshot();
        store.put("f", "six");

        expect(scan_keys(store, "a", "z", 100)
                   == std::vector<std::string>{"a", "c", "d", "e", "f"},
               "current scan sees the new write");
        expect(scan_keys(store, "a", "z", 100, *snapshot)
                   == std::vector<std::string>{"a", "c", "d", "e"},
               "snapshot scan ignores the new write");

        expect(collect(store.new_streaming_iterator(*snapshot))
                   == std::vector<std::string>{"a", "c", "d", "e"},
               "snapshot streaming iterator ignores the new write");
    }

    {
        KVStore reopened(path, 3);
        expect(collect(reopened.new_streaming_iterator())
                   == std::vector<std::string>{"a", "c", "d", "e", "f"},
               "streaming iterator works after reopen");
    }

    {
        const std::string empty_path = path + ".empty";
        reset_test_database(empty_path);
        std::remove(empty_path.c_str());
        std::remove((empty_path + ".wal").c_str());

        KVStore empty(empty_path, 3);
        expect(!empty.new_streaming_iterator().valid(),
               "empty database yields an invalid streaming iterator");
        expect(empty.scan("a", "z", 10).empty(),
               "empty database scan returns no rows");

        reset_test_database(empty_path);
        std::remove(empty_path.c_str());
        std::remove((empty_path + ".wal").c_str());
    }

    {
        const std::string prefix_path = "streaming-prefix-test.log";
        reset_test_database(prefix_path);
        std::remove(prefix_path.c_str());
        std::remove((prefix_path + ".wal").c_str());

        KVStore store(prefix_path, 64);
        store.put("a", "1");
        store.put(std::string("a\0b", 3), "nul");
        store.put("aa", "2");
        store.put("ab", "3");
        store.put("b", "4");

        const std::vector<std::string> prefix_order{
            "a", std::string("a\0b", 3), "aa", "ab"
        };
        expect(scan_keys(store, "a", "b", 100) == prefix_order,
               "scan does not skip keys prefixed by the begin key");

        KVStore::StreamingIterator iterator = store.new_streaming_iterator();
        iterator.seek("a");
        std::vector<std::string> keys;
        while (iterator.valid()) {
            keys.push_back(iterator.key());
            iterator.next();
        }
        expect(keys == std::vector<std::string>{
                   "a", std::string("a\0b", 3), "aa", "ab", "b"},
               "streaming seek handles prefix and NUL keys");

        reset_test_database(prefix_path);
        std::remove(prefix_path.c_str());
        std::remove((prefix_path + ".wal").c_str());
    }

    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    if (failures != 0) {
        std::cerr << failures << " streaming checks failed\n";
        return 1;
    }
    std::cout << "PASS: streaming MergingIterator and scan\n";
    return 0;
}
