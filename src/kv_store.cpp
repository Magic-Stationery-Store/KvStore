#include"kv_store.h"
#include "log_record.h"
#include "platform.h"
#include <algorithm>
#include <cerrno>
#include<fstream>
#include <iomanip>
#include <sstream>
#include<stdexcept>
#include <filesystem>
#include <limits>
#include <system_error>

namespace {

bool ensure_log_file(const std::string& path) {
    {
        std::ofstream create(path, std::ios::binary | std::ios::app);
        if (!create.is_open()) {
            throw std::runtime_error("Cannot create log file");
        }
        create.close();
        if (!create) {
            throw std::runtime_error("Cannot close new log file");
        }
    }

    std::ifstream inspect(path, std::ios::binary | std::ios::ate);
    if (!inspect.is_open()) {
        throw std::runtime_error("Cannot inspect log file");
    }
    const std::streampos size = inspect.tellg();
    if (size == std::streampos(-1)) {
        throw std::runtime_error("Cannot determine log file size");
    }
    inspect.close();

    if (size != std::streampos(0)) {
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("Cannot initialize log file");
    }
    write_log_header(output);
    output.close();
    if (!output) {
        throw std::runtime_error("Cannot finish log file header");
    }
    return true;
}

template <typename Apply>
bool replay_file(const std::string& path, Apply apply) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("Cannot open log for replay");
    }
    read_log_header(input);

    while (true) {
        const std::streampos record_start = input.tellg();
        if (record_start == std::streampos(-1)) {
            throw std::runtime_error("Cannot get record position");
        }

        LogRecord record;
        RecordLocation location{};
        try {
            if (!read_record(input, record, location)) {
                return false;
            }
        } catch (const IncompleteRecordError&) {
            input.clear();
            input.close();
            if (input.fail()) {
                throw std::runtime_error("Cannot close log before repair");
            }

            const auto offset = static_cast<std::streamoff>(record_start);
            if (offset < 0) {
                throw std::runtime_error("Invalid repair offset");
            }
            std::filesystem::resize_file(
                path, static_cast<std::uintmax_t>(offset));
            return true;
        }

        apply(record, location);
    }
}

void replace_with_empty_log(
    const std::string& path,
    const std::string& temporary_suffix
) {
    const std::filesystem::path temporary_path =
        std::filesystem::path(path).concat(temporary_suffix);
    std::error_code error;
    std::filesystem::remove(temporary_path, error);
    if (error) {
        throw std::runtime_error(
            "Cannot remove stale WAL temporary file: "
            + error.message());
    }

    try {
        std::ofstream output(
            temporary_path,
            std::ios::binary | std::ios::trunc
        );
        if (!output.is_open()) {
            throw std::runtime_error("Cannot create replacement WAL");
        }
        write_log_header(output);
        output.close();
        if (!output) {
            throw std::runtime_error("Cannot finish replacement WAL");
        }

        kvplatform::sync_file(temporary_path);
        try {
            kvplatform::replace_file(temporary_path, path);
        } catch (const std::exception& replace_error) {
            throw std::runtime_error(
                std::string("Cannot replace WAL: ")
                + replace_error.what());
        }
        kvplatform::sync_parent_directory(path);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        throw;
    }
}

} // namespace

class KVStore::ProcessLock {
public:
    explicit ProcessLock(const std::string& log_path)
        : lock_(
              std::filesystem::absolute(log_path)
                  .lexically_normal()
                  .string()
              + ".lock") {}

private:
    kvplatform::FileLock lock_;
};

KVStore::KVStore() = default;
KVStore::~KVStore() {
    if (!background_thread_.joinable()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        stop_background_ = true;
    }
    background_condition_.notify_all();
    background_thread_.join();
}

