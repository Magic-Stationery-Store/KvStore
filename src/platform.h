#pragma once

#include <filesystem>
#include <string>

// Small POSIX/Windows compatibility layer for durability and locking.
//
// The storage engine needs three primitives that the C++ standard library
// does not expose: fsync on a file, fsync on a directory, and an exclusive
// whole-process file lock.  On POSIX these map to open/fsync/rename/flock;
// on Windows they map to CreateFile/FlushFileBuffers/MoveFileEx/CloseHandle.
namespace kvplatform {

// Flush a file's contents (and metadata) to stable storage.
void sync_file(const std::filesystem::path& path);

// Flush the directory entry that names the file.  Windows has no durable
// equivalent, so this is a no-op there (documented in the README).
void sync_directory(const std::filesystem::path& path);

// Flush the directory that contains the given file.
void sync_parent_directory(const std::filesystem::path& file);

// Rename without replacing an existing destination.
void rename_file(const std::filesystem::path& from,
                 const std::filesystem::path& to);

// Atomically replace the destination if it exists.
void replace_file(const std::filesystem::path& from,
                  const std::filesystem::path& to);

// Exclusive lock held for the lifetime of the object.  A second lock attempt
// (same process or another one) fails immediately instead of blocking.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path);
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    std::string path_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int descriptor_ = -1;
#endif
};

} // namespace kvplatform
