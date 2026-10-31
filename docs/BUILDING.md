# Building LiteUI

This guide covers building the LiteUI examples and tests from a checkout, and building *your own* program against `liteui.hpp`, on Windows (MSVC) and Linux (GCC, Wayland).

- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Windows setup](#windows-setup)
- [Linux setup](#linux-setup)
- [Presets](#presets)
- [Configuring without presets](#configuring-without-presets)
- [CMake options](#cmake-options)
- [Where things end up](#where-things-end-up)
- [Examples](#examples)
- [Tests](#tests)
- [Profiling build](#profiling-build)
- [Using LiteUI in your own project](#using-liteui-in-your-own-project)
- [Wayland protocol bindings](#wayland-protocol-bindings)
- [Troubleshooting](#troubleshooting)

## Requirements

| | Windows | Linux |
|---|---|---|
| Compiler | MSVC with C++23 support (`cl.exe`) | GCC with C++23 support (`g++`) |
| CMake | 3.21 or newer | 3.21 or newer |
| Generator | Ninja | Ninja |
| Display | Windows desktop | A **Wayland** session |
| Libraries | Windows SDK (`user32`, `gdi32`, `d2d1`, `dwrite`, `windowscodecs`, `ole32`) | See [Linux setup](#linux-setup) |

CMake 3.21 is the minimum because the presets file uses preset schema version 3. The `liteui::liteui` target sets `cxx_std_23` for you.

Network access is needed on the first configure *only if* Catch2 v3 is not installed, because the tests then fetch v3.8.1 (see [Tests](#tests)).

## Quick start

From the repository root:

```sh
cmake --preset msvc-release          # Linux: gcc-release
cmake --build --preset msvc-release
ctest --preset msvc-release
```

Then run an example:

```sh
build/release/examples/hello/hello          # hello.exe on Windows
```

That is all most people need. The rest of this document explains each step and what to do when something goes wrong.

## Windows setup

1. Install **Visual Studio** (or the Build Tools) with the *Desktop development with C++* workload. This provides `cl.exe`, the Windows SDK, CMake and Ninja.
2. Open an **x64 Native Tools Command Prompt for VS** (or run `vcvars64.bat` in your shell). The MSVC presets pin the compiler to `cl.exe` and set the architecture/toolset *strategy* to `external`, which means CMake does not select the toolchain for you. The environment you launch from decides it. If `cl.exe` is not on `PATH`, configuration fails.
3. Run the commands from [Quick start](#quick-start) with the `msvc-*` presets.

Pass `/utf-8` to MSVC so source files are read as UTF-8 rather than the local code page. The examples do this themselves, and you should in your own targets:

```cmake
if(MSVC)
    target_compile_options(app PRIVATE /utf-8)
endif()
```

The sanitizer preset (`msvc-debug-asan`) needs the AddressSanitizer component of Visual Studio installed.

## Linux setup

LiteUI renders through Wayland, EGL/GLES2, Cairo and Pango, so you need the development packages for each of those. `CMakeLists.txt` finds them through `pkg-config`, one module each:

| pkg-config module | Used for |
|---|---|
| `wayland-client` | Core Wayland protocol |
| `wayland-cursor` | Resize/arrow cursors |
| `wayland-egl` | EGL window surface |
| `egl` | EGL context |
| `glesv2` | GLES2 rendering |
| `cairo` | Canvas surfaces |
| `pangocairo` | Text shaping and layout |
| `xkbcommon` | Keyboard layout and keysyms |
| `libpng` | PNG decoding |
| `libjpeg` | JPEG decoding |

On Debian/Ubuntu (package names vary by distribution):

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
    libwayland-dev libxkbcommon-dev libcairo2-dev libpango1.0-dev \
    libegl-dev libgles-dev libpng-dev libjpeg-dev
```

You do **not** need `wayland-scanner` or the `wayland-protocols` package to build. The generated `xdg-shell` and `xdg-decoration` bindings are vendored in `platform/wayland-protocols/`. See [Wayland protocol bindings](#wayland-protocol-bindings) for when you would need them.

Linux builds also enable the C language (for those vendored `.c` files) and link `libutil`. Windows builds never compile any C.

### Optional runtime tools

These are not needed to build, only for some features at run time:

| Feature | Tool | If missing |
|---|---|---|
| File and folder pickers | `zenity`, then `kdialog` | Pickers return `std::nullopt` or an empty vector |
| System clipboard | `wl-copy` + `wl-paste` (needs `WAYLAND_DISPLAY`), or `xclip` / `xsel` (needs `DISPLAY`) | A same-process buffer is used instead |

## Presets

`CMakePresets.json` defines configure, build and test presets. All of them use the **Ninja** generator (single-config, so the build type is fixed per preset).

| Preset | Toolchain | Build type | Extra |
|---|---|---|---|
| `msvc-debug` | MSVC | Debug | |
| `msvc-release` | MSVC | Release | |
| `msvc-debug-profile` | MSVC | Debug | `LITEUI_PROFILE=ON` |
| `msvc-release-profile` | MSVC | Release | `LITEUI_PROFILE=ON` |
| `msvc-debug-asan` | MSVC | Debug | `LITEUI_SANITIZE=ON` |
| `gcc-debug` | GCC | Debug | |
| `gcc-release` | GCC | Release | |
| `gcc-debug-profile` | GCC | Debug | `LITEUI_PROFILE=ON` |
| `gcc-release-profile` | GCC | Release | `LITEUI_PROFILE=ON` |
| `gcc-debug-asan` | GCC | Debug | `LITEUI_SANITIZE=ON` (ASan + UBSan) |

Some details worth knowing:

- **Host conditions.** The `msvc-*` presets only appear on a Windows host and the `gcc-*` presets only on a Linux host, so `cmake --list-presets` shows just the ones that apply to you.
- **Build directories.** Each preset builds in `build/<variant>/` (`build/debug`, `build/release`, `build/debug-asan`, `build/release-profile`, ...). The MSVC and GCC presets of the same variant share a directory name. That is harmless when each checkout lives on one OS, but if you share a checkout between Windows and Linux (a network drive, WSL over `/mnt/c`), configure into different directories or clean between switches.
- **Compilers.** The GCC presets set `CMAKE_C_COMPILER=gcc` and `CMAKE_CXX_COMPILER=g++`. To use Clang, configure manually.
- **Test presets** exist for `msvc-debug`, `msvc-release`, `msvc-debug-asan`, `gcc-debug`, `gcc-release` and `gcc-debug-asan`. They print output on failure, apply a 120 second default timeout (tests registered with `liteui_add_test` carry their own 60 second timeout, which takes precedence), and report an error if no tests are found.

Most CMake-aware IDEs read the presets file directly: Visual Studio ("Open Folder"), VS Code with the CMake Tools extension, and CLion.

Useful commands:

```sh
cmake --list-presets                        # configure presets for this host
cmake --build --preset gcc-release          # build everything
cmake --build --preset gcc-release --target liteui_hello   # build one target
ctest --preset gcc-release                  # run every test
ctest --preset gcc-release -R layout        # run tests whose name matches "layout"
```

Target names are prefixed (the hello example's target is `liteui_hello`) because CMake target names are global to the whole project. The resulting executable keeps its plain name.

## Configuring without presets

Ninja is a single-config generator, so you must pass the build type yourself:

```sh
cmake -S . -B build/mybuild -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/mybuild
```

On Windows, run this from a developer prompt so `cl.exe` and Ninja are found. Any option from the next section can be added with `-D`.

## CMake options

These are defined at the top level of the repository.

| Option | Default | Effect |
|---|---|---|
| `LITEUI_BUILD_TESTS` | `ON` | Find or fetch Catch2 and build the unit tests, including any `tests/` folder inside an example |
| `LITEUI_BUILD_EXAMPLES` | `ON` | Build the programs under `examples/` |
| `LITEUI_SANITIZE` | `OFF` | Build the tests with AddressSanitizer (plus UBSan on GCC/Clang) |
| `LITEUI_PROFILE` | `OFF` | Add `-DLITEUI_PROFILE` to everything that links `liteui::liteui` (see [Profiling build](#profiling-build)) |
| `LITEUI_EXAMPLE_<NAME>` | `ON` | One switch per example directory (see [Examples](#examples)) |

`LITEUI_BUILD_TESTS`, `LITEUI_BUILD_EXAMPLES` and `LITEUI_SANITIZE` only exist when LiteUI is the **top-level** project. When another project pulls LiteUI in with `add_subdirectory` or `FetchContent`, it never downloads Catch2 and never builds tests or examples. `LITEUI_PROFILE` is available in both situations.

## Where things end up

With a preset, everything lands under `build/<variant>/`. Examples keep their directory structure:

```
build/release/examples/hello/hello          (hello.exe on Windows)
build/release/examples/counter/...
```

Ninja puts executables directly in the build tree (no `Debug/` or `Release/` subfolder) because each preset is single-config.

## Examples

Every subdirectory of `examples/` that has its own `CMakeLists.txt` is added automatically the next time CMake configures. Adding `examples/foo/CMakeLists.txt` and `examples/foo/main.cpp` is enough, with no list to edit. Each one gets an option named after its directory (uppercased, and converted to a valid C identifier):

| Directory | Option |
|---|---|
| `examples/hello` | `LITEUI_EXAMPLE_HELLO` |
| `examples/counter` | `LITEUI_EXAMPLE_COUNTER` |
| `examples/literoom` | `LITEUI_EXAMPLE_LITEROOM` |

Turn the heavy ones off when you do not need them:

```sh
cmake --preset gcc-release -DLITEUI_EXAMPLE_LITEROOM=OFF
```

An example's `CMakeLists.txt` only works inside the LiteUI tree. The hello example checks `if(NOT TARGET liteui::liteui)` and stops with an explanatory error otherwise.

The single-file demos in `examples/others/` have no `CMakeLists.txt` and are **not** part of this build. To try one, compile it like your own program (see [Using LiteUI in your own project](#using-liteui-in-your-own-project)) or give it a small `CMakeLists.txt` modelled on the hello example's.

## Tests

LiteUI uses Catch2 v3.

**How Catch2 is found.** The root `CMakeLists.txt` first looks for an installed Catch2 v3 (`apt install catch2`, vcpkg, Conan, ...). If none is found, it fetches the pinned release v3.8.1 with `FetchContent`, which needs network access and `git`. Catch2 is located once, in the root, so the core tests and every example's tests see the same `Catch2::Catch2WithMain`.

**What the suites are.** The core suite (`liteui_core`) is one translation unit, `tests/liteui_tests.cpp`. It defines `LITEUI_IMPLEMENTATION` itself and then includes each case file from `tests/cases/*.inc`: core types, layout, platform, XML, and SVG paths, primitives and documents. New `.inc` files are picked up on the next configure. Examples with a `tests/` folder, such as `counter`, register their own tests through the same helper, `liteui_add_test()`.

**Running them.**

```sh
ctest --preset gcc-release
ctest --preset gcc-release -R liteui_core      # only the core suite
ctest --preset gcc-release --output-on-failure # (already on in the test presets)
```

Test names look like `liteui_core.<Catch2 test case name>`.

**Sanitizers.** Use the `*-asan` presets (or `-DLITEUI_SANITIZE=ON`) to build the tests with AddressSanitizer, plus UBSan on GCC and Clang.

For the helpers, tags, sanitizer details and how to write a new test, see [TESTING.md](TESTING.md).

**Turning tests off** (faster configure, no Catch2):

```sh
cmake --preset gcc-release -DLITEUI_BUILD_TESTS=OFF
```

## Profiling build

Configure with `LITEUI_PROFILE=ON`, either with a `*-profile` preset or by hand:

```sh
cmake --preset gcc-release-profile
cmake --build --preset gcc-release-profile
```

Programs built this way print a short report to **stderr** about once per second, giving the average and worst-case time (in milliseconds) and the run count of each stage: mouse handling, painting, canvas drawing, canvas blits and presenting. With the option off, the profiler expands to nothing.

> **Windows only for now.** The timing probes exist only in the Direct2D backend. A Linux build with `LITEUI_PROFILE` compiles and runs normally but prints nothing. The stage list, how to read the output and the known gaps are in [PROFILING.md](PROFILING.md).

Without CMake, define `LITEUI_PROFILE` for the translation unit that defines `LITEUI_IMPLEMENTATION`.

## Using LiteUI in your own project

### Rule 1: one implementation per program

`liteui.hpp` is cheap to include everywhere. The implementation is compiled only where you define `LITEUI_IMPLEMENTATION` first, and that must happen in **exactly one** `.cpp` file of each executable:

```cpp
// ui.cpp
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"
```

### With CMake

```cmake
add_subdirectory(path/to/liteui)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE liteui::liteui)
if(MSVC)
    target_compile_options(app PRIVATE /utf-8)
endif()
```

Or with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(liteui
    GIT_REPOSITORY <your liteui repository URL>
    GIT_TAG        main)
FetchContent_MakeAvailable(liteui)
```

`liteui::liteui` is a header-only `INTERFACE` library. Because the implementation is compiled inside your executable, everything it needs propagates to whatever links it: the include path, `cxx_std_23`, the optional `LITEUI_PROFILE` define, the platform libraries, and on Linux the vendored `liteui_wayland_protocols` static library that carries the include directory for the generated protocol headers.

To enable the profiler for a subproject build, set the option before adding LiteUI:

```cmake
set(LITEUI_PROFILE ON CACHE BOOL "" FORCE)
add_subdirectory(path/to/liteui)
```

### Without CMake

These commands are a starting point; adjust paths to your layout.

**Windows (MSVC, from a developer prompt):**

```bat
cl /std:c++latest /EHsc /utf-8 /I path\to\liteui main.cpp ^
   user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib
```

**Linux (GCC):** the two generated protocol files are C, so compile them with a C compiler and link the objects in:

```sh
P=path/to/liteui/platform/wayland-protocols
gcc -c $P/xdg-shell-protocol.c      -o xdg-shell-protocol.o
gcc -c $P/xdg-decoration-protocol.c -o xdg-decoration-protocol.o

g++ -std=c++23 -I path/to/liteui -I $P main.cpp \
    xdg-shell-protocol.o xdg-decoration-protocol.o \
    $(pkg-config --cflags --libs wayland-client wayland-cursor wayland-egl \
        cairo pangocairo egl glesv2 xkbcommon libpng libjpeg) \
    -o app
```

The `-I $P` is required: the implementation includes `xdg-shell-client-protocol.h` and `xdg-decoration-client-protocol.h` by bare name.

## Wayland protocol bindings

This variant of LiteUI **vendors** the generated bindings, so a consumer building on Linux needs only the runtime libraries above. The expected files are:

```
platform/wayland-protocols/xdg-shell-protocol.c
platform/wayland-protocols/xdg-shell-client-protocol.h
platform/wayland-protocols/xdg-decoration-protocol.c
platform/wayland-protocols/xdg-decoration-client-protocol.h
```

The trade-off is that the vendored files are frozen at the `wayland-protocols` version that generated them. A maintainer picks up upstream changes by regenerating and committing them.

### Regenerating

The `regen-protocols` target exists whenever `wayland-scanner` and the `wayland-protocols` package are both available. It creates the directory if needed and regenerates all four files from the installed protocol XML (`stable/xdg-shell/xdg-shell.xml` and `unstable/xdg-decoration/xdg-decoration-unstable-v1.xml`):

```sh
sudo apt install libwayland-bin wayland-protocols
cmake --build build/release --target regen-protocols
git add platform/wayland-protocols && git commit
```

Then re-run the configure step.

### What happens when files are missing

All four files must exist. If any is missing, configuration behaves like this:

| Situation | Result |
|---|---|
| `wayland-scanner` and `wayland-protocols` are available | A **warning**. Configure continues so the `regen-protocols` target exists for you to run. The `liteui_wayland_protocols` library is skipped until the files exist, so the build will not link or compile until you regenerate and re-configure. |
| They are not available | A **fatal error** that tells you which files are missing and which packages to install. |

Windows never needs any of this.

## Troubleshooting

**`cl.exe` not found / CMake cannot determine the compiler (Windows).**
You are not in a developer prompt. Open an *x64 Native Tools Command Prompt for VS*, or run `vcvars64.bat`, then configure again. The MSVC presets deliberately leave toolchain selection to the environment.

**`cmake --list-presets` does not show the preset I want.**
Presets are conditioned on the host OS: `msvc-*` on Windows only, `gcc-*` on Linux only.

**`pkg_check_modules` fails with "No package '...' found" (Linux).**
Install the development package for the named module, using the table in [Linux setup](#linux-setup). Make sure `pkg-config` itself is installed.

**Fatal error about missing vendored Wayland protocol files.**
See [What happens when files are missing](#what-happens-when-files-are-missing). Install `libwayland-bin` and `wayland-protocols`, run `regen-protocols`, commit the result, and re-run CMake.

**`xdg-shell-client-protocol.h: No such file or directory`.**
Your target is not linking `liteui::liteui`, or you are compiling by hand without the protocol directory on the include path. Link the CMake target, or add `-I platform/wayland-protocols`.

**Duplicate symbol / multiple definition errors at link time.**
`LITEUI_IMPLEMENTATION` is defined in more than one `.cpp` of the same executable. Keep it in exactly one.

**Undefined references to `LiteUI::...` or `View::...`.**
The opposite problem: no translation unit in the executable defines `LITEUI_IMPLEMENTATION` before including the header.

**The program throws `std::runtime_error` at startup on Linux.**
The backend reports initialization failures with exceptions. "Failed to connect to Wayland display" means you are not in a Wayland session (or `WAYLAND_DISPLAY` is not set, for example over SSH or in a container). "Missing required Wayland globals" means the compositor did not offer an interface LiteUI needs. An EGL error means EGL/GLES2 initialization failed, which usually points to missing GPU drivers or Mesa libraries.

**Garbled non-ASCII text in source on MSVC.**
Add `/utf-8` to your compile options.

**Catch2 download fails at configure.**
Install Catch2 v3 from your package manager or vcpkg so the root `CMakeLists.txt` finds it, or configure with `-DLITEUI_BUILD_TESTS=OFF`.

**File dialogs never appear (Linux).**
Install `zenity` or `kdialog`. Without either, the pickers return an empty result.

**Copy/paste does not reach other applications (Linux).**
Install `wl-clipboard` (Wayland) or `xclip` / `xsel` (X11/XWayland). Without them LiteUI keeps the clipboard in a process-local buffer.

**I changed the sanitizer or profiler setting and nothing changed.**
Those are cached CMake variables. Use a preset's own build directory, pass the option again explicitly, or delete the build directory.
