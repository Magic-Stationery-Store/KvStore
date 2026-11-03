#pragma once
#include<cstddef>
#include<cstdint>
#include<condition_variable>
#include<exception>
#include<istream>
#include<memory>
#include<map>
#include<mutex>
#include<optional>
#include<string>
#include<thread>
#include<unordered_map>
#include<vector>

#include "sstable.h"
#include "manifest.h"

class KVStore{
    public:
    KVStore();

    explicit KVStore(
        const std::string& log_path,
        std::size_t memtable_entry_limit = 64
    );

    ~KVStore();

    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    void put(const std::string& key, const std::string &value);
    
    std::optional<std::string> get(const std::string &key) const;

    bool remove(const std::string &key);

    void sync();

    // Persist the current MemTable as a new level-0 SSTable.
    void flush();

    void compact();

    // ---- Read-only inspection (debugging tools, GUI) -------------------
    // These helpers take the same mutex as normal reads; they never mutate
    // storage.  Values from the base file are read on demand.

    struct InspectRow {
        std::string layer;
        std::string key;
        std::optional<std::string> value; // nullopt = tombstone
    };

    struct ProbeRow {
        std::string layer;
        bool found = false;
        std::optional<std::string> value; // valid when found
    };

    struct TableInfo {
        std::string kind;                 // "L0" or "BASE"
        std::uint64_t generation = 0;
        std::string path;
        std::size_t entries = 0;
    };

    std::vector<InspectRow> inspect_entries(
        std::size_t limit = 5000) const;

    std::vector<ProbeRow> explain_key(const std::string& key) const;

    std::vector<TableInfo> inspect_tables() const;

    ManifestState manifest_snapshot() const;

    std::size_t memtable_entry_count() const;

    bool has_immutable_memtable() const;

    std::string current_base_path() const;

    std::string wal_file_path() const;

    std::string manifest_file_path() const;

    private:
    class ProcessLock;

    struct IndexEntry {
        std::uint64_t value_offset;
        std::uint32_t value_size;
    };

    void replay();

    void replay_wal();

    void load_sstables();

    void initialize_manifest();
    void publish_manifest(ManifestState state);
    void cleanup_obsolete_files();

    void freeze_memtable_locked(
        std::unique_lock<std::mutex>& lock
    );

    void wait_for_background_flush_locked(
        std::unique_lock<std::mutex>& lock
    );

    void background_flush_loop();

    void rethrow_background_error_locked() const;

    void recover_immutable_wals();

    std::optional<std::string>
        get_locked(const std::string& key) const;

    std::string sstable_path_for_generation(
        std::uint64_t generation
    ) const;

    std::string immutable_wal_path_for_generation(
        std::uint64_t generation
    ) const;

    std::string read_value(
        std::istream& input,
        const IndexEntry& entry
    ) const;

    std::unordered_map<std::string, std::string>
        memory_data_;

    std::unordered_map<std::string, IndexEntry>
        index_;

    // Only writes since the last compaction live here. nullopt is a tombstone.
    std::map<std::string, std::optional<std::string>> memtable_;

    // The active MemTable is swapped here while a worker writes it to L0.
    std::shared_ptr<const SSTable::Entries> immutable_memtable_;
    std::uint64_t immutable_generation_ = 0;
    std::string immutable_wal_path_;

    struct Level0Table {
        std::uint64_t generation;
        std::string path;
        std::unique_ptr<SSTable> table;
    };

    // Stored from oldest to newest. Reads walk this vector backwards.
    std::vector<Level0Table> level0_tables_;

    std::uint64_t next_sstable_generation_ = 1;

    std::size_t memtable_entry_limit_ = 64;

    std::string log_path_;

    std::string wal_path_;

    std::string manifest_path_;
    std::string base_path_;
    ManifestState manifest_;

    bool dirty_ = false;

    bool stop_background_ = false;
    std::exception_ptr background_error_;

    mutable std::mutex mutex_;
    std::condition_variable background_condition_;
    std::thread background_thread_;
    std::unique_ptr<ProcessLock> process_lock_;
};
