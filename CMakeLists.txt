cmake_minimum_required(VERSION 3.21)
project(liteui LANGUAGES C CXX)

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
# maintainer has to re-run the `regen-protocols` target (see below) and
# commit the result to pick up upstream protocol changes.
#
# Expected layout:
#   platform/wayland-protocols/xdg-shell-protocol.c
#   platform/wayland-protocols/xdg-shell-client-protocol.h
#   platform/wayland-protocols/xdg-decoration-protocol.c
#   platform/wayland-protocols/xdg-decoration-client-protocol.h
#
# On a FRESH CHECKOUT (or the first time you build liteui itself rather
# than just consuming it) those files don't exist yet. This file
# registers the `regen-protocols` target BEFORE checking for the
# vendored files, and downgrades the missing-files case to a WARNING
# (instead of a hard FATAL_ERROR) whenever wayland-scanner and
# wayland-protocols are available to generate them — so a plain
# `cmake -B build` still configures successfully and you can run:
#
#   cmake --build build --target regen-protocols
#   git add platform/wayland-protocols && git commit
#
# ...then re-run cmake's configure step. If wayland-scanner/
# wayland-protocols aren't installed AND the vendored files are
# missing, configure still fails with a FATAL_ERROR, since there's no
# way to produce a working build either way.
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

    # ------------------------------------------------------------------
    # Register `regen-protocols` FIRST, before the existence check below.
    # A FATAL_ERROR anywhere in this file halts configure immediately,
    # so if the check came first (as it used to), this target would
    # never get registered on a fresh checkout — the exact chicken-and-
    # egg problem that produced the original build failure. Registering
    # it up front, gated only on finding wayland-scanner +
    # wayland-protocols (not on the vendored files existing), lets a
    # fresh checkout configure far enough to actually run it.
    # ------------------------------------------------------------------
    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
    pkg_get_variable(_liteui_wp_dir wayland-protocols pkgdatadir)

    if(WAYLAND_SCANNER_EXECUTABLE AND _liteui_wp_dir)
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

    # Deliberately no requirement on wayland-scanner/wayland-protocols
    # being installed for a NORMAL build — the whole point of vendoring
    # is that a consumer with the committed platform/wayland-protocols/
    # files doesn't need either of those. This check only fires when
    # those files are missing (first build, or a fresh checkout before
    # regen-protocols has been run and committed).
    if(NOT EXISTS "${LITEUI_PROTO_DIR}/xdg-shell-protocol.c")
        if(TARGET regen-protocols)
            message(WARNING
                "liteui: vendored Wayland protocol sources not found under "
                "${LITEUI_PROTO_DIR}. Run:\n"
                "    cmake --build <builddir> --target regen-protocols\n"
                "    git add platform/wayland-protocols && git commit\n"
                "then re-run cmake's configure step. Continuing configure "
                "for now so that target is available to run; the "
                "liteui_wayland_protocols library below will be skipped "
                "until the files exist.")
        else()
            message(FATAL_ERROR
                "liteui: vendored Wayland protocol sources not found under "
                "${LITEUI_PROTO_DIR}, and wayland-scanner + wayland-protocols "
                "aren't available to generate them. Install both "
                "(e.g. `sudo apt install libwayland-bin wayland-protocols`) "
                "and re-run cmake, or obtain the vendored files another way.")
        endif()
    endif()

    # Only build (and link) the protocol bindings once the sources
    # actually exist — lets configure succeed even on a fresh checkout
    # so `regen-protocols` itself can be built; a normal `liteui` build
    # target will still fail obviously (missing sources) until you've
    # run regen-protocols and re-configured, rather than failing here
    # with an opaque error.
    if(EXISTS "${LITEUI_PROTO_DIR}/xdg-shell-protocol.c")
        add_library(liteui_wayland_protocols STATIC
            ${LITEUI_PROTO_DIR}/xdg-shell-protocol.c
            ${LITEUI_PROTO_DIR}/xdg-decoration-protocol.c
        )
        target_include_directories(liteui_wayland_protocols PUBLIC ${LITEUI_PROTO_DIR})
        target_compile_options(liteui_wayland_protocols PRIVATE -w)

        target_link_libraries(liteui INTERFACE liteui_wayland_protocols)
    endif()

    target_link_libraries(liteui INTERFACE
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