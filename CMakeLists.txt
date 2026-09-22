cmake_minimum_required(VERSION 3.21)
project(liteui LANGUAGES CXX)

# ------------------------------------------------------------------------
# liteui::liteui — INTERFACE target for the single-header library.
#
# Consumers do:
#     add_subdirectory(path/to/liteui)          # or FetchContent
#     target_link_libraries(app PRIVATE liteui::liteui)
#
# This variant VENDORS the generated xdg-shell / xdg-decoration protocol
# bindings under platform/wayland-protocols/ (checked into git). A
# consumer building on Linux therefore only needs the runtime libraries
# below (wayland-client, cairo, egl, etc.) — NOT wayland-scanner or the
# wayland-protocols package. The trade-off: the vendored files are
# frozen at whatever wayland-protocols version generated them, and a
# maintainer has to re-run the `regen-protocols` target (bottom of this
# file) and commit the result to pick up upstream protocol changes.
#
# Expected layout:
#   platform/wayland-protocols/xdg-shell-protocol.c
#   platform/wayland-protocols/xdg-shell-client-protocol.h
#   platform/wayland-protocols/xdg-decoration-protocol.c
#   platform/wayland-protocols/xdg-decoration-client-protocol.h
# ------------------------------------------------------------------------

add_library(liteui INTERFACE)
add_library(liteui::liteui ALIAS liteui)

target_include_directories(liteui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(liteui INTERFACE cxx_std_23)

set(LITEUI_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/platform/wayland-protocols")

if(WIN32)
    target_link_libraries(liteui INTERFACE
        user32
        gdi32
        d2d1
        dwrite
        windowscodecs
        ole32
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND        REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)
    pkg_check_modules(WAYLAND_EGL    REQUIRED IMPORTED_TARGET wayland-egl)
    pkg_check_modules(CAIRO          REQUIRED IMPORTED_TARGET cairo)
    pkg_check_modules(PANGOCAIRO     REQUIRED IMPORTED_TARGET pangocairo)
    pkg_check_modules(EGL            REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(GLES           REQUIRED IMPORTED_TARGET glesv2)
    pkg_check_modules(XKBCOMMON      REQUIRED IMPORTED_TARGET xkbcommon)
    pkg_check_modules(PNG            REQUIRED IMPORTED_TARGET libpng)
    pkg_check_modules(JPEG           REQUIRED IMPORTED_TARGET libjpeg)
    # Deliberately no find_program(wayland-scanner) and no
    # pkg_get_variable(wayland-protocols ...) in this normal build path —
    # the whole point of vendoring is that a consumer doesn't need
    # either of those installed.

    if(NOT EXISTS "${LITEUI_PROTO_DIR}/xdg-shell-protocol.c")
        message(FATAL_ERROR
            "liteui: vendored Wayland protocol sources not found under "
            "${LITEUI_PROTO_DIR}. If you're building liteui itself (not "
            "just consuming it), run the 'regen-protocols' target once "
            "(requires wayland-scanner + wayland-protocols) and commit "
            "the generated files.")
    endif()

    add_library(liteui_wayland_protocols STATIC
        ${LITEUI_PROTO_DIR}/xdg-shell-protocol.c
        ${LITEUI_PROTO_DIR}/xdg-decoration-protocol.c
    )
    target_include_directories(liteui_wayland_protocols PUBLIC ${LITEUI_PROTO_DIR})
    target_compile_options(liteui_wayland_protocols PRIVATE -w)

    target_link_libraries(liteui INTERFACE
        liteui_wayland_protocols
        PkgConfig::WAYLAND
        PkgConfig::WAYLAND_CURSOR
        PkgConfig::WAYLAND_EGL
        PkgConfig::CAIRO
        PkgConfig::PANGOCAIRO
        PkgConfig::EGL
        PkgConfig::GLES
        PkgConfig::XKBCOMMON
        PkgConfig::PNG
        PkgConfig::JPEG
        util
    )
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(LITEUI_BUILD_EXAMPLES "Build the liteui example programs" ON)
    if(LITEUI_BUILD_EXAMPLES)
        add_subdirectory(examples)
    endif()
endif()

# ------------------------------------------------------------------------
# Maintainer-only: regenerate the vendored protocol files.
#
#   cmake --build build --target regen-protocols
#   git add platform/wayland-protocols && git commit
#
# Not part of the normal configure/build path — only registered if
# wayland-scanner happens to be present, and never required for an
# ordinary `cmake -B build && cmake --build build`.
# ------------------------------------------------------------------------
if(NOT WIN32)
    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
    find_package(PkgConfig)
    if(WAYLAND_SCANNER_EXECUTABLE AND PkgConfig_FOUND)
        pkg_get_variable(_liteui_wp_dir wayland-protocols pkgdatadir)
        if(_liteui_wp_dir)
            set(_xdg_shell_xml      "${_liteui_wp_dir}/stable/xdg-shell/xdg-shell.xml")
            set(_xdg_decoration_xml "${_liteui_wp_dir}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
            file(MAKE_DIRECTORY ${LITEUI_PROTO_DIR})
            add_custom_target(regen-protocols
                COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code  ${_xdg_shell_xml}      ${LITEUI_PROTO_DIR}/xdg-shell-protocol.c
                COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${_xdg_shell_xml}      ${LITEUI_PROTO_DIR}/xdg-shell-client-protocol.h
                COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code  ${_xdg_decoration_xml} ${LITEUI_PROTO_DIR}/xdg-decoration-protocol.c
                COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${_xdg_decoration_xml} ${LITEUI_PROTO_DIR}/xdg-decoration-client-protocol.h
                COMMENT "liteui: regenerating platform/wayland-protocols/ — remember to commit the result"
                VERBATIM
            )
        endif()
    endif()
endif()