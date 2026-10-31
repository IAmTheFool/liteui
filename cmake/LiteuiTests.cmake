# LiteuiTests.cmake — shared test setup for liteui and its examples.
#
# Included from the top-level CMakeLists inside `if(LITEUI_BUILD_TESTS)`, AFTER
# Catch2 has been found / fetched and include(Catch) has run, and BEFORE
# add_subdirectory(tests) / add_subdirectory(examples).
#
# Provides:
#
#   liteui_prepare_sanitizers()
#       Macro. Called once by the root. Under MSVC + LITEUI_SANITIZE it removes
#       the Debug flags ASan refuses to build with (/RTC1, /ZI). Those flags are
#       per-directory variables, so calling it in the root covers every
#       subdirectory added afterwards.
#
#   liteui_enable_sanitizers(<target> <env_var_out> [NO_LEAK_CHECK])
#       Applies the sanitizer flags to <target> and stores the ASAN_OPTIONS
#       string for its tests in <env_var_out> (empty when sanitizers are off).
#       Used by liteui_add_test and by literoom's plain (non-Catch2) tests.
#
#   liteui_add_test(<name> SOURCES <files...>
#                   [INCLUDES <dirs...>] [LITEUI] [THREADS] [NO_SANITIZE]
#                   [LABEL <label>] [TIMEOUT <seconds>])
#       Catch2 test executable `liteui_<name>_test` with one CTest entry per
#       Catch2 test case, named `liteui_<name>.<case>`.
#         LITEUI       link liteui::liteui (the test needs the library)
#         THREADS      link the platform thread library
#         NO_SANITIZE  never sanitize this one (float-exact tests, ...)
#         LABEL        CTest label, so `ctest -L <label>` selects it
#         TIMEOUT      per-case timeout in seconds (default 60)
#
# Sanitizers (opt in with -DLITEUI_SANITIZE=ON, see the *-asan presets):
#   GCC / Clang (Linux, macOS, WSL)  ASan + UBSan
#   MSVC / clang-cl                  ASan only (needs VS 2019 16.9+)
#   MinGW                            unsupported, tests run unsanitized
#
# Target and CTest names are global to the whole project, hence the prefixes.

include_guard(GLOBAL)

find_package(Threads REQUIRED)

# MSVC's ASan refuses to build with the default Debug runtime checks (/RTC1)
# and edit-and-continue debug info (/ZI). The flag variables are per directory,
# so this is a macro (it runs in the caller's scope) and the root calls it once
# before adding any subdirectory.
macro(liteui_prepare_sanitizers)
    if(LITEUI_SANITIZE AND MSVC)
        foreach(_cfg DEBUG RELWITHDEBINFO)
            string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
            string(REPLACE "/ZI" "/Zi" CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
        endforeach()
    endif()
endmacro()

function(liteui_enable_sanitizers tgt env_out)
    cmake_parse_arguments(S "NO_LEAK_CHECK" "" "" ${ARGN})
    set(_env "")

    if(LITEUI_SANITIZE)
        if((CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang") AND NOT WIN32)
            target_compile_options(${tgt} PRIVATE
                -fsanitize=address,undefined -fno-sanitize-recover=undefined
                -fno-omit-frame-pointer -g)
            target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
            # System GL / Wayland libraries routinely "leak" at exit, so tests
            # that link the platform libraries skip LeakSanitizer.
            if(S_NO_LEAK_CHECK)
                set(_env "ASAN_OPTIONS=halt_on_error=1:detect_leaks=0")
            else()
                set(_env "ASAN_OPTIONS=halt_on_error=1:detect_leaks=1")
            endif()
        elseif(MSVC)
            target_compile_options(${tgt} PRIVATE /fsanitize=address)
            target_link_options(${tgt} PRIVATE /INCREMENTAL:NO)

            # MSVC's STL annotates std::vector / string / optional for ASan and
            # the linker rejects mixing annotated and unannotated objects
            # (LNK2038 'annotate_vector'). Catch2 is built without ASan, so
            # switch the annotations off so the test object matches it.
            # Use-after-free detection comes from ASan's allocator and is
            # unaffected; only container-overflow checks on reserved capacity
            # are lost. (Several spellings: the catch-all names cover
            # containers added in newer STLs.)
            target_compile_definitions(${tgt} PRIVATE
                _DISABLE_ASAN_ANNOTATIONS _DISABLE_STL_ANNOTATION
                _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)

            # The ASan runtime DLL lives next to cl.exe. Copy it beside the test
            # exe so it starts even when ctest is not run from a Developer
            # Command Prompt.
            get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
            file(GLOB _asan_dlls "${_cl_dir}/clang_rt.asan_dynamic-*.dll")
            foreach(_dll IN LISTS _asan_dlls)
                add_custom_command(TARGET ${tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_dll}" "$<TARGET_FILE_DIR:${tgt}>")
            endforeach()
            # No detect_leaks on Windows: LeakSanitizer is not available there.
            set(_env "ASAN_OPTIONS=halt_on_error=1")
        else()
            get_property(_warned GLOBAL PROPERTY LITEUI_SANITIZE_WARNED)
            if(NOT _warned)
                message(STATUS "liteui tests: sanitizers not enabled for this compiler/platform")
                set_property(GLOBAL PROPERTY LITEUI_SANITIZE_WARNED TRUE)
            endif()
        endif()
    endif()

    set(${env_out} "${_env}" PARENT_SCOPE)
endfunction()

function(liteui_add_test name)
    cmake_parse_arguments(A "LITEUI;THREADS;NO_SANITIZE" "TIMEOUT;LABEL" "SOURCES;INCLUDES" ${ARGN})
    if(NOT A_SOURCES)
        message(FATAL_ERROR "liteui_add_test(${name}): SOURCES is required")
    endif()
    if(NOT A_TIMEOUT)
        set(A_TIMEOUT 60)
    endif()

    set(tgt liteui_${name}_test)
    add_executable(${tgt} ${A_SOURCES})
    target_compile_features(${tgt} PRIVATE cxx_std_23)
    if(A_INCLUDES)
        target_include_directories(${tgt} PRIVATE ${A_INCLUDES})
    endif()
    target_link_libraries(${tgt} PRIVATE Catch2::Catch2WithMain)
    if(A_LITEUI)
        target_link_libraries(${tgt} PRIVATE liteui::liteui)
    endif()
    if(A_THREADS)
        target_link_libraries(${tgt} PRIVATE Threads::Threads)
    endif()
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /utf-8)  # sources are UTF-8
    endif()

    set(_env "")
    if(NOT A_NO_SANITIZE)
        if(A_LITEUI)
            liteui_enable_sanitizers(${tgt} _env NO_LEAK_CHECK)
        else()
            liteui_enable_sanitizers(${tgt} _env)
        endif()
    endif()

    set(_props TIMEOUT ${A_TIMEOUT})
    if(A_LABEL)
        list(APPEND _props LABELS ${A_LABEL})
    endif()
    if(_env)
        list(APPEND _props ENVIRONMENT "${_env}")
    endif()

    # PRE_TEST = discover test names when `ctest` runs, not as a post-build
    # step, so building never depends on being able to execute the binary.
    catch_discover_tests(${tgt}
        TEST_PREFIX "liteui_${name}."
        DISCOVERY_MODE PRE_TEST
        PROPERTIES ${_props})
endfunction()
