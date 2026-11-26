# examples/textviewer/CMakeLists.txt
# Register it from examples/CMakeLists.txt with:  add_subdirectory(textviewer)
# (mirror whatever imageviewer's CMakeLists does if it differs, e.g. WIN32 GUI flag)
add_executable(textviewer main.cpp)
target_link_libraries(textviewer PRIVATE liteui::liteui)
if(WIN32)
    set_target_properties(textviewer PROPERTIES WIN32_EXECUTABLE OFF)
endif()