KVStore::KVStore(
    const std::string& log_path,
    std::size_t memtable_entry_limit
)
    : memtable_entry_limit_(memtable_entry_limit),
      log_path_(log_path) {
    if (log_path_.empty()) {
        throw std::invalid_argument(
            "Log path cannot be empty");
    }
    if (memtable_entry_limit_ == 0) {
        throw std::invalid_argument(
            "MemTable entry limit must be greater than zero");
    }
    process_lock_ = std::make_unique<ProcessLock>(log_path_);
    wal_path_ = log_path_ + ".wal";
    manifest_path_ = log_path_ + ".manifest";
    initialize_manifest();
    recover_immutable_wals();
    cleanup_obsolete_files();
    dirty_ = ensure_log_file(wal_path_) || dirty_;
    replay_wal();
    background_thread_ =
        std::thread(&KVStore::background_flush_loop, this);
}
void KVStore::put(const std::string& key,
                  const std::string& value) {
    std::unique_lock<std::mutex> lock(mutex_);
    rethrow_background_error_locked();
    if (log_path_.empty()) {
        memory_data_[key] = value;
        return;
    }

    std::ofstream output(
        wal_path_,
        std::ios::binary | std::ios::app
    );

    if (!output.is_open()) {
        throw std::runtime_error(
            "Cannot open log for writing");
    }

    write_record(
        output,
        LogRecord{Operation::Put, key, value}
    );

    output.close();

    if (!output) {
        throw std::runtime_error(
            "Cannot close log after writing");
    }

    dirty_ = true;
    memtable_[key] = value;
    if (memtable_.size() >= memtable_entry_limit_) {
        freeze_memtable_locked(lock);
    }
}
std::optional<std::string>
KVStore::get(const std::string& key) const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();
    return get_locked(key);
}

std::optional<std::string>
KVStore::get_locked(const std::string& key) const {
    if (log_path_.empty()) {
        const auto it = memory_data_.find(key);

        if (it == memory_data_.end()) {
            return std::nullopt;
        }

        return it->second;
    }

    const auto pending = memtable_.find(key);
    if (pending != memtable_.end()) {
        return pending->second;
    }

    if (immutable_memtable_) {
        const auto immutable = immutable_memtable_->find(key);
        if (immutable != immutable_memtable_->end()) {
            return immutable->second;
        }
    }

    for (auto table = level0_tables_.rbegin();
         table != level0_tables_.rend();
         ++table) {
        const SSTableLookupResult result =
            table->table->get(key);
        if (result.found) {
            return result.value;
        }
    }

    const auto it = index_.find(key);

    if (it == index_.end()) {
        return std::nullopt;
    }

    std::ifstream input(
        base_path_,
        std::ios::binary
    );

    if (!input.is_open()) {
        throw std::runtime_error(
            "Cannot open log for reading value");
    }

    return read_value(input, it->second);
}

std::string KVStore::read_value(
    std::istream& input,
    const IndexEntry& entry
) const {
    if (entry.value_offset
        > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error(
            "Value offset is too large for this platform");
    }

    input.seekg(
        static_cast<std::streamoff>(entry.value_offset),
        std::ios::beg
    );

    if (!input) {
        throw std::runtime_error(
            "Cannot seek to value position");
    }

    std::string value(entry.value_size, '\0');

    if (entry.value_size != 0) {
        input.read(
            value.data(),
            static_cast<std::streamsize>(entry.value_size)
        );

        if (!input) {
            throw std::runtime_error(
                "Cannot read value from log");
        }
    }

    return value;
}
bool KVStore::remove(const std::string& key) {
    std::unique_lock<std::mutex> lock(mutex_);
    rethrow_background_error_locked();
    if (log_path_.empty()) {
        return memory_data_.erase(key) > 0;
    }

    const bool exists = get_locked(key).has_value();

    if (!exists) {
        return false;
    }

    std::ofstream output(
        wal_path_,
        std::ios::binary | std::ios::app
    );

    if (!output.is_open()) {
        throw std::runtime_error(
            "Cannot open log for writing");
    }

    write_record(
        output,
        LogRecord{Operation::Delete, key, ""}
    );

    output.close();

    if (!output) {
        throw std::runtime_error(
            "Cannot close log after writing");
    }

    dirty_ = true;
    memtable_[key] = std::nullopt;
    if (memtable_.size() >= memtable_entry_limit_) {
        freeze_memtable_locked(lock);
    }
    return true;
}

void KVStore::sync() {
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();
    if (log_path_.empty() || !dirty_) {
        return;
    }

    kvplatform::sync_file(base_path_);
    kvplatform::sync_file(wal_path_);
    kvplatform::sync_parent_directory(log_path_);

    dirty_ = false;
}

void KVStore::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    rethrow_background_error_locked();
    if (!log_path_.empty() && !memtable_.empty()) {
        freeze_memtable_locked(lock);
    }
    wait_for_background_flush_locked(lock);
}

