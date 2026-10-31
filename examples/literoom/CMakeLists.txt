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
    target_compile_options(literoom PRIVATE /utf-8)
    # Need to split main in small build functions this is temporary but works
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

# ---- Unit tests for the header-only helpers (no UI needed) ----
# Defaults to the root switch, so LITEUI_BUILD_TESTS=OFF turns these off too.
# These use the app's own check.hpp harness, not Catch2; enable_testing() comes
# from the root's include(CTest). Target names (literoom_<name>_test) and CTest
# names (literoom_<name>) are prefixed because both are global to the whole
# project.
option(LITEROOM_TESTS "literoom: build unit tests" ${LITEUI_BUILD_TESTS})
if(LITEUI_BUILD_TESTS AND LITEROOM_TESTS)
    # Tests include headers by their src-relative path ("mask/mask.hpp") and the
    # shared harness ("check.hpp") from tests/.
    set(LITEROOM_TEST_INCLUDES ${LITEROOM_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # literoom_add_test(<name> [LITEUI] [THREADS] [NO_SANITIZE]
    #                   [LABEL <label>] [TIMEOUT <seconds>] [ARGS <args...>])
    #   LITEUI       link liteui (test uses CanvasImage etc.)
    #   THREADS      link the platform thread library (std::thread / std::jthread)
    #   NO_SANITIZE  never sanitize (the golden test's float fingerprints)
    #   LABEL        CTest label: `ctest -L <label>` selects it
    #   TIMEOUT      seconds before CTest kills it (default 60)
    #   ARGS         command-line arguments passed to the test executable
    # With -DLITEUI_SANITIZE=ON every test except NO_SANITIZE ones is built with
    # AddressSanitizer (see cmake/LiteuiTests.cmake).
    function(literoom_add_test name)
        cmake_parse_arguments(A "LITEUI;THREADS;NO_SANITIZE" "LABEL;TIMEOUT" "ARGS" ${ARGN})
        if(NOT A_TIMEOUT)
            set(A_TIMEOUT 60)
        endif()
        set(tgt literoom_${name}_test)
        add_executable(${tgt} tests/${name}_test.cpp)
        target_include_directories(${tgt} PRIVATE ${LITEROOM_TEST_INCLUDES})
        target_compile_features(${tgt} PRIVATE cxx_std_23)
        if(A_LITEUI)
            target_link_libraries(${tgt} PRIVATE liteui::liteui)
        endif()
        if(A_THREADS)
            target_link_libraries(${tgt} PRIVATE Threads::Threads)
        endif()
        if(MSVC)
            target_compile_options(${tgt} PRIVATE /utf-8)
        endif()

        set(_env "")
        if(NOT A_NO_SANITIZE)
            if(A_LITEUI)
                liteui_enable_sanitizers(${tgt} _env NO_LEAK_CHECK)
            else()
                liteui_enable_sanitizers(${tgt} _env)
            endif()
        endif()

        add_test(NAME literoom_${name} COMMAND ${tgt} ${A_ARGS})
        set_tests_properties(literoom_${name} PROPERTIES TIMEOUT ${A_TIMEOUT})
        if(A_LABEL)
            set_tests_properties(literoom_${name} PROPERTIES LABELS ${A_LABEL})
        endif()
        if(_env)
            set_tests_properties(literoom_${name} PROPERTIES ENVIRONMENT "${_env}")
        endif()
    endfunction()

    # Pure header tests: no liteui, no threads.
    foreach(t minijson exif
              pixel_math adjustments auto_tone color_mix color_grade color_pass geometry curves film_fx
              mask_geom mask_raster mask_brush mask_range mask_object mask_rect mask_polygon mask
              sidecar photo_meta render_params adjustment_set file_utils virtual_copy image_meta preset sam_segmenter
              lut3d lut_cube)
        literoom_add_test(${t})
    endforeach()

    # Tests that start threads carry the "threaded" label, so loop them to
    # shake out timing flakiness without naming each one:
    #   ctest -C Release -L threaded --repeat until-fail:50
    #
    # detail_layer and neighbor_fx start std::threads (shared-cache race tests);
    # latest_worker runs a std::jthread. lut_library starts std::threads (several
    # threads resolving one LUT at once).
    foreach(t detail_layer neighbor_fx latest_worker lut_library)
        literoom_add_test(${t} THREADS LABEL threaded TIMEOUT 120)
    endforeach()

    # thumb_worker decodes real files (liteui's decoder, the app's encoders in
    # the test) on a std::jthread, so it needs both liteui and threads.
    literoom_add_test(thumb_worker LITEUI THREADS LABEL threaded TIMEOUT 120)

    # export_worker renders and encodes real files on a std::jthread (the whole
    # batch-export pipeline), so like thumb_worker it needs liteui and threads.
    literoom_add_test(export_worker LITEUI THREADS LABEL threaded TIMEOUT 120)

    # lut_preview_worker renders LUT previews on a std::jthread (the LUTs pane's
    # list), so like thumb_worker it needs liteui (CanvasImage) and threads.
    literoom_add_test(lut_preview_worker LITEUI THREADS LABEL threaded TIMEOUT 120)

    # lut_pane compiles src/main.cpp into the test (like golden_render, below)
    # and drives the LUTs pane's logic without a window: refresh, previews,
    # apply / remove, import, status texts.
    literoom_add_test(lut_pane LITEUI THREADS LABEL threaded TIMEOUT 120)

    # These take liteui's CanvasImage, so their tests link liteui like the app.
    foreach(t raster_ops seg_basic render image_codec histogram scopes lut_render)
        literoom_add_test(${t} LITEUI)
    endforeach()

    # ---- Golden render test: the refactoring safety net ----
    # Compiles src/main.cpp itself (main() renamed) and fingerprints every render
    # scenario (the test prints the exact count when it runs).
    # One golden file per compiler + configuration, because float results differ
    # between compilers and flags:
    #   cmake --build . --config Release --target literoom_golden_record  (once, BEFORE refactoring)
    #   ctest -C Release -R literoom_golden_render                        (after every step)
    # CI and scripts can leave it out with:  ctest -LE golden
    #
    # NOTE: if the golden file for this compiler/config doesn't exist yet, make
    # golden_render_test.cpp exit with code 77 so CTest reports "skipped"
    # instead of "failed" (SKIP_RETURN_CODE below).
    #
    # NO_SANITIZE: the fingerprints are float-exact, and sanitizer builds change
    # code generation and stack use, so this one always builds plain.
    set(GOLDEN_FILE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/render_${CMAKE_CXX_COMPILER_ID}_$<CONFIG>.txt)
    literoom_add_test(golden_render LITEUI NO_SANITIZE LABEL golden TIMEOUT 300 ARGS ${GOLDEN_FILE})
    set_tests_properties(literoom_golden_render PROPERTIES SKIP_RETURN_CODE 77)
    if(MSVC)
        # Same large frame as the app's main() (see /STACK on literoom above).
        target_link_options(literoom_golden_render_test PRIVATE /STACK:8388608)
    endif()
    add_custom_target(literoom_golden_record
        COMMAND literoom_golden_render_test --record ${GOLDEN_FILE}
        DEPENDS literoom_golden_render_test
        COMMENT "golden: recording ${GOLDEN_FILE}")
endif()