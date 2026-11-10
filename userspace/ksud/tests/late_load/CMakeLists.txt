cmake_minimum_required(VERSION 3.20)
project(ksud_late_load_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-include linux/types.h)
enable_testing()
set(KSUD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

add_executable(late_load_cli_test
    ../late_load_cli_test.cpp
    ${KSUD_ROOT}/src/cli_args.cpp
    ${KSUD_ROOT}/src/terminal.cpp)
add_test(NAME late_load_cli COMMAND late_load_cli_test)

add_executable(kernelsu_loader_test
    ../kernelsu_loader_test.cpp
    ${KSUD_ROOT}/src/kernelsu_loader.cpp)
target_include_directories(kernelsu_loader_test PRIVATE "${KSUD_ROOT}/../..")
target_link_options(kernelsu_loader_test PRIVATE "-Wl,--wrap=open" "-Wl,--wrap=syscall")
add_test(NAME kernelsu_loader COMMAND kernelsu_loader_test)

set_tests_properties(late_load_cli kernelsu_loader PROPERTIES TIMEOUT 20)
