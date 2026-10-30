# libzygisk64.so / libzygisk32.so -- the Zygisk core (see src/core.cpp).
# Standalone NDK CMake build.

cmake_minimum_required(VERSION 3.22)
project(zygisk_core VERSION 1.0.0 LANGUAGES C CXX ASM)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    message(FATAL_ERROR "YukiZygisk supports Android targets only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(YUKISU_CLANG_TIDY_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/../.clang-tidy")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/ClangTidy.cmake")

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a" AND
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
    message(FATAL_ERROR
        "YukiZygisk supports Android arm64-v8a and armeabi-v7a only")
endif()

set(YUKILINKER_ARCH_SOURCES)
set(YUKIZYGISK_ENTRY_SOURCES src/jni_entry.S)
if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    set(YUKIZYGISK_ABI_SUFFIX 64)
    list(APPEND YUKILINKER_ARCH_SOURCES src/yukilinker_tlsdesc.S)
else()
    set(YUKIZYGISK_ABI_SUFFIX 32)
endif()

# Remove outputs left by builds from before filenames became ABI-explicit.
file(REMOVE
    "${CMAKE_CURRENT_BINARY_DIR}/libzygisk.so"
    "${CMAKE_CURRENT_BINARY_DIR}/libyukilinker.so"
    "${CMAKE_CURRENT_BINARY_DIR}/libyukizncore.so")

# Each Core owns a private YukiPLT registry until its explicit retirement.
set(YUKIPLT_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "${CMAKE_ANDROID_ARCH_ABI}")
endif()
add_subdirectory(../third_party/yukiplt ${CMAKE_BINARY_DIR}/yukiplt)
target_compile_definitions(yukiplt PRIVATE YUKIPLT_SYSTEM_PHDR_ITERATOR=core_system_iterate_phdr)
target_compile_options(yukiplt PRIVATE -fexceptions -fno-stack-protector
    -fno-c++-static-destructors -ffunction-sections -fdata-sections)
