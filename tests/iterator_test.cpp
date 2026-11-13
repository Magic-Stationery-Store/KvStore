#include "kv_store.h"
#include "test_files.h"

#include <cstdio>
#include <iostream>
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

std::vector<std::string> collect_keys(KVStore::Iterator iterator) {
    std::vector<std::string> keys;
    for (iterator.seek_to_first(); iterator.valid(); iterator.next()) {
        keys.push_back(iterator.key());
    }
    return keys;
}

} // namespace

int main() {
    const std::string path = "iterator-test.log";
    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    {
        KVStore store(path, 3);

        // BASE: a=old, b=keep, c=stable
        store.put("a", "old");
        store.put("b", "keep");
        store.put("c", "stable");
        store.flush();
        store.compact();

        // L0: a=new, b=tombstone, d=four
        store.put("a", "new");
        store.remove("b");
        store.put("d", "four");
        store.flush();

        // Active: e=five
        store.put("e", "five");

        KVStore::Iterator iterator = store.new_iterator();
        expect(
            collect_keys(iterator) == std::vector<std::string>{"a", "c", "d", "e"},
            "merged iterator skips tombstones and returns sorted keys");

        iterator.seek_to_first();
        expect(iterator.valid() && iterator.key() == "a"
               && iterator.value() == "new",
               "newest layer wins: a=new");

        iterator.seek("b");
        expect(iterator.valid() && iterator.key() == "c",
               "seek skips a deleted key and lands on the next key");

        iterator.seek("d");
        expect(iterator.valid() && iterator.key() == "d"
               && iterator.value() == "four",
               "seek lands exactly on an existing key");

        iterator.seek("z");
        expect(!iterator.valid(), "seek past the end is invalid");

        iterator.seek_to_first();
        iterator.next();
        expect(iterator.valid() && iterator.key() == "c",
               "next advances in key order");

        // Snapshot isolation: the iterator must not observe later writes.
        KVStore::Iterator snapshot = store.new_iterator();
        store.put("f", "six");

        expect(collect_keys(snapshot)
                   == std::vector<std::string>{"a", "c", "d", "e"},
               "iterator keeps its snapshot after a write");
        expect(collect_keys(store.new_iterator())
                   == std::vector<std::string>{"a", "c", "d", "e", "f"},
               "a new iterator sees the new write");
    }

    {
        KVStore reopened(path, 3);
        expect(collect_keys(reopened.new_iterator())
                   == std::vector<std::string>{"a", "c", "d", "e", "f"},
               "iterator works after reopen");
    }

    {
        KVStore memory_store;
        memory_store.put("x", "1");
        memory_store.put("y", "2");

        KVStore::Iterator iterator = memory_store.new_iterator();
        expect(collect_keys(iterator) == std::vector<std::string>{"x", "y"},
               "in-memory mode also supports iterators");
    }

    {
        const std::string empty_path = path + ".empty";
        reset_test_database(empty_path);
        std::remove(empty_path.c_str());
        std::remove((empty_path + ".wal").c_str());

        KVStore empty(empty_path, 3);
        expect(!empty.new_iterator().valid(),
               "empty database yields an invalid iterator");

        reset_test_database(empty_path);
        std::remove(empty_path.c_str());
        std::remove((empty_path + ".wal").c_str());
    }

    reset_test_database(path);
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    if (failures != 0) {
        std::cerr << failures << " iterator checks failed\n";
        return 1;
    }
    std::cout << "PASS: snapshot iterator\n";
    return 0;
}
