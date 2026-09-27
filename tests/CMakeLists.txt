# ---------------------------------------------------------------------
# Unit tests for liteui's parsing / math internals:
#   - liteui_xml   (the tolerant single-header XML parser)
#   - liteui_svg   (Mat2x3 + parseTransform, path-data parsing, color
#                   and style parsing, and full-document parseString)
#
# Uses Catch2 v3, fetched via FetchContent so a fresh checkout doesn't
# need Catch2 preinstalled. The test binary links against liteui::liteui
# so it picks up the same include dirs and platform libraries (Wayland/
# EGL/cairo/... or Win32/Direct2D) that liteui.hpp itself needs to
# compile, even though these tests only exercise its parsing code.
# ---------------------------------------------------------------------

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(liteui_tests
    test_xml.cpp
    test_matrix.cpp
    test_svg.cpp
)

target_link_libraries(liteui_tests PRIVATE liteui::liteui Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(liteui_tests)