std::string KVStore::sstable_path_for_generation(
    std::uint64_t generation
) const {
    const std::filesystem::path base =
        std::filesystem::absolute(log_path_).lexically_normal();
    std::ostringstream filename;
    filename
        << base.filename().string()
        << ".L0."
        << std::setw(20)
        << std::setfill('0')
        << generation
        << ".sst";
    return (base.parent_path() / filename.str()).string();
}

std::string KVStore::immutable_wal_path_for_generation(
    std::uint64_t generation
) const {
    const std::filesystem::path base =
        std::filesystem::absolute(log_path_).lexically_normal();
    std::ostringstream filename;
    filename
        << base.filename().string()
        << ".wal.L0."
        << std::setw(20)
        << std::setfill('0')
        << generation
        << ".imm";
    return (base.parent_path() / filename.str()).string();
}

void KVStore::publish_manifest(ManifestState state) {
    state.next_generation = next_sstable_generation_;
    try {
        write_manifest(manifest_path_, state);
    } catch (...) {
        // A failed directory sync may happen after rename. Do not continue
        // using a possibly different in-memory version; reopen for recovery.
        background_error_ = std::current_exception();
        throw;
    }
    manifest_ = std::move(state);
}

void KVStore::initialize_manifest() {
    if (std::filesystem::exists(manifest_path_)) {
        manifest_ = read_manifest(manifest_path_);
        next_sstable_generation_ = manifest_.next_generation;
        base_path_ = base_file_path(log_path_, manifest_.base_generation);
        if (!std::filesystem::exists(base_path_)) {
            throw std::runtime_error("MANIFEST references a missing base file");
        }
        replay();
        for (const auto generation : manifest_.level0) {
            const std::string path = sstable_path_for_generation(generation);
            level0_tables_.push_back(Level0Table{
                generation, path, std::make_unique<SSTable>(path)
            });
        }
        return;
    }

    // One-time migration from the previous format. Validate before publishing.
    base_path_ = log_path_;
    dirty_ = ensure_log_file(base_path_);
    replay();
    load_sstables();
    ManifestState state;
    for (const auto& table : level0_tables_) {
        state.level0.push_back(table.generation);
    }
    kvplatform::sync_file(base_path_);
    for (const auto& table : level0_tables_) {
        kvplatform::sync_file(table.path);
    }
    kvplatform::sync_parent_directory(log_path_);
    publish_manifest(std::move(state));
}

void KVStore::cleanup_obsolete_files() {
    const auto database = std::filesystem::absolute(log_path_).lexically_normal();
    const std::string name = database.filename().string();
    const auto parse_id = [](const std::string& filename,
                             const std::string& prefix,
                             const std::string& suffix) -> std::optional<std::uint64_t> {
        if (filename.size() <= prefix.size() + suffix.size()
            || filename.compare(0, prefix.size(), prefix) != 0
            || filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return std::nullopt;
        }
        const std::string digits = filename.substr(prefix.size(),
            filename.size() - prefix.size() - suffix.size());
        if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        try { return std::stoull(digits); } catch (const std::exception&) { return std::nullopt; }
    };
    bool removed = false;
    for (const auto& item : std::filesystem::directory_iterator(database.parent_path())) {
        if (!item.is_regular_file()) { continue; }
        const std::string filename = item.path().filename().string();
        const auto table = parse_id(filename, name + ".L0.", ".sst");
        const auto base = parse_id(filename, name + ".base.", ".log");
        const bool obsolete_table = table && !std::binary_search(
            manifest_.level0.begin(), manifest_.level0.end(), *table);
        const bool obsolete_base = base && *base != manifest_.base_generation;
        const bool temporary = parse_id(filename, name + ".L0.", ".sst.tmp")
            || parse_id(filename, name + ".base.", ".log.tmp")
            || filename == name + ".manifest.tmp";
        if (obsolete_table || obsolete_base || temporary) {
            std::filesystem::remove(item.path());
            removed = true;
        }
    }
    if (removed) { kvplatform::sync_parent_directory(log_path_); }
}

