#include "kv_store.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
    std::size_t put_count = 50000;
    std::size_t get_count = 50000;
    std::size_t value_size = 100;
    std::size_t l0_batches = 50;
    std::size_t l0_batch_size = 256;
    bool quick = false;
};

struct GetStats {
    double ops_per_sec = 0.0;
    double p50_us = 0.0;
    double p99_us = 0.0;
    std::size_t missing = 0;
};

double seconds(Clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

double micros(Clock::duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count();
}

std::string make_key(std::size_t index) {
    std::ostringstream out;
    out << "key-" << std::setw(10) << std::setfill('0') << index;
    return out.str();
}

double percentile(std::vector<double> values, double ratio) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        ratio * static_cast<double>(values.size() - 1));
    return values[index];
}

std::uintmax_t database_size(const KVStore& store) {
    std::uintmax_t total = 0;

    const auto add = [&](const std::string& path) {
        if (path.empty()) {
            return;
        }
        std::error_code local_error;
        if (std::filesystem::exists(path, local_error) && !local_error) {
            total += std::filesystem::file_size(path, local_error);
        }
    };

    add(store.wal_file_path());
    for (const auto& table : store.inspect_tables()) {
        add(table.path);
    }
    return total;
}

GetStats measure_random_gets(
    KVStore& store,
    std::size_t key_count,
    std::size_t count,
    unsigned int seed
) {
    std::mt19937 random(seed);
    std::uniform_int_distribution<std::size_t> pick(0, key_count - 1);
    std::vector<double> latencies;
    latencies.reserve(count);

    std::size_t missing = 0;
    const auto started = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const std::string key = make_key(pick(random));
        const auto before = Clock::now();
        const auto value = store.get(key);
        const auto after = Clock::now();
        latencies.push_back(micros(after - before));
        if (!value.has_value()) {
            ++missing;
        }
    }
    const auto finished = Clock::now();

    GetStats stats;
    stats.ops_per_sec =
        static_cast<double>(count) / seconds(finished - started);
    stats.p50_us = percentile(latencies, 0.50);
    stats.p99_us = percentile(latencies, 0.99);
    stats.missing = missing;
    return stats;
}

GetStats measure_key(KVStore& store, const std::string& key, std::size_t count) {
    std::vector<double> latencies;
    latencies.reserve(count);

    std::size_t missing = 0;
    const auto started = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const auto before = Clock::now();
        const auto value = store.get(key);
        const auto after = Clock::now();
        latencies.push_back(micros(after - before));
        if (!value.has_value()) {
            ++missing;
        }
    }
    const auto finished = Clock::now();

    GetStats stats;
    stats.ops_per_sec =
        static_cast<double>(count) / seconds(finished - started);
    stats.p50_us = percentile(latencies, 0.50);
    stats.p99_us = percentile(latencies, 0.99);
    stats.missing = missing;
    return stats;
}

