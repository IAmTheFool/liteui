# hello — the smallest liteui example.
#
# Must be built as part of the liteui tree (examples/hello is added from
# examples/CMakeLists.txt).

if(NOT TARGET liteui::liteui)
    message(FATAL_ERROR "hello must be built as part of the liteui tree "
                        "(add_subdirectory from liteui's examples/).")
endif()

# Target names are global to the whole project, so the target is prefixed;
# OUTPUT_NAME keeps the executable called plain "hello".
add_executable(liteui_hello main.cpp)
set_target_properties(liteui_hello PROPERTIES OUTPUT_NAME hello)
target_link_libraries(liteui_hello PRIVATE liteui::liteui)
if(MSVC)
    target_compile_options(liteui_hello PRIVATE /utf-8)
endif()