void KVStore::load_sstables() {
    const std::filesystem::path base =
        std::filesystem::absolute(log_path_).lexically_normal();
    const std::string prefix =
        base.filename().string() + ".L0.";
    const std::string suffix = ".sst";

    std::vector<std::pair<std::uint64_t, std::string>> files;
    for (const auto& item :
         std::filesystem::directory_iterator(base.parent_path())) {
        if (!item.is_regular_file()) {
            continue;
        }
        const std::string name = item.path().filename().string();
        if (name.size() <= prefix.size() + suffix.size()
            || name.compare(0, prefix.size(), prefix) != 0
            || name.compare(
                   name.size() - suffix.size(),
                   suffix.size(), suffix) != 0) {
            continue;
        }

        const std::string number = name.substr(
            prefix.size(),
            name.size() - prefix.size() - suffix.size()
        );
        const bool all_digits = !number.empty()
            && std::all_of(
                number.begin(), number.end(),
                [](char character) {
                    return character >= '0' && character <= '9';
                });
        if (!all_digits) {
            continue;
        }

        std::size_t parsed = 0;
        const std::uint64_t generation =
            std::stoull(number, &parsed);
        if (parsed == number.size()) {
            files.emplace_back(
                generation,
                item.path().string()
            );
        }
    }

    std::sort(files.begin(), files.end());
    for (const auto& [generation, path] : files) {
        level0_tables_.push_back(Level0Table{
            generation,
            path,
            std::make_unique<SSTable>(path)
        });
        if (generation
            == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error(
                "SSTable generation number is exhausted");
        }
        next_sstable_generation_ =
            std::max(next_sstable_generation_, generation + 1);
    }
}

void KVStore::recover_immutable_wals() {
    const std::filesystem::path base =
        std::filesystem::absolute(log_path_).lexically_normal();
    const std::string prefix =
        base.filename().string() + ".wal.L0.";
    const std::string suffix = ".imm";
    std::vector<std::pair<std::uint64_t, std::string>> files;

    for (const auto& item :
         std::filesystem::directory_iterator(base.parent_path())) {
        if (!item.is_regular_file()) {
            continue;
        }
        const std::string name = item.path().filename().string();
        if (name.size() <= prefix.size() + suffix.size()
            || name.compare(0, prefix.size(), prefix) != 0
            || name.compare(
                   name.size() - suffix.size(),
                   suffix.size(), suffix) != 0) {
            continue;
        }
        const std::string number = name.substr(
            prefix.size(),
            name.size() - prefix.size() - suffix.size()
        );
        const bool all_digits = !number.empty()
            && std::all_of(
                number.begin(), number.end(),
                [](char character) {
                    return character >= '0' && character <= '9';
                });
        if (!all_digits) {
            continue;
        }
        std::size_t parsed = 0;
        const std::uint64_t generation =
            std::stoull(number, &parsed);
        if (parsed == number.size()) {
            files.emplace_back(generation, item.path().string());
        }
    }

    std::sort(files.begin(), files.end());
    for (const auto& [generation, immutable_wal] : files) {
        if (generation <= manifest_.retired_through) {
            std::filesystem::remove(immutable_wal);
            kvplatform::sync_parent_directory(immutable_wal);
            continue;
        }
        if (generation == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("SSTable generation number is exhausted");
        }
        next_sstable_generation_ =
            std::max(next_sstable_generation_, generation + 1);
        const auto existing = std::find_if(
            level0_tables_.begin(),
            level0_tables_.end(),
            [generation](const Level0Table& state) {
                return state.generation == generation;
            }
        );

        if (existing == level0_tables_.end()) {
            SSTable::Entries recovered;
            replay_file(
                immutable_wal,
                [&recovered](
                    const LogRecord& record,
                    const RecordLocation&) {
                    if (record.operation == Operation::Put) {
                        recovered[record.key] = record.value;
                    } else {
                        recovered[record.key] = std::nullopt;
                    }
                }
            );

            const std::string table_path =
                sstable_path_for_generation(generation);
            // An SSTable without a MANIFEST entry is uncommitted. Rebuild it
            // from the immutable WAL, even if its bytes look valid.
            std::filesystem::remove(table_path);
            SSTable::build(table_path, recovered);
            auto table = std::make_unique<SSTable>(table_path);
            auto state = manifest_;
            state.level0.push_back(generation);
            std::sort(state.level0.begin(), state.level0.end());
            level0_tables_.reserve(level0_tables_.size() + 1);
            publish_manifest(std::move(state));
            level0_tables_.push_back(Level0Table{
                generation,
                table_path,
                std::move(table)
            });
        }

        if (generation
            == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error(
                "SSTable generation number is exhausted");
        }
        next_sstable_generation_ =
            std::max(next_sstable_generation_, generation + 1);

        std::error_code error;
        std::filesystem::remove(immutable_wal, error);
        if (error) {
            throw std::runtime_error(
                "Cannot remove recovered immutable WAL: "
                + error.message());
        }
        kvplatform::sync_parent_directory(immutable_wal);
    }

    std::sort(
        level0_tables_.begin(),
        level0_tables_.end(),
        [](const Level0Table& left, const Level0Table& right) {
            return left.generation < right.generation;
        }
    );
}

