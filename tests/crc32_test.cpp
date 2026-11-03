#include "test_files.h"
#include "kv_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    const std::string log_path = "checksum-test.log";
    const std::string wal_path = log_path + ".wal";
    reset_test_database(log_path);
std::remove(log_path.c_str());
    std::remove(wal_path.c_str());

    {
        KVStore store(log_path);
        store.put("name", "alice");
    }

    const std::uintmax_t original_size =
        std::filesystem::file_size(wal_path);

    bool byte_was_corrupted = false;

    {
        std::fstream file(
            wal_path,
            std::ios::binary | std::ios::in | std::ios::out
        );

        if (file.is_open() && original_size >= 5) {
            const auto position = static_cast<std::streamoff>(
                original_size - 5
            );

            file.seekg(position);

            char byte = 0;
            if (file.get(byte)) {
                byte ^= 0x01;
                file.seekp(position);
                file.put(byte);
                byte_was_corrupted = static_cast<bool>(file);
            }
        }
    }

    if (!byte_was_corrupted) {
        std::cerr << "Failed to corrupt the test log\n";
        reset_test_database(log_path);
std::remove(log_path.c_str());
        std::remove(wal_path.c_str());
        return 1;
    }

    bool checksum_error_detected = false;

    try {
        KVStore damaged(log_path);
    } catch (const std::runtime_error& error) {
        checksum_error_detected =
            std::string(error.what()).find("checksum")
            != std::string::npos;
    }

    const bool size_unchanged =
        std::filesystem::file_size(wal_path) == original_size;

    reset_test_database(log_path);
std::remove(log_path.c_str());
    std::remove(wal_path.c_str());

    if (!checksum_error_detected) {
        std::cerr << "CRC32 did not detect the corrupted record\n";
        return 1;
    }

    if (!size_unchanged) {
        std::cerr << "Checksum failure truncated the log\n";
        return 1;
    }

    std::cout << "CRC32 corruption test passed\n";
    return 0;
}
