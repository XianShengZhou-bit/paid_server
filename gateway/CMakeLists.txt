cmake_minimum_required(VERSION 3.16)
project(payment_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)

if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "未找到 hiredis，请安装 libhiredis-dev")
endif()

if (NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "未找到 nlohmann/json，请安装 nlohmann-json3-dev")
endif()

add_executable(payment_gateway
    main.cpp
)

target_include_directories(payment_gateway
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${NLOHMANN_JSON_INCLUDE_DIR}
        ${HIREDIS_INCLUDE_DIR}
)

target_link_libraries(payment_gateway
    PRIVATE
        OpenSSL::Crypto
        spdlog::spdlog
        ${HIREDIS_LIBRARY}
)
