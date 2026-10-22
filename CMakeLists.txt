cmake_minimum_required(VERSION 3.21)
project(liteui LANGUAGES CXX)

# C is only needed on Linux, for the vendored Wayland protocol .c files.
# Windows builds never compile any C, so don't require a C toolchain there.
if(NOT WIN32)
    enable_language(C)
endif()

# ------------------------------------------------------------------------
# liteui::liteui — header-only INTERFACE library (liteui.hpp).
#
# The implementation lives in the same header, guarded by
# LITEUI_IMPLEMENTATION. Each executable must define it in EXACTLY ONE .cpp:
#
#     #define LITEUI_IMPLEMENTATION
#     #include "liteui.hpp"
#
# Consumers do:
#     add_subdirectory(path/to/liteui)          # or FetchContent
#     target_link_libraries(app PRIVATE liteui::liteui)
#
# Because the implementation is compiled inside the consumer, the platform
# libraries (Win32/D2D, or Wayland/Cairo/EGL/...) and the LITEUI_PROFILE
# define are all INTERFACE: they propagate to whatever links liteui.
#
# This variant VENDORS the generated xdg-shell / xdg-decoration protocol
# bindings under platform/wayland-protocols/ (checked into git). A
# consumer building on Linux therefore only needs the runtime libraries
# below (wayland-client, cairo, egl, etc.) — NOT wayland-scanner or the
# wayland-protocols package. The trade-off: the vendored files are
# frozen at whatever wayland-protocols version generated them, and a
# maintainer has to re-run the `regen-protocols` target and commit the
# result to pick up upstream protocol changes.
#
# Expected layout:
#   platform/wayland-protocols/xdg-shell-protocol.c
#   platform/wayland-protocols/xdg-shell-client-protocol.h
#   platform/wayland-protocols/xdg-decoration-protocol.c
#   platform/wayland-protocols/xdg-decoration-client-protocol.h
#
# On a FRESH CHECKOUT those files don't exist yet. This file registers the
# `regen-protocols` target BEFORE checking for the vendored files, and
# downgrades the missing-files case to a WARNING whenever wayland-scanner
# and wayland-protocols are available, so a plain `cmake -B build` still
# configures and you can run:
#
#   cmake --build build --target regen-protocols
#   git add platform/wayland-protocols && git commit
#
# ...then re-run cmake's configure step. If wayland-scanner/
# wayland-protocols aren't installed AND the vendored files are missing,
# configure fails with a FATAL_ERROR.
# ------------------------------------------------------------------------

add_library(liteui INTERFACE)
add_library(liteui::liteui ALIAS liteui)

target_include_directories(liteui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(liteui INTERFACE cxx_std_23)

# Optional frame profiler (see LITEUI_PROFILE in liteui.hpp). When ON,
# consumers are compiled with -DLITEUI_PROFILE and print per-second timing
# of mouse handling / painting / etc. to stderr. OFF by default: compiles
# to nothing.
option(LITEUI_PROFILE "Print per-second frame timing to stderr" OFF)
if(LITEUI_PROFILE)
    target_compile_definitions(liteui INTERFACE LITEUI_PROFILE)
endif()

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
    # Register `regen-protocols` FIRST, before the existence check below,
    # so a fresh checkout can configure far enough to run it.
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

    # All four generated files must exist: the two .c files are compiled
    # into liteui_wayland_protocols, and the two headers are what
    # liteui.hpp includes. Checking only one would let a partial checkout
    # slip through and fail later with a confusing compile/link error.
    set(LITEUI_PROTO_FILES
        xdg-shell-protocol.c
        xdg-shell-client-protocol.h
        xdg-decoration-protocol.c
        xdg-decoration-client-protocol.h
    )
    set(LITEUI_PROTO_MISSING "")
    foreach(_f IN LISTS LITEUI_PROTO_FILES)
        if(NOT EXISTS "${LITEUI_PROTO_DIR}/${_f}")
            list(APPEND LITEUI_PROTO_MISSING "${_f}")
        endif()
    endforeach()

    # Only fires when any vendored file is missing (first build, or a
    # fresh checkout before regen-protocols has been run and committed).
    if(LITEUI_PROTO_MISSING)
        list(JOIN LITEUI_PROTO_MISSING ", " _missing_str)
        if(TARGET regen-protocols)
            message(WARNING
                "liteui: vendored Wayland protocol files missing under "
                "${LITEUI_PROTO_DIR}: ${_missing_str}. Run:\n"
                "    cmake --build <builddir> --target regen-protocols\n"
                "    git add platform/wayland-protocols && git commit\n"
                "then re-run cmake's configure step. Continuing configure "
                "for now so that target is available to run; the "
                "liteui_wayland_protocols library below will be skipped "
                "until the files exist.")
        else()
            message(FATAL_ERROR
                "liteui: vendored Wayland protocol files missing under "
                "${LITEUI_PROTO_DIR}: ${_missing_str}, and wayland-scanner + wayland-protocols "
                "aren't available to generate them. Install both "
                "(e.g. `sudo apt install libwayland-bin wayland-protocols`) "
                "and re-run cmake, or obtain the vendored files another way.")
        endif()
    endif()

    # Only build (and link) the protocol bindings once all four files exist.
    if(NOT LITEUI_PROTO_MISSING)
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
        util  #TODO :: used in codeeditor for terminal needs to move there
    )
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Tests are configured BEFORE examples so that Catch2 and enable_testing()
    # exist when an example registers its own tests. Examples are optional
    # about this: one without a tests/ folder simply registers none.
    option(LITEUI_BUILD_TESTS "Build the liteui unit tests (Catch2)" ON)
    if(LITEUI_BUILD_TESTS)
        include(CTest)   # also generates DartConfiguration.tcl for IDE/`ctest -T` runs
        add_subdirectory(tests)
    endif()

    option(LITEUI_BUILD_EXAMPLES "Build the liteui example programs" ON)
    if(LITEUI_BUILD_EXAMPLES)
        add_subdirectory(examples)
    endif()
endif()