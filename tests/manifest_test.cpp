#include "kv_store.h"
#include "manifest.h"
#include "src/log_record.h"
#include "test_files.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
    std::cout << "PASS: " << message << '\n';
}

std::string table_path(const std::string& db, std::uint64_t id) {
    std::ostringstream path;
    path << db << ".L0." << std::setw(20) << std::setfill('0') << id << ".sst";
    return path.str();
}

std::string imm_path(const std::string& db, std::uint64_t id) {
    std::ostringstream path;
    path << db << ".wal.L0." << std::setw(20) << std::setfill('0') << id << ".imm";
    return path.str();
}

void write_log(const std::string& path, const std::vector<LogRecord>& records) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    write_log_header(output);
    for (const auto& record : records) { write_record(output, record); }
    output.close();
    if (!output) { throw std::runtime_error("test fixture write failed"); }
}

bool open_rejected(const std::string& db) {
    try { KVStore store(db); } catch (const std::exception&) { return true; }
    return false;
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path()
        / ("kv-manifest-test-" + std::to_string(::getpid()));
    std::filesystem::create_directory(directory);
    try {
        const std::string legacy = (directory / "legacy.log").string();
        write_log(legacy, {{Operation::Put, "key", "legacy"}});
        SSTable::build(table_path(legacy, 3), {{"key", "table"}});
        {
            KVStore store(legacy);
            const auto state = read_manifest(legacy + ".manifest");
            check(store.get("key") == "table" && state.level0 == std::vector<std::uint64_t>{3},
                  "legacy database migrates to MANIFEST without losing L0 data");
        }

        // Disk output exists but was never committed to MANIFEST.
        SSTable::build(table_path(legacy, 99), {{"key", "uncommitted"}});
        write_log(base_file_path(legacy, 100), {{Operation::Put, "key", "wrong"}});
        auto unfinished = read_manifest(legacy + ".manifest");
        unfinished.level0 = {99};
        unfinished.next_generation = 101;
        write_manifest(legacy + ".manifest.tmp", unfinished);
        {
            KVStore store(legacy);
            check(store.get("key") == "table", "uncommitted output and MANIFEST.tmp are ignored");
            check(!std::filesystem::exists(table_path(legacy, 99))
                  && !std::filesystem::exists(base_file_path(legacy, 100)),
                  "orphan data files are reclaimed after recovery");
        }

        // Crash after SSTable rename but before MANIFEST update: replay imm,
        // not the uncommitted SSTable. Active WAL remains newer than both.
        const std::string recovery = (directory / "recovery.log").string();
        { KVStore store(recovery); }
        write_log(imm_path(recovery, 1), {{Operation::Put, "key", "frozen"}});
        SSTable::build(table_path(recovery, 1), {{"key", "bad-output"}});
        write_log(recovery + ".wal", {{Operation::Put, "key", "active"}});
        {
            KVStore store(recovery);
            SSTable recovered(table_path(recovery, 1));
            check(recovered.get("key").value == "frozen" && store.get("key") == "active",
                  "immutable WAL rebuilds uncommitted L0; newer active WAL wins");
        }

        // Crash after metadata commit but before deleting immutable WAL.
        write_log(imm_path(recovery, 1), {{Operation::Put, "key", "stale-duplicate"}});
        {
            KVStore store(recovery);
            check(store.get("key") == "active" && !std::filesystem::exists(imm_path(recovery, 1)),
                  "committed L0 is kept and its duplicate immutable WAL is removed");
        }

        // Save old inputs; after a compaction commit they must never be loaded.
        const std::string compact = (directory / "compact.log").string();
        std::string old_table;
        std::uint64_t old_id = 0;
        const std::string saved = (directory / "saved-input.sst").string();
        {
            KVStore store(compact);
            store.put("deleted", "old");
            store.flush();
            old_id = read_manifest(compact + ".manifest").level0.front();
            old_table = table_path(compact, old_id);
            std::filesystem::copy_file(old_table, saved);
            store.remove("deleted");
            store.put("keep", "new");
            store.compact();
            const auto state = read_manifest(compact + ".manifest");
            check(state.level0.empty() && state.base_generation > old_id
                  && std::filesystem::exists(base_file_path(compact, state.base_generation)),
                  "compaction commits a new base and retires all L0 in one snapshot");
        }
        std::filesystem::copy_file(saved, old_table);
        write_log(imm_path(compact, old_id), {{Operation::Put, "deleted", "old"}});
        {
            KVStore store(compact);
            check(!store.get("deleted") && store.get("keep") == "new",
                  "leftover old SSTable and WAL cannot resurrect deleted keys");
        }

        // Missing metadata references must fail, never silently fall back.
        std::filesystem::rename(table_path(legacy, 3), table_path(legacy, 3) + ".saved");
        check(open_rejected(legacy), "missing referenced SSTable rejects database open");
        std::filesystem::rename(table_path(legacy, 3) + ".saved", table_path(legacy, 3));

        {
            std::ofstream corrupt(legacy + ".manifest", std::ios::binary | std::ios::trunc);
            corrupt << "KV_MANIFEST 1\nNEXT 4\n";
        }
        check(open_rejected(legacy), "truncated MANIFEST rejects open without directory fallback");

        ManifestState valid;
        write_manifest(legacy + ".manifest", valid);
        {
            std::fstream corrupt(legacy + ".manifest", std::ios::binary | std::ios::in | std::ios::out);
            corrupt.seekp(0);
            corrupt.put('X');
        }
        check(open_rejected(legacy), "MANIFEST checksum detects byte corruption");

        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\nFixtures kept at " << directory << '\n';
        return 1;
    }
}
