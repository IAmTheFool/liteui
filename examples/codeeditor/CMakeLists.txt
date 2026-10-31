# codeeditor — a code editor with an embedded terminal, built on liteui.
#
# Must be built as part of the liteui tree (examples/codeeditor is added from
# examples/CMakeLists.txt).

if(NOT TARGET liteui::liteui)
    message(FATAL_ERROR "codeeditor must be built as part of the liteui tree "
                        "(add_subdirectory from liteui's examples/).")
endif()

# std::thread (terminal reader thread) needs pthreads on Linux.
find_package(Threads REQUIRED)

# Target names are global to the whole project, so the target is prefixed;
# OUTPUT_NAME keeps the executable called plain "codeeditor".
add_executable(liteui_codeeditor main.cpp)
set_target_properties(liteui_codeeditor PROPERTIES OUTPUT_NAME codeeditor)

# main.cpp is the single translation unit of this executable and does
# not define the macro itself, so define it here to compile the liteui
# implementation (Win32/D2D or Wayland/Cairo/EGL) into it.
target_compile_definitions(liteui_codeeditor PRIVATE LITEUI_IMPLEMENTATION)

target_link_libraries(liteui_codeeditor PRIVATE
    liteui::liteui
    Threads::Threads
)

# The Linux terminal uses forkpty() (<pty.h>), which lives in libutil.
# The root CMakeLists currently links `util` into liteui::liteui with a TODO
# to move it here. Once that is removed from the root, this line takes over;
# until then it's a harmless duplicate.
if(NOT WIN32)
    target_link_libraries(liteui_codeeditor PRIVATE util)
endif()

if(MSVC)
    # Source files are UTF-8 (the file also uses CRLF line endings, fine).
    target_compile_options(liteui_codeeditor PRIVATE /utf-8)
endif()