# ------------------------------------------------------------------------
# liteui_core_test — Catch2 unit tests for liteui.hpp
#
# Included from the top-level CMakeLists when LITEUI_BUILD_TESTS is ON, after
# Catch2 has been found / fetched (in the root) and cmake/LiteuiTests.cmake is
# loaded (it provides liteui_add_test and the sanitizer setup).
#
# liteui::liteui is a header-only INTERFACE library: the implementation
# (XML parser, SVG parser, layout engine, ...) is compiled inside whichever
# executable defines LITEUI_IMPLEMENTATION. This test executable has ONE
# translation unit (liteui_tests.cpp) that defines LITEUI_IMPLEMENTATION
# itself and then #includes every test case file, so linking liteui::liteui
# is safe (no duplicate symbols) and gives us everything the header needs:
#   - the include path for liteui.hpp
#   - cxx_std_23 and the optional LITEUI_PROFILE define
#   - platform libraries (Direct2D on Windows; wayland/cairo/pango/EGL/...
#     on Linux)
#   - liteui_wayland_protocols, which carries the include directory for
#     the vendored xdg-shell / xdg-decoration client headers
#
# CTest names: liteui_core.<Catch2 test case>
# ------------------------------------------------------------------------

# The .inc case files and the helper are listed only so they show up in the
# IDE project tree; the compiler finds them through #include and tracks them
# as dependencies either way.
file(GLOB LITEUI_TEST_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cases/*.inc)

liteui_add_test(core
    SOURCES  liteui_tests.cpp test_helpers.hpp ${LITEUI_TEST_CASES}
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}     # test_helpers.hpp, cases/*.inc
    LITEUI)