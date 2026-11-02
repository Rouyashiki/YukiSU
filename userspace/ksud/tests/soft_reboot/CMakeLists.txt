cmake_minimum_required(VERSION 3.16)
project(soft_reboot_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
enable_testing()

add_executable(soft_reboot_script_test ../soft_reboot_script_test.cpp
    ../../src/module/module.cpp)
target_include_directories(soft_reboot_script_test PRIVATE ../../../..)
target_compile_options(soft_reboot_script_test PRIVATE -UNDEBUG -include linux/types.h
    -ffunction-sections -fdata-sections)
target_link_options(soft_reboot_script_test PRIVATE -Wl,--gc-sections
    -Wl,--wrap=execl -Wl,--wrap=fork -Wl,--wrap=waitpid)
add_test(NAME soft_reboot_scripts COMMAND soft_reboot_script_test)
