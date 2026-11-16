#include "sstable.h"

#include "internal_key.h"
#include "log_record.h"
#include "platform.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

constexpr std::array<char, 4> kSSTableMagic{
    'S', 'S', 'T', 'B'
};
constexpr std::uint32_t kSSTableVersion = 1;

void write_u32(std::ostream& output, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu)
    };
    output.write(bytes.data(),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("Failed to write SSTable version");
    }
}

std::uint32_t read_u32(std::istream& input) {
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("Incomplete SSTable header");
    }
    return static_cast<std::uint32_t>(bytes[0])
         | (static_cast<std::uint32_t>(bytes[1]) << 8u)
         | (static_cast<std::uint32_t>(bytes[2]) << 16u)
         | (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

void write_header(std::ostream& output) {
    output.write(kSSTableMagic.data(),
                 static_cast<std::streamsize>(kSSTableMagic.size()));
    if (!output) {
        throw std::runtime_error("Failed to write SSTable magic");
    }
    write_u32(output, kSSTableVersion);
}

void read_header(std::istream& input) {
    std::array<char, 4> magic{};
    input.read(magic.data(),
               static_cast<std::streamsize>(magic.size()));
    if (!input) {
        throw std::runtime_error("Incomplete SSTable header");
    }
    if (magic != kSSTableMagic) {
        throw std::runtime_error("Invalid SSTable magic");
    }
    if (read_u32(input) != kSSTableVersion) {
        throw std::runtime_error("Unsupported SSTable version");
    }
}

} // namespace

BlockCache::BlockCache(std::size_t capacity_bytes)
    : capacity_bytes_(capacity_bytes) {}

std::shared_ptr<const BlockCache::Block> BlockCache::get(
    const std::string& id
) {
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    lru_.splice(lru_.begin(), lru_, found->second.position);
    return found->second.block;
}

void BlockCache::put(
    const std::string& id,
    std::shared_ptr<const Block> block,
    std::size_t bytes
) {
    if (!block || bytes > capacity_bytes_) {
        return;
    }

    const std::lock_guard<std::mutex> guard(mutex_);
    const auto existing = entries_.find(id);
    if (existing != entries_.end()) {
        current_bytes_ -= existing->second.bytes;
        existing->second.block = std::move(block);
        existing->second.bytes = bytes;
        current_bytes_ += bytes;
        lru_.splice(lru_.begin(), lru_, existing->second.position);
        return;
    }

    while (!lru_.empty()
           && current_bytes_ + bytes > capacity_bytes_) {
        const std::string victim = lru_.back();
        const auto victim_entry = entries_.find(victim);
        if (victim_entry != entries_.end()) {
            current_bytes_ -= victim_entry->second.bytes;
            entries_.erase(victim_entry);
            ++evictions_;
        }
        lru_.pop_back();
    }

    lru_.push_front(id);
    entries_.emplace(id, Entry{
        std::move(block),
        bytes,
        lru_.begin()
    });
    current_bytes_ += bytes;
}

BlockCache::Stats BlockCache::stats() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    Stats result;
    result.hits = hits_;
    result.misses = misses_;
    result.evictions = evictions_;
    result.entries = entries_.size();
    result.bytes = current_bytes_;
    return result;
}

void SSTable::build(
    const std::string& path,
    const Entries& entries
) {
    if (path.empty()) {
        throw std::invalid_argument("SSTable path cannot be empty");
    }
    if (std::filesystem::exists(path)) {
        throw std::invalid_argument(
            "SSTable is immutable and already exists");
    }

    const std::filesystem::path temporary_path =
        std::filesystem::path(path).concat(".tmp");
    std::error_code error;
    std::filesystem::remove(temporary_path, error);
    if (error) {
        throw std::runtime_error(
            "Cannot remove stale SSTable temporary file: "
            + error.message());
    }

    try {
        std::ofstream output(
            temporary_path,
            std::ios::binary | std::ios::trunc
        );
        if (!output.is_open()) {
            throw std::runtime_error("Cannot create SSTable");
        }

        write_header(output);
        for (const auto& [key, value] : entries) {
            if (value.has_value()) {
                write_record(
                    output,
                    LogRecord{Operation::Put, key, *value}
                );
            } else {
                write_record(
                    output,
                    LogRecord{Operation::Delete, key, ""}
                );
            }
        }

        output.close();
        if (!output) {
            throw std::runtime_error("Cannot finish SSTable");
        }

        kvplatform::sync_file(temporary_path);

        try {
            kvplatform::rename_file(temporary_path, path);
        } catch (const std::exception& rename_error) {
            throw std::runtime_error(
                std::string("Cannot publish SSTable: ")
                + rename_error.what());
        }
        kvplatform::sync_parent_directory(path);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        throw;
    }
}

