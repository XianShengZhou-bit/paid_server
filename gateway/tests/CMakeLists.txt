cmake_minimum_required(VERSION 3.16)
project(gateway_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PAYMENT_GATEWAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)
find_package(GTest)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)

if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "未找到 hiredis，请安装 libhiredis-dev")
endif()

if (NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "未找到 nlohmann/json，请安装 nlohmann-json3-dev")
endif()

if (NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

enable_testing()

add_executable(gateway_tests
    test_main.cpp
    test_routing.cpp
    test_ws_helpers.cpp
    test_session_store.cpp
    test_payment_result.cpp
    test_handle_request.cpp
    test_http_worker_pool.cpp
    test_http_server.cpp
)

target_include_directories(gateway_tests
    PRIVATE
        ${PAYMENT_GATEWAY_ROOT}
        ${PAYMENT_GATEWAY_ROOT}/common
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${NLOHMANN_JSON_INCLUDE_DIR}
        ${HIREDIS_INCLUDE_DIR}
)

target_link_libraries(gateway_tests
    PRIVATE
        GTest::gtest
        OpenSSL::Crypto
        spdlog::spdlog
        ${HIREDIS_LIBRARY}
        pthread
)

# 与 backend/tests 一致：探测时靠环境变量注入 env；手跑传 argv。
set(PAYMENT_TEST_ENV_FILE "" CACHE FILEPATH
    "传给 gateway_tests 的 env 文件路径")

include(GoogleTest)
if (PAYMENT_TEST_ENV_FILE STREQUAL "")
    message(STATUS
        "未设置 PAYMENT_TEST_ENV_FILE：不注册 ctest。"
        "请直接运行 ./gateway_tests <env路径>，"
        "或 cmake 时加 -DPAYMENT_TEST_ENV_FILE=<env路径>")
else()
    set_property(TARGET gateway_tests PROPERTY CROSSCOMPILING_EMULATOR
        "${CMAKE_COMMAND};-E;env;PAYMENT_TEST_ENV_FILE=${PAYMENT_TEST_ENV_FILE}")
    gtest_discover_tests(gateway_tests
        EXTRA_ARGS "${PAYMENT_TEST_ENV_FILE}"
        PROPERTIES ENVIRONMENT "PAYMENT_TEST_ENV_FILE=${PAYMENT_TEST_ENV_FILE}"
    )
endif()
