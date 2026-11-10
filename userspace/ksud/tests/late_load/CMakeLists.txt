cmake_minimum_required(VERSION 3.20)
project(ksud_late_load_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-UNDEBUG -include linux/types.h -ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections)
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

add_executable(ksucalls_sigsys_test
    ../ksucalls_sigsys_test.cpp
    ${KSUD_ROOT}/src/core/ksucalls.cpp)
target_include_directories(ksucalls_sigsys_test PRIVATE "${KSUD_ROOT}/../..")
add_test(NAME ksucalls_sigsys COMMAND ksucalls_sigsys_test)
# LeakSanitizer's helper inherits the test's seccomp trap for getppid.
set_tests_properties(ksucalls_sigsys PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

add_executable(ksucalls_probe_test
    ../ksucalls_probe_test.cpp
    ${KSUD_ROOT}/src/core/ksucalls.cpp)
target_include_directories(ksucalls_probe_test PRIVATE "${KSUD_ROOT}/../..")
target_link_options(ksucalls_probe_test PRIVATE
    "-Wl,--wrap=prctl" "-Wl,--wrap=syscall" "-Wl,--wrap=ioctl" "-Wl,--wrap=readlink")
add_test(NAME ksucalls_probe COMMAND ksucalls_probe_test)
set_tests_properties(late_load_cli kernelsu_loader ksucalls_sigsys ksucalls_probe
    PROPERTIES TIMEOUT 20)

add_executable(kmi_detection_test ../kmi_detection_test.cpp
    ${KSUD_ROOT}/src/boot/boot_patch.cpp ${KSUD_ROOT}/src/utils.cpp)
target_include_directories(kmi_detection_test PRIVATE "${KSUD_ROOT}/../.."
    "${KSUD_ROOT}/third_party/mbedtls/include")
target_link_options(kmi_detection_test PRIVATE -Wl,--wrap=open -Wl,--wrap=opendir
    -Wl,--wrap=uname)
add_test(NAME kmi_detection COMMAND kmi_detection_test)
set_tests_properties(kmi_detection PROPERTIES TIMEOUT 20)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(LibLZMA REQUIRED)
find_package(ZLIB REQUIRED)
set(ASSET_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/assets")
file(MAKE_DIRECTORY "${ASSET_FIXTURE}")
string(REPEAT "first-kernel-payload" 9000 first)
string(REPEAT "second-kernel-payload" 9000 second)
file(WRITE "${ASSET_FIXTURE}/android14-6.1_kernelsu.ko" "${first}")
file(WRITE "${ASSET_FIXTURE}/android16-6.12_kernelsu.ko" "${second}")
file(WRITE "${ASSET_FIXTURE}/payload.bin" "ordinary payload")
file(WRITE "${ASSET_FIXTURE}/empty.bin" "")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp"
    COMMAND ${Python3_EXECUTABLE} "${KSUD_ROOT}/scripts/embed_assets.py"
        "${ASSET_FIXTURE}" "${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp"
    DEPENDS "${KSUD_ROOT}/scripts/embed_assets.py"
    VERBATIM)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/asset_mbedtls_config.h" "#define MBEDTLS_SHA256_C\n")
add_executable(asset_data_test ../asset_data_test.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp" ${KSUD_ROOT}/src/xz_range.c
    ${KSUD_ROOT}/third_party/mbedtls/library/sha256.c
    ${KSUD_ROOT}/third_party/mbedtls/library/platform_util.c)
target_include_directories(asset_data_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${KSUD_ROOT}/src" "${KSUD_ROOT}/../.." "${KSUD_ROOT}/third_party/mbedtls/include")
target_compile_definitions(asset_data_test PRIVATE MBEDTLS_CONFIG_FILE="asset_mbedtls_config.h")
target_link_libraries(asset_data_test PRIVATE LibLZMA::LibLZMA ZLIB::ZLIB)
target_link_options(asset_data_test PRIVATE -Wl,--wrap=mbedtls_sha256)
add_test(NAME asset_data COMMAND asset_data_test "${ASSET_FIXTURE}")
set_tests_properties(asset_data PROPERTIES TIMEOUT 20)