std::string make_database_path(const std::string& tag) {
    const auto nonce = static_cast<long long>(
        Clock::now().time_since_epoch().count());
    const auto directory =
        std::filesystem::temp_directory_path()
        / ("kvstore-bench-" + tag + "-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    return (directory / "kvstore.log").string();
}

void cleanup_database(const std::string& database) {
    std::error_code error;
    std::filesystem::remove_all(
        std::filesystem::path(database).parent_path(), error);
}


void run_main_benchmark(const Config& config) {
    const std::string database = make_database_path("main");
    const std::string value(config.value_size, 'x');

    {
        KVStore store(database, config.put_count + 16);

        // ---- Put / Get（MemTable）------------------------------------
        const auto put_started = Clock::now();
        for (std::size_t i = 0; i < config.put_count; ++i) {
            store.put(make_key(i), value);
        }
        const auto put_finished = Clock::now();
        const double put_seconds = seconds(put_finished - put_started);

        const auto sync_started = Clock::now();
        store.sync();
        const auto sync_finished = Clock::now();

        const GetStats memtable_get = measure_random_gets(
            store, config.put_count, config.get_count, 12345);

        std::cout << "\n### Put/Get（value=" << config.value_size << " B）\n\n";
        std::cout << "| 指标 | 数值 |\n| --- | --- |\n";
        std::cout << "| Put 吞吐（未 fsync） | "
                  << std::fixed << std::setprecision(1)
                  << static_cast<double>(config.put_count) / put_seconds
                  << " ops/s |\n";
        std::cout << "| 一次 sync 耗时 | "
                  << std::setprecision(1)
                  << micros(sync_finished - sync_started) / 1000.0
                  << " ms |\n";
        std::cout << "| Get（MemTable）吞吐 | "
                  << std::setprecision(1) << memtable_get.ops_per_sec
                  << " ops/s |\n";
        std::cout << "| Get（MemTable）P50 / P99 | "
                  << std::setprecision(1) << memtable_get.p50_us
                  << " / " << memtable_get.p99_us
                  << " us |\n";

        // ---- Flush 后读 L0 -------------------------------------------
        const auto flush_started = Clock::now();
        store.flush();
        const auto flush_finished = Clock::now();
        const GetStats l0_get = measure_random_gets(
            store, config.put_count, config.get_count, 23456);

        std::cout << "| Flush 耗时 | "
                  << std::setprecision(1)
                  << micros(flush_finished - flush_started) / 1000.0
                  << " ms |\n";
        std::cout << "| Get（L0）吞吐 | "
                  << std::setprecision(1) << l0_get.ops_per_sec
                  << " ops/s |\n";
        std::cout << "| Get（L0）P50 / P99 | "
                  << std::setprecision(1) << l0_get.p50_us
                  << " / " << l0_get.p99_us
                  << " us |\n";

        // ---- Compact 前后 -------------------------------------------
        const std::uintmax_t size_before = database_size(store);
        const auto compact_started = Clock::now();
        store.compact();
        const auto compact_finished = Clock::now();
        const std::uintmax_t size_after = database_size(store);
        const GetStats base_get = measure_random_gets(
            store, config.put_count, config.get_count, 34567);

        std::cout << "| Compact 耗时 | "
                  << std::setprecision(1)
                  << micros(compact_finished - compact_started) / 1000.0
                  << " ms |\n";
        std::cout << "| Compact 前 / 后数据大小 | "
                  << size_before / 1024 << " / " << size_after / 1024
                  << " KiB |\n";
        std::cout << "| Get（BASE）吞吐 | "
                  << std::setprecision(1) << base_get.ops_per_sec
                  << " ops/s |\n";
        std::cout << "| Get（BASE）P50 / P99 | "
                  << std::setprecision(1) << base_get.p50_us
                  << " / " << base_get.p99_us
                  << " us |\n";
    }

    cleanup_database(database);
}

void run_value_size_sweep(const Config& config) {
    std::cout << "\n### value 大小对 Put/Get 的影响\n\n";
    std::cout << "| value | 记录数 | Put ops/s | Get ops/s | Get P99 (us) |\n";
    std::cout << "| --- | --- | --- | --- | --- |\n";

    for (const std::size_t value_size : {64u, 1024u, 4096u}) {
        const std::size_t count = value_size >= 4096 ? 10000 : 20000;
        const std::string database = make_database_path(
            "value-" + std::to_string(value_size));
        const std::string value(value_size, 'x');
        double put_ops = 0.0;
        GetStats get_stats;

        {
            KVStore store(database, count + 16);
            const auto started = Clock::now();
            for (std::size_t i = 0; i < count; ++i) {
                store.put(make_key(i), value);
            }
            const auto finished = Clock::now();
            put_ops =
                static_cast<double>(count) / seconds(finished - started);
            get_stats = measure_random_gets(store, count, config.quick ? 5000 : 20000, 4567);
        }
        cleanup_database(database);

        std::cout << "| " << value_size << " B | " << count
                  << " | " << std::fixed << std::setprecision(1) << put_ops
                  << " | " << std::setprecision(1) << get_stats.ops_per_sec
                  << " | " << std::setprecision(1) << get_stats.p99_us
                  << " |\n";
    }
}

void run_l0_amplification(const Config& config) {
    const std::string database = make_database_path("l0");
    const std::string value(100, 'x');

    std::cout << "\n### L0 数量对读延迟的影响\n\n";
    std::cout << "| L0 表数 | missing P50 (us) | victim P50 (us) |\n";
    std::cout << "| --- | --- | --- |\n";

    {
        KVStore store(database, config.l0_batch_size);

        const auto write_batch = [&](std::size_t batch, bool with_victim) {
            if (with_victim) {
                store.put("victim", value);
            }
            const std::size_t begin = with_victim ? 1 : 0;
            for (std::size_t i = begin; i < config.l0_batch_size; ++i) {
                store.put(
                    "filler-" + std::to_string(batch) + "-"
                        + std::to_string(i),
                    value);
            }
            store.flush();
        };

        write_batch(0, true);

        const auto print_row = [&] {
            const auto tables = store.inspect_tables();
            const std::size_t table_count = static_cast<std::size_t>(
                std::count_if(
                    tables.begin(),
                    tables.end(),
                    [](const KVStore::TableInfo& table) {
                        return table.kind == "L0";
                    }));
            const GetStats missing =
                measure_key(store, "missing", 200);
            const GetStats victim =
                measure_key(store, "victim", 200);
            std::cout << "| " << table_count
                      << " | " << std::fixed << std::setprecision(2)
                      << missing.p50_us
                      << " | " << std::setprecision(2)
                      << victim.p50_us
                      << " |\n";
        };

        print_row();

        for (std::size_t batch = 1; batch < config.l0_batches; ++batch) {
            write_batch(batch, false);

            if (batch == 9 || batch == 24
                || batch + 1 == config.l0_batches) {
                print_row();
            }
        }
    }

    cleanup_database(database);
}

} // namespace

int main(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--quick") {
            config.quick = true;
            config.put_count = 10000;
            config.get_count = 10000;
            config.l0_batches = 20;
            config.l0_batch_size = 128;
        }
    }

    std::cout << "# KVStore 基准测试\n\n";
    std::cout << "- 数据目录：`"
              << std::filesystem::temp_directory_path().string()
              << "` 下的临时目录\n";
    std::cout << "- 单线程，Put 不含 fsync；Get 为随机 key\n";
    std::cout << "- 记录数 " << config.put_count
              << "，value " << config.value_size << " B\n";

    run_main_benchmark(config);
    run_value_size_sweep(config);
    run_l0_amplification(config);

    std::cout << "\n完成。\n";
    return 0;
}
