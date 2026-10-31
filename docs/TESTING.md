# Testing

LiteUI's unit tests use [Catch2](https://github.com/catchorg/Catch2) v3 and run through CTest. This guide covers running the tests, how the core suite is organized and why, how to write a new test, the CMake helpers that wire tests up, and how to run them under sanitizers.

- [Running the tests](#running-the-tests)
- [How the core suite is organized](#how-the-core-suite-is-organized)
- [Writing a test](#writing-a-test)
- [The test helpers](#the-test-helpers)
- [Testing patterns](#testing-patterns)
- [CMake helpers](#cmake-helpers)
- [Sanitizers](#sanitizers)
- [Tests in the examples](#tests-in-the-examples)
- [Troubleshooting](#troubleshooting)

## Running the tests

### With CTest

```sh
cmake --preset gcc-release          # or msvc-release
cmake --build --preset gcc-release
ctest --preset gcc-release
```

Useful variations:

```sh
ctest --preset gcc-release -N                    # list the tests without running them
ctest --preset gcc-release -R "layout:"          # only tests whose name matches a regex
ctest --preset gcc-release -R liteui_core        # only the core suite
ctest --preset gcc-release -j 8                  # run test cases in parallel
ctest --preset gcc-release --rerun-failed        # only what failed last time
```

**What CTest sees.** Every Catch2 `TEST_CASE` becomes its own CTest entry, named `liteui_<suite>.<test case name>` (for example `liteui_core.layout: flexGrow splits free space in proportion`). Each entry runs in a **fresh process**, so cases cannot leak state into each other, and `SECTION`s inside one case all run within that one entry.

**Discovery happens when you run `ctest`**, not at build time (`DISCOVERY_MODE PRE_TEST`). Building never needs to execute the test binary, but it does need to have been built before `ctest` can list or run anything.

**Timeouts.** Tests registered through `liteui_add_test` get a **60 second** per-test timeout unless you pass `TIMEOUT`. The test presets also set a 120 second default, which only applies to tests that have no timeout of their own. The presets additionally print output on failure and treat "no tests found" as an error.

### Running the executable directly

The core suite is a normal Catch2 program, `liteui_core_test`, built under the `tests/` folder of your build directory (for example `build/release/tests/liteui_core_test`, with `.exe` on Windows). Run it directly when you want Catch2's own filtering or more output:

```sh
build/release/tests/liteui_core_test --list-tests
build/release/tests/liteui_core_test "[layout]"              # by tag
build/release/tests/liteui_core_test "[layout][scroll]"      # tags AND-ed
build/release/tests/liteui_core_test "layout: resolveAxis"   # by name
build/release/tests/liteui_core_test -s "[flex]"             # show passing assertions too
```

Tags are **not** turned into CTest labels, so tag filtering is only available through the executable. (`ctest -L` works only for suites registered with a `LABEL`, and the core suite has none.)

## How the core suite is organized

The whole core suite is **one translation unit**. That is deliberate: the internals under test (`liteui_layout`, `liteui_xml`, `liteui_svg`, the keysym tables, ...) exist only in the `LITEUI_IMPLEMENTATION` section of `liteui.hpp`, and that section may be compiled into exactly one translation unit per program. So `tests/liteui_tests.cpp` defines `LITEUI_IMPLEMENTATION` itself and then `#include`s every test file:

```cpp
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include "test_helpers.hpp"

#include "cases/core_types.inc"
#include "cases/xml.inc"
#include "cases/svg_primitives.inc"
#include "cases/svg_paths.inc"
#include "cases/svg_documents.inc"
#include "cases/layout.inc"
#include "cases/platform.inc"
```

The `cases/*.inc` files are **not standalone `.cpp` files**. They are fragments that rely on everything above them in `liteui_tests.cpp`. Because the test links `liteui::liteui`, the test executable also picks up the platform libraries (Direct2D on Windows, Wayland/Cairo/Pango/EGL on Linux), but the suites are pure logic and do not open a window.

| File | Covers | Main tags |
|---|---|---|
| `core_types.inc` | `Color`, `Size`, `EdgeInsets`, `Dynamic<T>` | `[core]` `[color]` `[size]` `[dynamic]` |
| `xml.inc` | `liteui_xml`, the small, forgiving XML parser behind the SVG loader: structure, attributes (quotes, entities, namespace prefixes, lenient unquoted and bare forms, duplicates), text, entities and numeric character references, CDATA, comments/PIs/DOCTYPE, malformed and truncated input, deep nesting | `[xml]` `[attrs]` `[text]` `[entities]` `[cdata]` `[misc]` `[errors]` `[limits]` |
| `svg_primitives.inc` | The low-level SVG helpers: `scanNumber` and compact number lists, `Mat2x3` and `parseTransform`, hex/named/`rgb()`/`rgba()` colours and `parsePaint`, `parseOpacity`, dash arrays, CSS declarations and class-only stylesheets | `[svg]` `[number]` `[matrix]` `[transform]` `[color]` `[style]` |
| `svg_paths.inc` | `parsePathData` (absolute and relative commands, implicit repeats, smooth curves, arcs converted to cubics, compact notation, truncated data) and the shape-to-path converters (`rectToOps`, `ellipseToOps`, `polyToOps`) | `[svg]` `[path]` `[shapes]` |
| `svg_documents.inc` | End-to-end `svg::parseString` / `svg::parseFile`: sizing from `viewBox`/`width`/`height`, shape generation, fill/stroke/opacity, group inheritance and transforms, style precedence, `<defs>`/`<symbol>`/`<use>` (including cycles and depth limits), and file errors | `[svg]` `[doc]` `[shapes]` `[style]` `[group]` `[css]` `[use]` `[file]` |
| `layout.inc` | The flexbox engine: helpers, flow, spacing, justify/align, grow/shrink, sizes and clamps, display/visibility, absolute positioning, wrapping, scrolling, `onLayout`, polled styles | `[layout]` plus `[helpers]` `[flex]` `[box]` `[grow]` `[shrink]` `[size]` `[display]` `[absolute]` `[wrap]` `[scroll]` `[callbacks]` `[dynamic]` |
| `platform.inc` | Platform helpers that are still pure logic. Currently the Linux keysym → `Key` mapping (compiled only when `!defined(_WIN32)`) | `[platform]` `[keys]` |

Case files are picked up by the build automatically for IDE display (`file(GLOB ... CONFIGURE_DEPENDS cases/*.inc)`), but **adding a file to the glob does not run it**. It only runs once `liteui_tests.cpp` includes it.

## Writing a test

### Add to an existing file

Most additions belong in an existing `.inc`. Add a `TEST_CASE` with a descriptive name and tags. Use the `area: description` naming style the suite already follows so `ctest -R "layout:"` keeps working:

```cpp
TEST_CASE("layout: a left margin shifts later siblings", "[layout][box]") {
  View root = container(200, 100);
  root.style.alignItems = Align::Start;
  root.addChild(box(40, 20));
  View b = box(40, 20);
  b.style.margin = EdgeInsets{0, 0, 0, 8};      // top right bottom left
  root.addChild(b);

  layout::layoutRoot(root, 800, 600, 0, 0);

  CHECK(root.children[0].computed.x == approx(0));
  CHECK(root.children[1].computed.x == approx(48));   // 40 + left margin 8
}
```

### Add a new file

1. Create `tests/cases/<name>.inc`.
2. Add `#include "cases/<name>.inc"` to `tests/liteui_tests.cpp`, after `test_helpers.hpp` (and after `layout.inc` if you want to use its `doLayout` helper, see below).
3. Build. (The `cases/*.inc` glob only feeds the IDE file list; nothing else needs registering.)

Start the file with a comment like the existing ones: what it covers, and that it is included from `liteui_tests.cpp` rather than being a standalone `.cpp`.

### Platform-specific cases

Guard tests that need platform symbols with the same `#if` the implementation uses:

```cpp
#if !defined(_WIN32)
TEST_CASE("linux: ...", "[platform]") { /* uses xkbKeysymToKey etc. */ }
#endif
```

## The test helpers

`tests/test_helpers.hpp` is included once, after `liteui.hpp`, so the `liteui_*` namespaces already exist.

| Helper | Purpose |
|---|---|
| `xml`, `svg`, `layout` | Namespace aliases for `liteui_xml`, `liteui_svg`, `liteui_layout`. |
| `approx(v)` | `Catch::Approx(v).margin(1e-3)`: the comparison tolerance for single-precision geometry. Use it for every float comparison. |
| `apply(mat, x, y)` | Applies an `svg::Mat2x3` to a point, returning `{x', y'}`. |
| `box(w, h)` | A `View` with a fixed pixel width and height: the building block of layout tests. |
| `container(w, h)` | Same as `box`, named for intent when the view will hold children. |
| `mustParse(text)` | Parses an SVG string and `REQUIRE`s that it succeeded, returning the `svg::Document`. |
| `TempFile` | RAII: writes contents to a file in the system temp directory under the name you give, and deletes it on destruction. |

`layout.inc` additionally defines a file-local `doLayout(View &root, float ox = 0, float oy = 0)` that runs `layout::layoutRoot` in an **800 × 600** window. It lives in an anonymous namespace in that file, so it is available to files included *after* `layout.inc`. Elsewhere, call `layout::layoutRoot(root, 800, 600, ox, oy)` directly.

> **`TempFile` names are shared.** The file is created as `<temp dir>/<name>`, so the name you pass is the only uniqueness guarantee. Use a name unique to the test case. `ctest -j` can run cases from the same suite in parallel, and two cases writing the same name would collide.

## Testing patterns

**Use fixed-size boxes and no text for layout.** The layout tests deliberately avoid text so results are exact and independent of fonts and the platform's text renderer. Build a tree from `box()`/`container()`, run layout, and read `computed`:

```cpp
View root = container(400, 100);
root.addChild(box(50, 20));
layout::layoutRoot(root, 800, 600, 0, 0);
CHECK(root.children[0].computed.w == approx(50));
```

**Test the engine's helper functions directly.** `resolveAxis`, `clampSize` and `axisScrollbarVisible` are callable from the test TU, which is how the suite pins down edge cases such as `max < min`, indefinite percentages, and the half-pixel scrollbar tolerance.

**Simulate polled `Dynamic<>` values.** A callback-valued field is read through `View::computed.resolved*`, which the app's update loop normally refreshes. A test can set that cache itself to stand in for "what the poll returned":

```cpp
child.style.width = std::function<Size()>([] { return Size::pixel(10); });
child.computed.resolvedWidth = Size::pixel(123);   // what the poll "returned"
layout::layoutRoot(root, 800, 600, 0, 0);
CHECK(root.children[0].computed.w == approx(123));
```

**Drive scrolling by setting offsets.** Set `root.computed.scrollY`, run layout again, and check that children moved and that the offset was clamped:

```cpp
root.computed.scrollY = 30;
layout::layoutRoot(root, 800, 600, 0, 0);
CHECK(root.children[0].computed.y == approx(-30));
```

**Use `SECTION`s to share setup.** A `SECTION` per variant (as in the `justifyContent` test) keeps related cases together while each still runs from a clean state.

**Test file-based code with `TempFile`.**

```cpp
TEST_CASE("svg: parseFile reads a document from disk", "[svg][io]") {
  TempFile f(R"xml(<svg viewBox="0 0 4 4"><rect width="4" height="4"/></svg>)xml",
             "liteui_test_parsefile_basic.svg");   // unique per case
  std::string err;
  auto doc = svg::parseFile(f.path.string(), &err);
  REQUIRE(doc.has_value());
  CHECK(err.empty());
  CHECK(doc->shapes.size() == 1);
}

// A missing or non-SVG file yields nullopt and a message ("failed to open ..." /
// "failed to parse ..."); the error out-parameter is optional.
```

**Parsers: assert on termination, not just on success.** The XML and SVG parsers are deliberately lenient (they return what they managed to read), so alongside the happy paths the suite checks that malformed and truncated input *terminates without crashing*. Loop over a table of bad inputs with `DYNAMIC_SECTION` so each one reports separately:

```cpp
const char *inputs[] = {"<", "<a", "<a x=\"", "<!--", "<a>&#"};
for (const char *in : inputs) {
  DYNAMIC_SECTION("input: " << in) { REQUIRE_NOTHROW(xml::parse(in)); }
}
```

The same file pins **documented limitations as tests** (for example, that mixed text and child elements concatenate into `Node::text`, or that a `<use>` chain deeper than the implementation's cap of 32 levels yields no shapes), so a deliberate change in behavior shows up as a test change rather than a silent difference.

**Parsers: test through the data, not through pixels.** SVG tests parse a document and inspect the resulting `svg::Document` (`width`, `height`, `viewBoxX/Y`, `shapes`) and each `Shape` (`fill`, `stroke`, `hasFill`, `hasStroke`, `strokeWidth`, `lineCap`, `lineJoin`, `miterLimit`, `dashArray`, `evenOdd`, `transform`, `ops`). Path tests inspect `svg::PathOp` values (`kind`, `x`, `y`, ...) and use a file-local alias (`using Op = svg::PathOp::Kind;`) that is available to files included after `svg_paths.inc`. `apply(mat, x, y)` checks a matrix by where it sends a point.

**What is not covered by this suite.** By design the suite does not create a window, so painting, real input delivery, the event loop and the platform text renderers are exercised by running the examples rather than by unit tests. The layout cases also deliberately do not cover text-dependent sizing.

## CMake helpers

`cmake/LiteuiTests.cmake` provides the shared setup. It is included from the root `CMakeLists.txt` after Catch2 has been found or fetched and before `tests/` and `examples/` are added, so both can use it.

### `liteui_add_test`

```cmake
liteui_add_test(<name>
    SOURCES  <files...>
    [INCLUDES <dirs...>]
    [LITEUI] [THREADS] [NO_SANITIZE]
    [LABEL <label>] [TIMEOUT <seconds>])
```

Creates a Catch2 executable named **`liteui_<name>_test`** with one CTest entry per test case, named **`liteui_<name>.<case>`**.

| Argument | Effect |
|---|---|
| `SOURCES` | Required. The test's source files. |
| `INCLUDES` | Extra private include directories (the core suite uses it so `test_helpers.hpp` and `cases/*.inc` resolve). |
| `LITEUI` | Link `liteui::liteui`: include path, C++23, platform libraries, and the vendored Wayland bindings. |
| `THREADS` | Link the platform thread library. |
| `NO_SANITIZE` | Never apply sanitizers to this test (for example, float-exact tests). |
| `LABEL` | Adds a CTest label so `ctest -L <label>` selects the suite. |
| `TIMEOUT` | Per-case timeout in seconds. Default `60`. |

Always applied: `cxx_std_23`, `Catch2::Catch2WithMain`, and `/utf-8` under MSVC (sources are UTF-8). The core suite is registered as:

```cmake
liteui_add_test(core
    SOURCES  liteui_tests.cpp test_helpers.hpp ${LITEUI_TEST_CASES}
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    LITEUI)
```

To add a suite of your own from a directory processed after the root has set up testing (an example's `CMakeLists.txt`, say):

```cmake
liteui_add_test(mything
    SOURCES  tests/mything_tests.cpp
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    LITEUI
    LABEL mything)
```

### `liteui_enable_sanitizers`

```cmake
liteui_enable_sanitizers(<target> <env_var_out> [NO_LEAK_CHECK])
```

Applies the sanitizer flags to `<target>` and stores the `ASAN_OPTIONS` string its tests should run with in `<env_var_out>` (empty when sanitizers are off). `liteui_add_test` calls it for you. Call it directly for tests that are **not** Catch2 (the literoom example's plain tests do), then attach the result as the test's environment:

```cmake
liteui_enable_sanitizers(my_plain_test _env)
add_test(NAME my_plain_test COMMAND my_plain_test)
if(_env)
    set_tests_properties(my_plain_test PROPERTIES ENVIRONMENT "${_env}")
endif()
```

### `liteui_prepare_sanitizers`

A macro the root calls once. Under MSVC with `LITEUI_SANITIZE`, it strips the Debug flags that ASan refuses to build with (`/RTC1` and the edit-and-continue `/ZI`, replaced by `/Zi`). Those flag variables are per directory, so calling it in the root covers every subdirectory added afterwards.

## Sanitizers

Enable with the `*-asan` presets or `-DLITEUI_SANITIZE=ON`:

```sh
cmake --preset gcc-debug-asan          # or msvc-debug-asan
cmake --build --preset gcc-debug-asan
ctest --preset gcc-debug-asan
```

Sanitizers apply to the **test executables** (not to the examples).

| Toolchain | What you get |
|---|---|
| GCC / Clang (non-Windows) | AddressSanitizer **and** UndefinedBehaviorSanitizer. UB aborts the test instead of continuing (`-fno-sanitize-recover=undefined`), with frame pointers and `-g` for readable traces. |
| MSVC / clang-cl | AddressSanitizer only; needs Visual Studio 2019 16.9 or newer (and the ASan component installed). |
| MinGW | Not supported. Tests run unsanitized, with a status message at configure time. |

The tests run with `ASAN_OPTIONS` set by the helper:

- `halt_on_error=1` everywhere.
- **Leak detection** (`detect_leaks=1`) on GCC/Clang for tests that do **not** link `liteui::liteui`. Tests that do link it run with `detect_leaks=0`, because the system GL and Wayland libraries routinely "leak" at exit and would fail every run. The core suite links `liteui::liteui`, so it runs without leak checking.
- No leak detection on Windows, where LeakSanitizer is not available.

MSVC-specific behavior worth knowing:

- The STL's ASan container annotations are **switched off** for test targets (`_DISABLE_ASAN_ANNOTATIONS` and related definitions). Catch2 is built without ASan, and mixing annotated and unannotated objects fails to link (LNK2038). Use-after-free detection still comes from ASan's allocator; only container-overflow checks on reserved capacity are lost.
- The ASan runtime DLL (`clang_rt.asan_dynamic-*.dll`, found next to `cl.exe`) is **copied next to each test executable** after build, so the tests start even when you do not run `ctest` from a Developer Command Prompt.
- Incremental linking is disabled for sanitized test targets.

## Tests in the examples

Examples with a `tests/` folder register their own tests, and they are built and discovered through the same helpers. They are only built when `LITEUI_BUILD_TESTS` and the example itself are enabled (`LITEUI_EXAMPLE_<NAME>`).

- **`examples/counter`** keeps its model in `counter_model.hpp`, apart from `main.cpp`, with tests in `tests/counter_tests.cpp`.
- **`examples/literoom`** ships a much larger test set under `examples/literoom/tests/` (one file per module, a shared `check.hpp`, and golden-render files such as `golden/render_MSVC_Release.txt`) that uses the same sanitizer helper.

## Troubleshooting

**`ctest` fails with "No tests were found".**
The test presets treat that as an error. Either tests were not built (`LITEUI_BUILD_TESTS=OFF`, or you configured with a different preset than you are running), or the build has not been done yet. Discovery runs the built binary, so build first.

**Configure fails or stalls fetching Catch2.**
Catch2 v3 is fetched (v3.8.1) when not installed. Install it with your package manager or vcpkg so `find_package(Catch2 3)` succeeds, or configure with `-DLITEUI_BUILD_TESTS=OFF`.

**A new `.inc` file's tests do not run.**
Add `#include "cases/<name>.inc"` to `tests/liteui_tests.cpp`. The glob only adds it to the IDE file list.

**Duplicate-symbol or "multiple definition" errors in a new test.**
Another translation unit defined `LITEUI_IMPLEMENTATION`. The core suite must stay a single TU. Add new tests as `.inc` includes rather than separate `.cpp` files, or give a separate suite that does not need the internals its own `liteui_add_test`.

**`LNK2038 mismatch detected for 'annotate_vector'` (MSVC, sanitizers).**
Mixed ASan annotation settings. The helper defines the disable macros for you; this usually means a target was sanitized without going through `liteui_add_test` / `liteui_enable_sanitizers`.

**ASan tests fail to start on Windows with a missing DLL.**
The runtime DLL is copied from the directory of `cl.exe` after build. If Visual Studio's ASan component is not installed, no DLL is found. Install the component and rebuild.

**Sanitized build fails on Linux with "cannot find -lasan" or similar.**
Install your distribution's ASan/UBSan runtime packages (for GCC, `libasan` and `libubsan`).

**Two test cases interfere with each other's temp files.**
Give each `TempFile` a unique name, because cases can run in parallel under `ctest -j`.

**A float comparison fails by a hair.**
Compare with `approx(v)` (a 1e-3 margin) instead of `==`, as the existing tests do.
