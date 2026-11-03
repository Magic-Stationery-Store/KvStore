#include "test_files.h"
#include "kv_store.h"

#include <atomic>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    const std::string path = "concurrency-test.log";
    reset_test_database(path);
std::remove(path.c_str());
    std::remove((path + ".wal").c_str());

    constexpr int kWriters = 4;
    constexpr int kKeysPerWriter = 40;
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};

    {
        // Keep this test focused on locking; automatic L0 flush has its own test.
        KVStore store(path, 1000);
        store.put("stable", "unchanged");

        bool second_open_rejected = false;
        try {
            KVStore another(path);
        } catch (const std::system_error&) {
            second_open_rejected = true;
        }

        if (!second_open_rejected) {
            std::cerr << "Second database handle acquired the lock\n";
            return 1;
        }

        const pid_t child = ::fork();

        if (child == -1) {
            std::cerr << "Could not start lock test process\n";
            return 1;
        }

        if (child == 0) {
            try {
                KVStore another_process(path);
                ::_exit(1);
            } catch (const std::system_error&) {
                ::_exit(0);
            } catch (...) {
                ::_exit(2);
            }
        }

        int child_status = 0;
        if (::waitpid(child, &child_status, 0) == -1
            || !WIFEXITED(child_status)
            || WEXITSTATUS(child_status) != 0) {
            std::cerr << "Another process acquired the database lock\n";
            return 1;
        }

        std::vector<std::thread> threads;

        for (int writer = 0; writer < kWriters; ++writer) {
            threads.emplace_back([&, writer] {
                while (!start.load()) {
                    std::this_thread::yield();
                }

                try {
                    for (int item = 0; item < kKeysPerWriter; ++item) {
                        const std::string key =
                            "worker-" + std::to_string(writer)
                            + "-" + std::to_string(item);
                        store.put(key, key);
                        if (store.get(key) != key) {
                            ++failures;
                        }
                    }
                } catch (const std::exception&) {
                    ++failures;
                }
            });
        }

        threads.emplace_back([&] {
            while (!start.load()) {
                std::this_thread::yield();
            }

            try {
                for (int run = 0; run < 4; ++run) {
                    store.compact();
                    if (store.get("stable") != "unchanged") {
                        ++failures;
                    }
                }
            } catch (const std::exception&) {
                ++failures;
            }
        });

        start = true;

        for (auto& thread : threads) {
            thread.join();
        }

        store.sync();
    }

    {
        KVStore reopened(path, 1000);
        if (reopened.get("stable") != "unchanged") {
            ++failures;
        }

        for (int writer = 0; writer < kWriters; ++writer) {
            for (int item = 0; item < kKeysPerWriter; ++item) {
                const std::string key =
                    "worker-" + std::to_string(writer)
                    + "-" + std::to_string(item);
                if (reopened.get(key) != key) {
                    ++failures;
                }
            }
        }
    }

    reset_test_database(path);
std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
    std::remove((path + ".lock").c_str());

    if (failures != 0) {
        std::cerr << failures << " concurrency checks failed\n";
        return 1;
    }

    std::cout << "Concurrent access and lock test passed\n";
    return 0;
}
