# examples/tooltip/CMakeLists.txt
# Register it from examples/CMakeLists.txt with:  add_subdirectory(tooltip)
# (mirror whatever imageviewer's CMakeLists does if it differs, e.g. WIN32 GUI flag)
add_executable(tooltip main.cpp)
target_link_libraries(tooltip PRIVATE liteui::liteui)
if(WIN32)
    set_target_properties(tooltip PROPERTIES WIN32_EXECUTABLE OFF)
endif()
