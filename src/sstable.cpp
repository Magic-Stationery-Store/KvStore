#include "sstable.h"

#include "log_record.h"
#include "platform.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
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

SSTable::SSTable(std::string path)
    : path_(std::move(path)) {
    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("Cannot open SSTable");
    }
    read_header(input);

    while (true) {
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
            location.value_size
        });
    }
}

SSTableLookupResult SSTable::get(
    const std::string& key
) const {
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

    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("Cannot open SSTable for lookup");
    }
    input.seekg(
        static_cast<std::streamoff>(it->value_offset),
        std::ios::beg
    );
    if (!input) {
        throw std::runtime_error("Cannot seek to SSTable value");
    }

    std::string value(it->value_size, '\0');
    if (it->value_size != 0) {
        input.read(
            value.data(),
            static_cast<std::streamsize>(it->value_size)
        );
        if (!input) {
            throw std::runtime_error("Cannot read SSTable value");
        }
    }
    return SSTableLookupResult{true, std::move(value)};
}

SSTableSeekResult SSTable::seek(
    const std::string& key
) const {
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

    SSTableSeekResult result;
    result.found = true;
    result.key = it->key;
    if (it->tombstone) {
        return result;
    }

    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("Cannot open SSTable for seek");
    }
    input.seekg(
        static_cast<std::streamoff>(it->value_offset),
        std::ios::beg
    );
    if (!input) {
        throw std::runtime_error("Cannot seek to SSTable value");
    }

    std::string value(it->value_size, '\0');
    if (it->value_size != 0) {
        input.read(
            value.data(),
            static_cast<std::streamsize>(it->value_size)
        );
        if (!input) {
            throw std::runtime_error("Cannot read SSTable value");
        }
    }
    result.value = std::move(value);
    return result;
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
