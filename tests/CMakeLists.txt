# ------------------------------------------------------------------------
# liteui_tests — Catch2 unit tests for liteui.hpp
#
# WHY THIS DOESN'T LINK liteui::liteui
# The XML parser, SVG parser and layout engine are all `inline` code that
# lives inside the LITEUI_IMPLEMENTATION section of the header. The static
# library liteui::liteui compiles that section in its own translation unit,
# so a test file can't see those internals from the outside. Instead, this
# executable has ONE translation unit (liteui_tests.cpp) that defines
# LITEUI_IMPLEMENTATION itself and then #includes every test case file.
# Linking liteui::liteui as well would compile the same non-inline
# definitions twice (duplicate-symbol errors), so we deliberately don't.
#
# It still needs the same platform libraries as the library (Direct2D on
# Windows; wayland/cairo/pango/EGL/... on Linux), so we borrow them from
# liteui's own private link list rather than repeating them here.
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
target_compile_features(liteui_tests PRIVATE cxx_std_23)
target_include_directories(liteui_tests PRIVATE
    ${PROJECT_SOURCE_DIR}           # liteui.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}     # test_helpers.hpp, cases/*.inc
)
if(MSVC)
    target_compile_options(liteui_tests PRIVATE /utf-8)  # sources are UTF-8
endif()

target_link_libraries(liteui_tests PRIVATE
    Catch2::Catch2WithMain
    $<TARGET_PROPERTY:liteui,LINK_LIBRARIES>
)

# PRE_TEST = discover test names when `ctest` runs, not as a post-build step,
# so building never depends on being able to execute the binary.
catch_discover_tests(liteui_tests DISCOVERY_MODE PRE_TEST)
