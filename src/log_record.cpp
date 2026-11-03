#include "log_record.h"

#include <array>
#include <stdexcept>

namespace
{
constexpr std::array<char, 4> kMagic{
    'K', 'V', 'D', 'B'
};

constexpr std::uint32_t kFormatVersion = 2;
constexpr std::uint32_t kMaxKeySize = 1024 * 1024;
constexpr std::uint32_t kMaxValueSize = 16 * 1024 * 1024;
constexpr std::uint32_t kCrc32Polynomial = 0xedb88320u;
constexpr std::uint64_t kRecordPrefixSize = 1 + 4 + 4;

std::uint64_t position_to_u64(
    std::streampos position,
    const char* error_message
) {
    if (position == std::streampos(-1)) {
        throw std::runtime_error(error_message);
    }

    const auto offset =
        static_cast<std::streamoff>(position);

    if (offset < 0) {
        throw std::runtime_error(error_message);
    }

    return static_cast<std::uint64_t>(offset);
}

std::array<char, 4> encode_u32(std::uint32_t value) {
    return {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu)
    };
}

void update_crc32(std::uint32_t& crc,
                  const char* data,
                  std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<unsigned char>(data[i]);

        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1u) != 0) {
                crc =
                    (crc >> 1u)
                    ^ kCrc32Polynomial;
            } else {
                crc >>= 1u;
            }
        }
    }
}

std::uint32_t calculate_record_crc32(const LogRecord& record) {
    std::uint32_t crc = 0xffffffffu;

    const char operation_byte = static_cast<char>(
        static_cast<std::uint8_t>(record.operation));
    update_crc32(crc, &operation_byte, 1);

    const auto key_size_bytes = encode_u32(
        static_cast<std::uint32_t>(record.key.size()));
    update_crc32(
        crc, key_size_bytes.data(), key_size_bytes.size());

    const auto value_size_bytes = encode_u32(
        static_cast<std::uint32_t>(record.value.size()));
    update_crc32(
        crc, value_size_bytes.data(), value_size_bytes.size());

    update_crc32(crc, record.key.data(), record.key.size());
    update_crc32(crc, record.value.data(), record.value.size());

    return crc ^ 0xffffffffu;
}

void write_u8(std::ostream& output, std::uint8_t value) {
    output.put(static_cast<char>(value));

    if (!output) {
        throw std::runtime_error("Failed to write uint8");
    }
}

void write_u32(std::ostream& output,
               std::uint32_t value) {
    const auto bytes = encode_u32(value);

    output.write(
        bytes.data(),
        static_cast<std::streamsize>(bytes.size())
    );

    if (!output) {
        throw std::runtime_error(
            "Failed to write uint32");
    }
}

std::uint32_t read_u32(std::istream& input) {
    std::array<unsigned char, 4> bytes{};

    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );

    if (!input) {
        if (input.bad()) {
            throw std::runtime_error(
                "I/O error while reading uint32");
        }

        if (input.eof()) {
            throw IncompleteRecordError(
                "Incomplete uint32 at end of log");
        }

        throw std::runtime_error(
            "Failed to read uint32");
    }

    return static_cast<std::uint32_t>(bytes[0])
         | (static_cast<std::uint32_t>(bytes[1]) << 8u)
         | (static_cast<std::uint32_t>(bytes[2]) << 16u)
         | (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

std::string read_string(std::istream& input,
                        std::uint32_t size) {
    std::string result(size, '\0');

    if (size == 0) {
        return result;
    }

    input.read(
        result.data(),
        static_cast<std::streamsize>(size)
    );

    if (!input) {
        if (input.bad()) {
            throw std::runtime_error(
                "I/O error while reading string");
        }

        if (input.eof()) {
            throw IncompleteRecordError(
                "Incomplete string at end of log");
        }

        throw std::runtime_error(
            "Failed to read string");
    }

    return result;
}
} // namespace

void write_log_header(std::ostream& output) {
    output.write(
        kMagic.data(),
        static_cast<std::streamsize>(kMagic.size())
    );

    if (!output) {
        throw std::runtime_error(
            "Failed to write log magic");
    }

    write_u32(output, kFormatVersion);
}

