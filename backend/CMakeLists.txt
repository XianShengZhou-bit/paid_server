cmake_minimum_required(VERSION 3.16)
project(payment_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)
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

add_executable(payment_backend
    main.cpp
)

target_include_directories(payment_backend
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${MYSQL_INCLUDE_DIR}
        ${NLOHMANN_JSON_INCLUDE_DIR}
        ${HIREDIS_INCLUDE_DIR}
)

target_link_libraries(payment_backend
    PRIVATE
        OpenSSL::Crypto
        spdlog::spdlog
        ${MYSQL_CLIENT_LIBRARY}
        ${HIREDIS_LIBRARY}
)
