# Embed upstream Lua 5.5 and expose its native module ABI.
set(lua_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/lua")
set(LUA_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/lua")
file(MAKE_DIRECTORY "${LUA_BUILD_DIR}")
file(GLOB LUA_HEADERS CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/*.h")
foreach(header IN LISTS LUA_HEADERS)
    get_filename_component(name "${header}" NAME)
    if(NOT name STREQUAL "luaconf.h")
        configure_file("${header}" "${LUA_BUILD_DIR}/${name}" COPYONLY)
    endif()
endforeach()

# Lua uses C++ exceptions when compiled as C++, but its public API must retain C linkage.
set(LUA_CONFIG_HEADER "${lua_SOURCE_DIR}/luaconf.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LUA_CONFIG_HEADER}")
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
endif()
file(CONFIGURE OUTPUT "${LUA_BUILD_DIR}/luaconf.h" CONTENT "${LUA_CONFIG_CONTENT}" @ONLY)

set(LUA_SOURCES
    ${LUA_BUILD_DIR}/lapi.c
    ${LUA_BUILD_DIR}/lcode.c
    ${LUA_BUILD_DIR}/lctype.c
    ${LUA_BUILD_DIR}/ldebug.c
    ${LUA_BUILD_DIR}/ldo.c
    ${LUA_BUILD_DIR}/ldump.c
    ${LUA_BUILD_DIR}/lfunc.c
    ${LUA_BUILD_DIR}/lgc.c
    ${LUA_BUILD_DIR}/llex.c
    ${LUA_BUILD_DIR}/lmem.c
    ${LUA_BUILD_DIR}/lobject.c
    ${LUA_BUILD_DIR}/lopcodes.c
    ${LUA_BUILD_DIR}/lparser.c
    ${LUA_BUILD_DIR}/lstate.c
    ${LUA_BUILD_DIR}/lstring.c
    ${LUA_BUILD_DIR}/ltable.c
    ${LUA_BUILD_DIR}/ltm.c
    ${LUA_BUILD_DIR}/lundump.c
    ${LUA_BUILD_DIR}/lvm.c
    ${LUA_BUILD_DIR}/lzio.c
    ${LUA_BUILD_DIR}/lauxlib.c
    ${LUA_BUILD_DIR}/lbaselib.c
    ${LUA_BUILD_DIR}/lcorolib.c
    ${LUA_BUILD_DIR}/ldblib.c
    ${LUA_BUILD_DIR}/liolib.c
    ${LUA_BUILD_DIR}/lmathlib.c
    ${LUA_BUILD_DIR}/loslib.c
    ${LUA_BUILD_DIR}/lstrlib.c
    ${LUA_BUILD_DIR}/ltablib.c
    ${LUA_BUILD_DIR}/lutf8lib.c
    ${LUA_BUILD_DIR}/loadlib.c
    ${LUA_BUILD_DIR}/linit.c
)

foreach(source IN LISTS LUA_SOURCES)
    get_filename_component(name "${source}" NAME)
    configure_file("${lua_SOURCE_DIR}/${name}" "${source}" COPYONLY)
endforeach()
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua OBJECT ${LUA_SOURCES})
target_include_directories(lua PUBLIC "${LUA_BUILD_DIR}")
target_compile_definitions(lua PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
target_compile_options(lua PRIVATE -fvisibility=default -Wno-deprecated)
if(ANDROID)
    target_link_libraries(lua PRIVATE dl m)
endif()
if(NOT ANDROID)
    target_link_libraries(lua PRIVATE pthread m)
endif()
