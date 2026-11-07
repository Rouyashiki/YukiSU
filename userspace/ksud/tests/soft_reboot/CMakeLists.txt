cmake_minimum_required(VERSION 3.16)
project(soft_reboot_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
enable_testing()
add_executable(soft_reboot_flow_test ../soft_reboot_flow_test.cpp)
add_test(NAME soft_reboot_flow COMMAND soft_reboot_flow_test)
add_executable(soft_reboot_state_test ../soft_reboot_state_test.cpp
    ../../kagami/src/core/reboot_state.cpp)
target_include_directories(soft_reboot_state_test PRIVATE ../../kagami/src
    ../../kagami/include ../../src ../../../..)
target_compile_options(soft_reboot_flow_test PRIVATE -UNDEBUG)
target_compile_options(soft_reboot_state_test PRIVATE -UNDEBUG -include linux/types.h)
add_test(NAME soft_reboot_state COMMAND soft_reboot_state_test)

add_executable(soft_reboot_script_test ../soft_reboot_script_test.cpp
    ../../src/module/module.cpp)
target_include_directories(soft_reboot_script_test PRIVATE ../../../..)
target_compile_options(soft_reboot_script_test PRIVATE -UNDEBUG -include linux/types.h
    -ffunction-sections -fdata-sections)
target_link_options(soft_reboot_script_test PRIVATE -Wl,--gc-sections
    -Wl,--wrap=execl -Wl,--wrap=fork -Wl,--wrap=waitpid)
add_test(NAME soft_reboot_scripts COMMAND soft_reboot_script_test)

add_executable(soft_reboot_boundary_test ../soft_reboot_boundary_test.cpp
    ../../src/init_event.cpp ../../kagami/src/core/reboot_state.cpp
    ../../kagami/src/core/config.cpp ../../kagami/src/mount/kasumi.cpp)
target_include_directories(soft_reboot_boundary_test PRIVATE ../../kagami/src
    ../../kagami/include ../../src ../../../..)
target_compile_options(soft_reboot_boundary_test PRIVATE -UNDEBUG -include linux/types.h
    -ffunction-sections -fdata-sections)
target_link_options(soft_reboot_boundary_test PRIVATE -Wl,--gc-sections)
add_test(NAME soft_reboot_boundaries COMMAND soft_reboot_boundary_test)

find_package(Threads REQUIRED)
add_executable(soft_reboot_request_test ../soft_reboot_request_test.cpp)
target_compile_options(soft_reboot_request_test PRIVATE -UNDEBUG)
target_link_libraries(soft_reboot_request_test PRIVATE Threads::Threads)
add_test(NAME soft_reboot_requests COMMAND soft_reboot_request_test)
set_tests_properties(soft_reboot_requests PROPERTIES TIMEOUT 10)

add_executable(kagami_daemon_test ../kagami_daemon_test.cpp
    ../../kagami/src/core/daemon.cpp ../../kagami/src/core/reboot_state.cpp)
target_include_directories(kagami_daemon_test PRIVATE ../../kagami/src
    ../../kagami/include ../../src ../../../..)
target_compile_options(kagami_daemon_test PRIVATE -UNDEBUG -include linux/types.h
    -ffunction-sections -fdata-sections)
target_link_options(kagami_daemon_test PRIVATE -Wl,--gc-sections -Wl,--wrap=fcntl)
target_link_libraries(kagami_daemon_test PRIVATE Threads::Threads)
foreach(scenario lifecycle concurrent recovery pending recovery-race)
    add_test(NAME kagami_daemon_${scenario} COMMAND kagami_daemon_test ${scenario})
    set_tests_properties(kagami_daemon_${scenario} PROPERTIES TIMEOUT 60)
endforeach()

set(KAGAMI_JNI_INCLUDE_DIR "" CACHE PATH "Directory containing the Android NDK jni.h")
if(KAGAMI_JNI_INCLUDE_DIR)
    configure_file("${KAGAMI_JNI_INCLUDE_DIR}/jni.h" "jni/jni.h" COPYONLY)
    add_executable(kagami_manager_client_test ../kagami_manager_client_test.cpp)
    target_include_directories(kagami_manager_client_test PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/jni" ../../kagami/include ../../../..)
    target_compile_options(kagami_manager_client_test PRIVATE -UNDEBUG -include linux/types.h
        -ffunction-sections -fdata-sections)
    target_link_options(kagami_manager_client_test PRIVATE -Wl,--gc-sections
        -Wl,--wrap=open -Wl,--wrap=connect -Wl,--wrap=getsockopt -Wl,--wrap=posix_spawn)
    target_link_libraries(kagami_manager_client_test PRIVATE Threads::Threads)
    add_test(NAME kagami_manager_client COMMAND kagami_manager_client_test)
    set_tests_properties(kagami_manager_client PROPERTIES TIMEOUT 20)
endif()
