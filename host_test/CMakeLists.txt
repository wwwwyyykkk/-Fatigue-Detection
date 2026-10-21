cmake_minimum_required(VERSION 3.16)
project(fatigueguard_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_executable(test_perclos
    test/test_perclos.cpp
)

target_include_directories(test_perclos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

if(MSVC)
    target_compile_options(test_perclos PRIVATE /utf-8)
endif()

add_test(NAME perclos COMMAND test_perclos)
