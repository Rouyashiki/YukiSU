# Prefer Ninja: cmake -G Ninja .. [options] && ninja
cmake_minimum_required(VERSION 3.14)

# Android API floor: 31 (Android 12). This is a compatibility floor, not just a
# bionic-symbol requirement -- it stops YukiSU being rebuilt against an older
# platform with builtin support stripped out.
#
# CMAKE_ANDROID_API does NOT control this: CMake derives its own --target from
# CMAKE_SYSTEM_VERSION and appends it after the versioned NDK wrapper's --target,
# so the wrapper loses and CMAKE_SYSTEM_VERSION is what actually selects the API
# level. Fail at configure time when it is too low; userspace/common/api_floor.hpp
# then backs this up at compile time.
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  if(CMAKE_SYSTEM_VERSION LESS 31)
    message(FATAL_ERROR
      "YukiSU requires Android API 31+; got CMAKE_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION}. "
      "Configure with -DCMAKE_SYSTEM_VERSION=31 (setting CMAKE_ANDROID_API has no effect).")
  endif()
endif()

# Prefer Clang for C and CXX (LTO, -faddrsig, etc. require Clang; do not use GCC)
if(NOT ANDROID AND NOT CMAKE_C_COMPILER)
  find_program(CLANG_CC NAMES clang-18 clang-17 clang-16 clang-15 clang-14 clang)
  find_program(CLANG_CXX NAMES clang++-18 clang++-17 clang++-16 clang++-15 clang++-14 clang++)
  if(CLANG_CC AND CLANG_CXX)
    set(CMAKE_C_COMPILER "${CLANG_CC}" CACHE STRING "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${CLANG_CXX}" CACHE STRING "CXX compiler" FORCE)
    message(STATUS "Using Clang: C=${CLANG_CC} CXX=${CLANG_CXX}")
  endif()
endif()

project(ksud VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android" OR
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "YukiSU userspace supports Android arm64-v8a only")
endif()

# Allow FetchContent deps (e.g. miniz) that use cmake_minimum_required < 3.5
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Min CMake version for subprojects")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use ccache to speed up builds (if available)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

# Backward-compatible alias for existing local invocations.
if(DEFINED KSUD_ENABLE_CLANG_TIDY AND
   NOT DEFINED YUKISU_ENABLE_CLANG_TIDY)
    set(YUKISU_ENABLE_CLANG_TIDY "${KSUD_ENABLE_CLANG_TIDY}"
        CACHE BOOL "Run clang-tidy on YukiSU first-party userspace sources")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ClangTidy.cmake")

# ============================================================================
# Super Aggressive Size Optimization
# ============================================================================
# Disable LTO if linker reports undefined symbols (e.g. ksud::boot_patch) in some
# environments (OrbStack host build with object libs + LTO). Use: -DKSUD_DISABLE_LTO=ON
option(KSUD_DISABLE_LTO "Disable LTO (use if link fails with undefined symbols)" OFF)

# --- Common compile flags (shared by C and C++) ---
set(_COMMON_COMPILE_FLAGS
    -ffunction-sections          # Place each function in its own section (enables --gc-sections dead code elimination)
    -fdata-sections              # Place each data item in its own section
    -fvisibility=hidden          # Hide all symbols by default, shrink export table
    -fvisibility-inlines-hidden  # Hide inline function symbols
    -fno-rtti                    # Disable RTTI (no dynamic_cast/typeid usage in codebase)
    -fno-asynchronous-unwind-tables  # Remove async unwind tables (C++ exceptions still work)
    -fmerge-all-constants        # Merge identical constants across translation units
    -fno-ident                   # Don't embed compiler version string into .comment section
    -fno-stack-protector         # Remove stack canary checks (unnecessary for root daemon)
    -faddrsig                    # Emit address-significance table for more precise ICF
    -fno-semantic-interposition  # Disable semantic interposition, allow more aggressive inlining
    -fno-math-errno              # Math functions don't set errno
)
string(REPLACE ";" " " _COMMON_FLAGS_STR "${_COMMON_COMPILE_FLAGS}")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${_COMMON_FLAGS_STR}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_COMMON_FLAGS_STR}")

