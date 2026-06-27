cmake_minimum_required(VERSION 3.16)
project(sqli VERSION 0.1.25 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Warnings: treat the codebase as if compiled with -Wall -Wextra -Werror -pedantic
add_compile_options(-Wall -Wextra -Werror -pedantic)

option(SQLI_ENABLE_SANITIZERS "Build with address/undefined sanitizers" ON)
option(SQLI_ENABLE_LIVE_TESTS "Enable live Informix integration test(s)" OFF)

if (SQLI_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# Source directory
set(SQLI_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SQLI_INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(SQLI_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(URI_PARSER REQUIRED IMPORTED_TARGET liburiparser)
include(FetchContent)

# ----------------------------------------------------------------
# Library target
# ----------------------------------------------------------------

add_library(sqli STATIC
    ${SQLI_SRC_DIR}/sqli_conn.c
    ${SQLI_SRC_DIR}/sqli_tcp.c
    ${SQLI_SRC_DIR}/sqli_tls.c
    ${SQLI_SRC_DIR}/sqli_unix.c
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
target_include_directories(sqli PUBLIC
    ${SQLI_INC_DIR}
    ${SQLI_SRC_DIR}
)
target_link_libraries(sqli PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads PkgConfig::URI_PARSER)

# ----------------------------------------------------------------
# Unity test framework (fetched into build tree via FetchContent)
# ----------------------------------------------------------------

FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.6.0
)
FetchContent_MakeAvailable(unity)

set(UNITY_SRC
    ${unity_SOURCE_DIR}/src/unity.c
)

# ----------------------------------------------------------------
# linenoise (readline-like library for REPL)
# ----------------------------------------------------------------

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

# ----------------------------------------------------------------
# Test executable
# ----------------------------------------------------------------

add_executable(sqli_test
    ${SQLI_TEST_DIR}/sqli_test_main.c
    ${SQLI_TEST_DIR}/test_sqli.c
    ${SQLI_TEST_DIR}/test_sqli_tcp.c
    ${SQLI_TEST_DIR}/test_sqli_wire.c
    ${SQLI_TEST_DIR}/test_sqli_dispatch.c
    ${SQLI_TEST_DIR}/test_sqli_types.c
    ${SQLI_TEST_DIR}/test_stability.c
    ${SQLI_TEST_DIR}/test_sqli_callable.c
    ${SQLI_TEST_DIR}/test_sqli_transaction.c
    ${SQLI_TEST_DIR}/test_sqli_sqlhosts.c
    ${SQLI_TEST_DIR}/test_sqli_prepare.c
    ${SQLI_TEST_DIR}/test_sqli_uri.c
    ${SQLI_TEST_DIR}/test_integration.c
    ${UNITY_SRC}
)
target_include_directories(sqli_test PRIVATE
    ${SQLI_INC_DIR}
    ${SQLI_SRC_DIR}
    ${unity_SOURCE_DIR}/src
)
target_link_libraries(sqli_test PRIVATE sqli)
if (SQLI_ENABLE_LIVE_TESTS)
    target_compile_definitions(sqli_test PRIVATE SQLI_ENABLE_LIVE_TESTS=1)
endif()

enable_testing()
add_test(NAME sqli_unit COMMAND sqli_test)
if (SQLI_ENABLE_SANITIZERS)
    set_tests_properties(sqli_unit PROPERTIES
        ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
endif()

# ----------------------------------------------------------------
# Demo tool
# ----------------------------------------------------------------

set(SQLI_TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools)

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

add_executable(sqlicon
    ${SQLI_TOOLS_DIR}/sqlicon_main.c
    ${SQLI_TOOLS_DIR}/sqlicon_cli.c
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
target_include_directories(sqlicon PRIVATE ${SQLI_INC_DIR} ${SQLI_TOOLS_DIR})
target_link_libraries(sqlicon PRIVATE sqli linenoise_lib)

# ----------------------------------------------------------------
# Wire protocol capture client
# Connects to a real Informix server and logs all bytes via SQLI_LOG_BYTES=1
# ----------------------------------------------------------------

add_executable(sqli_wire_capture ${SQLI_TEST_DIR}/test_wire_capture.c)
target_include_directories(sqli_wire_capture PRIVATE ${SQLI_INC_DIR} ${SQLI_SRC_DIR})
target_link_libraries(sqli_wire_capture PRIVATE sqli)

# ----------------------------------------------------------------
# Install
# ----------------------------------------------------------------

install(TARGETS sqli
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES ${SQLI_INC_DIR}/libsqli/sqli.h DESTINATION include/libsqli)
install(TARGETS sqlicon DESTINATION bin)
install(FILES ${SQLI_TOOLS_DIR}/sqlicon.1 DESTINATION share/man/man1)