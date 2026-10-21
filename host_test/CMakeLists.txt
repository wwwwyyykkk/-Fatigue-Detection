cmake_minimum_required(VERSION 3.16)
project(drivesafe_host_tests C)

enable_testing()

add_executable(test_perclos
    test/test_perclos.c
    src/perclos.c
)
target_include_directories(test_perclos PRIVATE src)

add_test(NAME perclos COMMAND test_perclos)
