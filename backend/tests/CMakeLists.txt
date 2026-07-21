cmake_minimum_required(VERSION 3.16)
project(payment_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 后端源码根目录（本文件在 backend/tests/）
set(PAYMENT_BACKEND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)
find_package(GTest)
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)
find_library(MYSQL_CLIENT_LIBRARY mysqlclient)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)

if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "未找到 hiredis，请安装 libhiredis-dev")
endif()

if (NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_CLIENT_LIBRARY)
    message(FATAL_ERROR "未找到 MySQL C client 开发库，请安装 libmysqlclient-dev 或 mysql-community-devel")
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

add_executable(payment_tests
    test_main.cpp
    test_password_verifier.cpp
    test_jwt.cpp
    test_payment_finalize.cpp
    test_redis_session.cpp
    test_auth_login.cpp
    test_http_worker_pool.cpp
    test_http_server.cpp
)

target_include_directories(payment_tests
    PRIVATE
        ${PAYMENT_BACKEND_ROOT}
        ${PAYMENT_BACKEND_ROOT}/common
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MYSQL_INCLUDE_DIR}
        ${NLOHMANN_JSON_INCLUDE_DIR}
        ${HIREDIS_INCLUDE_DIR}
)

target_link_libraries(payment_tests
    PRIVATE
        GTest::gtest
        OpenSSL::Crypto
        spdlog::spdlog
        ${MYSQL_CLIENT_LIBRARY}
        ${HIREDIS_LIBRARY}
        pthread
)

# env 文件：
# - 手跑: ./payment_tests /path/to/.env [--gtest_filter=...]
# - ctest: cmake ... -DPAYMENT_TEST_ENV_FILE=/path/to/.env && cmake --build . && ctest
#
# 注意：gtest_discover_tests 探测命令固定为
#   payment_tests --gtest_list_tests
# 不会带上 EXTRA_ARGS，因此通过 CROSSCOMPILING_EMULATOR + 环境变量注入 env 路径。
set(PAYMENT_TEST_ENV_FILE "" CACHE FILEPATH
    "传给 payment_tests 的 env 文件路径")

include(GoogleTest)
if (PAYMENT_TEST_ENV_FILE STREQUAL "")
    message(STATUS
        "未设置 PAYMENT_TEST_ENV_FILE：不注册 ctest。"
        "请直接运行 ./payment_tests <env路径>，"
        "或 cmake 时加 -DPAYMENT_TEST_ENV_FILE=<env路径>")
else()
    set_property(TARGET payment_tests PROPERTY CROSSCOMPILING_EMULATOR
        "${CMAKE_COMMAND};-E;env;PAYMENT_TEST_ENV_FILE=${PAYMENT_TEST_ENV_FILE}")
    gtest_discover_tests(payment_tests
        EXTRA_ARGS "${PAYMENT_TEST_ENV_FILE}"
        PROPERTIES ENVIRONMENT "PAYMENT_TEST_ENV_FILE=${PAYMENT_TEST_ENV_FILE}"
    )
endif()
