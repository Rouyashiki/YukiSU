cmake_minimum_required(VERSION 3.20)
project(plugin_compatibility_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(KSUD_ROOT "../.." ABSOLUTE)
include("${KSUD_ROOT}/cmake/Lua.cmake")
include("${KSUD_ROOT}/cmake/ArchiveLibraries.cmake")
set_source_files_properties("${KSUD_ROOT}/src/cli_args.cpp" PROPERTIES
    COMPILE_OPTIONS -Wno-missing-field-initializers)
add_executable(plugin_test_host
    host.cpp
    "${KSUD_ROOT}/src/plugin/plugin.cpp"
    "${KSUD_ROOT}/src/plugin/lua_engine.cpp"
    "${KSUD_ROOT}/src/cli_args.cpp"
    "${KSUD_ROOT}/src/terminal.cpp"
    "${KSUD_ROOT}/src/archive_zip.cpp")
target_include_directories(plugin_test_host PRIVATE "${KSUD_ROOT}/src" "${KSUD_ROOT}/../..")
target_compile_definitions(plugin_test_host PRIVATE
    KSUD_PLUGIN_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}/sandbox")
target_compile_options(plugin_test_host PRIVATE -Wall -Wextra -Werror -include linux/types.h)
target_link_libraries(plugin_test_host PRIVATE lua zip dl m)
set_target_properties(plugin_test_host PROPERTIES OUTPUT_NAME ksud
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sandbox")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_test(NAME plugin_compatibility COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_compatibility.py" "$<TARGET_FILE:plugin_test_host>")
set_tests_properties(plugin_compatibility PROPERTIES TIMEOUT 90)

add_executable(plugin_exec_failure_test exec_failure_test.cpp host.cpp
    "${KSUD_ROOT}/src/plugin/plugin.cpp" "${KSUD_ROOT}/src/archive_zip.cpp"
    "${KSUD_ROOT}/src/terminal.cpp")
target_include_directories(plugin_exec_failure_test PRIVATE "${KSUD_ROOT}/src" "${KSUD_ROOT}/../..")
target_compile_definitions(plugin_exec_failure_test PRIVATE KSUD_PLUGIN_TEST_SUPPORT_ONLY
    KSUD_PLUGIN_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}/sandbox")
target_compile_options(plugin_exec_failure_test PRIVATE -Wall -Wextra -Werror -UNDEBUG -include linux/types.h)
target_link_libraries(plugin_exec_failure_test PRIVATE lua zip dl m)
target_link_options(plugin_exec_failure_test PRIVATE
    -Wl,--wrap=pipe2 -Wl,--wrap=fork -Wl,--wrap=open -Wl,--wrap=dup2 -Wl,--wrap=execvp
    -Wl,--wrap=read -Wl,--wrap=poll -Wl,--wrap=waitpid -Wl,--wrap=fcntl)
add_test(NAME plugin_exec_failures COMMAND plugin_exec_failure_test)
set_tests_properties(plugin_exec_failures PROPERTIES TIMEOUT 15)
