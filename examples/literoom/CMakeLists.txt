# literoom — example app for liteui.
#
# Must be built as part of the liteui tree (examples/literoom is added from
# examples/CMakeLists.txt). Catch2/CTest setup comes from the root project.

if(NOT TARGET liteui::liteui)
    message(FATAL_ERROR "literoom must be built as part of the liteui tree "
                        "(add_subdirectory from liteui's examples/).")
endif()

find_package(Threads REQUIRED)

# ---- Layout ----
# All sources live under src/. That folder is the include root, so any file can
# write  #include "mask/mask.hpp"  no matter which folder it sits in.
set(LITEROOM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(LITEROOM_HEADERS
    # core: the shared vocabulary (no dependencies on other folders)
    src/core/pixel_math.hpp
    src/core/geometry.hpp
    src/core/adjustments.hpp
    src/core/auto_tone.hpp
    src/core/raster_ops.hpp

    # color
    src/color/color_mix.hpp
    src/color/color_grade.hpp
    src/color/color_pass.hpp
    src/color/curves.hpp

    # detail effects (need neighbor pixels)
    src/detail/detail_layer.hpp
    src/detail/neighbor_fx.hpp

    # masks and object selection
    src/mask/mask.hpp
    src/mask/mask_geom.hpp
    src/mask/mask_range.hpp
    src/mask/mask_raster.hpp
    src/mask/mask_brush.hpp
    src/mask/mask_object.hpp
    src/mask/seg_basic.hpp
    src/mask/sam_segmenter.hpp

    # edit state
    src/edit/adjustment_set.hpp
    src/edit/render_params.hpp

    # rendering and analysis
    src/render/render.hpp
    src/render/histogram.hpp

    # saving and files
    src/io/sidecar.hpp
    src/io/minijson.hpp
    src/io/file_utils.hpp
    src/io/image_codec.hpp
    src/io/exif.hpp
    src/io/image_meta.hpp
    src/io/preset.hpp

    # background work
    src/workers/latest_worker.hpp
    src/workers/thumb_worker.hpp)

add_executable(literoom src/main.cpp ${LITEROOM_HEADERS})
target_include_directories(literoom PRIVATE ${LITEROOM_SRC})
# Threads: workers/*.hpp use std::jthread (needed explicitly on older glibc).
target_link_libraries(literoom PRIVATE liteui::liteui Threads::Threads)

# Show the same folder tree in Visual Studio's Solution Explorer.
source_group(TREE ${LITEROOM_SRC} FILES src/main.cpp ${LITEROOM_HEADERS})

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

    target_include_directories(literoom PRIVATE ${ORT_INCLUDE_DIR})
    target_link_libraries(literoom PRIVATE ${ORT_LIBRARY})
    target_compile_definitions(literoom PRIVATE LITEROOM_ONNX)

    if(WIN32)
        # onnxruntime.dll (and the providers DLL, if the release ships one)
        # must sit next to the exe.
        foreach(_dll onnxruntime.dll onnxruntime_providers_shared.dll)
            if(EXISTS "${ONNXRUNTIME_DIR}/lib/${_dll}")
                add_custom_command(TARGET literoom POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${ONNXRUNTIME_DIR}/lib/${_dll}"
                            $<TARGET_FILE_DIR:literoom>)
            endif()
        endforeach()
    else()
        # Find libonnxruntime.so when running from the build folder.
        set_target_properties(literoom PROPERTIES
            BUILD_RPATH "${ONNXRUNTIME_DIR}/lib")
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
# Catch2/CTest are NOT used here; enable_testing() comes from the root's
# include(CTest). Target names (literoom_<name>_test) and CTest names
# (literoom_<name>) are prefixed because both are global to the whole project.
option(LITEROOM_TESTS "literoom: build unit tests" ${LITEUI_BUILD_TESTS})
if(LITEROOM_TESTS)
    # Tests include headers by their src-relative path ("mask/mask.hpp") and the
    # shared harness ("check.hpp") from tests/.
    set(LITEROOM_TEST_INCLUDES ${LITEROOM_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # literoom_add_test(<name> [LITEUI] [THREADS] [ARGS <args...>])
    #   LITEUI  link liteui (test uses CanvasImage etc.)
    #   THREADS link the platform thread library (std::thread / std::jthread)
    #   ARGS    command-line arguments passed to the test executable
    function(literoom_add_test name)
        cmake_parse_arguments(A "LITEUI;THREADS" "" "ARGS" ${ARGN})
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
        add_test(NAME literoom_${name} COMMAND ${tgt} ${A_ARGS})
    endfunction()

    # Pure header tests: no liteui, no threads.
    foreach(t minijson exif
              pixel_math adjustments auto_tone color_mix color_grade color_pass geometry curves
              mask_geom mask_raster mask_brush mask_range mask_object mask
              sidecar render_params adjustment_set file_utils image_meta preset sam_segmenter)
        literoom_add_test(${t})
    endforeach()

    # detail_layer and neighbor_fx start std::threads (shared-cache race tests);
    # latest_worker runs a std::jthread. Loop it to shake out timing flakiness:
    #   ctest -C Release -R literoom_latest_worker --repeat until-fail:50
    foreach(t detail_layer neighbor_fx latest_worker)
        literoom_add_test(${t} THREADS)
    endforeach()

    # thumb_worker decodes real files (liteui's decoder, the app's encoders in
    # the test) on a std::jthread, so it needs both liteui and threads.
    literoom_add_test(thumb_worker LITEUI THREADS)

    # These take liteui's CanvasImage, so their tests link liteui like the app.
    foreach(t raster_ops seg_basic render image_codec histogram)
        literoom_add_test(${t} LITEUI)
    endforeach()

    # ---- Golden render test: the refactoring safety net ----
    # Compiles src/main.cpp itself (main() renamed) and fingerprints every render
    # scenario (the test prints the exact count when it runs).
    # One golden file per compiler + configuration, because float results differ
    # between compilers and flags:
    #   cmake --build . --config Release --target literoom_golden_record  (once, BEFORE refactoring)
    #   ctest -C Release -R literoom_golden_render                        (after every step)
    #
    # NOTE: if the golden file for this compiler/config doesn't exist yet, make
    # golden_render_test.cpp exit with code 77 so CTest reports "skipped"
    # instead of "failed" (SKIP_RETURN_CODE below).
    set(GOLDEN_FILE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/render_${CMAKE_CXX_COMPILER_ID}_$<CONFIG>.txt)
    literoom_add_test(golden_render LITEUI ARGS ${GOLDEN_FILE})
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