# --- Release build optimization ---
if(KSUD_DISABLE_LTO)
    set(CMAKE_C_FLAGS_RELEASE   "-Os -DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")
else()
    set(CMAKE_C_FLAGS_RELEASE   "-Os -DNDEBUG -flto=full")
    set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG -flto=full")
endif()

# --- Aggressive linker optimization ---
if(KSUD_DISABLE_LTO)
    string(CONCAT _LINKER_FLAGS
        " -fuse-ld=lld"
        " -Wl,--gc-sections"
        " -Wl,--strip-all"
        " -Wl,--exclude-libs,ALL"
        " -Wl,-O2"
        " -Wl,--hash-style=gnu"
        " -Wl,--build-id=none"
        " -Wl,-z,norelro"
    )
else()
    string(CONCAT _LINKER_FLAGS
        " -fuse-ld=lld"               # Use lld linker
        " -flto=full"                 # Full LTO: maximum cross-module optimization (smaller than thin LTO)
        " -fwhole-program-vtables"    # Matches the per-target compile flag; enables LTO-time devirtualization
        " -Wl,--gc-sections"          # Garbage-collect unused sections
        " -Wl,--icf=all"              # Identical Code Folding: merge functions with identical bodies
        " -Wl,--strip-all"            # Strip all symbols
        " -Wl,--exclude-libs,ALL"     # Don't export symbols from static libraries
        " -Wl,-O2"                    # Linker optimization level 2 (string table optimization etc.)
        " -Wl,--lto-O3"              # LTO at O3 level (compile with Oz, but LTO pass uses O3 for aggressive inlining/elimination)
        " -Wl,--hash-style=gnu"       # GNU hash is more compact than sysv
        " -Wl,--build-id=none"        # Remove build-id note section
        " -Wl,-z,norelro"             # Remove RELRO segment (root tool doesn't need this security feature)
    )
endif()
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}${_LINKER_FLAGS}")

# --- Shrink miniz: disable unused features ---
add_compile_definitions(
    MINIZ_NO_STDIO               # No FILE* interface needed
    MINIZ_NO_TIME                 # No timestamp support needed
    MINIZ_NO_ARCHIVE_WRITING_APIS # Decompression only, disable write APIs
)

# Fetch third-party dependencies
include(FetchContent)

# Embed upstream Lua 5.5 and expose its native module ABI.
FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.5.1.tar.gz
    URL_HASH SHA256=1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(lua)

# Lua uses C++ exceptions when compiled as C++, but its public API must retain C linkage.
set(LUA_CONFIG_HEADER "${lua_SOURCE_DIR}/src/luaconf.h")
file(READ "${LUA_CONFIG_HEADER}" LUA_CONFIG_CONTENT)
string(FIND "${LUA_CONFIG_CONTENT}" "YUKISU_LUA_CXX_ABI" LUA_CONFIG_PATCHED)
if(LUA_CONFIG_PATCHED EQUAL -1)
    set(LUA_CONFIG_ORIGINAL "${LUA_CONFIG_CONTENT}")
    set(LUA_CXX_API_DECL
        "#if defined(__cplusplus)\n#define LUA_API extern \"C\" /* YUKISU_LUA_CXX_ABI */\n#else\n#define LUA_API extern\n#endif"
    )
    string(REGEX REPLACE "#define LUA_API[ \t]+extern" "${LUA_CXX_API_DECL}"
        LUA_CONFIG_CONTENT "${LUA_CONFIG_CONTENT}")
    if(LUA_CONFIG_CONTENT STREQUAL LUA_CONFIG_ORIGINAL)
        message(FATAL_ERROR "Unable to preserve the Lua C ABI")
    endif()
    file(WRITE "${LUA_CONFIG_HEADER}" "${LUA_CONFIG_CONTENT}")
endif()

