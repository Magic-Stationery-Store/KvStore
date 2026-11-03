#include "manifest.h"
#include "platform.h"

#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

std::uint32_t checksum(const std::string& text) {
    std::uint32_t crc = 0xffffffffu;
    for (const unsigned char byte : text) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return crc ^ 0xffffffffu;
}

std::uint64_t number(std::istream& input) {
    std::string token;
    input >> token;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(
        token.data(), token.data() + token.size(), value);
    if (token.empty() || parsed.ec != std::errc{}
        || parsed.ptr != token.data() + token.size()) {
        throw std::runtime_error("Invalid MANIFEST integer");
    }
    return value;
}

void expect_token(std::istream& input, const char* wanted) {
    std::string token;
    if (!(input >> token) || token != wanted) {
        throw std::runtime_error("Invalid or incomplete MANIFEST");
    }
}

void validate(const ManifestState& state) {
    if (state.next_generation == 0
        || state.base_generation >= state.next_generation
        || state.retired_through >= state.next_generation
        || state.base_generation != state.retired_through) {
        throw std::runtime_error("Invalid MANIFEST generation bounds");
    }
    std::uint64_t previous = state.retired_through;
    for (const auto generation : state.level0) {
        if (generation <= previous || generation >= state.next_generation) {
            throw std::runtime_error("Invalid MANIFEST L0 ordering");
        }
        previous = generation;
    }
}

} // namespace

std::string base_file_path(const std::string& database, std::uint64_t generation) {
    if (generation == 0) { return database; }
    std::ostringstream path;
    path << database << ".base." << std::setw(20) << std::setfill('0')
         << generation << ".log";
    return path.str();
}

ManifestState read_manifest(const std::string& path) {
    // Bound memory consumption for corrupt metadata.
    if (std::filesystem::file_size(path) > 32 * 1024 * 1024) {
        throw std::runtime_error("MANIFEST is too large");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) { throw std::runtime_error("Cannot open MANIFEST"); }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) { throw std::runtime_error("Cannot read MANIFEST"); }
    const std::string text = buffer.str();
    const auto marker = text.rfind("CRC32 ");
    if (marker == std::string::npos || marker == 0 || text[marker - 1] != '\n') {
        throw std::runtime_error("Incomplete MANIFEST checksum");
    }
    const std::string payload = text.substr(0, marker);
    std::istringstream trailer(text.substr(marker));
    expect_token(trailer, "CRC32");
    const auto stored = number(trailer);
    std::string extra;
    if ((trailer >> extra) || stored != checksum(payload)) {
        throw std::runtime_error("MANIFEST checksum mismatch");
    }

    std::istringstream input(payload);
    expect_token(input, "KV_MANIFEST");
    if (number(input) != 1) { throw std::runtime_error("Unsupported MANIFEST version"); }
    ManifestState state;
    expect_token(input, "NEXT"); state.next_generation = number(input);
    expect_token(input, "BASE"); state.base_generation = number(input);
    expect_token(input, "RETIRED"); state.retired_through = number(input);
    expect_token(input, "L0"); const auto count = number(input);
    if (count > 1000000) { throw std::runtime_error("Too many MANIFEST tables"); }
    for (std::uint64_t i = 0; i < count; ++i) {
        expect_token(input, "TABLE"); state.level0.push_back(number(input));
    }
    expect_token(input, "END");
    if (input >> extra) { throw std::runtime_error("Unexpected MANIFEST data"); }
    validate(state);
    return state;
}

void write_manifest(const std::string& path, const ManifestState& state) {
    validate(state);
    std::ostringstream payload;
    payload << "KV_MANIFEST 1\nNEXT " << state.next_generation
            << "\nBASE " << state.base_generation
            << "\nRETIRED " << state.retired_through
            << "\nL0 " << state.level0.size() << '\n';
    for (const auto id : state.level0) { payload << "TABLE " << id << '\n'; }
    payload << "END\n";
    const std::string text = payload.str();
    const std::string temporary = path + ".tmp";
    // On publication errors preserve both snapshots; callers must reopen.
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { throw std::runtime_error("Cannot create MANIFEST temporary file"); }
    output << text << "CRC32 " << checksum(text) << '\n';
    output.close();
    if (!output) { throw std::runtime_error("Cannot finish MANIFEST"); }
    kvplatform::sync_file(temporary);
    kvplatform::replace_file(temporary, path);
    kvplatform::sync_directory(
        std::filesystem::absolute(path).parent_path());
}
