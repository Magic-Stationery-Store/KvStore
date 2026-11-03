#include "kv_store.h"
#include <iostream>
#include <filesystem>

void list_files(const std::string& tag) {
    std::cout << "[" << tag << "] files:\n";
    for (auto& f : std::filesystem::directory_iterator(".")) {
        std::cout << "  " << f.path().filename() << "\n";
    }
}

int main() {
    std::string path = "unit1-minimal-test.log";
    std::filesystem::remove_all(path);

    auto db = std::make_unique<KVStore>(path, 3);

    db->put("a", "1");
    db->put("b", "2");
    db->put("c", "3");
    list_files("after 3 puts");   // 应该看到 .wal、.sst、.manifest

    db->put("d", "4");
    list_files("after 4th put");  // 观察是否又生成新的 .sst

    // 查询：验证读路径
    auto v = db->get("a");
    std::cout << "Get(a) = " << (v.has_value() ? *v : "NOT FOUND") << "\n";

    // 销毁，模拟关闭
    db.reset();

    // 重开，验证恢复
    auto db2 = std::make_unique<KVStore>(path, 3);
    auto v2 = db2->get("a");
    std::cout << "After reopen, Get(a) = "
              << (v2.has_value() ? *v2 : "NOT FOUND") << "\n";
        db.reset();  // 关闭

    auto db2 = std::make_unique<KVStore>(path, 3);
    auto v2 = db2->get("a");
    std::cout << "After reopen, Get(a) = "
              << (v2.has_value() ? *v2 : "NOT FOUND") << "\n";
    return 0;
}