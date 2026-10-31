# Each subdirectory with its own CMakeLists.txt is an example.
# Add examples/foo/{CMakeLists.txt,main.cpp} and it's picked up on the next configure.
#
# Every example gets a switch, LITEUI_EXAMPLE_<NAME> (ON by default), so the
# heavy ones can be skipped:  cmake -DLITEUI_EXAMPLE_LITEROOM=OFF ...
file(GLOB _children RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _children)   # same configure order on every platform

foreach(child ${_children})
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${child}
       AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${child}/CMakeLists.txt)
        string(TOUPPER "${child}" _name)
        string(MAKE_C_IDENTIFIER "${_name}" _name)
        option(LITEUI_EXAMPLE_${_name} "Build the ${child} example" ON)
        if(LITEUI_EXAMPLE_${_name})
            add_subdirectory(${child})
        endif()
    endif()
endforeach()