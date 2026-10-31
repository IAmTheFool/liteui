# counter — example app for liteui.
#
# Must be built as part of the liteui tree (examples/counter is added from
# examples/CMakeLists.txt). Catch2/CTest setup and liteui_add_test() come from
# the root project.

if(NOT TARGET liteui::liteui)
    message(FATAL_ERROR "counter must be built as part of the liteui tree "
                        "(add_subdirectory from liteui's examples/).")
endif()

# Target names are global to the whole project, so the target is prefixed;
# OUTPUT_NAME keeps the executable called plain "counter".
add_executable(liteui_counter main.cpp counter_model.hpp)
set_target_properties(liteui_counter PROPERTIES OUTPUT_NAME counter)
target_link_libraries(liteui_counter PRIVATE liteui::liteui)
if(MSVC)
    target_compile_options(liteui_counter PRIVATE /utf-8)
endif()

# ---- Unit tests for the model (no UI needed) ----
# The tests link only Catch2, not liteui: counter_model.hpp stays free of UI
# and platform libraries, and this keeps it that way.
# Defaults to the root switch, so LITEUI_BUILD_TESTS=OFF turns these off too.
option(LITEUI_COUNTER_TESTS "counter: build unit tests" ${LITEUI_BUILD_TESTS})
if(LITEUI_BUILD_TESTS AND LITEUI_COUNTER_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    file(GLOB COUNTER_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    liteui_add_test(counter
        SOURCES  ${COUNTER_TEST_SOURCES}
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})
endif()