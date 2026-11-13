#include"kv_store.h"
#include "log_record.h"
#include "platform.h"
#include <algorithm>
#include <cerrno>
#include<fstream>
#include <iomanip>
#include <queue>
#include <set>
#include <sstream>
#include<stdexcept>
#include <filesystem>
#include <limits>
#include <system_error>

namespace {

constexpr std::size_t kInternalSuffixSize = 9;

std::string encode_internal_key(
    const std::string& user_key,
    std::uint64_t sequence,
    bool tombstone
) {
    std::string internal;
    internal.reserve(user_key.size() + kInternalSuffixSize);
    internal.append(user_key);

    const std::uint64_t inverted = ~sequence;
    for (int shift = 56; shift >= 0; shift -= 8) {
        internal.push_back(static_cast<char>(
            (inverted >> shift) & 0xFFu));
    }
    internal.push_back(tombstone ? '\1' : '\0');
    return internal;
}

bool decode_internal_key(
    const std::string& internal,
    std::string& user_key,
    std::uint64_t& sequence,
    bool& tombstone
) {
    if (internal.size() < kInternalSuffixSize) {
        return false;
    }
    const std::size_t offset = internal.size() - kInternalSuffixSize;
    std::uint64_t inverted = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        inverted = (inverted << 8)
            | static_cast<unsigned char>(internal[offset + i]);
    }
    user_key = internal.substr(0, offset);
    sequence = ~inverted;
    tombstone = internal.back() != '\0';
    return true;
}

std::string encode_seek_key(
    const std::string& user_key,
    std::uint64_t sequence
) {
    std::string seek;
    seek.reserve(user_key.size() + kInternalSuffixSize);
    seek.append(user_key);

    const std::uint64_t inverted = ~sequence;
    for (int shift = 56; shift >= 0; shift -= 8) {
        seek.push_back(static_cast<char>(
            (inverted >> shift) & 0xFFu));
    }
    seek.push_back('\0');
    return seek;
}

void append_u32(std::string& output, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<char>(
            (value >> shift) & 0xFFu));
    }
}

std::uint32_t read_u32(
    const std::string& input,
    std::size_t& position
) {
    if (position + 4 > input.size()) {
        throw std::runtime_error("Truncated batch record");
    }
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(input[position++])) << shift;
    }
    return value;
}

std::string encode_batch_payload(
    const std::vector<std::pair<
        Operation,
        std::pair<std::string, std::string>>>& operations
) {
    std::string payload;
    append_u32(payload, static_cast<std::uint32_t>(operations.size()));
    for (const auto& [operation, entry] : operations) {
        payload.push_back(static_cast<char>(operation));
        append_u32(
            payload, static_cast<std::uint32_t>(entry.first.size()));
        append_u32(
            payload, static_cast<std::uint32_t>(entry.second.size()));
        payload.append(entry.first);
        payload.append(entry.second);
    }
    return payload;
}

std::vector<std::pair<Operation, std::pair<std::string, std::string>>>
decode_batch_payload(const std::string& payload) {
    std::size_t position = 0;
    const std::uint32_t count = read_u32(payload, position);
    if (count > 1000000u) {
        throw std::runtime_error("Batch record is too large");
    }

    std::vector<std::pair<
        Operation,
        std::pair<std::string, std::string>>> operations;
    operations.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        if (position >= payload.size()) {
            throw std::runtime_error("Truncated batch record");
        }
        const auto operation = static_cast<Operation>(
            static_cast<unsigned char>(payload[position++]));
        if (operation != Operation::Put
            && operation != Operation::Delete) {
            throw std::runtime_error("Invalid operation in batch record");
        }

        const std::uint32_t key_size = read_u32(payload, position);
        const std::uint32_t value_size = read_u32(payload, position);
        if (position + key_size + value_size > payload.size()) {
            throw std::runtime_error("Truncated batch record");
        }
        std::string key = payload.substr(position, key_size);
        position += key_size;
        std::string value = payload.substr(position, value_size);
        position += value_size;

        if (operation == Operation::Delete && !value.empty()) {
            throw std::runtime_error("DELETE in batch contains a value");
        }
        operations.push_back({
            operation,
            {std::move(key), std::move(value)}
        });
    }

    if (position != payload.size()) {
        throw std::runtime_error("Trailing bytes in batch record");
    }
    return operations;
}

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

