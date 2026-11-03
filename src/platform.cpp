#include "platform.h"

#include <cerrno>
#include <cstdio>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace kvplatform {
namespace {

#ifdef _WIN32
std::system_error windows_error(const char* message) {
    return std::system_error(
        static_cast<int>(::GetLastError()),
        std::system_category(),
        message
    );
}
#endif

} // namespace

void sync_file(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (handle == INVALID_HANDLE_VALUE) {
        throw windows_error("Cannot open file for sync");
    }

    if (!::FlushFileBuffers(handle)) {
        const std::system_error error = windows_error("Cannot sync file");
        ::CloseHandle(handle);
        throw error;
    }

    if (!::CloseHandle(handle)) {
        throw windows_error("Cannot close synced file");
    }
#else
    const int file_descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);

    if (file_descriptor == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Cannot open file for sync"
        );
    }

    int sync_result = 0;

    do {
        sync_result = ::fsync(file_descriptor);
    } while (sync_result == -1 && errno == EINTR);

    if (sync_result == -1) {
        const int saved_errno = errno;
        ::close(file_descriptor);

        throw std::system_error(
            saved_errno,
            std::generic_category(),
            "Cannot sync file"
        );
    }

    if (::close(file_descriptor) == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Cannot close synced file"
        );
    }
#endif
}

void sync_directory(const std::filesystem::path& path) {
#ifdef _WIN32
    // NTFS journals metadata; there is no directory fsync equivalent that
    // FlushFileBuffers accepts.  MoveFileExW with MOVEFILE_WRITE_THROUGH is
    // used by rename_file/replace_file instead.
    (void)path;
#else
    const int descriptor = ::open(
        path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (descriptor == -1) {
        throw std::system_error(
            errno, std::generic_category(),
            "Cannot open directory for sync");
    }

    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        const int saved_errno = errno;
        ::close(descriptor);
        throw std::system_error(
            saved_errno, std::generic_category(),
            "Cannot sync directory");
    }

    if (::close(descriptor) == -1) {
        throw std::system_error(
            errno, std::generic_category(),
            "Cannot close synced directory");
    }
#endif
}

void sync_parent_directory(const std::filesystem::path& file) {
    sync_directory(std::filesystem::absolute(file).parent_path());
}

void rename_file(const std::filesystem::path& from,
                 const std::filesystem::path& to) {
#ifdef _WIN32
    if (!::MoveFileExW(
            from.c_str(),
            to.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        throw windows_error("Cannot rename file");
    }
#else
    if (::rename(from.c_str(), to.c_str()) != 0) {
        throw std::system_error(
            errno, std::generic_category(),
            "Cannot rename file");
    }
#endif
}

void replace_file(const std::filesystem::path& from,
                  const std::filesystem::path& to) {
#ifdef _WIN32
    if (!::MoveFileExW(
            from.c_str(),
            to.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw windows_error("Cannot replace file");
    }
#else
    if (::rename(from.c_str(), to.c_str()) != 0) {
        throw std::system_error(
            errno, std::generic_category(),
            "Cannot replace file");
    }
#endif
}

FileLock::FileLock(const std::filesystem::path& path)
    : path_(path.string()) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,                       // no sharing while the lock is held
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (handle == INVALID_HANDLE_VALUE) {
        throw windows_error(
            "Database is already open or cannot be locked");
    }

    handle_ = handle;
#else
    descriptor_ = ::open(
        path.c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC,
        0666
    );

    if (descriptor_ == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Cannot open database lock file"
        );
    }

    if (::flock(descriptor_, LOCK_EX | LOCK_NB) == -1) {
        const int saved_errno = errno;
        ::close(descriptor_);
        descriptor_ = -1;

        throw std::system_error(
            saved_errno,
            std::generic_category(),
            "Database is already open or cannot be locked"
        );
    }
#endif
}

FileLock::~FileLock() {
#ifdef _WIN32
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
    }
#else
    if (descriptor_ != -1) {
        ::flock(descriptor_, LOCK_UN);
        ::close(descriptor_);
    }
#endif
}

} // namespace kvplatform
