cmake_minimum_required(VERSION 3.16)

project(
    KVStore
    VERSION 0.3.0
    DESCRIPTION "A persistent key-value storage engine"
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(kvstore STATIC
    src/kv_store.cpp
    src/log_record.cpp
    src/sstable.cpp
    src/manifest.cpp
    src/platform.cpp
)

target_compile_features(kvstore PUBLIC cxx_std_17)
set_target_properties(kvstore PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(kvstore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

function(enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endfunction()

enable_project_warnings(kvstore)

add_executable(kvstore_cli
    app/main.cpp
)

target_link_libraries(kvstore_cli PRIVATE kvstore)
set_target_properties(kvstore_cli PROPERTIES CXX_EXTENSIONS OFF)
enable_project_warnings(kvstore_cli)

add_executable(kvstore_bench
    bench/benchmark.cpp
)

target_link_libraries(kvstore_bench PRIVATE kvstore)
set_target_properties(kvstore_bench PROPERTIES CXX_EXTENSIONS OFF)
enable_project_warnings(kvstore_bench)

include(CTest)

find_package(Threads REQUIRED)
target_link_libraries(kvstore PUBLIC Threads::Threads)

if(BUILD_TESTING AND UNIX)
    find_package(Threads REQUIRED)
    add_executable(kvstore_tests
        tests/kv_store_test.cpp
    )

    target_link_libraries(kvstore_tests PRIVATE kvstore)
    set_target_properties(kvstore_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_tests)

    add_test(
        NAME kvstore_basic_tests
        COMMAND kvstore_tests
    )

    add_executable(kvstore_crc32_tests
        tests/crc32_test.cpp
    )

    target_link_libraries(kvstore_crc32_tests PRIVATE kvstore)
    set_target_properties(kvstore_crc32_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_crc32_tests)

    add_test(
        NAME kvstore_crc32_tests
        COMMAND kvstore_crc32_tests
    )

    add_executable(kvstore_disk_index_tests
        tests/disk_index_test.cpp
    )

    target_link_libraries(kvstore_disk_index_tests PRIVATE kvstore)
    set_target_properties(kvstore_disk_index_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_disk_index_tests)

    add_test(
        NAME kvstore_disk_index_tests
        COMMAND kvstore_disk_index_tests
    )

    add_executable(kvstore_compaction_tests
        tests/compaction_test.cpp
    )

    target_link_libraries(kvstore_compaction_tests PRIVATE kvstore)
    set_target_properties(kvstore_compaction_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_compaction_tests)

    add_test(
        NAME kvstore_compaction_tests
        COMMAND kvstore_compaction_tests
    )

    add_executable(kvstore_concurrency_tests
        tests/concurrency_test.cpp
    )

    target_link_libraries(kvstore_concurrency_tests
        PRIVATE kvstore Threads::Threads
    )
    set_target_properties(kvstore_concurrency_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_concurrency_tests)

    add_test(
        NAME kvstore_concurrency_tests
        COMMAND kvstore_concurrency_tests
    )

    add_executable(kvstore_wal_memtable_tests
        tests/wal_memtable_test.cpp
    )
    target_link_libraries(kvstore_wal_memtable_tests PRIVATE kvstore)
    set_target_properties(kvstore_wal_memtable_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_wal_memtable_tests)
    add_test(
        NAME kvstore_wal_memtable_tests
        COMMAND kvstore_wal_memtable_tests
    )

    add_executable(kvstore_shadow_tests
        tests/my_shadow_test.cpp
    )
    target_link_libraries(kvstore_shadow_tests PRIVATE kvstore)
    set_target_properties(kvstore_shadow_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_shadow_tests)
    add_test(
        NAME kvstore_shadow_tests
        COMMAND kvstore_shadow_tests
    )

    add_executable(kvstore_model_tests
        tests/model_test.cpp
    )
    target_link_libraries(kvstore_model_tests PRIVATE kvstore)
    set_target_properties(kvstore_model_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_model_tests)
    add_test(
        NAME kvstore_model_tests
        COMMAND kvstore_model_tests
    )

    add_executable(kvstore_sstable_tests
        tests/sstable_test.cpp
    )
    target_link_libraries(kvstore_sstable_tests PRIVATE kvstore)
    set_target_properties(kvstore_sstable_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_sstable_tests)
    add_test(
        NAME kvstore_sstable_tests
        COMMAND kvstore_sstable_tests
    )

    add_executable(kvstore_level0_flush_tests
        tests/level0_flush_test.cpp
    )
    target_link_libraries(kvstore_level0_flush_tests PRIVATE kvstore)
    set_target_properties(kvstore_level0_flush_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_level0_flush_tests)
    add_test(
        NAME kvstore_level0_flush_tests
        COMMAND kvstore_level0_flush_tests
    )

    add_executable(kvstore_background_flush_tests
        tests/background_flush_test.cpp
    )
    target_link_libraries(kvstore_background_flush_tests PRIVATE kvstore)
    set_target_properties(kvstore_background_flush_tests PROPERTIES CXX_EXTENSIONS OFF)
    enable_project_warnings(kvstore_background_flush_tests)
    add_test(
        NAME kvstore_background_flush_tests
        COMMAND kvstore_background_flush_tests
    )

    add_executable(kvstore_manifest_tests tests/manifest_test.cpp)
    target_link_libraries(kvstore_manifest_tests PRIVATE kvstore)
    enable_project_warnings(kvstore_manifest_tests)
    add_test(NAME kvstore_manifest_tests COMMAND kvstore_manifest_tests)

    add_executable(kvstore_inspect_tests tests/inspect_test.cpp)
    target_link_libraries(kvstore_inspect_tests PRIVATE kvstore)
    enable_project_warnings(kvstore_inspect_tests)
    add_test(NAME kvstore_inspect_tests COMMAND kvstore_inspect_tests)

    set_tests_properties(kvstore_background_flush_tests kvstore_manifest_tests
        PROPERTIES TIMEOUT 60)
endif()

if(BUILD_TESTING AND NOT UNIX)
    message(STATUS "KVStore tests use POSIX APIs and are skipped on this platform")
endif()

option(KVSTORE_BUILD_QT_DEMO "Build the Qt Widgets demo" OFF)
if(KVSTORE_BUILD_QT_DEMO)
    add_subdirectory(qt-demo)
endif()

add_executable(file_demo file_demo.cpp)
target_compile_features(file_demo PRIVATE cxx_std_17)
set_target_properties(file_demo PROPERTIES CXX_EXTENSIONS OFF)