struct KVStore::SnapshotRegistry {
    mutable std::mutex mutex;
    std::multiset<std::uint64_t> active;

    void add(std::uint64_t sequence) {
        const std::lock_guard<std::mutex> guard(mutex);
        active.insert(sequence);
    }

    void release(std::uint64_t sequence) {
        const std::lock_guard<std::mutex> guard(mutex);
        const auto it = active.find(sequence);
        if (it != active.end()) {
            active.erase(it);
        }
    }

    std::uint64_t oldest_or(std::uint64_t fallback) const {
        const std::lock_guard<std::mutex> guard(mutex);
        return active.empty() ? fallback : *active.begin();
    }

    bool has_active() const {
        const std::lock_guard<std::mutex> guard(mutex);
        return !active.empty();
    }
};

struct KVStore::Snapshot::State {
    std::uint64_t sequence = 0;
    std::weak_ptr<SnapshotRegistry> registry;

    ~State() {
        if (const auto locked = registry.lock()) {
            locked->release(sequence);
        }
    }
};

KVStore::KVStore() {
    snapshot_registry_ = std::make_shared<SnapshotRegistry>();
}

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
    snapshot_registry_ = std::make_shared<SnapshotRegistry>();
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
        ++last_sequence_;
        return;
    }

    const std::uint64_t sequence = ++last_sequence_;
    const std::string internal =
        encode_internal_key(key, sequence, false);

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
        LogRecord{Operation::Put, internal, value}
    );

    output.close();

    if (!output) {
        throw std::runtime_error(
            "Cannot close log after writing");
    }

    dirty_ = true;
    memtable_[internal] = value;
    if (memtable_.size() >= memtable_entry_limit_) {
        freeze_memtable_locked(lock);
    }
}

std::optional<std::string>
KVStore::get(const std::string& key) const
{
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();
    const LookupResult result =
        lookup_locked(key, last_sequence_);
    return result.value;
}

std::optional<std::string>
KVStore::get(
    const std::string& key,
    const Snapshot& snapshot
) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();
    const LookupResult result =
        lookup_locked(key, snapshot.sequence());
    return result.value;
}

KVStore::LookupResult KVStore::lookup_locked(
    const std::string& key,
    std::uint64_t sequence
) const {
    if (log_path_.empty()) {
        const auto it = memory_data_.find(key);
        if (it == memory_data_.end()) {
            return LookupResult{};
        }
        return LookupResult{true, it->second};
    }

    const std::string seek = encode_seek_key(key, sequence);

    const auto matches = [&key](
        const std::string& internal,
        const std::optional<std::string>& value
    ) -> std::optional<LookupResult> {
        std::string decoded_key;
        std::uint64_t decoded_sequence = 0;
        bool tombstone = false;
        if (!decode_internal_key(
                internal, decoded_key, decoded_sequence, tombstone)) {
            return std::nullopt;
        }
        if (decoded_key != key) {
            return std::nullopt;
        }
        return LookupResult{true, value};
    };

    const auto active = memtable_.lower_bound(seek);
    if (active != memtable_.end()) {
        const auto result = matches(active->first, active->second);
        if (result.has_value()) {
            return *result;
        }
    }

    if (immutable_memtable_) {
        const auto immutable = immutable_memtable_->lower_bound(seek);
        if (immutable != immutable_memtable_->end()) {
            const auto result = matches(immutable->first, immutable->second);
            if (result.has_value()) {
                return *result;
            }
        }
    }

    for (auto table = level0_tables_.rbegin();
         table != level0_tables_.rend();
         ++table) {
        const SSTableSeekResult result = table->table->seek(seek);
        if (!result.found) {
            continue;
        }
        const auto match = matches(result.key, result.value);
        if (match.has_value()) {
            return *match;
        }
    }

    const auto base = index_.lower_bound(seek);
    if (base == index_.end()) {
        return LookupResult{};
    }

    std::string decoded_key;
    std::uint64_t decoded_sequence = 0;
    bool tombstone = false;
    if (!decode_internal_key(
            base->first, decoded_key, decoded_sequence, tombstone)) {
        return LookupResult{};
    }
    if (decoded_key != key) {
        return LookupResult{};
    }
    if (base->second.tombstone) {
        return LookupResult{true, std::nullopt};
    }

    std::ifstream input(base_path_, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error(
            "Cannot open log for reading value");
    }
    return LookupResult{true, read_value(input, base->second)};
}

