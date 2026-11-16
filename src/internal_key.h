#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kvstore_internal {

constexpr std::size_t kInternalSuffixSize = 11;
constexpr char kEscapedNul = static_cast<char>(0xFF);

inline std::string escape_user_key(const std::string& user_key) {
    std::string escaped;
    escaped.reserve(user_key.size());
    for (const char character : user_key) {
        if (character == '\0') {
            escaped.push_back('\0');
            escaped.push_back(kEscapedNul);
        } else {
            escaped.push_back(character);
        }
    }
    return escaped;
}

inline bool unescape_user_key(
    const std::string& escaped,
    std::string& user_key
) {
    user_key.clear();
    user_key.reserve(escaped.size());
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] == '\0') {
            if (i + 1 >= escaped.size()
                || escaped[i + 1] != kEscapedNul) {
                return false;
            }
            user_key.push_back('\0');
            ++i;
        } else {
            user_key.push_back(escaped[i]);
        }
    }
    return true;
}

inline std::string encode_internal_key(
    const std::string& user_key,
    std::uint64_t sequence,
    bool tombstone
) {
    std::string internal = escape_user_key(user_key);
    internal.reserve(internal.size() + kInternalSuffixSize);
    internal.push_back('\0');
    internal.push_back('\0');

    const std::uint64_t inverted = ~sequence;
    for (int shift = 56; shift >= 0; shift -= 8) {
        internal.push_back(static_cast<char>(
            (inverted >> shift) & 0xFFu));
    }
    internal.push_back(tombstone ? '\1' : '\0');
    return internal;
}

inline bool decode_internal_key(
    const std::string& internal,
    std::string& user_key,
    std::uint64_t& sequence,
    bool& tombstone
) {
    if (internal.size() < kInternalSuffixSize) {
        return false;
    }
    const std::size_t offset = internal.size() - kInternalSuffixSize;
    if (internal[offset] != '\0' || internal[offset + 1] != '\0') {
        return false;
    }

    std::uint64_t inverted = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        inverted = (inverted << 8)
            | static_cast<unsigned char>(internal[offset + 2 + i]);
    }
    sequence = ~inverted;
    tombstone = internal.back() != '\0';
    return unescape_user_key(
        internal.substr(0, offset), user_key);
}

inline std::string encode_seek_key(
    const std::string& user_key,
    std::uint64_t sequence
) {
    std::string seek = escape_user_key(user_key);
    seek.reserve(seek.size() + kInternalSuffixSize);
    seek.push_back('\0');
    seek.push_back('\0');

    const std::uint64_t inverted = ~sequence;
    for (int shift = 56; shift >= 0; shift -= 8) {
        seek.push_back(static_cast<char>(
            (inverted >> shift) & 0xFFu));
    }
    seek.push_back('\0');
    return seek;
}

} // namespace kvstore_internal
