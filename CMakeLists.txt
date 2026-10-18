cmake_minimum_required(VERSION 3.20)

# Version: CI passes the commit's date (-DEXPLORERPLUS_VERSION=2026.10.10), the run number
# (-DEXPLORERPLUS_BUILD=57) and a label shown in the file properties
# (-DEXPLORERPLUS_VERSION_LABEL=2026.10.10.57); local builds keep these defaults
set(EXPLORERPLUS_VERSION "1.0.0" CACHE STRING "Product version (major.minor.patch)")
set(EXPLORERPLUS_VERSION_LABEL "" CACHE STRING "Version text shown in the file properties (default: the version)")
set(EXPLORERPLUS_BUILD "0" CACHE STRING "Fourth version number (CI run number)")

project(explorer-plus VERSION ${EXPLORERPLUS_VERSION} LANGUAGES C CXX RC)

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

# Sync folders (docs/sync-plan.md): everything but the UI, shared by the app and the sync
# tests. The one third-party library is Monocypher (third_party/monocypher, CC0 / BSD-2).
set(EP_SYNC_CORE_SOURCES
    third_party/monocypher/monocypher.c
    src/crypto.cpp
    src/crypto_file.cpp
    src/crypto_names.cpp
    src/credstore.cpp
    src/proc.cpp
    src/http.cpp
    src/sync_backend.cpp
    src/sync_paths.cpp
    src/sync_state.cpp
    src/sync_plan.cpp
    src/sync_local.cpp
    src/sync_engine.cpp
    src/sync_folder.cpp
    src/sync_s3.cpp
    src/sync_git.cpp
    src/sync_azdo.cpp
    src/sync_azdo_rest.cpp
    src/sync_gitpack.cpp
    src/sync_manifest.cpp
    src/json.cpp
    src/sync_keys.cpp
    src/sync_crypt.cpp
    src/sync_config.cpp
    src/sync_ipc.cpp
    src/sigv4.cpp
)
set(EP_SYNC_LIBS bcrypt winhttp advapi32 credui normaliz shell32 ole32 uuid)
set_source_files_properties(third_party/monocypher/monocypher.c src/crypto.cpp src/crypto_file.cpp src/crypto_names.cpp
    PROPERTIES COMPILE_OPTIONS -O2)

add_executable(explorer-plus WIN32
    ${EP_SYNC_CORE_SOURCES}
    src/sync_agent.cpp
    src/sync_ui.cpp
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
    src/imageformats.cpp
    src/loudness.cpp
    src/spectrum.cpp
    src/visual.cpp
    src/playlist.cpp
    src/player.cpp
    src/shapes.cpp
    src/imageedit.cpp
    src/screenshot.cpp
    src/assoc.cpp
    src/userchoice.cpp
    src/textfile.cpp
    src/syntax.cpp
    src/textops.cpp
    src/notepad.cpp
    res/resource.rc
)

# Windows resources (res/): manifest, icon, version info; IDs from src/resource.h,
# version numbers from the generated version.h
set_source_files_properties(res/resource.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/res;${CMAKE_CURRENT_SOURCE_DIR}/src;${CMAKE_CURRENT_BINARY_DIR}/generated"
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/player.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/notepad.ico;${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h;${CMAKE_CURRENT_BINARY_DIR}/generated/version.h")

# optimized for size: a third smaller than -O2, and tools/perf.py measures no difference
# (listing, sorting, scrolling 30,000 files). The number crunching (loudness scans whole
# files, the spectrum and the visualizations run every frame; the Notepad's text of tens of
# megabytes is converted, lexed, sorted) keeps -O2. Each function
# and variable in its own section: the linker drops the ones nothing calls.
target_compile_options(explorer-plus PRIVATE -Os -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type)
set_source_files_properties(src/loudness.cpp src/spectrum.cpp src/visual.cpp src/textfile.cpp src/syntax.cpp src/textops.cpp
    PROPERTIES COMPILE_OPTIONS -O2)
# (-Os merges constructors into one "unified" C4 symbol, which libstdc++'s std::string
# doesn't export: a moved std::string then fails to link with GCC 16)
target_compile_options(explorer-plus PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-declone-ctor-dtor>)
target_link_options(explorer-plus PRIVATE -Wl,--gc-sections)
# STRICT_TYPED_ITEMIDS: absolute ID lists and child IDs are different types, so the compiler
# tells an absolute pidl passed where a child belongs (and the other way round)
target_compile_definitions(explorer-plus PRIVATE UNICODE _UNICODE STRICT_TYPED_ITEMIDS)
# the Windows 10 API for every file, whatever it includes first (some GCC 16 builds'
# <thread> brings windows.h in with an older default, before shell_utils.h sets these)
target_compile_definitions(explorer-plus PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)
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
    ${EP_SYNC_LIBS}
    # Media Foundation's GUIDs (static); mf.dll / mfplat.dll themselves load on first use
    mfuuid)

# unit tests of the pure text handling (settings INI, command line, typed paths):
# ctest --test-dir build --output-on-failure
option(EXPLORERPLUS_TESTS "Build the unit tests" ON)
if(EXPLORERPLUS_TESTS)
    enable_testing()
    add_executable(explorer-plus-tests tests/unit_tests.cpp src/ini.cpp src/cmdline.cpp src/playlist.cpp src/mediadata.cpp src/shapes.cpp src/imageformats.cpp src/userchoice.cpp src/textfile.cpp src/syntax.cpp src/textops.cpp)
    target_include_directories(explorer-plus-tests PRIVATE src)
    target_compile_options(explorer-plus-tests PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    target_compile_definitions(explorer-plus-tests PRIVATE UNICODE _UNICODE)
    target_link_options(explorer-plus-tests PRIVATE -municode -static -static-libgcc -static-libstdc++)
    target_link_libraries(explorer-plus-tests PRIVATE shell32 shlwapi)
    add_test(NAME unit COMMAND explorer-plus-tests)

    # the sync feature: crypto (published vectors), file format, names, planner, engine, stores
    add_executable(explorer-plus-sync-tests tests/sync_tests.cpp tests/sync_engine_tests.cpp tests/sync_crypt_tests.cpp tests/sync_git_tests.cpp tests/sync_azdo_tests.cpp tests/sync_s3_tests.cpp tests/sync_bench.cpp ${EP_SYNC_CORE_SOURCES})
    target_include_directories(explorer-plus-sync-tests PRIVATE src)
    target_compile_options(explorer-plus-sync-tests PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    target_compile_definitions(explorer-plus-sync-tests PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)
    target_link_options(explorer-plus-sync-tests PRIVATE -municode -static -static-libgcc -static-libstdc++)
    target_link_libraries(explorer-plus-sync-tests PRIVATE ${EP_SYNC_LIBS} ws2_32)
    add_test(NAME sync COMMAND explorer-plus-sync-tests)
endif()