void KVStore::rethrow_background_error_locked() const {
    if (background_error_) {
        std::rethrow_exception(background_error_);
    }
}

void KVStore::wait_for_background_flush_locked(
    std::unique_lock<std::mutex>& lock
) {
    background_condition_.wait(
        lock,
        [this] {
            return !immutable_memtable_ || background_error_;
        }
    );
    rethrow_background_error_locked();
}

void KVStore::freeze_memtable_locked(
    std::unique_lock<std::mutex>& lock
) {
    if (log_path_.empty() || memtable_.empty()) {
        return;
    }
    if (immutable_memtable_) {
        wait_for_background_flush_locked(lock);
    }
    if (next_sstable_generation_
        == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(
            "SSTable generation number is exhausted");
    }

    auto frozen = std::make_shared<SSTable::Entries>();
    frozen->swap(memtable_);

    const std::uint64_t generation =
        next_sstable_generation_++;
    const std::string immutable_wal =
        immutable_wal_path_for_generation(generation);

    try {
        // The immutable WAL owns the frozen snapshot. The new active WAL is
        // made durable before foreground writes are allowed to continue.
        kvplatform::sync_file(base_path_);
        kvplatform::sync_file(wal_path_);
        try {
            kvplatform::rename_file(wal_path_, immutable_wal);
        } catch (const std::exception& rename_error) {
            throw std::runtime_error(
                std::string("Cannot rotate active WAL: ")
                + rename_error.what());
        }
        kvplatform::sync_parent_directory(immutable_wal);

        try {
            ensure_log_file(wal_path_);
            kvplatform::sync_file(wal_path_);
            kvplatform::sync_parent_directory(wal_path_);
        } catch (...) {
            try {
                kvplatform::rename_file(immutable_wal, wal_path_);
            } catch (...) {
                // Keep the original error: recovery will report the state.
            }
            throw;
        }
    } catch (...) {
        frozen->swap(memtable_);
        throw;
    }

    immutable_memtable_ = std::move(frozen);
    immutable_generation_ = generation;
    immutable_wal_path_ = immutable_wal;
    dirty_ = false;
    background_condition_.notify_all();
}

void KVStore::background_flush_loop() {
    while (true) {
        std::shared_ptr<const SSTable::Entries> snapshot;
        std::uint64_t generation = 0;
        std::string immutable_wal;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            background_condition_.wait(
                lock,
                [this] {
                    return stop_background_ || immutable_memtable_;
                }
            );
            if (!immutable_memtable_) {
                if (stop_background_) {
                    return;
                }
                continue;
            }
            snapshot = immutable_memtable_;
            generation = immutable_generation_;
            immutable_wal = immutable_wal_path_;
        }

        try {
            const std::string table_path =
                sstable_path_for_generation(generation);
            SSTable::build(table_path, *snapshot);
            auto table = std::make_unique<SSTable>(table_path);

            {
                const std::lock_guard<std::mutex> guard(mutex_);
                auto state = manifest_;
                state.level0.push_back(generation);
                level0_tables_.reserve(level0_tables_.size() + 1);
                // Commit the new table before deleting its recovery source.
                publish_manifest(std::move(state));
                std::error_code remove_error;
                std::filesystem::remove(
                    immutable_wal, remove_error);
                if (remove_error) {
                    throw std::runtime_error(
                        "Cannot remove flushed immutable WAL: "
                        + remove_error.message());
                }
                kvplatform::sync_parent_directory(immutable_wal);

                level0_tables_.push_back(Level0Table{
                    generation,
                    table_path,
                    std::move(table)
                });
                immutable_memtable_.reset();
                immutable_generation_ = 0;
                immutable_wal_path_.clear();
            }
            background_condition_.notify_all();
        } catch (...) {
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                background_error_ = std::current_exception();
                stop_background_ = true;
            }
            background_condition_.notify_all();
            return;
        }
    }
}

