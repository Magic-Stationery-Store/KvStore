#include "test_files.h"
#include<iostream>
#include"kv_store.h"
#include<string>
#include <filesystem>
int main()
{
    KVStore store;
    int failures = 0;
    auto check = [&](bool condition, const std::string& message)
    {
        if(!condition)
        {
            std::cout << "Test failed: " << message << "\n";
            ++failures;
        }
        else
        {
            std::cout << "Test passed: " << message << "\n";
        }
    };
    const std::string recovery_log =
    "recovery-test.log";
const std::string recovery_wal = recovery_log + ".wal";

reset_test_database(recovery_log);
std::remove(recovery_log.c_str());
std::remove(recovery_wal.c_str());

std::uintmax_t size_after_first = 0;

{
    KVStore store(recovery_log);

    store.put("safe", "value1");

    size_after_first =
        std::filesystem::file_size(recovery_wal);

    store.put("broken", "value2");
}

// 模拟第二条记录没有写完整：
// 删除日志最后3个字节。
{
    const std::uintmax_t full_size =
        std::filesystem::file_size(recovery_wal);

    std::filesystem::resize_file(
        recovery_wal,
        full_size - 3
    );
}

// 重新打开时应该自动删除残缺的第二条记录。
{
    KVStore recovered(recovery_log);

    auto safe = recovered.get("safe");

    check(
        safe.has_value() && *safe == "value1",
        "complete record survives truncated tail"
    );

    check(
        !recovered.get("broken").has_value(),
        "incomplete record is ignored"
    );

    check(
        std::filesystem::file_size(recovery_wal)
            == size_after_first,
        "incomplete tail is physically removed"
    );

    // 修复后继续写入。
    recovered.put("new", "value3");
}

// 再重启一次，确认日志仍然可以使用。
{
    KVStore reopened(recovery_log);

    auto safe = reopened.get("safe");
    auto new_value = reopened.get("new");

    check(
        safe.has_value() && *safe == "value1",
        "old complete record survives second restart"
    );

    check(
        new_value.has_value()
            && *new_value == "value3",
        "new record survives after tail repair"
    );
}

reset_test_database(recovery_log);
std::remove(recovery_log.c_str());
std::remove(recovery_wal.c_str());
    check(!store.get("missing").has_value(), "Get missing key should return nullopt");
    store.put("name", "Alice");
    auto value = store.get("name");
    check(value.has_value() && value.value() == "Alice", "Get existing key should return correct value");
    store.put("name", "Bob");
    value = store.get("name");
    check(value.has_value() && value.value() == "Bob", "Overwrite existing key should update value");
    store.put("empty","");
    value = store.get("empty");
    check(value.has_value() && value->empty(),"empty value is not a missing key");
    check(store.remove("name"), "Remove existing key");
    check(!store.get("name").has_value(),"removed key cannot be found");
    check(!store.remove("missing"),"remove missing key");

    if(failures != 0)
    {
        std::cerr << failures << " tests failed.\n";
        return 1;
    }
    std::cout << "All tests passed.\n";
    return 0;
}