std::string KVStore::make_internal_key(
    const std::string& key,
    std::uint64_t sequence,
    bool tombstone
) {
    return encode_internal_key(key, sequence, tombstone);
}

void KVStore::observe_internal_key(const std::string& internal_key) {
    std::string key;
    std::uint64_t sequence = 0;
    bool tombstone = false;
    if (decode_internal_key(
            internal_key, key, sequence, tombstone)) {
        last_sequence_ = std::max(last_sequence_, sequence);
    }
}

void KVStore::apply_log_record(const LogRecord& record) {
    if (record.operation == Operation::Batch) {
        for (const auto& [operation, entry] :
             decode_batch_payload(record.value)) {
            observe_internal_key(entry.first);
            if (operation == Operation::Put) {
                memtable_[entry.first] = entry.second;
            } else {
                memtable_[entry.first] = std::nullopt;
            }
        }
        return;
    }

    observe_internal_key(record.key);
    if (record.operation == Operation::Put) {
        memtable_[record.key] = record.value;
    } else {
        memtable_[record.key] = std::nullopt;
    }
}

void KVStore::WriteBatch::put(
    const std::string& key,
    const std::string& value
) {
    items_.push_back(Item{Operation::Put, key, value});
}

void KVStore::WriteBatch::erase(const std::string& key) {
    items_.push_back(Item{Operation::Delete, key, ""});
}

std::size_t KVStore::WriteBatch::size() const noexcept {
    return items_.size();
}

void KVStore::write(const WriteBatch& batch) {
    std::unique_lock<std::mutex> lock(mutex_);
    rethrow_background_error_locked();

    if (batch.items_.empty()) {
        return;
    }

    if (log_path_.empty()) {
        for (const auto& item : batch.items_) {
            if (item.operation == Operation::Put) {
                memory_data_[item.key] = item.value;
            } else {
                memory_data_.erase(item.key);
            }
        }
        ++last_sequence_;
        return;
    }

    const std::uint64_t sequence = ++last_sequence_;

    // A batch may touch the same key multiple times; only the last operation
    // matters, and all operations share one commit sequence.
    std::map<std::string, WriteBatch::Item> collapsed;
    for (const auto& item : batch.items_) {
        collapsed[item.key] = item;
    }

    std::vector<std::pair<
        Operation,
        std::pair<std::string, std::string>>> payload_operations;
    std::vector<std::pair<std::string, std::optional<std::string>>>
        memtable_operations;

    for (const auto& [user_key, item] : collapsed) {
        const bool tombstone = item.operation == Operation::Delete;
        const std::string internal =
            encode_internal_key(user_key, sequence, tombstone);
        payload_operations.push_back({
            item.operation,
            {internal, tombstone ? std::string() : item.value}
        });
        memtable_operations.push_back({
            internal,
            tombstone
                ? std::optional<std::string>{}
                : std::optional<std::string>{item.value}
        });
    }

    const std::string payload = encode_batch_payload(payload_operations);

    std::ofstream output(
        wal_path_,
        std::ios::binary | std::ios::app
    );
    if (!output.is_open()) {
        throw std::runtime_error(
            "Cannot open log for writing batch");
    }

    write_record(
        output,
        LogRecord{Operation::Batch, "", payload}
    );

    output.close();
    if (!output) {
        throw std::runtime_error(
            "Cannot close log after writing batch");
    }

    dirty_ = true;
    for (const auto& [internal, value] : memtable_operations) {
        memtable_[internal] = value;
    }
    if (memtable_.size() >= memtable_entry_limit_) {
        freeze_memtable_locked(lock);
    }
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

    const LookupResult current =
        lookup_locked(key, last_sequence_);
    if (!current.found || !current.value.has_value()) {
        return false;
    }

    const std::uint64_t sequence = ++last_sequence_;
    const std::string internal =
        encode_internal_key(key, sequence, true);

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
        LogRecord{Operation::Delete, internal, ""}
    );

    output.close();

    if (!output) {
        throw std::runtime_error(
            "Cannot close log after writing");
    }

    dirty_ = true;
    memtable_[internal] = std::nullopt;
    if (memtable_.size() >= memtable_entry_limit_) {
        freeze_memtable_locked(lock);
    }
    return true;
}

