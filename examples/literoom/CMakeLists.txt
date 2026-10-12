add_executable(literoom main.cpp sam_segmenter.hpp minijson.hpp)
target_link_libraries(literoom PRIVATE liteui::liteui)

if(MSVC)
    target_compile_options(literoom PRIVATE /utf-8)
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

# ---- Unit tests for the JSON helper behind the edit sidecar (no UI needed) ----
option(LITEROOM_TESTS "literoom: build unit tests" OFF)
if(LITEROOM_TESTS)
    add_executable(minijson_test tests/minijson_test.cpp)
    target_include_directories(minijson_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(minijson_test PRIVATE cxx_std_20)
    enable_testing()
    add_test(NAME minijson COMMAND minijson_test)
endif()