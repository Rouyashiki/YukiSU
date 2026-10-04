cmake_minimum_required(VERSION 3.14)
project(yukizygisk_daemon VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    message(FATAL_ERROR "YukiZygisk daemon requires an Android target")
endif()

if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    set(ZYGISKD_TARGET zygiskd64)
elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
    set(ZYGISKD_TARGET zygiskd32)
else()
    message(FATAL_ERROR
        "YukiZygisk daemon only supports arm64-v8a and armeabi-v7a")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_executable(${ZYGISKD_TARGET}
    catalog.cpp
    crash_monitor.cpp
    exit_history.cpp
    log.cpp
    zygiskd.cpp
    standalone.cpp
    # Resolves libc++abi's __cxa_demangle reference locally so the real demangler
    # and its unwinder never link in. -fno-exceptions does not cover this: libc++'s
    # prebuilt archive still reaches it via operator new's bad_alloc path.
    ${REPO_ROOT}/userspace/common/cxx_slim.cpp)
target_include_directories(${ZYGISKD_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REPO_ROOT}
    ${REPO_ROOT}/userspace/ksud/src)
# This target previously built without LTO or hidden visibility, so it kept an
# export table and cross-TU dead code a standalone daemon never needs.
target_compile_options(${ZYGISKD_TARGET} PRIVATE
    -Os -flto=full -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections
    -fvisibility=hidden -fvisibility-inlines-hidden
    -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident
    -fno-stack-protector -faddrsig -fno-semantic-interposition)
target_link_options(${ZYGISKD_TARGET} PRIVATE
    -fuse-ld=lld -flto=full
    -Wl,--gc-sections -Wl,--icf=all -Wl,--strip-all -Wl,--exclude-libs,ALL
    -Wl,-O2 -Wl,--lto-O3 -Wl,--hash-style=gnu -Wl,--build-id=none)
target_link_libraries(${ZYGISKD_TARGET} PRIVATE dl)

set(YUKISU_CLANG_TIDY_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/../.clang-tidy")
include("${REPO_ROOT}/cmake/ClangTidy.cmake")
yukisu_enable_clang_tidy(${ZYGISKD_TARGET} CXX)
