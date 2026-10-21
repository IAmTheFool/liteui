add_executable(literoom main.cpp sam_segmenter.hpp minijson.hpp exif.hpp detail_layer.hpp neighbor_fx.hpp
    pixel_math.hpp adjustments.hpp color_mix.hpp color_grade.hpp color_pass.hpp
    geometry.hpp curves.hpp raster_ops.hpp
    mask_geom.hpp mask_raster.hpp mask_brush.hpp mask_range.hpp mask_object.hpp
    mask.hpp seg_basic.hpp
    sidecar.hpp render_params.hpp adjustment_set.hpp render.hpp latest_worker.hpp thumb_worker.hpp
    file_utils.hpp image_meta.hpp image_codec.hpp histogram.hpp)
target_link_libraries(literoom PRIVATE liteui::liteui)

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
    find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h
        HINTS ${ONNXRUNTIME_DIR}/include ${ONNXRUNTIME_DIR}/include/onnxruntime)
    find_library(ORT_LIBRARY onnxruntime
        HINTS ${ONNXRUNTIME_DIR}/lib)
    if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIBRARY)
        message(FATAL_ERROR
            "LITEROOM_ONNX is ON but onnxruntime was not found. "
            "Set -DONNXRUNTIME_DIR=<unpacked onnxruntime folder>.")
    endif()

    target_include_directories(literoom PRIVATE ${ORT_INCLUDE_DIR})
    target_link_libraries(literoom PRIVATE ${ORT_LIBRARY})
    target_compile_definitions(literoom PRIVATE LITEROOM_ONNX)

    if(WIN32)
        # onnxruntime.dll must sit next to the exe.
        add_custom_command(TARGET literoom POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll"
                    $<TARGET_FILE_DIR:literoom>)
    else()
        # Find libonnxruntime.so when running from the build folder.
        set_target_properties(literoom PROPERTIES
            BUILD_RPATH "${ONNXRUNTIME_DIR}/lib")
    endif()
    message(STATUS "literoom: ONNX Runtime enabled (${ORT_LIBRARY})")
else()
    message(STATUS "literoom: built WITHOUT ONNX (basic object selection only)")
endif()

# ---- Models: copy models/ next to the executable ----
# The app looks in <exe folder>/models (or $LITEROOM_MODELS).
# Runs only when the exe is relinked, not on every build.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/models)
    add_custom_command(TARGET literoom POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/models
                $<TARGET_FILE_DIR:literoom>/models
        COMMENT "literoom: copying models/")
endif()

# ---- Unit tests for the header-only helpers (no UI needed) ----
option(LITEROOM_TESTS "literoom: build unit tests" OFF)
if(LITEROOM_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    foreach(t minijson exif
              pixel_math adjustments color_mix color_grade color_pass geometry curves
              mask_geom mask_raster mask_brush mask_range mask_object mask
              sidecar render_params adjustment_set file_utils image_meta sam_segmenter)
        add_executable(${t}_test tests/${t}_test.cpp)
        target_include_directories(${t}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(${t}_test PRIVATE cxx_std_20)
        add_test(NAME ${t} COMMAND ${t}_test)
    endforeach()

    # detail_layer and neighbor_fx start std::threads (shared-cache race tests),
    # so they link the platform thread library (a no-op on MSVC).
    foreach(t detail_layer neighbor_fx)
        add_executable(${t}_test tests/${t}_test.cpp)
        target_include_directories(${t}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(${t}_test PRIVATE cxx_std_20)
        target_link_libraries(${t}_test PRIVATE Threads::Threads)
        add_test(NAME ${t} COMMAND ${t}_test)
    endforeach()

    # latest_worker.hpp has no liteui dependency but runs a std::jthread, so its
    # test links the platform thread library (a no-op on MSVC). Run it in a loop
    # to shake out timing flakiness:  ctest -C Release -R latest_worker --repeat until-fail:50
    add_executable(latest_worker_test tests/latest_worker_test.cpp)
    target_include_directories(latest_worker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(latest_worker_test PRIVATE cxx_std_20)
    target_link_libraries(latest_worker_test PRIVATE Threads::Threads)
    add_test(NAME latest_worker COMMAND latest_worker_test)

    # thumb_worker.hpp decodes real files (liteui's decoder, the app's encoders in
    # the test) on a std::jthread, so it needs both liteui and the thread library.
    add_executable(thumb_worker_test tests/thumb_worker_test.cpp)
    target_include_directories(thumb_worker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(thumb_worker_test PRIVATE cxx_std_20)
    target_link_libraries(thumb_worker_test PRIVATE liteui::liteui Threads::Threads)
    if(MSVC)
        target_compile_options(thumb_worker_test PRIVATE /utf-8)
    endif()
    add_test(NAME thumb_worker COMMAND thumb_worker_test)

    # raster_ops.hpp, seg_basic.hpp, render.hpp, image_codec.hpp and histogram.hpp take liteui's CanvasImage, so
    # their tests link liteui like the app does.
    foreach(t raster_ops seg_basic render image_codec histogram)
        add_executable(${t}_test tests/${t}_test.cpp)
        target_include_directories(${t}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(${t}_test PRIVATE cxx_std_20)
        target_link_libraries(${t}_test PRIVATE liteui::liteui)
        if(MSVC)
            target_compile_options(${t}_test PRIVATE /utf-8)
        endif()
        add_test(NAME ${t} COMMAND ${t}_test)
    endforeach()

    # ---- Golden render test: the refactoring safety net ----
    # Compiles main.cpp itself (main() renamed) and fingerprints ~180 renders.
    # One golden file per configuration, because float results can differ
    # between compilers and flags:
    #   cmake --build . --config Release --target golden_record   (once, BEFORE refactoring)
    #   ctest -C Release -R golden_render                         (after every step)
    add_executable(golden_render_test tests/golden_render_test.cpp)
    target_include_directories(golden_render_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(golden_render_test PRIVATE cxx_std_20)
    target_link_libraries(golden_render_test PRIVATE liteui::liteui)
    if(MSVC)
        target_compile_options(golden_render_test PRIVATE /utf-8)
    endif()
    set(GOLDEN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/render_$<CONFIG>.txt)
    add_test(NAME golden_render COMMAND golden_render_test ${GOLDEN_FILE})
    add_custom_target(golden_record
        COMMAND golden_render_test --record ${GOLDEN_FILE}
        DEPENDS golden_render_test
        COMMENT "golden: recording ${GOLDEN_FILE}")
endif()