# imageviewer — open an image and show it on a Canvas inside a scrollable
# viewport.
#
# Must be built as part of the liteui tree (examples/imageviewer is added
# from examples/CMakeLists.txt).

if(NOT TARGET liteui::liteui)
    message(FATAL_ERROR "imageviewer must be built as part of the liteui tree "
                        "(add_subdirectory from liteui's examples/).")
endif()

# Target names are global to the whole project, so the target is prefixed;
# OUTPUT_NAME keeps the executable called plain "imageviewer".
add_executable(liteui_imageviewer main.cpp)
set_target_properties(liteui_imageviewer PROPERTIES OUTPUT_NAME imageviewer)
target_link_libraries(liteui_imageviewer PRIVATE liteui::liteui)
if(MSVC)
    target_compile_options(liteui_imageviewer PRIVATE /utf-8)
endif()