set(LUA_SOURCES
    ${lua_SOURCE_DIR}/src/lapi.c
    ${lua_SOURCE_DIR}/src/lcode.c
    ${lua_SOURCE_DIR}/src/lctype.c
    ${lua_SOURCE_DIR}/src/ldebug.c
    ${lua_SOURCE_DIR}/src/ldo.c
    ${lua_SOURCE_DIR}/src/ldump.c
    ${lua_SOURCE_DIR}/src/lfunc.c
    ${lua_SOURCE_DIR}/src/lgc.c
    ${lua_SOURCE_DIR}/src/llex.c
    ${lua_SOURCE_DIR}/src/lmem.c
    ${lua_SOURCE_DIR}/src/lobject.c
    ${lua_SOURCE_DIR}/src/lopcodes.c
    ${lua_SOURCE_DIR}/src/lparser.c
    ${lua_SOURCE_DIR}/src/lstate.c
    ${lua_SOURCE_DIR}/src/lstring.c
    ${lua_SOURCE_DIR}/src/ltable.c
    ${lua_SOURCE_DIR}/src/ltm.c
    ${lua_SOURCE_DIR}/src/lundump.c
    ${lua_SOURCE_DIR}/src/lvm.c
    ${lua_SOURCE_DIR}/src/lzio.c
    ${lua_SOURCE_DIR}/src/lauxlib.c
    ${lua_SOURCE_DIR}/src/lbaselib.c
    ${lua_SOURCE_DIR}/src/lcorolib.c
    ${lua_SOURCE_DIR}/src/ldblib.c
    ${lua_SOURCE_DIR}/src/liolib.c
    ${lua_SOURCE_DIR}/src/lmathlib.c
    ${lua_SOURCE_DIR}/src/loslib.c
    ${lua_SOURCE_DIR}/src/lstrlib.c
    ${lua_SOURCE_DIR}/src/ltablib.c
    ${lua_SOURCE_DIR}/src/lutf8lib.c
    ${lua_SOURCE_DIR}/src/loadlib.c
    ${lua_SOURCE_DIR}/src/linit.c
)

set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua OBJECT ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR}/src)
target_compile_definitions(lua PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
target_compile_options(lua PRIVATE -fvisibility=default -Wno-deprecated)
if(ANDROID)
    target_link_libraries(lua PRIVATE dl m)
endif()
if(NOT ANDROID)
    target_link_libraries(lua PRIVATE pthread m)
endif()

# mbedTLS - SHA1/SHA256 + X.509/RSA/ECDSA (for MagiskbootAlone AVB and flash_partition SHA256)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)  # avoid -Werror vs -fno-semantic-interposition on Apple Clang
FetchContent_Declare(
    mbedtls
    # The official release archive includes the framework submodule and generated files.
    # GitHub's automatic source archives and a plain shallow clone do not.
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
    URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
)
FetchContent_MakeAvailable(mbedtls)

# miniz - Lightweight ZIP library
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniz)

# LZ4 - prefer Magisk vendored inside MagiskbootAlone submodule (same ABI as Magisk magiskboot)
set(LZ4_IN_MAGISKBOOT_ALONE ${CMAKE_SOURCE_DIR}/third_party/MagiskbootAlone/external/lz4)
if(EXISTS ${LZ4_IN_MAGISKBOOT_ALONE}/lz4.c)
    set(LZ4_LIB_DIR ${LZ4_IN_MAGISKBOOT_ALONE})
    message(STATUS "Using vendored LZ4 (MagiskbootAlone/external/lz4) for magiskboot: ${LZ4_LIB_DIR}")