set_source_files_properties(src/core.cpp src/hook.cpp src/hyos_runtime.cpp
    src/yukizncore.cpp src/process_maps.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
# Resolves libc++abi's __cxa_demangle reference locally so the real demangler and
# its unwinder never link in. -fno-exceptions below does not cover this: libc++'s
# prebuilt archive still reaches the demangler via operator new's bad_alloc path.
set(CXX_SLIM_SOURCE ${REPO_ROOT}/userspace/common/cxx_slim.cpp)

add_library(zygisk SHARED
    ../endpoint.cpp
    src/core.cpp
    src/core_bindings.cpp
    src/plt_lifecycle.cpp
    src/process_maps.cpp
    src/loader_handoff.cpp
    src/hook.cpp
    src/runtime_log.cpp
    ${CXX_SLIM_SOURCE}
    # the anonymous in-memory loader, compiled INTO the core so libzygisk carries
    # its own copy -- the separate libyukilinker first stage is then unloaded
    # right after handoff (zygisk_finalize_loader) instead of lingering.
    src/yukilinker.cpp
    ${YUKILINKER_ARCH_SOURCES}
    ${YUKIZYGISK_ENTRY_SOURCES}
    # synchronous self-unmap: capture-stub template + tail-call munmap.
    src/self_unmap.S
    # solist unlink and maps hiding helpers.
    src/solist.cpp
)
# Keep target names stable while making both ELF filenames and SONAMEs explicit.
set_target_properties(zygisk PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "zygisk${YUKIZYGISK_ABI_SUFFIX}")

# The public ABI contract (zygisk.hpp) lives in ../include.
target_include_directories(zygisk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)  # repo root for uapi/yukizygisk.h

target_compile_options(zygisk PRIVATE
    -Os -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections -fvisibility=hidden
    # zygisk core is injected and runs across fork()+specialize, where the
    # stack canary in TLS shifts -- disable the stack protector to avoid
    # spurious __stack_chk_fail aborts in the specialize wrappers/dispatch.
    -fno-stack-protector
    # Don't register C++ static-global destructors into libc's atexit table.
    # When this dso later disappears from the linker's bookkeeping (either
    # because we munmap it, or because hide_from_solist unlinks it from the
    # solist), every registered atexit handler still has its address sitting in
    # libc's table, but dladdr can no longer resolve it back to a soinfo. A
    # detector that walks libc's atexit list and calls dladdr on each callback
    # then reports "found_injection" -- the same dangling-callback fingerprint
    # __cxa_finalize fixes on the unmap path, except hide_from_solist can't be
    # cleaned up after the fact (core stays live). Suppressing registration at
    # compile time covers both paths uniformly. Trade-off: process-lifetime
    # static destructors never run, but our exit path is the app process exit
    # (the OS reclaims the whole address space) so there is nothing to clean.
    -fno-c++-static-destructors)
target_compile_definitions(zygisk PRIVATE
    YUKILINKER_FULL=1
    YUKIZYGISK_LOG_SOURCE=1)
target_link_libraries(zygisk PRIVATE dl YukiPLT::yukiplt)
target_link_options(zygisk PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/zygisk.map" -Wl,--wrap=dl_iterate_phdr -Wl,--wrap=dl_unwind_find_exidx -Wl,--gc-sections -Wl,--strip-all
                    -Wl,--as-needed -Wl,--exclude-libs,ALL
                    -Wl,-z,now -Wl,-z,relro -Wl,-Bsymbolic-functions
                    -Wl,--wrap=pthread_key_create -Wl,--wrap=pthread_key_delete
                    -Wl,-z,max-page-size=16384)
yukisu_enable_clang_tidy(zygisk CXX)

add_library(yukizncore SHARED
    ../endpoint.cpp
    src/hyos_runtime.cpp
    src/yukizncore.cpp
    src/core_bindings.cpp
    src/plt_lifecycle.cpp
    src/process_maps.cpp
    src/loader_handoff.cpp
    src/native_bionic.cpp
    src/runtime_log.cpp
    src/yukilinker.cpp
    ${YUKILINKER_ARCH_SOURCES}
    src/self_unmap.S
    src/solist.cpp
    ${CXX_SLIM_SOURCE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ksud/third_party/ndk-busybox/archival/libarchive/unxz/xz_dec_lzma2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ksud/third_party/ndk-busybox/archival/libarchive/unxz/xz_dec_stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ksud/third_party/ndk-busybox/archival/libarchive/unxz/xz_dec_bcj.c
)
set_target_properties(yukizncore PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "yukizncore${YUKIZYGISK_ABI_SUFFIX}")
target_include_directories(yukizncore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../daemon
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ksud/third_party/ndk-busybox/archival/libarchive/unxz
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
target_compile_options(yukizncore PRIVATE
    -Os
    -ffunction-sections -fdata-sections -fvisibility=hidden
    -fno-stack-protector
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-c++-static-destructors>)
target_compile_definitions(yukizncore PRIVATE
    YUKILINKER_FULL=1
    YUKIZYGISK_LOG_SOURCE=2
    XZ_DEC_DYNALLOC=1
    XZ_DEC_ANY_CHECK=1
    XZ_INTERNAL_CRC32=1
    NOINLINE=noinline_for_stack)
target_link_libraries(yukizncore PRIVATE dl YukiPLT::yukiplt)
target_link_options(yukizncore PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/yukizncore.map" -Wl,--wrap=dl_iterate_phdr -Wl,--wrap=dl_unwind_find_exidx -Wl,--gc-sections -Wl,--strip-all
                    -Wl,--as-needed -Wl,--exclude-libs,ALL
                    -Wl,-z,now -Wl,-z,relro -Wl,-Bsymbolic-functions
                    -Wl,--wrap=pthread_key_create -Wl,--wrap=pthread_key_delete
                    -Wl,-z,max-page-size=16384)
# The C sources on this target are vendored BusyBox/XZ; check our C++ only.
yukisu_enable_clang_tidy(yukizncore CXX)

# libyukilinker64.so / libyukilinker32.so -- the anonymous in-memory module
# loader, split into its own
# .so that core dlopen+dlsym's at runtime (exports yuki_dlopen_memfd/dlsym/
# dlclose). Same source as compiled-in; no PLT-engine/solist dependencies.
add_library(yukilinker SHARED
    src/core_bindings.cpp
    ../endpoint.cpp
    src/yukilinker.cpp
    src/runtime_log.cpp
    ${YUKILINKER_ARCH_SOURCES}
    ${CXX_SLIM_SOURCE})
set_target_properties(yukilinker PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "yukilinker${YUKIZYGISK_ABI_SUFFIX}")
target_include_directories(yukilinker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)  # repo root
target_compile_options(yukilinker PRIVATE
    -Os -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections -fvisibility=hidden
    -fno-stack-protector
    # Match the core: don't register C++ static-global destructors. The
    # standalone first-stage already calls __cxa_finalize(&__dso_handle) before
    # the core munmaps it, so the dangling-callback risk is covered there too;
    # this flag is belt-and-suspenders, and prevents any handler from being
    # registered in the first place.
    -fno-c++-static-destructors)
target_compile_definitions(yukilinker PRIVATE
    YUKILINKER_BOOTSTRAP=1
    YUKIZYGISK_LOG_SOURCE=3)
target_link_libraries(yukilinker PRIVATE dl)
target_link_options(yukilinker PRIVATE -Wl,--gc-sections -Wl,--strip-all
    -Wl,-Bsymbolic-functions -Wl,--exclude-libs,ALL -Wl,--wrap=pthread_key_create -Wl,--wrap=pthread_key_delete)
yukisu_enable_clang_tidy(yukilinker CXX)

find_program(LLVM_STRIP NAMES llvm-strip)
if(LLVM_STRIP)
    add_custom_command(TARGET zygisk POST_BUILD
        COMMAND ${LLVM_STRIP}
            --strip-all
            --remove-section=.comment
            --remove-section=.note
            --remove-section=.note.android.ident
            --remove-section=.note.gnu.build-id
            $<TARGET_FILE:zygisk>)
    add_custom_command(TARGET yukizncore POST_BUILD
        COMMAND ${LLVM_STRIP}
            --strip-all
            --remove-section=.comment
            --remove-section=.note
            --remove-section=.note.android.ident
            --remove-section=.note.gnu.build-id
            $<TARGET_FILE:yukizncore>)
endif()
