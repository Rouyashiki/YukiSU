# Imported from Kagami cb8345501f3bf8c8f32c844b75751026e55801d1.
set(KAGAMI_SOURCES
    src/core/command.cpp src/core/daemon.cpp src/core/log.cpp
    src/core/config.cpp src/core/runtime.cpp src/core/reboot_state.cpp
    src/mount/backend.cpp src/mount/mount_fs.cpp src/mount/kasumi.cpp
    src/mount/kasumi_client.cpp src/mount/magic_mount.cpp
    src/mount/overlayfs.cpp src/mount/storage.cpp src/embedded.cpp)
add_library(ksud_kagami_objs OBJECT ${KAGAMI_SOURCES})
target_include_directories(ksud_kagami_objs BEFORE PRIVATE include src ../src ../../..)
target_compile_options(ksud_kagami_objs PRIVATE -fexceptions)
yukisu_enable_clang_tidy(ksud_kagami_objs CXX)
