#pragma once

#include<cstdint>
#include<istream>
#include<ostream>
#include<string>
#include <cstddef>
#include <stdexcept>
inline constexpr std::size_t kLogHeaderSize = 8;
class IncompleteRecordError final
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
enum class Operation : std::uint8_t{
    Put = 1, Delete = 2
};

struct LogRecord
{
    Operation operation;
    std::string key;
    std::string value;
};

struct RecordLocation
{
    std::uint64_t value_offset;
    std::uint32_t value_size;
};

void write_log_header(std::ostream& output);
void read_log_header(std::istream& input);
RecordLocation write_record(
    std::ostream& output,
    const LogRecord& record
);

bool read_record(
    std::istream& input,
    LogRecord& record,
    RecordLocation& location
);
