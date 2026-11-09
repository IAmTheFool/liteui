# literoom — example app for liteui.
#
# Must be built as part of the liteui tree (examples/literoom is added from
# examples/CMakeLists.txt). Catch2/CTest setup and the sanitizer helpers come
# from the root project (cmake/LiteuiTests.cmake).

if(NOT TARGET liteui::liteui)
    message(FATAL_ERROR "literoom must be built as part of the liteui tree "
                        "(add_subdirectory from liteui's examples/).")
endif()

find_package(Threads REQUIRED)

# ---- Layout ----
# All sources live under src/. That folder is the include root, so any file can
# write  #include "mask/mask.hpp"  no matter which folder it sits in.
set(LITEROOM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The headers are listed only so they show up in the IDE (and in the same folder
# tree); the compiler finds them through #include either way. CONFIGURE_DEPENDS
# re-globs on every build, so a new header appears without editing this file.
file(GLOB_RECURSE LITEROOM_HEADERS CONFIGURE_DEPENDS ${LITEROOM_SRC}/*.hpp)

add_executable(literoom ${LITEROOM_SRC}/main.cpp ${LITEROOM_HEADERS})
target_include_directories(literoom PRIVATE ${LITEROOM_SRC})
# Threads: workers/*.hpp use std::jthread (needed explicitly on older glibc).
target_link_libraries(literoom PRIVATE liteui::liteui Threads::Threads)

# Show the same folder tree in Visual Studio's Solution Explorer.
source_group(TREE ${LITEROOM_SRC} FILES ${LITEROOM_SRC}/main.cpp ${LITEROOM_HEADERS})

if(MSVC)
    target_compile_options(literoom PRIVATE /utf-8 /bigobj)
    # main() builds the whole UI in one large stack frame (src/main.cpp is ~12k
    # lines); until it is split into smaller build functions, raise the MSVC
    # default stack from 1 MB to 8 MB. The golden and lut_pane tests need the same.
    target_link_options(literoom PRIVATE /STACK:8388608)
endif()

# ---- Object selection (MobileSAM via ONNX Runtime), optional ----
option(LITEROOM_ONNX "literoom: object selection via ONNX Runtime" OFF)
set(ONNXRUNTIME_DIR "" CACHE PATH
    "Unpacked onnxruntime release (the folder containing include/ and lib/)")

if(LITEROOM_ONNX)
    # When a directory is given, search ONLY there so a system copy is never
    # picked up by accident. Results are cached: delete ORT_INCLUDE_DIR /
    # ORT_LIBRARY from the cache if you switch ONNXRUNTIME_DIR.
    set(_ort_search "")
    if(ONNXRUNTIME_DIR)
        set(_ort_search NO_DEFAULT_PATH)
    endif()

    find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h
        HINTS ${ONNXRUNTIME_DIR}
        PATH_SUFFIXES include include/onnxruntime
        ${_ort_search})
    find_library(ORT_LIBRARY onnxruntime
        HINTS ${ONNXRUNTIME_DIR}
        PATH_SUFFIXES lib
        ${_ort_search})
    if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIBRARY)
        message(FATAL_ERROR
            "LITEROOM_ONNX is ON but onnxruntime was not found. "
            "Set -DONNXRUNTIME_DIR=<unpacked onnxruntime folder>.")
    endif()

    # Where the runtime actually is, wherever it was found (not assumed to be
    # ${ONNXRUNTIME_DIR}/lib: a system copy lives elsewhere).
    get_filename_component(_ort_lib_dir "${ORT_LIBRARY}" DIRECTORY)

    # One imported target carries the include path, the library and the define,
    # so the app only has to link it. Only the app links it: the tests compile
    # without LITEROOM_ONNX, as before.
    add_library(literoom_onnxruntime UNKNOWN IMPORTED)
    set_target_properties(literoom_onnxruntime PROPERTIES
        IMPORTED_LOCATION             "${ORT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ORT_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS LITEROOM_ONNX)
    target_link_libraries(literoom PRIVATE literoom_onnxruntime)

    if(WIN32)
        # onnxruntime.dll (and the providers DLL, if the release ships one)
        # must sit next to the exe.
        foreach(_dll onnxruntime.dll onnxruntime_providers_shared.dll)
            if(EXISTS "${_ort_lib_dir}/${_dll}")
                add_custom_command(TARGET literoom POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_ort_lib_dir}/${_dll}"
                            $<TARGET_FILE_DIR:literoom>)
            endif()
        endforeach()
    else()
        # Find libonnxruntime.so when running from the build folder.
        set_target_properties(literoom PROPERTIES
            BUILD_RPATH "${_ort_lib_dir}")
    endif()
    message(STATUS "literoom: ONNX Runtime enabled (${ORT_LIBRARY})")

    # ---- Models: copy models/ next to the executable (ONNX builds only) ----
    # The app looks in <exe folder>/models (or $LITEROOM_MODELS).
    # Runs only when the exe is relinked, not on every build.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/models)
        add_custom_command(TARGET literoom POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/models
                    $<TARGET_FILE_DIR:literoom>/models
            COMMENT "literoom: copying models/")
    endif()
else()
    message(STATUS "literoom: built WITHOUT ONNX (basic object selection only)")
endif()

# ---- Unit tests (Catch2) ----
# One executable, literoom_test, built from tests/literoom_test.cpp. That single
# TU #includes every tests/cases/*.inc, like liteui's own tests/.
# To add a test: create tests/cases/<name>.inc and #include it from
# literoom_test.cpp. Nothing to change here.
# Defaults to the root switch, so LITEUI_BUILD_TESTS=OFF turns these off too.
# Catch2, enable_testing() and the sanitizer helpers come from the root
# (include(CTest), include(Catch), cmake/LiteuiTests.cmake).
# golden_render and lut_pane are separate executables (they compile src/main.cpp
# into their own TU and have their own main()); see the blocks at the end.
option(LITEROOM_TESTS "literoom: build unit tests" ${LITEUI_BUILD_TESTS})
if(LITEUI_BUILD_TESTS AND LITEROOM_TESTS)
    file(GLOB LITEROOM_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases/*.inc)

    add_executable(literoom_test tests/literoom_test.cpp ${LITEROOM_CASES})
    # Cases include headers by their src-relative path ("core/pixel_math.hpp")
    # and each other's files from tests/ ("cases/pixel_math.inc").
    target_include_directories(literoom_test PRIVATE ${LITEROOM_SRC}
                                                     ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_features(literoom_test PRIVATE cxx_std_23)
    # liteui::liteui: literoom_test.cpp defines LITEUI_IMPLEMENTATION once for all cases.
    # Threads: the worker cases use std::jthread / std::thread.
    target_link_libraries(literoom_test PRIVATE Catch2::Catch2WithMain liteui::liteui Threads::Threads)
    if(MSVC)
        target_compile_options(literoom_test PRIVATE /utf-8 /bigobj)
    endif()

    # -DLITEUI_SANITIZE=ON builds it with AddressSanitizer (see
    # cmake/LiteuiTests.cmake). NO_LEAK_CHECK because it links liteui.
    set(_catch_env "")
    liteui_enable_sanitizers(literoom_test _catch_env NO_LEAK_CHECK)

    # CTest names: literoom_<Catch2 test case name>, e.g.
    # "literoom_pixel_math: toU8Clamped".  ctest -R pixel_math  selects them.
    # ADD_TAGS_AS_LABELS turns every Catch2 tag into a CTest label, so the cases
    # tagged [threaded] run with:  ctest -C Release -L threaded --repeat until-fail:50
    # TIMEOUT is per test case (the threaded ones wait up to 10-20 s on purpose).
    set(_catch_props TIMEOUT 120)
    if(_catch_env)
        list(APPEND _catch_props ENVIRONMENT "${_catch_env}")
    endif()
    catch_discover_tests(literoom_test TEST_PREFIX "literoom_"
        ADD_TAGS_AS_LABELS
        PROPERTIES ${_catch_props})

    # ---- Golden render test: the refactoring safety net (not Catch2) ----
    # Its own executable: it compiles src/main.cpp itself (main() renamed) and has
    # its own main() with --record / --dump, so it can't join literoom_test's
    # shared TU. One golden file per compiler + configuration (float results
    # differ between compilers and flags):
    #   cmake --build . --config Release --target literoom_golden_record  (once, BEFORE refactoring)
    #   ctest -C Release -R literoom_golden_render                        (after every step)
    # Skipped (exit code 77) until a golden file for this compiler/config exists.
    # CI and scripts can leave it out with:  ctest -LE golden
    # Never sanitized: the fingerprints are float-exact.
    set(GOLDEN_FILE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/render_${CMAKE_CXX_COMPILER_ID}_$<CONFIG>.txt)

    add_executable(literoom_golden_render_test tests/golden_render_test.cpp)
    target_include_directories(literoom_golden_render_test PRIVATE ${LITEROOM_SRC})  # also finds main.cpp
    target_compile_features(literoom_golden_render_test PRIVATE cxx_std_23)
    target_link_libraries(literoom_golden_render_test PRIVATE liteui::liteui Threads::Threads)
    if(MSVC)
        target_compile_options(literoom_golden_render_test PRIVATE /utf-8 /bigobj)
        # Same large frame as the app's main() (see /STACK on literoom above).
        target_link_options(literoom_golden_render_test PRIVATE /STACK:8388608)
    endif()

    add_test(NAME literoom_golden_render COMMAND literoom_golden_render_test ${GOLDEN_FILE})
    set_tests_properties(literoom_golden_render PROPERTIES
        LABELS golden TIMEOUT 300 SKIP_RETURN_CODE 77)

    add_custom_target(literoom_golden_record
        COMMAND literoom_golden_render_test --record ${GOLDEN_FILE}
        DEPENDS literoom_golden_render_test
        COMMENT "golden: recording render_${CMAKE_CXX_COMPILER_ID}_<config>.txt")

    # ---- lut_pane: the LUTs pane's logic without a window (not Catch2) ----
    # Its own executable, like golden_render: it compiles src/main.cpp itself
    # (main() renamed) and has its own main() on the old check.hpp harness, so it
    # can't join literoom_test's shared TU. Runs a real LUT library in a temp
    # folder, the real preview worker and the real renderer on a std::jthread.
    #   ctest -C Release -R literoom_lut_pane
    #   ctest -C Release -L threaded --repeat until-fail:50      (timing flakiness)
    add_executable(literoom_lut_pane_test tests/lut_pane_test.cpp)
    target_include_directories(literoom_lut_pane_test PRIVATE ${LITEROOM_SRC}                    # also finds main.cpp
                                                              ${CMAKE_CURRENT_SOURCE_DIR}/tests) # check.hpp
    target_compile_features(literoom_lut_pane_test PRIVATE cxx_std_23)
    target_link_libraries(literoom_lut_pane_test PRIVATE liteui::liteui Threads::Threads)
    if(MSVC)
        target_compile_options(literoom_lut_pane_test PRIVATE /utf-8 /bigobj)
    endif()

    set(_lut_pane_env "")
    liteui_enable_sanitizers(literoom_lut_pane_test _lut_pane_env NO_LEAK_CHECK)

    add_test(NAME literoom_lut_pane COMMAND literoom_lut_pane_test)
    set_tests_properties(literoom_lut_pane PROPERTIES LABELS threaded TIMEOUT 120)
    if(_lut_pane_env)
        set_tests_properties(literoom_lut_pane PROPERTIES ENVIRONMENT "${_lut_pane_env}")
    endif()
endif()