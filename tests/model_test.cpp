#include "test_files.h"
#include "kv_store.h"

#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

namespace {

constexpr unsigned int kSeed = 20261007;
constexpr int kOperationCount = 1000;
constexpr int kKeyCount = 10;

int failures = 0;

void report_failure(
    int step,
    const std::string& operation,
    const std::string& key,
    const std::string& message
) {
    std::cerr
        << "FAIL\n"
        << "seed: " << kSeed << '\n'
        << "step: " << step << '\n'
        << "operation: " << operation << '\n'
        << "key: " << key << '\n'
        << "reason: " << message << '\n';

    ++failures;
}

std::optional<std::string> get_from_model(
    const std::unordered_map<std::string, std::string>& model,
    const std::string& key
) {
    const auto it = model.find(key);

    if (it == model.end()) {
        return std::nullopt;
    }

    return it->second;
}

bool check_key(
    KVStore& store,
    const std::unordered_map<std::string, std::string>& model,
    const std::string& key,
    int step,
    const std::string& operation
) {
    const auto actual = store.get(key);
    const auto expected = get_from_model(model, key);

    if (actual == expected) {
        return true;
    }

    report_failure(
        step,
        operation,
        key,
        "KVStore result differs from reference model"
    );

    std::cerr
        << "expected: "
        << (expected.has_value() ? *expected : "<missing>")
        << '\n'
        << "actual: "
        << (actual.has_value() ? *actual : "<missing>")
        << '\n';

    return false;
}

bool check_all_keys(
    KVStore& store,
    const std::unordered_map<std::string, std::string>& model,
    int step,
    const std::string& operation
) {
    for (int number = 0; number < kKeyCount; ++number) {
        const std::string key =
            "key-" + std::to_string(number);

        if (!check_key(store, model, key, step, operation)) {
            return false;
        }
    }

    return true;
}

} // namespace

int main() {
    const std::string path = "model-test.log";
    const std::string wal_path = path + ".wal";
    const std::string lock_path = path + ".lock";

    reset_test_database(path);
std::remove(path.c_str());
    std::remove(wal_path.c_str());
    std::remove(lock_path.c_str());

    std::unordered_map<std::string, std::string> model;

    std::mt19937 random_engine(kSeed);

    std::uniform_int_distribution<int> operation_distribution(0, 99);
    std::uniform_int_distribution<int> key_distribution(
        0,
        kKeyCount - 1
    );
    std::uniform_int_distribution<int> value_distribution(0, 999999);

    auto store = std::make_unique<KVStore>(path);

    for (int step = 1; step <= kOperationCount; ++step) {
        const int operation_number =
            operation_distribution(random_engine);

        const std::string key =
            "key-" + std::to_string(
                key_distribution(random_engine)
            );

        if (operation_number < 45) {
            // 45% 的操作是 Put。
            std::string value;

            // 定期写入空字符串，验证空值和不存在能够区分。
            if (step % 17 == 0) {
                value = "";
            } else {
                value =
                    "value-"
                    + std::to_string(
                        value_distribution(random_engine)
                    );
            }

            store->put(key, value);
            model[key] = value;

            if (!check_key(
                    *store,
                    model,
                    key,
                    step,
                    "Put")) {
                break;
            }
        } else if (operation_number < 75) {
            // 30% 的操作是 Get。
            if (!check_key(
                    *store,
                    model,
                    key,
                    step,
                    "Get")) {
                break;
            }
        } else {
            // 25% 的操作是 Delete。
            const bool actual_removed =
                store->remove(key);

            const bool expected_removed =
                model.erase(key) > 0;

            if (actual_removed != expected_removed) {
                report_failure(
                    step,
                    "Delete",
                    key,
                    "remove() return value differs from model"
                );
                break;
            }

            if (!check_key(
                    *store,
                    model,
                    key,
                    step,
                    "Delete")) {
                break;
            }
        }

        // 每 100 步压缩一次。
        if (step % 100 == 0) {
            store->compact();

            if (!check_all_keys(
                    *store,
                    model,
                    step,
                    "Compact")) {
                break;
            }
        }

        // 每 50 步关闭并重新打开数据库。
        if (step % 50 == 0) {
            store->sync();
            store.reset();
            store = std::make_unique<KVStore>(path);

            if (!check_all_keys(
                    *store,
                    model,
                    step,
                    "Restart")) {
                break;
            }
        }
    }

    // 最后再重启一次，检查最终持久化状态。
    if (failures == 0) {
        store->sync();
        store.reset();
        store = std::make_unique<KVStore>(path);

        check_all_keys(
            *store,
            model,
            kOperationCount,
            "Final restart"
        );
    }

    store.reset();

    reset_test_database(path);
std::remove(path.c_str());
    std::remove(wal_path.c_str());
    std::remove(lock_path.c_str());

    if (failures != 0) {
        std::cerr << failures << " model test failures\n";
        return 1;
    }

    std::cout
        << "PASS: "
        << kOperationCount
        << " randomized operations matched the model\n";

    return 0;
}