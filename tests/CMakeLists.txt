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


# # ---- photo editor tests ---------------------------------------------
# # Same source file as the literoom example, built a second time with
# # LITEROOM_TESTS so main() is replaced by Catch2's main.
# set(LITEROOM_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/literoom.cpp)

# if(EXISTS ${LITEROOM_SOURCE})
#     add_executable(literoom_tests ${LITEROOM_SOURCE})
#     target_compile_definitions(literoom_tests PRIVATE LITEROOM_TESTS)
#     target_link_libraries(literoom_tests PRIVATE liteui::liteui Catch2::Catch2WithMain)
#     catch_discover_tests(literoom_tests)
# else()
#     message(WARNING "tests: ${LITEROOM_SOURCE} not found, "
#                     "skipping literoom_tests")
# endif()