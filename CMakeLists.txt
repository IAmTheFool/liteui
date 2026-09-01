cmake_minimum_required(VERSION 3.21)

project(LearnCpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_executable(main
        src/main.cpp
    )
    target_include_directories(main PRIVATE include)
    target_link_libraries(main PRIVATE user32 gdi32 d3d11 dxgi d2d1 dwrite)

elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
    pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(GLES REQUIRED IMPORTED_TARGET glesv2)
    pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET wayland-egl)
    pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)

    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner REQUIRED)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
    set(XDG_DECORATION_XML "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")

    # --- xdg-shell ---
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
        COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
                ${XDG_SHELL_XML} ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
        DEPENDS ${XDG_SHELL_XML}
    )
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
        COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
                ${XDG_SHELL_XML} ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
        DEPENDS ${XDG_SHELL_XML}
    )

    # --- xdg-decoration ---
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-protocol.c
        COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
                ${XDG_DECORATION_XML} ${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-protocol.c
        DEPENDS ${XDG_DECORATION_XML}
    )
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-client-protocol.h
        COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
                ${XDG_DECORATION_XML} ${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-client-protocol.h
        DEPENDS ${XDG_DECORATION_XML}
    )

    add_custom_target(xdg_shell_protocol
        DEPENDS
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-protocol.c
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-client-protocol.h
    )

    add_executable(main
        src/main.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
        ${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-protocol.c
    )
    target_include_directories(main PRIVATE include ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(main PRIVATE
        PkgConfig::WAYLAND
        PkgConfig::CAIRO
        PkgConfig::PANGOCAIRO
        PkgConfig::EGL
        PkgConfig::GLES
        PkgConfig::WAYLAND_EGL
    )
    add_dependencies(main xdg_shell_protocol)

endif()