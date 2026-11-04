# imagebench — times the high-precision image pipeline (convertImage,
# makeDisplayImage, downscaleImage) on a 24-megapixel image. No window.
# Build in Release for meaningful numbers:  cmake --preset gcc-release
#
# Must be built as part of the liteui tree (examples/imagebench is added
# from examples/CMakeLists.txt).

if(NOT TARGET liteui::liteui)
    message(FATAL_ERROR "imagebench must be built as part of the liteui tree "
                        "(add_subdirectory from liteui's examples/).")
endif()

add_executable(liteui_imagebench main.cpp)
set_target_properties(liteui_imagebench PROPERTIES OUTPUT_NAME imagebench)
target_link_libraries(liteui_imagebench PRIVATE liteui::liteui)
if(MSVC)
    target_compile_options(liteui_imagebench PRIVATE /utf-8)
endif()