void read_log_header(std::istream& input) {
    std::array<char, 4> magic{};

    input.read(
        magic.data(),
        static_cast<std::streamsize>(magic.size())
    );

    if (!input) {
        throw std::runtime_error(
            "Incomplete log file header");
    }

    if (magic != kMagic) {
        throw std::runtime_error(
            "Invalid log file magic");
    }

    std::uint32_t version = 0;

    try {
    version = read_u32(input);
        } catch (const IncompleteRecordError&) {
    throw std::runtime_error(
        "Incomplete log file header");
    }
    if (version != kFormatVersion) {
    throw std::runtime_error(
        "Unsupported log file version: "
        + std::to_string(version));
}
}
RecordLocation write_record(
    std::ostream& output,
    const LogRecord& record
) {
    if (record.key.size() > kMaxKeySize) {
        throw std::invalid_argument("Key is too large");
    }

    if (record.value.size() > kMaxValueSize) {
        throw std::invalid_argument("Value is too large");
    }

    if (record.operation == Operation::Delete
        && !record.value.empty()) {
        throw std::invalid_argument(
            "DELETE record must not contain a value");
    }

    output.seekp(0, std::ios::end);

    if (!output) {
        throw std::runtime_error(
            "Failed to seek to end of log");
    }

    const std::uint64_t record_offset =
        position_to_u64(
            output.tellp(),
            "Failed to get record write position"
        );

    const RecordLocation location{
        record_offset
            + kRecordPrefixSize
            + static_cast<std::uint64_t>(record.key.size()),
        static_cast<std::uint32_t>(record.value.size())
    };

    write_u8(
        output,
        static_cast<std::uint8_t>(record.operation)
    );

    write_u32(
        output,
        static_cast<std::uint32_t>(record.key.size())
    );

    write_u32(
        output,
        static_cast<std::uint32_t>(record.value.size())
    );

    output.write(
        record.key.data(),
        static_cast<std::streamsize>(record.key.size())
    );

    output.write(
        record.value.data(),
        static_cast<std::streamsize>(record.value.size())
    );

    if (!output) {
        throw std::runtime_error(
            "Failed to write record data");
    }

    const std::uint32_t checksum =
        calculate_record_crc32(record);
    write_u32(output, checksum);

    return location;
}

bool read_record(std::istream& input,
                 LogRecord& record,
                 RecordLocation& location) {
    const int first_byte = input.get();

    if (first_byte == std::char_traits<char>::eof()) {
        if (input.bad()) {
            throw std::runtime_error(
                "I/O error while reading operation");
        }

        if (input.eof()) {
            return false;
        }

        throw std::runtime_error(
            "Failed to read operation");
    }

    const auto operation_value =
        static_cast<std::uint8_t>(first_byte);

    if (operation_value
        == static_cast<std::uint8_t>(Operation::Put)) {
        record.operation = Operation::Put;
    } else if (
        operation_value
        == static_cast<std::uint8_t>(Operation::Delete)) {
        record.operation = Operation::Delete;
    } else {
        throw std::runtime_error(
            "Unknown operation in log file");
    }

    const std::uint32_t key_size = read_u32(input);
    const std::uint32_t value_size = read_u32(input);

    if (key_size > kMaxKeySize) {
        throw std::runtime_error(
            "Key size in log is too large");
    }

    if (value_size > kMaxValueSize) {
        throw std::runtime_error(
            "Value size in log is too large");
    }

    if (record.operation == Operation::Delete
        && value_size != 0) {
        throw std::runtime_error(
            "DELETE record contains a value");
    }

    record.key = read_string(input, key_size);

    const RecordLocation decoded_location{
        position_to_u64(
            input.tellg(),
            "Failed to get value position"
        ),
        value_size
    };

    record.value = read_string(input, value_size);

    const std::uint32_t stored_checksum = read_u32(input);
    const std::uint32_t actual_checksum =
        calculate_record_crc32(record);

    if (stored_checksum != actual_checksum) {
        throw std::runtime_error(
            "Record checksum mismatch");
    }

    location = decoded_location;

    return true;
}
