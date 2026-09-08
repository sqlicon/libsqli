cmake_minimum_required(VERSION 3.16)
project(sqli VERSION 0.1.38 LANGUAGES C)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SQLI_IS_TOP_LEVEL ON)
else()
    set(SQLI_IS_TOP_LEVEL OFF)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if (MSVC)
    # Match the existing "warnings are errors" intent with MSVC-compatible flags.
    add_compile_options(/W4 /WX)
    # Minimize Release executable size: optimize for size instead of speed,
    # let the linker discard unreferenced functions/data (important for the
    # statically linked OpenSSL libs, which pull in far more than
    # this project actually calls), and fold identical COMDATs. Whole-program
    # optimization (/GL + LTCG) is required for /OPT:REF and /OPT:ICF to see
    # across translation units, including into the static "sqli" library.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    add_compile_options(
        $<$<CONFIG:Release>:/O1>
        $<$<CONFIG:Release>:/Gw>
    )
    add_link_options(
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
else()
    # Warnings: treat the codebase as if compiled with -Wall -Wextra -Werror -pedantic
    add_compile_options(-Wall -Wextra -Werror -pedantic)
endif()

option(SQLI_ENABLE_SANITIZERS "Build with address/undefined sanitizers" ON)
option(SQLI_ENABLE_LIVE_TESTS "Enable live Informix integration test(s)" OFF)
set(SQLI_BUILD_TESTS_DEFAULT ${SQLI_IS_TOP_LEVEL})
set(SQLI_BUILD_TOOLS_DEFAULT ${SQLI_IS_TOP_LEVEL})
set(SQLI_BUILD_CLI_DEFAULT ${SQLI_IS_TOP_LEVEL})
if (WIN32)
    set(SQLI_BUILD_TESTS_DEFAULT ${SQLI_IS_TOP_LEVEL})
    set(SQLI_BUILD_TOOLS_DEFAULT OFF)
    set(SQLI_BUILD_CLI_DEFAULT OFF)
endif()
option(SQLI_BUILD_TESTS "Build the unit/integration test executable" ${SQLI_BUILD_TESTS_DEFAULT})
option(SQLI_BUILD_TOOLS "Build helper tools and probes" ${SQLI_BUILD_TOOLS_DEFAULT})
option(SQLI_BUILD_CLI "Build the sqlicon interactive CLI" ${SQLI_BUILD_CLI_DEFAULT})

if (SQLI_ENABLE_SANITIZERS)
    if (MSVC)
        message(STATUS "SQLI_ENABLE_SANITIZERS is ignored for MSVC builds")
    else()
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# Source directory
set(SQLI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SQLI_INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(SQLI_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test)
set(SQLI_GENERATED_INC_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/include)
configure_file(${SQLI_INC_DIR}/libsqli/version.h.in
    ${SQLI_GENERATED_INC_DIR}/libsqli/version.h @ONLY)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
include(FetchContent)

# ----------------------------------------------------------------
# Library target
# ----------------------------------------------------------------

add_library(sqli STATIC
    ${SQLI_SRC_DIR}/sqli_charset.h
    ${SQLI_SRC_DIR}/sqli_charset.c
    ${SQLI_SRC_DIR}/sqli_conn.c
    ${SQLI_SRC_DIR}/sqli_tls.c
    ${SQLI_SRC_DIR}/sqli_wire.c
    ${SQLI_SRC_DIR}/sqli_asc.c
    ${SQLI_SRC_DIR}/sqli_handshake.c
    ${SQLI_SRC_DIR}/sqli_dispatch.c
    ${SQLI_SRC_DIR}/sqli_protocol_write.c
    ${SQLI_SRC_DIR}/sqli_query.c
    ${SQLI_SRC_DIR}/sqli_batch.c
    ${SQLI_SRC_DIR}/sqli_result.c
    ${SQLI_SRC_DIR}/sqli_result_value.c
    ${SQLI_SRC_DIR}/sqli_scalar.c
    ${SQLI_SRC_DIR}/sqli_status.c
    ${SQLI_SRC_DIR}/sqli_prepare.c
    ${SQLI_SRC_DIR}/sqli_types.c
    ${SQLI_SRC_DIR}/sqli_descriptor.c
    ${SQLI_SRC_DIR}/sqli_decimal.c
    ${SQLI_SRC_DIR}/sqli_decimal_codec.c
    ${SQLI_SRC_DIR}/sqli_temporal.c
    ${SQLI_SRC_DIR}/sqli_temporal_codec.c
    ${SQLI_SRC_DIR}/sqli_transaction.c
    ${SQLI_SRC_DIR}/sqli_pool.c
    ${SQLI_SRC_DIR}/sqli_sqlhosts.c
    ${SQLI_SRC_DIR}/sqli_log.c
    ${SQLI_SRC_DIR}/sqli_msg_catalog.c
    ${SQLI_SRC_DIR}/sqli_uri.c
    ${SQLI_SRC_DIR}/ifx_errmsg.c
    ${SQLI_SRC_DIR}/ifx_errmsg_data.c
    ${SQLI_SRC_DIR}/sqli_sblob.c
)
if (NOT WIN32)
    target_sources(sqli PRIVATE
        ${SQLI_SRC_DIR}/sqli_charset_posix.c
        ${SQLI_SRC_DIR}/sqli_tcp_posix.c
        ${SQLI_SRC_DIR}/sqli_cancel_posix.c
        ${SQLI_SRC_DIR}/sqli_unix_posix.c
    )
else()
    target_sources(sqli PRIVATE
        ${SQLI_SRC_DIR}/sqli_charset_win.c
        ${SQLI_SRC_DIR}/sqli_tcp_win.c
        ${SQLI_SRC_DIR}/sqli_unix_win.c
    )
    target_compile_definitions(sqli PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
endif()
target_include_directories(sqli
    PUBLIC
        $<BUILD_INTERFACE:${SQLI_INC_DIR}>
        $<BUILD_INTERFACE:${SQLI_GENERATED_INC_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${SQLI_SRC_DIR}
)
target_link_libraries(sqli PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
add_library(sqlicon::sqli ALIAS sqli)

# ----------------------------------------------------------------
# Test executable
# ----------------------------------------------------------------
if (SQLI_BUILD_TESTS)
    FetchContent_Declare(
        unity
        GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
        GIT_TAG v2.6.0
    )
    FetchContent_MakeAvailable(unity)

    set(UNITY_SRC
        ${unity_SOURCE_DIR}/src/unity.c
    )

    set(SQLI_TEST_SOURCES_COMMON
        ${SQLI_TEST_DIR}/sqli_test_main.c
        ${SQLI_TEST_DIR}/test_sqli_tcp.c
        ${SQLI_TEST_DIR}/test_sqli_wire.c
        ${SQLI_TEST_DIR}/test_sqli_types.c
        ${SQLI_TEST_DIR}/test_stability.c
        ${SQLI_TEST_DIR}/test_sqli_callable.c
        ${SQLI_TEST_DIR}/test_sqli_prepare.c
    )
    set(SQLI_TEST_SOURCES_POSIX
        ${SQLI_TEST_DIR}/test_sqli.c
        ${SQLI_TEST_DIR}/test_sqli_dispatch.c
        ${SQLI_TEST_DIR}/test_sqli_transaction.c
        ${SQLI_TEST_DIR}/test_sqli_sqlhosts.c
        ${SQLI_TEST_DIR}/test_sqli_uri.c
        ${SQLI_TEST_DIR}/test_integration.c
    )

    add_executable(sqli_test
        ${SQLI_TEST_SOURCES_COMMON}
        ${UNITY_SRC}
    )
    if (NOT WIN32)
        target_sources(sqli_test PRIVATE ${SQLI_TEST_SOURCES_POSIX})
    endif()
    target_include_directories(sqli_test PRIVATE
        ${SQLI_INC_DIR}
        ${SQLI_SRC_DIR}
        ${unity_SOURCE_DIR}/src
    )
    target_link_libraries(sqli_test PRIVATE sqli)
    if (WIN32)
        target_compile_definitions(sqli_test PRIVATE SQLI_HAVE_POSIX_TESTS=0)
    else()
        target_compile_definitions(sqli_test PRIVATE SQLI_HAVE_POSIX_TESTS=1)
    endif()
    if (SQLI_ENABLE_LIVE_TESTS)
        target_compile_definitions(sqli_test PRIVATE SQLI_ENABLE_LIVE_TESTS=1)
    endif()

    enable_testing()
    add_test(NAME sqli_unit COMMAND sqli_test)

    if (NOT WIN32)
        add_executable(sqli_sblob_progress_test ${SQLI_TEST_DIR}/test_sblob_progress_posix.c ${UNITY_SRC})
        target_include_directories(sqli_sblob_progress_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_sblob_progress_test PRIVATE sqli)
        add_test(NAME sqli_sblob_progress COMMAND sqli_sblob_progress_test)
        if (SQLI_ENABLE_SANITIZERS)
            set_tests_properties(sqli_sblob_progress PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
        add_executable(sqli_descriptor_test ${SQLI_TEST_DIR}/test_descriptor_posix.c ${UNITY_SRC})
        target_include_directories(sqli_descriptor_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_descriptor_test PRIVATE sqli)
        add_test(NAME sqli_descriptors COMMAND sqli_descriptor_test)
        if (SQLI_ENABLE_SANITIZERS)
            set_tests_properties(sqli_descriptors PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
    endif()

    add_executable(sqli_decimal_matrix ${SQLI_TEST_DIR}/test_decimal_matrix.c)
    target_include_directories(sqli_decimal_matrix PRIVATE ${SQLI_SRC_DIR})
    target_link_libraries(sqli_decimal_matrix PRIVATE sqli)
    if (SQLI_ENABLE_LIVE_TESTS)
        add_test(NAME sqli_decimal_live COMMAND sqli_decimal_matrix)
        set_tests_properties(sqli_decimal_live PROPERTIES
            LABELS "live;native;decimal" SKIP_RETURN_CODE 77 TIMEOUT 900)
    endif()

    add_executable(sqli_decimal_codec_test ${SQLI_TEST_DIR}/test_decimal_codec.c ${UNITY_SRC})
    target_include_directories(sqli_decimal_codec_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
    target_link_libraries(sqli_decimal_codec_test PRIVATE sqli)
    add_test(NAME sqli_decimal_codecs COMMAND sqli_decimal_codec_test)

    add_executable(sqli_temporal_codec_test ${SQLI_TEST_DIR}/test_temporal_codec.c ${UNITY_SRC})
    target_include_directories(sqli_temporal_codec_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
    target_link_libraries(sqli_temporal_codec_test PRIVATE sqli)
    add_test(NAME sqli_temporal_codecs COMMAND sqli_temporal_codec_test)

    add_executable(sqli_temporal_test ${SQLI_TEST_DIR}/test_sqli_temporal.c ${UNITY_SRC})
    target_include_directories(sqli_temporal_test PRIVATE ${unity_SOURCE_DIR}/src)
    target_link_libraries(sqli_temporal_test PRIVATE sqli)
    add_test(NAME sqli_temporal_values COMMAND sqli_temporal_test)

    add_executable(sqli_decimal_test ${SQLI_TEST_DIR}/test_sqli_decimal.c ${UNITY_SRC})
    target_include_directories(sqli_decimal_test PRIVATE ${unity_SOURCE_DIR}/src)
    target_link_libraries(sqli_decimal_test PRIVATE sqli)
    add_test(NAME sqli_decimal_values COMMAND sqli_decimal_test)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        add_executable(sqli_api_consistency_test
            ${SQLI_TEST_DIR}/test_api_consistency_linux.c
            ${SQLI_TEST_DIR}/allocation_test_linux.c ${UNITY_SRC})
        target_include_directories(sqli_api_consistency_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_api_consistency_test PRIVATE sqli)
        target_link_options(sqli_api_consistency_test PRIVATE
            "LINKER:--wrap=malloc" "LINKER:--wrap=calloc")
        add_test(NAME sqli_api_consistency COMMAND sqli_api_consistency_test)
        if (SQLI_ENABLE_SANITIZERS)
            set_tests_properties(sqli_api_consistency PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
        add_executable(sqli_native_temporal_test
            ${SQLI_TEST_DIR}/test_native_temporal_linux.c
            ${SQLI_TEST_DIR}/allocation_test_linux.c ${UNITY_SRC})
        target_include_directories(sqli_native_temporal_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_native_temporal_test PRIVATE sqli)
        target_link_options(sqli_native_temporal_test PRIVATE
            "LINKER:--wrap=malloc" "LINKER:--wrap=calloc")
        add_test(NAME sqli_native_temporal COMMAND sqli_native_temporal_test)
        if (SQLI_ENABLE_SANITIZERS)
            set_tests_properties(sqli_native_temporal PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
        add_executable(sqli_native_date_getter_test
            ${SQLI_TEST_DIR}/test_native_date_getter_linux.c
            ${SQLI_TEST_DIR}/allocation_test_linux.c ${UNITY_SRC})
        target_include_directories(sqli_native_date_getter_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_native_date_getter_test PRIVATE sqli)
        target_link_options(sqli_native_date_getter_test PRIVATE
            "LINKER:--wrap=malloc" "LINKER:--wrap=calloc")
        add_test(NAME sqli_native_date_getter COMMAND sqli_native_date_getter_test)
        if (SQLI_ENABLE_SANITIZERS)
            set_tests_properties(sqli_native_date_getter PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
        add_executable(sqli_native_decimal_getter_test
            ${SQLI_TEST_DIR}/test_native_decimal_getter_linux.c
            ${SQLI_TEST_DIR}/allocation_test_linux.c ${UNITY_SRC})
        target_include_directories(sqli_native_decimal_getter_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_native_decimal_getter_test PRIVATE sqli)
        target_link_options(sqli_native_decimal_getter_test PRIVATE
            "LINKER:--wrap=malloc" "LINKER:--wrap=calloc")
        add_test(NAME sqli_native_decimal_getter COMMAND sqli_native_decimal_getter_test)
        if (SQLI_ENABLE_SANITIZERS)
            set_tests_properties(sqli_native_decimal_getter PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
        add_executable(sqli_result_fetch_test
            ${SQLI_TEST_DIR}/test_result_fetch_linux.c
            ${SQLI_TEST_DIR}/allocation_test_linux.c ${UNITY_SRC})
        target_include_directories(sqli_result_fetch_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_result_fetch_test PRIVATE sqli)
        target_link_options(sqli_result_fetch_test PRIVATE
            "LINKER:--wrap=malloc" "LINKER:--wrap=calloc")
        add_test(NAME sqli_result_fetch COMMAND sqli_result_fetch_test)
        if (SQLI_ENABLE_SANITIZERS)
            set_tests_properties(sqli_result_fetch PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
        add_executable(sqli_descriptor_alloc_test
            ${SQLI_TEST_DIR}/test_descriptor_alloc_linux.c
            ${SQLI_TEST_DIR}/allocation_test_linux.c ${UNITY_SRC})
        target_include_directories(sqli_descriptor_alloc_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_descriptor_alloc_test PRIVATE sqli)
        target_link_options(sqli_descriptor_alloc_test PRIVATE
            "LINKER:--wrap=malloc" "LINKER:--wrap=calloc")
        add_test(NAME sqli_descriptor_allocation COMMAND sqli_descriptor_alloc_test)
        if (SQLI_ENABLE_SANITIZERS)
            set_tests_properties(sqli_descriptor_allocation PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
        add_executable(sqli_decimal_alloc_test
            ${SQLI_TEST_DIR}/test_decimal_alloc_linux.c
            ${SQLI_TEST_DIR}/allocation_test_linux.c ${UNITY_SRC})
        target_include_directories(sqli_decimal_alloc_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_decimal_alloc_test PRIVATE sqli)
        target_link_options(sqli_decimal_alloc_test PRIVATE
            "LINKER:--wrap=malloc" "LINKER:--wrap=calloc")
        add_test(NAME sqli_decimal_allocation COMMAND sqli_decimal_alloc_test)
        add_executable(sqli_temporal_alloc_test
            ${SQLI_TEST_DIR}/test_temporal_alloc_linux.c
            ${SQLI_TEST_DIR}/allocation_test_linux.c ${UNITY_SRC})
        target_include_directories(sqli_temporal_alloc_test PRIVATE ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_temporal_alloc_test PRIVATE sqli)
        target_link_options(sqli_temporal_alloc_test PRIVATE
            "LINKER:--wrap=malloc" "LINKER:--wrap=calloc")
        add_test(NAME sqli_temporal_allocation COMMAND sqli_temporal_alloc_test)
        if (SQLI_ENABLE_SANITIZERS)
            set_tests_properties(sqli_decimal_allocation sqli_temporal_allocation PROPERTIES
                ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
    endif()

    add_executable(sqli_native_wire_contract ${SQLI_TEST_DIR}/test_native_wire_contract.c
        ${SQLI_TEST_DIR}/native_wire_test.c)
    target_include_directories(sqli_native_wire_contract PRIVATE ${SQLI_SRC_DIR})
    target_link_libraries(sqli_native_wire_contract PRIVATE sqli)
    add_test(NAME sqli_native_wire_offline COMMAND sqli_native_wire_contract --offline)
    if (SQLI_ENABLE_LIVE_TESTS)
        add_test(NAME sqli_native_wire_live COMMAND sqli_native_wire_contract --live)
        add_test(NAME sqli_native_bind_live COMMAND sqli_native_wire_contract --live-bind)
        set_tests_properties(sqli_native_wire_live sqli_native_bind_live PROPERTIES
            LABELS "live;native" SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(sqli_cancel_lifecycle_test
            ${SQLI_TEST_DIR}/test_cancel_lifecycle_posix.c ${UNITY_SRC})
        target_include_directories(sqli_cancel_lifecycle_test PRIVATE
            ${unity_SOURCE_DIR}/src ${SQLI_SRC_DIR})
        target_link_libraries(sqli_cancel_lifecycle_test PRIVATE sqli)
        target_link_options(sqli_cancel_lifecycle_test PRIVATE
            "LINKER:--wrap=SSL_shutdown" "LINKER:--wrap=sqli_tcp_interrupt"
            "LINKER:--wrap=sqli_connect")
        add_test(NAME sqli_cancel_lifecycle COMMAND sqli_cancel_lifecycle_test)
        set_tests_properties(sqli_cancel_lifecycle PROPERTIES TIMEOUT 20)
    endif()

    # Manual protocol experiment; deliberately excluded from automatic CTest runs.
    if (SQLI_ENABLE_LIVE_TESTS AND UNIX)
        add_executable(sqli_cancel_probe ${SQLI_TEST_DIR}/cancel_probe_posix.c)
        target_include_directories(sqli_cancel_probe PRIVATE ${SQLI_SRC_DIR})
        target_link_libraries(sqli_cancel_probe PRIVATE sqli Threads::Threads)
        add_executable(sqli_cancel_race_probe ${SQLI_TEST_DIR}/cancel_race_probe_posix.c)
        target_include_directories(sqli_cancel_race_probe PRIVATE ${SQLI_SRC_DIR})
        target_link_libraries(sqli_cancel_race_probe PRIVATE sqli Threads::Threads)
    endif()

    add_executable(sqli_temporal_matrix ${SQLI_TEST_DIR}/test_temporal_matrix.c)
    target_include_directories(sqli_temporal_matrix PRIVATE ${SQLI_SRC_DIR})
    target_link_libraries(sqli_temporal_matrix PRIVATE sqli)
    add_test(NAME sqli_temporal_generator COMMAND sqli_temporal_matrix --self-test)
    if (SQLI_ENABLE_LIVE_TESTS)
        add_test(NAME sqli_temporal_live COMMAND sqli_temporal_matrix --live)
        set_tests_properties(sqli_temporal_live PROPERTIES
            LABELS "live;temporal" SKIP_RETURN_CODE 77 TIMEOUT 900)
    endif()

    if (SQLI_ENABLE_SANITIZERS AND NOT MSVC)
        set_tests_properties(sqli_unit sqli_temporal_generator sqli_native_wire_offline sqli_decimal_values sqli_temporal_values sqli_temporal_codecs sqli_decimal_codecs PROPERTIES
            ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
    endif()
endif()

# ----------------------------------------------------------------
# Demo tool
# ----------------------------------------------------------------

set(SQLI_TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools)

if (SQLI_BUILD_TOOLS)
    add_executable(sqliconn ${SQLI_TOOLS_DIR}/sqliconn.c)
    target_include_directories(sqliconn PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sqliconn PRIVATE sqli)

    add_executable(sqli_stress ${SQLI_TOOLS_DIR}/sqli_stress.c)
    target_include_directories(sqli_stress PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sqli_stress PRIVATE sqli)

    add_executable(sqli_export_csv ${SQLI_TOOLS_DIR}/sqli_export_csv.c)
    target_include_directories(sqli_export_csv PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sqli_export_csv PRIVATE sqli)

    add_executable(sqli_prepared_probe ${SQLI_TOOLS_DIR}/sqli_prepared_probe.c)
    target_include_directories(sqli_prepared_probe PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sqli_prepared_probe PRIVATE sqli)

    add_executable(sqli_select_probe ${SQLI_TOOLS_DIR}/sqli_select_probe.c)
    target_include_directories(sqli_select_probe PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sqli_select_probe PRIVATE sqli)

    add_executable(sqli_batch_probe ${SQLI_TOOLS_DIR}/sqli_batch_probe.c)
    target_include_directories(sqli_batch_probe PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sqli_batch_probe PRIVATE sqli)

    add_executable(sqli_wire_capture ${SQLI_TEST_DIR}/test_wire_capture.c)
    target_include_directories(sqli_wire_capture PRIVATE ${SQLI_INC_DIR} ${SQLI_SRC_DIR})
    target_link_libraries(sqli_wire_capture PRIVATE sqli)

    add_executable(sysdbslocale_dump ${SQLI_TOOLS_DIR}/sysdbslocale_dump.c)
    target_include_directories(sysdbslocale_dump PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sysdbslocale_dump PRIVATE sqli)

    add_executable(sqli_types_stress ${SQLI_TOOLS_DIR}/sqli_types_stress.c)
    target_include_directories(sqli_types_stress PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sqli_types_stress PRIVATE sqli)

    add_executable(sqli_blob_stress ${SQLI_TOOLS_DIR}/sqli_blob_stress.c)
    target_include_directories(sqli_blob_stress PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sqli_blob_stress PRIVATE sqli)

    add_executable(sqli_bench_connect ${SQLI_TOOLS_DIR}/sqli_bench_connect.c)
    target_include_directories(sqli_bench_connect PRIVATE ${SQLI_INC_DIR})
    target_link_libraries(sqli_bench_connect PRIVATE sqli m)
endif()

if (SQLI_BUILD_CLI)
    if (NOT WIN32)
        FetchContent_Declare(
            linenoise
            GIT_REPOSITORY https://github.com/antirez/linenoise.git
            GIT_TAG 1.0
        )
        FetchContent_MakeAvailable(linenoise)

        add_library(linenoise_lib STATIC
            ${linenoise_SOURCE_DIR}/linenoise.c
        )
        target_include_directories(linenoise_lib PUBLIC
            ${linenoise_SOURCE_DIR}
        )
        target_compile_options(linenoise_lib PRIVATE -w)
        target_compile_definitions(linenoise_lib PRIVATE _DEFAULT_SOURCE)
    endif()

    add_executable(sqlicon
        ${SQLI_TOOLS_DIR}/sqlicon_main.c
        ${SQLI_TOOLS_DIR}/sqlicon_cli.c
        ${SQLI_TOOLS_DIR}/sqlicon_platform.h
        ${SQLI_TOOLS_DIR}/sqlicon_profile.c
        ${SQLI_TOOLS_DIR}/sqlicon_crypto.c
        ${SQLI_TOOLS_DIR}/sqlicon_runtime.c
        ${SQLI_TOOLS_DIR}/sqlicon_table.c
        ${SQLI_TOOLS_DIR}/sqlicon_output.c
        ${SQLI_TOOLS_DIR}/sqlicon_exec.c
        ${SQLI_TOOLS_DIR}/sqlicon_commands.c
        ${SQLI_TOOLS_DIR}/sqlicon_dotcmd.c
        ${SQLI_TOOLS_DIR}/sqlicon_stream.c
    )
    if (NOT WIN32)
        target_sources(sqlicon PRIVATE ${SQLI_TOOLS_DIR}/sqlicon_platform_posix.c)
        target_link_libraries(sqlicon PRIVATE sqli linenoise_lib)
    else()
        target_sources(sqlicon PRIVATE ${SQLI_TOOLS_DIR}/sqlicon_platform_win.c)
        target_link_libraries(sqlicon PRIVATE sqli advapi32)
    endif()
    target_include_directories(sqlicon PRIVATE ${SQLI_INC_DIR} ${SQLI_TOOLS_DIR})
    configure_file(${SQLI_TOOLS_DIR}/sqlicon.1.in ${CMAKE_CURRENT_BINARY_DIR}/sqlicon.1 @ONLY)
    if(SQLI_BUILD_TESTS)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            add_executable(sqlicon_output_test ${SQLI_TEST_DIR}/test_sqlicon_output_linux.c
                ${SQLI_TEST_DIR}/allocation_test_linux.c ${UNITY_SRC}
                ${SQLI_TOOLS_DIR}/sqlicon_table.c ${SQLI_TOOLS_DIR}/sqlicon_output.c)
            target_include_directories(sqlicon_output_test PRIVATE
                ${SQLI_TOOLS_DIR} ${SQLI_SRC_DIR} ${unity_SOURCE_DIR}/src)
            target_link_libraries(sqlicon_output_test PRIVATE sqli)
            target_link_options(sqlicon_output_test PRIVATE "LINKER:--wrap=malloc" "LINKER:--wrap=calloc")
            add_test(NAME sqlicon_output COMMAND sqlicon_output_test)
            if(SQLI_ENABLE_SANITIZERS)
                set_tests_properties(sqlicon_output PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
            endif()
        endif()
        add_executable(sqlicon_cli_test ${SQLI_TEST_DIR}/test_sqlicon_cli.c
            ${SQLI_TOOLS_DIR}/sqlicon_cli.c ${SQLI_TOOLS_DIR}/sqlicon_profile.c
            ${SQLI_TOOLS_DIR}/sqlicon_crypto.c ${UNITY_SRC})
        target_include_directories(sqlicon_cli_test PRIVATE ${SQLI_TOOLS_DIR} ${unity_SOURCE_DIR}/src)
        target_link_libraries(sqlicon_cli_test PRIVATE sqli)
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cli-test-profiles)
        add_test(NAME sqlicon_cli_options COMMAND ${CMAKE_COMMAND} -E env
            SQLI_HOST=env-host SQLI_PASSWORD=env-password
            SQLICON_TEST_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-test-profiles
            $<TARGET_FILE:sqlicon_cli_test>)
        add_test(NAME sqlicon_cli_process COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:sqlicon> -DVERSION=${sqli_VERSION}
            -DMANPAGE=${CMAKE_CURRENT_BINARY_DIR}/sqlicon.1
            -P ${SQLI_TEST_DIR}/check_sqlicon_cli.cmake)
        if(SQLI_ENABLE_SANITIZERS AND NOT MSVC)
            set_tests_properties(sqlicon_cli_options sqlicon_cli_process PROPERTIES
                ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
        endif()
    endif()
endif()

# ----------------------------------------------------------------
# Install
# ----------------------------------------------------------------

install(TARGETS sqli
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES ${SQLI_INC_DIR}/libsqli/sqli.h ${SQLI_INC_DIR}/libsqli/sqli_sblob.h
    ${SQLI_INC_DIR}/libsqli/sqli_decimal.h ${SQLI_INC_DIR}/libsqli/sqli_temporal.h
    ${SQLI_GENERATED_INC_DIR}/libsqli/version.h
    DESTINATION include/libsqli)
if (TARGET sqlicon)
    install(TARGETS sqlicon DESTINATION bin)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sqlicon.1 DESTINATION share/man/man1)
endif()
