cmake_minimum_required(VERSION 3.20)
project(MiniShell C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(SHELL_SOURCES
    app.rc
    src/main.c
    src/mb_view.c
    src/agb_log.c
    src/build_info.c
    src/crash_page.c
    src/crash_report.c
    src/shot_png.c
    src/shell_ops.c
    src/ui_chrome.c
    src/ui_inspect.c
    src/ui_menu.c
    src/ui_menu_host.c
    src/ui_tabsearch.c
    src/ui_theme.c
    src/ui_widgets.c)

add_executable(minishell WIN32 ${SHELL_SOURCES})

target_compile_definitions(minishell PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
    ENABLE_MB=1)

# shell_version.h is generated at build time (tag or short commit, +dirty) and
# must exist before main.c compiles; the generated dir comes first so it wins
# over any stale copy.
set(SHELL_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/shell_version.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(minishell PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    src)

add_custom_command(TARGET minishell PRE_BUILD
    COMMAND pwsh -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-shell-version.ps1"
            -OutFile "${SHELL_VERSION_HEADER}"
            -RepoRoot "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating shell_version.h (git tag/short hash, +dirty)")

# Crash reports name frames as exe+0xRRRR; the map turns those into functions.
# /Zi + /DEBUG also produce minishell.pdb next to the exe, which the crash
# handler loads through DbgHelp so the report shows function names and lines.
target_compile_options(minishell PRIVATE "$<$<CONFIG:Release>:/Zi>")
target_link_options(minishell PRIVATE
    "$<$<CONFIG:Release>:/DEBUG>"
    "/MAP:${CMAKE_CURRENT_BINARY_DIR}/minishell.map")

target_link_libraries(minishell PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/mb132_x64.lib"
    gdiplus
    dwmapi
    shlwapi
    ole32
    oleaut32
    uuid
    windowscodecs
    comctl32
    user32
    gdi32
    shell32
    winhttp
    advapi32
    version
    dbghelp)

# Runtime payload: the DLL and the optional bundled fonts live next to the exe.
add_custom_command(TARGET minishell POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../mb132_x64.dll"
            "$<TARGET_FILE_DIR:minishell>/mb132_x64.dll"
    COMMENT "Copying mb132_x64.dll next to minishell.exe")

# assets/inspect.js is compiled into the exe by app.rc (RCDATA 201), so no
# runtime copy is needed. A file with that name next to the exe still overrides
# it while editing the script.

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fonts")
    add_custom_command(TARGET minishell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/fonts"
                "$<TARGET_FILE_DIR:minishell>/fonts"
        COMMENT "Copying bundled fonts")
endif()
