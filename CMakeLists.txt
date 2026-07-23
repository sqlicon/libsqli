cmake_minimum_required(VERSION 3.16)
project(sqli VERSION 0.1.32 LANGUAGES C)

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

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
if (WIN32)
    find_package(uriparser CONFIG REQUIRED)
    set(SQLI_URI_PARSER_TARGET uriparser::uriparser)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(URI_PARSER REQUIRED IMPORTED_TARGET liburiparser)
    set(SQLI_URI_PARSER_TARGET PkgConfig::URI_PARSER)
endif()
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
    ${SQLI_SRC_DIR}/sqli_prepare.c
    ${SQLI_SRC_DIR}/sqli_types.c
    ${SQLI_SRC_DIR}/sqli_transaction.c
    ${SQLI_SRC_DIR}/sqli_pool.c
    ${SQLI_SRC_DIR}/sqli_sqlhosts.c
    ${SQLI_SRC_DIR}/sqli_log.c
    ${SQLI_SRC_DIR}/sqli_msg_catalog.c
    ${SQLI_SRC_DIR}/sqli_uri.c
)
if (NOT WIN32)
    target_sources(sqli PRIVATE
        ${SQLI_SRC_DIR}/sqli_charset_posix.c
        ${SQLI_SRC_DIR}/sqli_tcp_posix.c
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
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${SQLI_SRC_DIR}
)
target_link_libraries(sqli PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ${SQLI_URI_PARSER_TARGET}
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
    if (SQLI_ENABLE_SANITIZERS AND NOT MSVC)
        set_tests_properties(sqli_unit PROPERTIES
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
endif()

# ----------------------------------------------------------------
# Install
# ----------------------------------------------------------------

install(TARGETS sqli
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES ${SQLI_INC_DIR}/libsqli/sqli.h DESTINATION include/libsqli)
if (TARGET sqlicon)
    install(TARGETS sqlicon DESTINATION bin)
    install(FILES ${SQLI_TOOLS_DIR}/sqlicon.1 DESTINATION share/man/man1)
endif()