else()
    FetchContent_Declare(
        lz4
        GIT_REPOSITORY https://github.com/lz4/lz4.git
        GIT_TAG v1.10.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(lz4)
    set(LZ4_LIB_DIR ${lz4_SOURCE_DIR}/lib)
endif()

# Generated sources / embedded assets
set(ASSETS_DIR ${CMAKE_SOURCE_DIR}/assets)
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
set(ASSETS_CPP ${GENERATED_DIR}/assets_data.cpp)
set(VERSION_CPP ${GENERATED_DIR}/defs.cpp)
file(MAKE_DIRECTORY ${GENERATED_DIR})

# Generate version file before every ksud build so Git and staged LKM state are
# reflected in the final daemon even when the CMake build directory is reused.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# readelf_toyboxAlone: a minimal Android 16 Toybox readelf applet. Its
# subproject namespaces all Toybox-defined symbols before exposing the archive,
# so it can coexist with the BusyBox library already welded into ksud.
set(READELF_TOYBOX_ALONE_DIR
    ${CMAKE_SOURCE_DIR}/third_party/readelf_toyboxAlone
)
if(NOT EXISTS ${READELF_TOYBOX_ALONE_DIR}/CMakeLists.txt)
    message(FATAL_ERROR
        "readelf_toyboxAlone submodule is missing; run git submodule update --init")
endif()
add_subdirectory(
    ${READELF_TOYBOX_ALONE_DIR}
    ${CMAKE_BINARY_DIR}/readelf-toybox
)
set(TOYBOX_READELF_AVAILABLE 1)
message(STATUS "readelf_toyboxAlone: building ksud multi-call applet")

add_custom_target(ksud_version
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/generate_version.py ${VERSION_CPP}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    BYPRODUCTS ${VERSION_CPP}
    COMMENT "Generating ksud version..."
    VERBATIM
)

# Run embed_assets.py to generate assets_data.cpp from everything in assets/. All
# assets (ko and ksuinit) are staged into assets/ by build.sh before configure,
# so the glob catches them. DEPEND on the asset files (not just the script) plus
# CONFIGURE_DEPENDS so the table regenerates whenever an asset is added/changed.
file(GLOB ASSET_FILES CONFIGURE_DEPENDS ${ASSETS_DIR}/*)
add_custom_command(
    OUTPUT ${ASSETS_CPP}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_assets.py
            ${ASSETS_DIR} ${ASSETS_CPP}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/embed_assets.py ${ASSET_FILES}
    COMMENT "Generating embedded assets..."
)

# The boot-patch-v2 bootstrap is deliberately kept as a small AArch64 ET_REL
# object.  Compile it with the same Android clang as ksud and convert it to a
# checked C++ byte array so the production tree remains C++-only.
set(LKM_BOOTSTRAP_SOURCE ${CMAKE_SOURCE_DIR}/src/boot/lkm_image_bootstrap.S)
set(LKM_BOOTSTRAP_OBJECT ${GENERATED_DIR}/lkm_image_bootstrap.o)
set(LKM_BOOTSTRAP_CPP ${GENERATED_DIR}/lkm_image_bootstrap_data.cpp)
add_custom_command(
    OUTPUT ${LKM_BOOTSTRAP_CPP}
    COMMAND ${CMAKE_C_COMPILER} --target=aarch64-linux-android -c -nostdlib
            -o ${LKM_BOOTSTRAP_OBJECT} ${LKM_BOOTSTRAP_SOURCE}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_lkm_bootstrap.py
            ${LKM_BOOTSTRAP_OBJECT} ${LKM_BOOTSTRAP_CPP}
    DEPENDS ${LKM_BOOTSTRAP_SOURCE}
            ${CMAKE_SOURCE_DIR}/src/boot/lkm_image_bootstrap.hpp
            ${CMAKE_SOURCE_DIR}/scripts/embed_lkm_bootstrap.py
    BYPRODUCTS ${LKM_BOOTSTRAP_OBJECT}
    COMMENT "Embedding the boot-patch-v2 AArch64 bootstrap"
    VERBATIM
)

# third_party tools: compile into ksud for multi-call
set(MAGISKTOOLS_ALONE_DIR ${CMAKE_SOURCE_DIR}/third_party)
set(MAGISKBOOT_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/MagiskbootAlone)
set(MAGISKBOOT_SOURCES
    ${MAGISKBOOT_ALONE_DIR}/src/base_host.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/boot_ramdisk.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/bootimg.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/boot_crypto.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/cpio.cpp
    ${MAGISKBOOT_ALONE_DIR}/src/magiskboot_main.cpp
    ${LZ4_LIB_DIR}/lz4.c
    ${LZ4_LIB_DIR}/lz4frame.c
    ${LZ4_LIB_DIR}/lz4hc.c
    ${LZ4_LIB_DIR}/xxhash.c
)
if(EXISTS ${MAGISKBOOT_ALONE_DIR}/src/magiskboot_main.cpp)
    include_directories(${MAGISKBOOT_ALONE_DIR}/src ${LZ4_LIB_DIR})
    set(MAGISKBOOT_ALONE_AVAILABLE 1)
    message(STATUS "MagiskbootAlone: building magiskboot into ksud (multi-call)")
else()
    message(WARNING "MagiskbootAlone submodule not found at ${MAGISKBOOT_ALONE_DIR}; run: git submodule update --init")
endif()

set(BOOTCTL_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/bootctlAlone)
set(RESETPROP_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/resetpropAlone)
set(MKBOOTFS_ALONE_DIR ${MAGISKTOOLS_ALONE_DIR}/mkbootfsAlone)
set(RESETPROP_ALONE_SOURCES)
if(EXISTS ${BOOTCTL_ALONE_DIR}/src/main.cpp)
    set(BOOTCTL_ALONE_AVAILABLE 1)
    message(STATUS "bootctlAlone: building bootctl into ksud (multi-call)")
endif()
if(EXISTS ${RESETPROP_ALONE_DIR}/src/main.cpp)
    file(GLOB RESETPROP_ALONE_SOURCES CONFIGURE_DEPENDS
        ${RESETPROP_ALONE_DIR}/src/*.cpp
    )
    set(RESETPROP_ALONE_AVAILABLE 1)
    message(STATUS "resetpropAlone: building resetprop into ksud (multi-call)")
endif()
if(EXISTS ${MKBOOTFS_ALONE_DIR}/src/mkbootfs.cpp)
    set(MKBOOTFS_ALONE_AVAILABLE 1)
    message(STATUS "mkbootfsAlone: building mkbootfs into ksud (multi-call)")
endif()

# ndk-busybox: build as static lib and link into ksud for multi-call (argv0 "busybox")
set(NDK_BUSYBOX_DIR ${MAGISKTOOLS_ALONE_DIR}/ndk-busybox)
if(EXISTS ${NDK_BUSYBOX_DIR}/CMakeLists.txt)
  set(BUSYBOX_EMBEDDED 1 CACHE BOOL "Build busybox as static lib for embedding")
  add_subdirectory(${NDK_BUSYBOX_DIR} ${CMAKE_BINARY_DIR}/ndk-busybox)
  target_sources(busybox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/xz_stream_adapter.c)
  set(NDK_BUSYBOX_AVAILABLE 1)
  message(STATUS "ndk-busybox: building busybox into ksud (multi-call)")
endif()

# Our own sources (clang-tidy will run on these only)
set(KSUD_OUR_SOURCES
    src/main.cpp
    src/cli.cpp
    src/cli_args.cpp
    src/terminal.cpp
    ${VERSION_CPP}
    src/log.cpp
    src/utils.cpp
    src/command_exec.cpp
    src/xz_range.c
    src/kernelsu_loader.cpp
    src/dynamic_manager.cpp
    src/core/ksucalls.cpp
    src/core/feature.cpp
    src/core/su_path.cpp
    src/core/uts_view.cpp
    src/core/restorecon.cpp
    src/core/assets.cpp
    src/module/module.cpp
    src/module/module_config.cpp
    src/module/metamodule.cpp
    src/boot/boot_patch.cpp
    src/boot/boot_backup.cpp
    src/boot/boot_patch_v2.cpp
    src/boot/boot_image_btf.cpp
    src/boot/lkm_image_core.cpp
    src/boot/lkm_image_bootstrap_linker.cpp
    src/boot/lkm_image.cpp
    src/boot/ramdisk_editor.cpp
    src/boot/tools.cpp
    src/boot/apk_sign.cpp
    src/profile/profile.cpp
    src/sepolicy/sepolicy.cpp
    src/sepolicy/xperm_parser.cpp
    src/su.cpp
    src/su_args.cpp
    src/init_event.cpp
    src/soft_reboot_waiter.cpp
    src/integrity_monitor.cpp
    src/yukizygisk_diagnostics.cpp
    src/yukizygisk_daemon.cpp
    src/yukizygisk_snapshot.cpp
    src/yzctl.cpp
    src/late_load.cpp
    src/umount.cpp
    src/debug.cpp
    src/sulog.cpp
    src/magisk_compat/msud.cpp
    src/magisk_compat/su_transition.cpp
    src/magica/adb_client.cpp
    src/magica/magica.cpp
    src/core/hide_bootloader.cpp
    src/flash/flash_ak3.cpp
    src/flash/flash_partition.cpp
    src/plugin/lua_engine.cpp
    src/plugin/plugin.cpp
)

# Third-party sources (no clang-tidy)
set(KSUD_THIRDPARTY_SOURCES)
if(MAGISKBOOT_ALONE_AVAILABLE)
    list(APPEND KSUD_THIRDPARTY_SOURCES ${MAGISKBOOT_SOURCES})
endif()
if(BOOTCTL_ALONE_AVAILABLE)
    list(APPEND KSUD_THIRDPARTY_SOURCES ${BOOTCTL_ALONE_DIR}/src/main.cpp)
endif()
if(RESETPROP_ALONE_AVAILABLE)
    list(APPEND KSUD_THIRDPARTY_SOURCES ${RESETPROP_ALONE_SOURCES})
endif()
if(MKBOOTFS_ALONE_AVAILABLE)
    list(APPEND KSUD_THIRDPARTY_SOURCES ${MKBOOTFS_ALONE_DIR}/src/mkbootfs.cpp)
endif()

get_filename_component(REPO_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)

# Include directories (for both object libs and ksud)
set(KSUD_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
    ${lua_SOURCE_DIR}/src
    ${mbedtls_SOURCE_DIR}/include
    ${miniz_SOURCE_DIR}
    ${miniz_BINARY_DIR}
    ${LZ4_LIB_DIR}
    ${MKBOOTFS_ALONE_DIR}/include
    ${READELF_TOYBOX_ALONE_DIR}/include
    ${GENERATED_DIR}
    ${REPO_ROOT}  # uapi/ kernel headers for userspace
)
include_directories(${KSUD_INCLUDE_DIRS})

# Compile definitions shared by our and third-party object libs
set(KSUD_DEFINES)
if(MAGISKBOOT_ALONE_AVAILABLE)
    list(APPEND KSUD_DEFINES MAGISKBOOT_ALONE_AVAILABLE=1 USE_MBEDTLS=1)
endif()
if(BOOTCTL_ALONE_AVAILABLE)
    list(APPEND KSUD_DEFINES BOOTCTL_ALONE_AVAILABLE=1)
endif()
if(RESETPROP_ALONE_AVAILABLE)
    list(APPEND KSUD_DEFINES RESETPROP_ALONE_AVAILABLE=1)
endif()
if(MKBOOTFS_ALONE_AVAILABLE)
    list(APPEND KSUD_DEFINES
        MKBOOTFS_ALONE_AVAILABLE=1
        MKBOOTFS_ALONE_VERSION="1.0.0"
        MKBOOTFS_AOSP_REVISION="a3b721a32242006b59cb12bd62c9133632af3a2d"
    )
endif()
if(NDK_BUSYBOX_AVAILABLE)
    list(APPEND KSUD_DEFINES NDK_BUSYBOX_AVAILABLE=1)
endif()
if(TOYBOX_READELF_AVAILABLE)
    list(APPEND KSUD_DEFINES TOYBOX_READELF_AVAILABLE=1)
endif()
# Whole-program devirtualization, applied only to first-party C++ objects.
# It pays off on MagiskbootAlone's dyn_img_hdr hierarchy: ~45 virtual
# getter/setters whose bodies are a single field load, reached through a
# dyn_img_hdr* so they can neither inline nor hoist out of a loop. Sound here
# because these objects hold every definition and use of those vtables, and
# nothing escapes the LTO unit.
#
# Deliberately NOT in the global CMAKE_CXX_FLAGS: FetchContent subprojects such
# as mbedTLS replace CMAKE_C_FLAGS_RELEASE with their own -O2, which drops the
# -flto=full this flag requires and makes clang reject it outright.
set(KSUD_DEVIRT_FLAGS)
if(NOT KSUD_DISABLE_LTO)
    set(KSUD_DEVIRT_FLAGS -fwhole-program-vtables)
endif()

# Object library: our sources (with clang-tidy when enabled). The Android NDK
# compilation database supplies the real target sysroot and flags to clang-tidy.
list(APPEND KSUD_OUR_SOURCES ${REPO_ROOT}/userspace/common/cxx_slim.cpp)
add_library(ksud_our_objs OBJECT ${KSUD_OUR_SOURCES})
add_dependencies(ksud_our_objs ksud_version)
target_include_directories(ksud_our_objs PRIVATE ${KSUD_INCLUDE_DIRS})
target_compile_definitions(ksud_our_objs PRIVATE ${KSUD_DEFINES})
target_compile_features(ksud_our_objs PUBLIC cxx_std_17)
# Most daemon code neither throws nor catches. Compile it without exception
# landing pads/unwind metadata, then opt the Lua/C++ API boundary back in: it
# translates native exceptions into Lua errors. Source options are appended
# after target options, so -fexceptions wins for that translation unit.
target_compile_options(ksud_our_objs PRIVATE ${KSUD_DEVIRT_FLAGS} -fno-exceptions)
set_source_files_properties(
    src/plugin/lua_engine.cpp
    PROPERTIES COMPILE_OPTIONS -fexceptions
)
yukisu_enable_clang_tidy(ksud_our_objs CXX)

# Generated compressed asset bytes are compiled into ksud, but are not useful
# clang-tidy input. Keep them separate from the hand-written first-party code.
add_library(ksud_generated_objs OBJECT ${ASSETS_CPP} ${LKM_BOOTSTRAP_CPP})
target_include_directories(ksud_generated_objs PRIVATE ${KSUD_INCLUDE_DIRS})
target_compile_definitions(ksud_generated_objs PRIVATE ${KSUD_DEFINES})
target_compile_options(ksud_generated_objs PRIVATE -fno-exceptions)

# Object library: third-party sources (no clang-tidy)
add_library(ksud_thirdparty_objs OBJECT ${KSUD_THIRDPARTY_SOURCES})
target_include_directories(ksud_thirdparty_objs PRIVATE ${KSUD_INCLUDE_DIRS})
target_compile_definitions(ksud_thirdparty_objs PRIVATE ${KSUD_DEFINES})
target_compile_options(ksud_thirdparty_objs PRIVATE ${KSUD_DEVIRT_FLAGS})
# Magiskboot throws through several of its translation units and catches at its
# command boundary; keep that submodule exception-enabled. The other welded
# tools have no exception boundary and already terminate on allocation failure,
# so landing pads only waste space.
set(KSUD_EXCEPTION_FREE_THIRDPARTY_SOURCES)
if(BOOTCTL_ALONE_AVAILABLE)
    list(APPEND KSUD_EXCEPTION_FREE_THIRDPARTY_SOURCES ${BOOTCTL_ALONE_DIR}/src/main.cpp)
endif()
if(RESETPROP_ALONE_AVAILABLE)
    list(APPEND KSUD_EXCEPTION_FREE_THIRDPARTY_SOURCES ${RESETPROP_ALONE_SOURCES})
endif()
if(MKBOOTFS_ALONE_AVAILABLE)
    list(APPEND KSUD_EXCEPTION_FREE_THIRDPARTY_SOURCES ${MKBOOTFS_ALONE_DIR}/src/mkbootfs.cpp)
endif()
if(KSUD_EXCEPTION_FREE_THIRDPARTY_SOURCES)
    set_source_files_properties(${KSUD_EXCEPTION_FREE_THIRDPARTY_SOURCES}
        PROPERTIES COMPILE_OPTIONS -fno-exceptions)
endif()
if(MAGISKBOOT_ALONE_AVAILABLE)
    target_compile_features(ksud_thirdparty_objs PUBLIC cxx_std_17)
endif()

# Executable: link object libs
add_subdirectory(kagami)
add_executable(ksud
    $<TARGET_OBJECTS:ksud_our_objs>
    $<TARGET_OBJECTS:ksud_generated_objs>
    $<TARGET_OBJECTS:ksud_thirdparty_objs>
    $<TARGET_OBJECTS:ksud_kagami_objs>
)

target_compile_definitions(ksud PRIVATE ${KSUD_DEFINES})
if(TOYBOX_READELF_AVAILABLE)
    target_link_libraries(ksud PRIVATE readelf_toybox)
endif()
if(NDK_BUSYBOX_AVAILABLE)
    target_compile_definitions(ksud PRIVATE NDK_BUSYBOX_AVAILABLE=1)
    target_link_libraries(ksud PRIVATE busybox)
    # libselinux not in NDK; link only when found (e.g. host or custom Android sysroot)
    find_library(SELINUX_LIB selinux)
    if(SELINUX_LIB)
      target_link_libraries(ksud PRIVATE selinux)
    endif()
    # busybox expects these wraps (from Android.mk)
    target_link_options(ksud PRIVATE -Wl,--wrap=realpath -Wl,--wrap=rename -Wl,--wrap=renameat)
endif()

# Link libraries - Android bionic libc has built-in pthread, no separate linking needed
if(NOT ANDROID)
    target_link_libraries(ksud PRIVATE pthread)
endif()

target_link_libraries(ksud PRIVATE lua z miniz mbedx509 mbedcrypto dl m)
if(ANDROID)
    target_link_options(ksud PRIVATE
        "-Wl,--export-dynamic-symbol=lua_*"
        "-Wl,--export-dynamic-symbol=luaL_*"
        "-Wl,--export-dynamic-symbol=luaopen_*"
    )
endif()

# ============================================================================
# Post-build Size Compression
# ============================================================================

# llvm-strip: more precise than --strip-all, removes .comment / .note misc sections
find_program(LLVM_STRIP llvm-strip)
if(LLVM_STRIP)
    add_custom_command(TARGET ksud POST_BUILD
        COMMAND ${LLVM_STRIP}
            --strip-all                  # Remove all symbols
            --remove-section=.comment    # Remove compiler version info
            --remove-section=.note       # Remove note section
            --remove-section=.note.android.ident
            --remove-section=.note.gnu.build-id
            $<TARGET_FILE:ksud>
        COMMENT "llvm-strip: removing all debug info & note sections..."
    )
endif()

# Optional: UPX binary compression (runtime self-extracting, typically 50-70% further reduction)
# Pass -DKSUD_UPX=ON to enable; off by default as some environments don't support UPX
option(KSUD_UPX "Use UPX to compress ksud binary" OFF)
if(KSUD_UPX)
    find_program(UPX_PROGRAM upx)
    if(UPX_PROGRAM)
        add_custom_command(TARGET ksud POST_BUILD
            COMMAND ${UPX_PROGRAM} --best --lzma $<TARGET_FILE:ksud>
            COMMENT "UPX: compressing ksud with LZMA (--best)..."
        )
        message(STATUS "UPX compression ENABLED (--best --lzma)")
    else()
        message(WARNING "KSUD_UPX=ON but upx not found in PATH; skipping UPX compression")
    endif()
endif()

# Report the final artifact. `ls` is not guaranteed to exist for a native
# Windows/Ninja build, while it remains useful for the Unix build script.
if(CMAKE_HOST_WIN32)
    add_custom_command(TARGET ksud POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "-- ksud built: $<TARGET_FILE:ksud>"
        COMMENT "Reporting final ksud artifact..."
    )
else()
    add_custom_command(TARGET ksud POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "-- ksud final size:"
        COMMAND ls -lh $<TARGET_FILE:ksud>
        COMMENT "Reporting final binary size..."
    )
endif()

# Install
install(TARGETS ksud DESTINATION bin)