std::optional<std::string>
KVStore::get_locked(const std::string& key) const {
    return lookup_locked(key, last_sequence_).value;
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
            auto table = std::make_unique<SSTable>(path);
            for (const auto& [key, value] : table->read_all()) {
                (void)value;
                observe_internal_key(key);
            }
            level0_tables_.push_back(Level0Table{
                generation, path, std::move(table)
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
        auto table = std::make_unique<SSTable>(path);
        for (const auto& [key, value] : table->read_all()) {
            (void)value;
            observe_internal_key(key);
        }
        level0_tables_.push_back(Level0Table{
            generation,
            path,
            std::move(table)
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
                [this, &recovered](
                    const LogRecord& record,
                    const RecordLocation&) {
                    if (record.operation == Operation::Batch) {
                        for (const auto& [operation, entry] :
                             decode_batch_payload(record.value)) {
                            observe_internal_key(entry.first);
                            if (operation == Operation::Put) {
                                recovered[entry.first] = entry.second;
                            } else {
                                recovered[entry.first] = std::nullopt;
                            }
                        }
                        return;
                    }

                    observe_internal_key(record.key);
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

    // Collect every internal-key version while holding the mutex. A writer
    // may have added active entries while our condition-variable wait
    // released it.
    std::ifstream source(base_path_, std::ios::binary);
    if (!source) { throw std::runtime_error("Cannot open base for compaction"); }

    std::map<std::string, std::optional<std::string>> merged;
    for (const auto& [key, entry] : index_) {
        if (entry.tombstone) {
            merged[key] = std::nullopt;
        } else {
            merged[key] = read_value(source, entry);
        }
    }
    source.close();
    for (const auto& table : level0_tables_) {
        for (const auto& [key, value] : table.table->read_all()) {
            merged[key] = value;
        }
    }
    for (const auto& [key, value] : memtable_) {
        merged[key] = value;
    }

    // With no live snapshot we can drop every superseded version. With a live
    // snapshot we conservatively keep all versions; snapshot-aware GC is the
    // next refinement.
    std::map<std::string, std::optional<std::string>> output_entries;
    if (snapshot_registry_->has_active()) {
        output_entries = merged;
    } else {
        bool has_user = false;
        std::string last_user;
        for (const auto& [key, value] : merged) {
            std::string user;
            std::uint64_t sequence = 0;
            bool tombstone = false;
            if (!decode_internal_key(
                    key, user, sequence, tombstone)) {
                continue;
            }
            if (!has_user || user != last_user) {
                has_user = true;
                last_user = user;
                if (value.has_value()) {
                    output_entries[key] = value;
                }
            }
        }
    }

    if (next_sstable_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("Base generation number is exhausted");
    }
    const auto generation = next_sstable_generation_++;
    const std::string new_base = base_file_path(log_path_, generation);
    const std::string temporary = new_base + ".tmp";
    std::map<std::string, IndexEntry> new_index;
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { throw std::runtime_error("Cannot create compacted base"); }
    write_log_header(output);
    for (const auto& [key, value] : output_entries) {
        if (value.has_value()) {
            const auto location = write_record(
                output, LogRecord{Operation::Put, key, *value});
            new_index.emplace(key, IndexEntry{
                location.value_offset, location.value_size, false});
        } else {
            const auto location = write_record(
                output, LogRecord{Operation::Delete, key, ""});
            new_index.emplace(key, IndexEntry{
                location.value_offset, location.value_size, true});
        }
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

        observe_internal_key(record.key);

        if (record.operation == Operation::Put) {
            index_[record.key] = IndexEntry{
                location.value_offset,
                location.value_size,
                false
            };
        } else {
            index_[record.key] = IndexEntry{
                location.value_offset,
                location.value_size,
                true
            };
        }
    }
}

void KVStore::replay_wal() {
    const bool repaired = replay_file(
        wal_path_,
        [this](const LogRecord& record, const RecordLocation&) {
            apply_log_record(record);
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
        std::string display_key = key;
        std::string decoded_key;
        std::uint64_t decoded_sequence = 0;
        bool tombstone = false;
        if (decode_internal_key(
                key, decoded_key, decoded_sequence, tombstone)) {
            display_key = decoded_key;
        }
        rows.push_back(InspectRow{layer, display_key, value});
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
            const IndexEntry& entry = index_.at(key);
            if (entry.tombstone) {
                if (!push("BASE", key, std::nullopt)) {
                    return rows;
                }
            } else if (!push("BASE", key, read_value(input, entry))) {
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

    const std::string seek = encode_seek_key(key, last_sequence_);
    const auto probe = [&key](
        const std::string& internal,
        const std::optional<std::string>& value) {
        std::string decoded;
        std::uint64_t sequence = 0;
        bool tombstone = false;
        if (!decode_internal_key(
                internal, decoded, sequence, tombstone)
            || decoded != key) {
            return LookupResult{};
        }
        return LookupResult{true, value};
    };

    const auto active = memtable_.lower_bound(seek);
    if (active == memtable_.end()) {
        add("Active", false, std::nullopt);
    } else {
        const auto result = probe(active->first, active->second);
        add("Active", result.found, result.value);
    }

    if (immutable_memtable_) {
        const auto immutable = immutable_memtable_->lower_bound(seek);
        if (immutable == immutable_memtable_->end()) {
            add("Immutable", false, std::nullopt);
        } else {
            const auto result = probe(immutable->first, immutable->second);
            add("Immutable", result.found, result.value);
        }
    } else {
        add("Immutable", false, std::nullopt);
    }

    for (auto table = level0_tables_.rbegin();
         table != level0_tables_.rend();
         ++table) {
        const SSTableSeekResult result = table->table->seek(seek);
        if (!result.found) {
            add(
                "L0#" + std::to_string(table->generation),
                false,
                std::nullopt);
            continue;
        }
        const auto match = probe(result.key, result.value);
        add(
            "L0#" + std::to_string(table->generation),
            match.found,
            match.value
        );
    }

    const auto base = index_.lower_bound(seek);
    if (base == index_.end()) {
        add("BASE", false, std::nullopt);
    } else {
        const auto match = probe(base->first, std::nullopt);
        if (!match.found) {
            add("BASE", false, std::nullopt);
        } else if (base->second.tombstone) {
            add("BASE", true, std::nullopt);
        } else {
            std::ifstream input(base_path_, std::ios::binary);
            if (!input.is_open()) {
                throw std::runtime_error(
                    "Cannot open base file for inspection");
            }
            add("BASE", true, read_value(input, base->second));
        }
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

KVStore::Iterator::Iterator(
    std::shared_ptr<const std::vector<Entry>> data,
    std::size_t position
) : data_(std::move(data)), position_(position) {}

bool KVStore::Iterator::valid() const noexcept {
    return data_ != nullptr && position_ < data_->size();
}

const std::string& KVStore::Iterator::key() const {
    if (!valid()) {
        throw std::out_of_range("Iterator::key on invalid iterator");
    }
    return (*data_)[position_].first;
}

const std::string& KVStore::Iterator::value() const {
    if (!valid()) {
        throw std::out_of_range("Iterator::value on invalid iterator");
    }
    return (*data_)[position_].second;
}

void KVStore::Iterator::seek(const std::string& target) {
    if (data_ == nullptr) {
        position_ = 0;
        return;
    }
    const auto it = std::lower_bound(
        data_->begin(),
        data_->end(),
        target,
        [](const Entry& entry, const std::string& wanted) {
            return entry.first < wanted;
        });
    position_ = static_cast<std::size_t>(it - data_->begin());
}

void KVStore::Iterator::seek_to_first() noexcept {
    position_ = 0;
}

void KVStore::Iterator::next() noexcept {
    if (valid()) {
        ++position_;
    }
}

KVStore::Snapshot::Snapshot(
    std::shared_ptr<const State> state
) : state_(std::move(state)) {}

std::uint64_t KVStore::Snapshot::sequence() const noexcept {
    return state_ ? state_->sequence : 0;
}

KVStore::Snapshot KVStore::snapshot() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();

    auto state = std::make_shared<Snapshot::State>();
    state->sequence = last_sequence_;
    state->registry = snapshot_registry_;
    snapshot_registry_->add(last_sequence_);
    return Snapshot(std::move(state));
}

KVStore::Iterator KVStore::new_iterator() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();
    return Iterator(snapshot_locked(last_sequence_), 0);
}

KVStore::Iterator KVStore::new_iterator(
    const Snapshot& snapshot
) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    rethrow_background_error_locked();
    return Iterator(snapshot_locked(snapshot.sequence()), 0);
}

std::uint64_t KVStore::oldest_snapshot_locked() const {
    if (!snapshot_registry_) {
        return last_sequence_;
    }
    return snapshot_registry_->oldest_or(last_sequence_);
}

std::shared_ptr<const std::vector<KVStore::Entry>>
KVStore::snapshot_locked(std::uint64_t sequence) const {
    if (log_path_.empty()) {
        std::vector<Entry> result;
        result.reserve(memory_data_.size());
        for (const auto& [key, value] : memory_data_) {
            result.emplace_back(key, value);
        }
        std::sort(
            result.begin(),
            result.end(),
            [](const Entry& left, const Entry& right) {
                return left.first < right.first;
            });
        return std::make_shared<const std::vector<Entry>>(
            std::move(result));
    }

    // user key -> (highest visible sequence, value/tombstone)
    std::map<std::string, std::pair<std::uint64_t, std::optional<std::string>>>
        chosen;

    const auto consider = [&chosen, sequence](
        const std::string& internal,
        const std::optional<std::string>& value
    ) {
        std::string user;
        std::uint64_t record_sequence = 0;
        bool tombstone = false;
        if (!decode_internal_key(
                internal, user, record_sequence, tombstone)) {
            return;
        }
        if (record_sequence > sequence) {
            return;
        }
        const auto found = chosen.find(user);
        if (found == chosen.end()
            || record_sequence > found->second.first) {
            chosen[user] = {record_sequence, value};
        }
    };

    for (const auto& [key, value] : memtable_) {
        consider(key, value);
    }
    if (immutable_memtable_) {
        for (const auto& [key, value] : *immutable_memtable_) {
            consider(key, value);
        }
    }
    for (const auto& table : level0_tables_) {
        for (const auto& [key, value] : table.table->read_all()) {
            consider(key, value);
        }
    }

    if (!index_.empty()) {
        std::ifstream input(base_path_, std::ios::binary);
        if (!input.is_open()) {
            throw std::runtime_error(
                "Cannot open base file for iterator");
        }
        for (const auto& [key, entry] : index_) {
            if (entry.tombstone) {
                consider(key, std::nullopt);
            } else {
                consider(key, read_value(input, entry));
            }
        }
    }

    std::vector<Entry> result;
    result.reserve(chosen.size());
    for (const auto& [user, record] : chosen) {
        if (record.second.has_value()) {
            result.emplace_back(user, *record.second);
        }
    }
    return std::make_shared<const std::vector<Entry>>(std::move(result));
}
