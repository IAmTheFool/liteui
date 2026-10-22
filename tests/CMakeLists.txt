# ------------------------------------------------------------------------
# liteui_tests — Catch2 unit tests for liteui.hpp
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
# ------------------------------------------------------------------------

include(FetchContent)

# 1) Prefer an installed Catch2 v3 (apt install catch2, vcpkg, conan, ...).
find_package(Catch2 3 QUIET)

# 2) Otherwise download a pinned release.
if(NOT Catch2_FOUND)
    message(STATUS "liteui tests: Catch2 not installed, fetching v3.8.1")
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.8.1
    )
    FetchContent_MakeAvailable(Catch2)
    # Provides catch_discover_tests() when Catch2 is built from source.
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

include(Catch)

add_executable(liteui_tests liteui_tests.cpp)
target_include_directories(liteui_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}     # test_helpers.hpp, cases/*.inc
)
if(MSVC)
    target_compile_options(liteui_tests PRIVATE /utf-8)  # sources are UTF-8
endif()

target_link_libraries(liteui_tests PRIVATE
    Catch2::Catch2WithMain
    liteui::liteui
)

# PRE_TEST = discover test names when `ctest` runs, not as a post-build step,
# so building never depends on being able to execute the binary.
catch_discover_tests(liteui_tests DISCOVERY_MODE PRE_TEST)