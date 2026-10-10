# One target per example source file. Add a new examples/whatever.cpp
# and it's picked up automatically on the next cmake configure — no
# edits needed here.
file(GLOB EXAMPLE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(src ${EXAMPLE_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE liteui::liteui)
endforeach()

# literoom lives in its own folder (main.cpp + sam_segmenter.hpp + models/)
# and has its own CMakeLists.txt with the optional ONNX Runtime setup.
add_subdirectory(literoom)

# If an example lives in its own subfolder instead of a flat
# examples/*.cpp (e.g. examples/counter/main.cpp), add it explicitly:
#
#   add_executable(counter counter/main.cpp)
#   target_link_libraries(counter PRIVATE liteui::liteui)
