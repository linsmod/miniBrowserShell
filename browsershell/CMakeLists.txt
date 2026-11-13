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

target_include_directories(minishell PRIVATE src)

# Crash reports name frames as exe+0xRRRR; the map turns those into functions.
target_link_options(minishell PRIVATE
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
    version)

# Runtime payload: the DLL and the optional bundled fonts live next to the exe.
add_custom_command(TARGET minishell POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../mb132_x64.dll"
            "$<TARGET_FILE_DIR:minishell>/mb132_x64.dll"
    COMMENT "Copying mb132_x64.dll next to minishell.exe")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_custom_command(TARGET minishell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:minishell>"
        COMMENT "Copying inspector.js / vconsole.min.js")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fonts")
    add_custom_command(TARGET minishell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/fonts"
                "$<TARGET_FILE_DIR:minishell>/fonts"
        COMMENT "Copying bundled fonts")
endif()