SSTable::SSTable(
    std::string path,
    std::shared_ptr<BlockCache> cache
) : path_(std::move(path)), cache_(std::move(cache)) {
    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("Cannot open SSTable");
    }
    read_header(input);

    while (true) {
        const std::streampos record_start = input.tellg();
        if (record_start == std::streampos(-1)) {
            throw std::runtime_error(
                "Cannot determine SSTable record offset");
        }

        LogRecord record;
        RecordLocation location{};
        if (!read_record(input, record, location)) {
            break;
        }

        if (!index_.empty()
            && record.key <= index_.back().key) {
            throw std::runtime_error(
                "SSTable keys are not strictly increasing");
        }

        index_.push_back(IndexEntry{
            std::move(record.key),
            record.operation == Operation::Delete,
            location.value_offset,
            location.value_size,
            static_cast<std::uint64_t>(record_start)
        });
    }

    // Group records into ~4 KiB blocks. The on-disk records stay compatible
    // with the existing format; the block index and Bloom filter are built at
    // open time and are intentionally metadata-only.
    constexpr std::uint64_t kTargetBlockBytes = 4096;
    constexpr std::size_t kMaxRecordsPerBlock = 64;
    block_for_index_.resize(index_.size());

    const std::uint64_t file_size =
        index_.empty()
            ? 0
            : static_cast<std::uint64_t>(
                std::filesystem::file_size(path_));

    std::size_t first = 0;
    while (first < index_.size()) {
        std::size_t last = first;
        const std::uint64_t begin = index_[first].record_offset;
        while (last + 1 < index_.size()
               && index_[last + 1].record_offset - begin
                      <= kTargetBlockBytes
               && last - first + 1 < kMaxRecordsPerBlock) {
            ++last;
        }
        const std::uint64_t end =
            last + 1 < index_.size()
                ? index_[last + 1].record_offset
                : file_size;

        blocks_.push_back(BlockRef{begin, end, first, last});
        for (std::size_t i = first; i <= last; ++i) {
            block_for_index_[i] = blocks_.size() - 1;
        }
        first = last + 1;
    }

    const std::size_t desired_bits =
        std::max<std::size_t>(64, index_.size() * 10);
    const std::size_t bloom_bits =
        ((desired_bits + 63) / 64) * 64;
    bloom_bits_.assign(bloom_bits / 64, 0);
    for (const auto& entry : index_) {
        std::string user;
        std::uint64_t sequence = 0;
        bool tombstone = false;
        if (kvstore_internal::decode_internal_key(
                entry.key, user, sequence, tombstone)) {
            bloom_add(user);
        } else {
            bloom_add(entry.key);
        }
    }
}

void SSTable::bloom_add(const std::string& key) {
    if (bloom_bits_.empty()) {
        return;
    }
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : key) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    std::uint64_t second = hash ^ (hash >> 33);
    second *= 0xff51afd7ed558ccdull;
    second ^= second >> 33;

    const std::size_t bit_count = bloom_bits_.size() * 64;
    for (std::uint32_t i = 0; i < bloom_hash_count_; ++i) {
        const std::size_t bit = static_cast<std::size_t>(
            (hash + static_cast<std::uint64_t>(i) * second)
            % bit_count);
        bloom_bits_[bit / 64] |= (1ull << (bit % 64));
    }
}

bool SSTable::bloom_maybe(const std::string& key) const {
    if (bloom_bits_.empty()) {
        return false;
    }
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : key) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    std::uint64_t second = hash ^ (hash >> 33);
    second *= 0xff51afd7ed558ccdull;
    second ^= second >> 33;

    const std::size_t bit_count = bloom_bits_.size() * 64;
    for (std::uint32_t i = 0; i < bloom_hash_count_; ++i) {
        const std::size_t bit = static_cast<std::size_t>(
            (hash + static_cast<std::uint64_t>(i) * second)
            % bit_count);
        if ((bloom_bits_[bit / 64] & (1ull << (bit % 64))) == 0) {
            return false;
        }
    }
    return true;
}

bool SSTable::might_contain_user(const std::string& user_key) const {
    return bloom_maybe(user_key);
}