void KVStore::compact() {
    std::unique_lock<std::mutex> lock(mutex_);
    rethrow_background_error_locked();
    if (log_path_.empty()) { return; }
    if (!memtable_.empty()) { freeze_memtable_locked(lock); }
    wait_for_background_flush_locked(lock);

    // Collect a consistent version while holding the mutex. A writer may have
    // added active entries while our condition-variable wait released it.
    std::ifstream source(base_path_, std::ios::binary);
    if (!source) { throw std::runtime_error("Cannot open base for compaction"); }
    std::map<std::string, std::string> live_values;
    for (const auto& [key, entry] : index_) {
        live_values[key] = read_value(source, entry);
    }
    source.close();
    for (const auto& table : level0_tables_) {
        for (const auto& [key, value] : table.table->read_all()) {
            if (value) { live_values[key] = *value; }
            else { live_values.erase(key); }
        }
    }
    for (const auto& [key, value] : memtable_) {
        if (value) { live_values[key] = *value; }
        else { live_values.erase(key); }
    }

    if (next_sstable_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("Base generation number is exhausted");
    }
    const auto generation = next_sstable_generation_++;
    const std::string new_base = base_file_path(log_path_, generation);
    const std::string temporary = new_base + ".tmp";
    std::unordered_map<std::string, IndexEntry> new_index;
    new_index.reserve(live_values.size());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { throw std::runtime_error("Cannot create compacted base"); }
    write_log_header(output);
    for (const auto& [key, value] : live_values) {
        const auto location = write_record(output, LogRecord{Operation::Put, key, value});
        new_index.emplace(key, IndexEntry{location.value_offset, location.value_size});
    }
    output.close();
    if (!output) { throw std::runtime_error("Cannot finish compacted base"); }
    kvplatform::sync_file(temporary);
    try {
        kvplatform::rename_file(temporary, new_base);
    } catch (const std::exception& rename_error) {
        throw std::runtime_error(
            std::string("Cannot publish compacted base: ")
            + rename_error.what());
    }
    kvplatform::sync_parent_directory(new_base);

    // One metadata replacement commits the new base AND retires every L0.
    // Before this commit the old version remains intact. Afterwards leftover
    // old files are ignored and can be removed on the next startup.
    ManifestState state;
    state.base_generation = generation;
    state.retired_through = generation;
    publish_manifest(std::move(state));
    base_path_ = new_base;
    index_ = std::move(new_index);
    level0_tables_.clear();
    replace_with_empty_log(wal_path_, ".compact.tmp");
    memtable_.clear();
    dirty_ = false;
    cleanup_obsolete_files();
}

void KVStore::replay() {
    std::ifstream input(
        base_path_,
        std::ios::binary
    );

    if (!input.is_open()) {
        throw std::runtime_error(
            "Cannot open log for reading");
    }

    read_log_header(input);

    while (true) {
        const std::streampos record_start =
            input.tellg();

        if (record_start == std::streampos(-1)) {
            throw std::runtime_error(
                "Failed to get record position");
        }

        LogRecord record;
        RecordLocation location{};

        try {
            if (!read_record(input, record, location)) {
                break;
            }
        } catch (const IncompleteRecordError&) {
            /*
             * 读取失败后，流带有 failbit/eofbit。
             * clear() 只清除状态标志，不修改文件内容。
             */
            input.clear();
            input.close();

            if (input.fail()) {
                throw std::runtime_error(
                    "Failed to close log before repair");
            }

            const auto new_size =
                static_cast<std::uintmax_t>(
                    static_cast<std::streamoff>(
                        record_start
                    )
                );

            std::error_code error;

            std::filesystem::resize_file(
                base_path_,
                new_size,
                error
            );

            if (error) {
                throw std::runtime_error(
                    "Failed to repair incomplete log: "
                    + error.message());
            }

            dirty_ = true;

            // 残缺记录已删除，之前的完整记录已经恢复。
            return;
        }

        if (record.operation == Operation::Put) {
            index_[record.key] = IndexEntry{
                location.value_offset,
                location.value_size
            };
        } else {
            index_.erase(record.key);
        }
    }
}

void KVStore::replay_wal() {
    const bool repaired = replay_file(
        wal_path_,
        [this](const LogRecord& record, const RecordLocation&) {
            if (record.operation == Operation::Put) {
                memtable_[record.key] = record.value;
            } else {
                memtable_[record.key] = std::nullopt;
            }
        }
    );
    dirty_ = dirty_ || repaired;
}

