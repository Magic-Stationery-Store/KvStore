#include "kv_store.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr std::size_t kKeyCount = 50000;
constexpr std::size_t kMemTableLimit = 128;

std::string make_key(std::size_t index) {
    return "key-" + std::to_string(index);
}

std::string make_value(std::size_t index) {
    return "value-" + std::to_string(index);
}

[[noreturn]] void run_child(const std::string& database, int ready_fd) {
    try {
        KVStore store(database, kMemTableLimit);
        const char ready = '1';
        if (::write(ready_fd, &ready, 1) != 1) {
            ::_exit(2);
        }
        ::close(ready_fd);

        for (std::size_t i = 0; i < kKeyCount; ++i) {
            store.put(make_key(i), make_value(i));
        }
        ::_exit(0);
    } catch (...) {
        ::_exit(3);
    }
}

bool verify_prefix(
    const std::string& database,
    std::size_t& survived,
    std::string& error
) {
    try {
        KVStore store(database, kMemTableLimit);

        // Keys are written in order, so a valid recovery must leave a prefix.
        std::size_t low = 0;
        std::size_t high = kKeyCount;
        while (low < high) {
            const std::size_t middle = low + (high - low) / 2;
            if (store.get(make_key(middle)).has_value()) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        const std::size_t first_missing = low;

        for (std::size_t i = 0; i < first_missing; ++i) {
            const auto value = store.get(make_key(i));
            if (!value.has_value()) {
                error = "gap before boundary: " + make_key(i);
                return false;
            }
            if (*value != make_value(i)) {
                error = "value mismatch at " + make_key(i);
                return false;
            }
        }

        if (first_missing < kKeyCount
            && store.get(make_key(first_missing)).has_value()) {
            error = "boundary key unexpectedly present: "
                + make_key(first_missing);
            return false;
        }
        if (first_missing + 1 < kKeyCount
            && store.get(make_key(first_missing + 1)).has_value()) {
            error = "a key survived after the first missing key";
            return false;
        }

        survived = first_missing;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::size_t iteration_count() {
    const char* text = std::getenv("KVSTORE_CRASH_ITERATIONS");
    if (text == nullptr) {
        return 100;
    }
    const long parsed = std::strtol(text, nullptr, 10);
    if (parsed <= 0) {
        return 100;
    }
    return static_cast<std::size_t>(parsed);
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path()
        / ("kvstore-crash-" + std::to_string(::getpid()));
    std::error_code error_code;
    std::filesystem::remove_all(root, error_code);
    std::filesystem::create_directories(root, error_code);

    const std::size_t iterations = iteration_count();
    std::mt19937 random(20261113u);
    std::uniform_int_distribution<int> delay_ms(1, 15);

    std::size_t failures = 0;
    std::size_t attempted = 0;
    std::size_t killed = 0;
    std::size_t total_survived = 0;
    std::size_t min_survived = kKeyCount;
    std::size_t max_survived = 0;

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        ++attempted;
        std::filesystem::remove_all(root, error_code);
        std::filesystem::create_directories(root, error_code);

        const std::string database = (root / "kvstore.log").string();
        int pipe_fds[2] = {-1, -1};
        if (::pipe(pipe_fds) != 0) {
            std::cerr << "FAIL: cannot create pipe\n";
            return 1;
        }

        const pid_t child = ::fork();
        if (child == -1) {
            std::cerr << "FAIL: fork failed\n";
            return 1;
        }
        if (child == 0) {
            ::close(pipe_fds[0]);
            run_child(database, pipe_fds[1]);
        }

        ::close(pipe_fds[1]);
        char ready = 0;
        ssize_t read_result = 0;
        do {
            read_result = ::read(pipe_fds[0], &ready, 1);
        } while (read_result == -1 && errno == EINTR);
        ::close(pipe_fds[0]);

        if (read_result != 1) {
            int status = 0;
            ::waitpid(child, &status, 0);
            std::cerr << "FAIL: child did not initialize database\n";
            ++failures;
            break;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(delay_ms(random)));

        if (::kill(child, SIGKILL) == 0) {
            ++killed;
        }

        int status = 0;
        ::waitpid(child, &status, 0);

        std::size_t survived = 0;
        std::string error;
        if (!verify_prefix(database, survived, error)) {
            std::cerr << "FAIL iteration " << iteration
                      << ": " << error << '\n';
            ++failures;
        } else {
            total_survived += survived;
            min_survived = std::min(min_survived, survived);
            max_survived = std::max(max_survived, survived);
        }
    }

    std::filesystem::remove_all(root, error_code);

    const std::size_t verified = attempted - failures;
    if (verified == 0) {
        std::cerr << "FAIL: no iteration survived verification\n";
        return 1;
    }

    std::cout << "crash iterations: " << iterations
              << ", killed: " << killed
              << ", verified: " << verified
              << ", failures: " << failures << '\n';
    std::cout << "surviving prefix keys: min=" << min_survived
              << ", avg=" << (total_survived / verified)
              << ", max=" << max_survived << '\n';

    if (failures != 0) {
        return 1;
    }
    std::cout << "PASS: SIGKILL recovery keeps a valid WAL prefix\n";
    return 0;
}
