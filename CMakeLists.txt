cmake_minimum_required(VERSION 3.20)

# Version: CI passes the commit's date (-DEXPLORERPLUS_VERSION=2026.10.10), the run number
# (-DEXPLORERPLUS_BUILD=57) and a label shown in the file properties
# (-DEXPLORERPLUS_VERSION_LABEL=2026.10.10.57); local builds keep these defaults
set(EXPLORERPLUS_VERSION "1.0.0" CACHE STRING "Product version (major.minor.patch)")
set(EXPLORERPLUS_VERSION_LABEL "" CACHE STRING "Version text shown in the file properties (default: the version)")
set(EXPLORERPLUS_BUILD "0" CACHE STRING "Fourth version number (CI run number)")

project(explorer-plus VERSION ${EXPLORERPLUS_VERSION} LANGUAGES CXX RC)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EP_VERSION_BUILD ${EXPLORERPLUS_BUILD})
set(EP_VERSION_LABEL ${EXPLORERPLUS_VERSION_LABEL})
if(NOT EP_VERSION_LABEL)
    set(EP_VERSION_LABEL ${PROJECT_VERSION})
endif()
configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)

add_executable(explorer-plus WIN32
    src/appstate.cpp
    src/shell_utils.cpp
    src/main.cpp
    src/layout.cpp
    src/navbar.cpp
    src/tabs.cpp
    src/detailspane.cpp
    src/tree.cpp
    src/listview.cpp
    src/enumerate.cpp
    src/navigation.cpp
    src/fileops.cpp
    src/contextmenu.cpp
    src/dnd.cpp
    src/watcher.cpp
    src/thumbs.cpp
    src/settings.cpp
    src/searchbox.cpp
    src/theme.cpp
    src/chrome.cpp
    src/cmdbar.cpp
    src/statusbar.cpp
    src/breadcrumb.cpp
    src/groups.cpp
    src/columns.cpp
    src/preview.cpp
    src/flyout.cpp
    src/perf.cpp
    src/crash.cpp
    src/workers.cpp
    src/acc.cpp
    src/foldersizes.cpp
    src/ini.cpp
    src/cmdline.cpp
    src/media.cpp
    src/mediadata.cpp
    src/mediainfo.cpp
    src/imageview.cpp
    src/loudness.cpp
    src/spectrum.cpp
    src/playlist.cpp
    src/player.cpp
    res/resource.rc
)

# Windows resources (res/): manifest, icon, version info; IDs from src/resource.h,
# version numbers from the generated version.h
set_source_files_properties(res/resource.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/res;${CMAKE_CURRENT_SOURCE_DIR}/src;${CMAKE_CURRENT_BINARY_DIR}/generated"
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/player.ico;${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h;${CMAKE_CURRENT_BINARY_DIR}/generated/version.h")

target_compile_options(explorer-plus PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type)
# STRICT_TYPED_ITEMIDS: absolute ID lists and child IDs are different types, so the compiler
# tells an absolute pidl passed where a child belongs (and the other way round)
target_compile_definitions(explorer-plus PRIVATE UNICODE _UNICODE STRICT_TYPED_ITEMIDS)
# stack buffer overruns end the process instead of returning through a smashed frame
target_compile_options(explorer-plus PRIVATE -fstack-protector-strong)
target_link_options(explorer-plus PRIVATE -fstack-protector-strong)
target_link_options(explorer-plus PRIVATE -municode)
target_link_options(explorer-plus PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
# ASLR (64-bit), DEP: recent binutils default to these, older ones don't
target_link_options(explorer-plus PRIVATE -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va)

# release: the debug info goes to explorer-plus.debug next to the exe, the shipped exe is
# stripped. Keep the .debug of every published build: a crash dump's module offsets are
# resolved against it (addr2line -f -C -e explorer-plus.debug 0x<address>).
target_compile_options(explorer-plus PRIVATE $<$<CONFIG:Release>:-g>)
if(CMAKE_OBJCOPY AND CMAKE_STRIP)
    add_custom_command(TARGET explorer-plus POST_BUILD
        COMMAND "$<$<CONFIG:Release>:${CMAKE_OBJCOPY};--only-keep-debug;$<TARGET_FILE:explorer-plus>;$<TARGET_FILE_DIR:explorer-plus>/explorer-plus.debug>"
        COMMAND "$<$<CONFIG:Release>:${CMAKE_STRIP};--strip-all;$<TARGET_FILE:explorer-plus>>"
        COMMAND "$<$<CONFIG:Release>:${CMAKE_OBJCOPY};--add-gnu-debuglink=explorer-plus.debug;$<TARGET_FILE:explorer-plus>>"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:explorer-plus>"
        COMMAND_EXPAND_LISTS
        VERBATIM)
else()
    target_link_options(explorer-plus PRIVATE $<$<CONFIG:Release>:-s>)
endif()

target_link_libraries(explorer-plus PRIVATE
    comctl32 shell32 ole32 oleaut32 shlwapi uuid uxtheme comdlg32 gdi32 user32 propsys windowscodecs gdiplus dwmapi msimg32 oleacc
    # Media Foundation's GUIDs (static); mf.dll / mfplat.dll themselves load on first use
    mfuuid)

# unit tests of the pure text handling (settings INI, command line, typed paths):
# ctest --test-dir build --output-on-failure
option(EXPLORERPLUS_TESTS "Build the unit tests" ON)
if(EXPLORERPLUS_TESTS)
    enable_testing()
    add_executable(explorer-plus-tests tests/unit_tests.cpp src/ini.cpp src/cmdline.cpp src/playlist.cpp src/mediadata.cpp)
    target_include_directories(explorer-plus-tests PRIVATE src)
    target_compile_options(explorer-plus-tests PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    target_compile_definitions(explorer-plus-tests PRIVATE UNICODE _UNICODE)
    target_link_options(explorer-plus-tests PRIVATE -municode -static -static-libgcc -static-libstdc++)
    target_link_libraries(explorer-plus-tests PRIVATE shell32 shlwapi)
    add_test(NAME unit COMMAND explorer-plus-tests)
endif()