std::shared_ptr<const BlockCache::Block> SSTable::load_block(
    std::size_t block_index
) const {
    const BlockRef& block = blocks_[block_index];
    const std::string id =
        path_ + "#" + std::to_string(block.begin);

    if (cache_) {
        if (const auto cached = cache_->get(id)) {
            return cached;
        }
    }

    const std::uint64_t size = block.end - block.begin;
    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("Cannot open SSTable block");
    }
    input.seekg(
        static_cast<std::streamoff>(block.begin),
        std::ios::beg);
    if (!input) {
        throw std::runtime_error("Cannot seek to SSTable block");
    }

    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size != 0) {
        input.read(bytes.data(), static_cast<std::streamsize>(size));
        if (!input) {
            throw std::runtime_error("Cannot read SSTable block");
        }
    }

    std::istringstream stream(bytes, std::ios::binary);
    auto parsed = std::make_shared<BlockCache::Block>();
    while (true) {
        LogRecord record;
        RecordLocation location{};
        if (!read_record(stream, record, location)) {
            break;
        }
        if (record.operation == Operation::Delete) {
            (*parsed)[std::move(record.key)] = std::nullopt;
        } else {
            (*parsed)[std::move(record.key)] = std::move(record.value);
        }
    }

    std::shared_ptr<const BlockCache::Block> result = parsed;
    if (cache_) {
        cache_->put(id, result, bytes.size());
    }
    return result;
}

SSTableLookupResult SSTable::get(
    const std::string& key
) const {
    std::string user;
    std::uint64_t sequence = 0;
    bool tombstone = false;
    if (kvstore_internal::decode_internal_key(
            key, user, sequence, tombstone)
        && !bloom_maybe(user)) {
        return {};
    }

    const auto it = std::lower_bound(
        index_.begin(),
        index_.end(),
        key,
        [](const IndexEntry& entry, const std::string& wanted) {
            return entry.key < wanted;
        }
    );

    if (it == index_.end() || it->key != key) {
        return {};
    }
    if (it->tombstone) {
        return SSTableLookupResult{true, std::nullopt};
    }

    const std::size_t position = static_cast<std::size_t>(
        it - index_.begin());
    const auto block = load_block(block_for_index_[position]);
    const auto found = block->find(key);
    if (found == block->end()) {
        throw std::runtime_error(
            "SSTable block index is inconsistent");
    }
    return SSTableLookupResult{true, found->second};
}

SSTableSeekResult SSTable::seek(
    const std::string& key
) const {
    std::string user;
    std::uint64_t sequence = 0;
    bool tombstone = false;
    if (kvstore_internal::decode_internal_key(
            key, user, sequence, tombstone)
        && !bloom_maybe(user)) {
        return {};
    }

    const auto it = std::lower_bound(
        index_.begin(),
        index_.end(),
        key,
        [](const IndexEntry& entry, const std::string& wanted) {
            return entry.key < wanted;
        }
    );
    if (it == index_.end()) {
        return {};
    }

    const std::size_t position = static_cast<std::size_t>(
        it - index_.begin());
    const auto block = load_block(block_for_index_[position]);
    const auto found = block->lower_bound(key);
    if (found == block->end()) {
        return {};
    }
    return SSTableSeekResult{
        true,
        found->first,
        found->second
    };
}

std::vector<SSTable::EntryRef> SSTable::index_snapshot() const {
    std::vector<EntryRef> result;
    result.reserve(index_.size());
    for (const auto& entry : index_) {
        result.push_back(EntryRef{
            entry.key,
            entry.value_offset,
            entry.value_size,
            entry.tombstone
        });
    }
    return result;
}

const std::string& SSTable::path() const noexcept {
    return path_;
}

SSTable::Entries SSTable::read_all() const {
    Entries entries;
    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("Cannot open SSTable for scan");
    }

    for (const auto& entry : index_) {
        if (entry.tombstone) {
            entries.emplace(entry.key, std::nullopt);
            continue;
        }

        input.seekg(
            static_cast<std::streamoff>(entry.value_offset),
            std::ios::beg
        );
        if (!input) {
            throw std::runtime_error("Cannot seek during SSTable scan");
        }
        std::string value(entry.value_size, '\0');
        if (entry.value_size != 0) {
            input.read(
                value.data(),
                static_cast<std::streamsize>(entry.value_size)
            );
            if (!input) {
                throw std::runtime_error(
                    "Cannot read value during SSTable scan");
            }
        }
        entries.emplace(entry.key, std::move(value));
    }
    return entries;
}

std::size_t SSTable::entry_count() const noexcept {
    return index_.size();
}
