# The GBA games' library for the all-games program (SSR_GBA_DIR=gbawide): mGBA (MPL-2.0, fetched by
# setup.sh into deps/mgba) and PokeDaisy's EXPAND (GPL-3.0, third_party/pokedaisy), behind
# frontend/embed.h.
cmake_minimum_required(VERSION 3.16)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/mgba/CMakeLists.txt")
    message(FATAL_ERROR "mGBA is not in ${CMAKE_CURRENT_SOURCE_DIR}/deps/mgba: run gbawide/setup.sh")
endif()
# Only mGBA's library: no frontend, nothing optional.
foreach(feature BUILD_QT BUILD_SDL BUILD_GL BUILD_GLES2 BUILD_GLES3 BUILD_SHARED BUILD_PERF BUILD_TEST BUILD_SUITE
        BUILD_CINEMA BUILD_EXAMPLE BUILD_PYTHON BUILD_LIBRETRO BUILD_OPENEMU USE_DISCORD_RPC USE_LUA USE_FFMPEG
        USE_LIBZIP USE_MINIZIP USE_PNG USE_SQLITE3 USE_ELF USE_LZMA USE_DEBUGGERS USE_GDB_STUB USE_EDITLINE USE_EPOXY
        USE_JSON_C USE_ZLIB ENABLE_SCRIPTING M_CORE_GB)
    set(${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(M_CORE_GBA ON CACHE BOOL "" FORCE)
set(SKIP_LIBRARY OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
add_subdirectory(deps/mgba mgba EXCLUDE_FROM_ALL)
if(NINTENDO_SWITCH OR CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # (devkitPro's newlib with -std=c11 hides strdup, strlcpy and locale_t without it.)
    target_compile_definitions(mgba PRIVATE _GNU_SOURCE)
    # (Its files on the SD card: mGBA adds these for its own Switch frontend, which is not built here.)
    target_sources(mgba PRIVATE deps/mgba/src/util/vfs/vfs-fd.c deps/mgba/src/util/vfs/vfs-dirent.c)
    # (No POSIX signals there for EXPAND's fault guard; its reads are bounds-checked anyway.)
    set(GBAW_SWITCH ON)
endif()
add_library(gbawide STATIC
    src/gbawide.cpp
    third_party/pokedaisy/pk_wide.c
    third_party/pokedaisy/pk_cheats.c
    src/pk_wide_gb_none.c)
target_include_directories(gbawide PUBLIC frontend
    PRIVATE deps/mgba/include "${CMAKE_CURRENT_BINARY_DIR}/mgba/include")
set_source_files_properties(third_party/pokedaisy/pk_wide.c third_party/pokedaisy/pk_cheats.c
    PROPERTIES COMPILE_OPTIONS "-w")
target_compile_features(gbawide PRIVATE cxx_std_17)
if(GBAW_SWITCH)
    target_compile_definitions(gbawide PRIVATE PK_WIDE_NO_GUARD _GNU_SOURCE)
endif()
target_link_libraries(gbawide PRIVATE mgba)
if(TARGET SDL2::SDL2)
    target_link_libraries(gbawide PRIVATE SDL2::SDL2)
elseif(SDL2_STATIC_INCLUDE_DIRS OR SDL2_INCLUDE_DIRS)
    target_include_directories(gbawide PRIVATE ${SDL2_STATIC_INCLUDE_DIRS} ${SDL2_INCLUDE_DIRS})
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(gbawide PRIVATE SDL2::SDL2)
endif()
# One game without the list, for tests.
add_executable(gbawide-play EXCLUDE_FROM_ALL test/play.cpp)
target_link_libraries(gbawide-play PRIVATE gbawide)