std::vector<KVStore::InspectRow>
KVStore::inspect_entries(std::size_t limit) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();

    std::vector<InspectRow> rows;
    rows.reserve(std::min<std::size_t>(limit, 256));

    const auto push = [&rows, limit](
                          const std::string& layer,
                          const std::string& key,
                          const std::optional<std::string>& value) {
        if (rows.size() >= limit) {
            return false;
        }
        rows.push_back(InspectRow{layer, key, value});
        return true;
    };

    if (limit == 0) {
        return rows;
    }

    if (log_path_.empty()) {
        for (const auto& [key, value] : memory_data_) {
            if (!push("Memory", key, value)) {
                break;
            }
        }
        return rows;
    }

    for (const auto& [key, value] : memtable_) {
        if (!push("Active", key, value)) {
            return rows;
        }
    }

    if (immutable_memtable_) {
        for (const auto& [key, value] : *immutable_memtable_) {
            if (!push("Immutable", key, value)) {
                return rows;
            }
        }
    }

    for (auto table = level0_tables_.rbegin();
         table != level0_tables_.rend();
         ++table) {
        const std::string layer =
            "L0#" + std::to_string(table->generation);
        for (const auto& [key, value] : table->table->read_all()) {
            if (!push(layer, key, value)) {
                return rows;
            }
        }
    }

    if (!index_.empty()) {
        std::ifstream input(base_path_, std::ios::binary);
        if (!input.is_open()) {
            throw std::runtime_error(
                "Cannot open base file for inspection");
        }

        std::vector<std::string> keys;
        keys.reserve(index_.size());
        for (const auto& [key, entry] : index_) {
            (void)entry;
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());

        for (const auto& key : keys) {
            if (!push("BASE", key, read_value(input, index_.at(key)))) {
                return rows;
            }
        }
    }

    return rows;
}

std::vector<KVStore::ProbeRow>
KVStore::explain_key(const std::string& key) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();

    std::vector<ProbeRow> rows;
    const auto add = [&rows](
                         const std::string& layer,
                         bool found,
                         std::optional<std::string> value) {
        rows.push_back(ProbeRow{layer, found, std::move(value)});
    };

    if (log_path_.empty()) {
        const auto it = memory_data_.find(key);
        if (it == memory_data_.end()) {
            add("Memory", false, std::nullopt);
        } else {
            add("Memory", true, it->second);
        }
        return rows;
    }

    const auto active = memtable_.find(key);
    if (active == memtable_.end()) {
        add("Active", false, std::nullopt);
    } else {
        add("Active", true, active->second);
    }

    if (immutable_memtable_) {
        const auto immutable = immutable_memtable_->find(key);
        if (immutable == immutable_memtable_->end()) {
            add("Immutable", false, std::nullopt);
        } else {
            add("Immutable", true, immutable->second);
        }
    } else {
        add("Immutable", false, std::nullopt);
    }

    for (auto table = level0_tables_.rbegin();
         table != level0_tables_.rend();
         ++table) {
        const SSTableLookupResult result = table->table->get(key);
        add(
            "L0#" + std::to_string(table->generation),
            result.found,
            result.value
        );
    }

    const auto base = index_.find(key);
    if (base == index_.end()) {
        add("BASE", false, std::nullopt);
    } else {
        std::ifstream input(base_path_, std::ios::binary);
        if (!input.is_open()) {
            throw std::runtime_error(
                "Cannot open base file for inspection");
        }
        add("BASE", true, read_value(input, base->second));
    }

    return rows;
}

std::vector<KVStore::TableInfo> KVStore::inspect_tables() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();

    std::vector<TableInfo> tables;
    tables.reserve(level0_tables_.size() + 1);

    for (const auto& table : level0_tables_) {
        tables.push_back(TableInfo{
            "L0",
            table.generation,
            table.path,
            table.table->entry_count()
        });
    }

    if (!log_path_.empty()) {
        tables.push_back(TableInfo{
            "BASE",
            manifest_.base_generation,
            base_path_,
            index_.size()
        });
    }

    return tables;
}

ManifestState KVStore::manifest_snapshot() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return manifest_;
}

std::size_t KVStore::memtable_entry_count() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (log_path_.empty()) {
        return memory_data_.size();
    }
    return memtable_.size();
}

bool KVStore::has_immutable_memtable() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return static_cast<bool>(immutable_memtable_);
}

std::string KVStore::current_base_path() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return base_path_;
}

std::string KVStore::wal_file_path() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return wal_path_;
}

std::string KVStore::manifest_file_path() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return manifest_path_;
}
