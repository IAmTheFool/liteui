# Testing

Literoom's engine headers have no UI, so almost all of them can be tested without opening a window. This page explains the kinds of tests, how to run them, what each one covers, and how the **golden render test** protects you when you refactor.

## Running the tests

Literoom is an example inside the liteui tree (`examples/literoom`), and its `CMakeLists.txt` stops with an error if it is configured on its own. Always configure, build and run `ctest` from the **liteui root**.

The tests are **on by default**: `LITEROOM_TESTS` follows the root switch `LITEUI_BUILD_TESTS`, which is ON for a top-level build, so a normal build already compiles them. Pass `-DLITEROOM_TESTS=OFF` to skip only the literoom tests, or `-DLITEUI_BUILD_TESTS=OFF` to skip every test in the tree.

```powershell
# from the liteui root (CMakePresets.json has msvc-release, gcc-release, ...)
cmake --preset msvc-release
cmake --build build/release
ctest --test-dir build/release -C Release --output-on-failure
```

`-C Release` (the configuration) is needed with Visual Studio's multi-configuration generator and is harmless with Ninja. To run just one test, filter by name: `ctest --test-dir build/release -C Release -R mask_brush`.

Literoom builds three test executables. **`literoom_test`** is a single Catch2 program that holds every unit case: one `tests/cases/<header>.inc` per header, all `#include`d from `tests/literoom_test.cpp`. **`literoom_golden_render_test`** and **`literoom_lut_pane_test`** are stand-alone programs (see kinds 3 and 4 below). CTest lists each Catch2 case on its own, named `literoom_<case name>` (case names look like `pixel_math: toU8Clamped`), so `-R mask_brush` selects one file's cases; the two programs are the CTest entries `literoom_golden_render` and `literoom_lut_pane`. Catch2 tags become CTest labels. Every case carries a tag with its file's name (for example `[color_pass]`, so `ctest -L color_pass` selects one file's cases as well); the timing-sensitive worker cases are tagged `[threaded]` (and `literoom_lut_pane` has the label `threaded` too), and the golden test has the label `golden`. A plain `ctest -N` from the liteui root lists more, because it also includes liteui's own Catch2 cases and the `counter` example. To list only literoom's:

```powershell
ctest --test-dir build/release -N -R literoom_
```

The golden render test needs a recorded reference file before it can pass; see [The golden render test](#3-the-golden-render-test).

## The harnesses: Catch2 and `check.hpp`

The unit cases use **Catch2** (`TEST_CASE`, `CHECK`, `REQUIRE`), so there is nothing extra to install beyond what the liteui root provides. `tests/literoom_test.cpp` adds one helper for floats: `CHECK(x == within(v, tol))` passes when `|x − v| <= tol` (an absolute margin, no relative epsilon). A case name starts with its header's name (`pixel_math: toU8Clamped`), which is what makes `ctest -R pixel_math` work.

The two stand-alone programs (`golden_render_test.cpp` and `lut_pane_test.cpp`) have their own `main()` and are not Catch2. The golden test keeps its own tally (failures and notes, described in [its section](#3-the-golden-render-test)); `lut_pane_test.cpp` uses the small **`check.hpp`** harness:

| Piece | What it does |
|---|---|
| `CHECK(cond)` | Records a failure (with file and line) and **carries on**, so one program reports all its failures at once. |
| `CHECK_NEAR(a, b, tol)` | Passes when `|a − b| <= tol`. Use it for floats. A NaN on either side fails. |
| `return finish();` | Prints `all ok` or `N FAILED`, and sets the exit code (0 or 1) that `ctest` reads. |

`CHECK` is variadic so that braced initializers with commas, like `CHECK(crop == CropRect{1, 2, 3, 4})`, work inside it.

Things worth knowing:

- **Assert on the main thread only.** Tests that run threads (`latest_worker`, `thumb_worker`, `export_worker`, `lut_preview_worker`, and the LUT-pane program) call `CHECK` only on the main thread; worker threads record into atomics or mutex-guarded state, and the main thread asserts afterwards. For `check.hpp` the reason is concrete: its failure counter (`tcheck::failures`) is a plain `int`.
- **The macro names clash.** `check.hpp`'s `CHECK` and `CHECK_NEAR` would collide with Catch2's own macros, so it cannot be included in `literoom_test.cpp`'s single translation unit. The two programs that use it are compiled separately.

## The kinds of test

### 1. Unit cases (`literoom_test`, one `.inc` per header)

There is one `tests/cases/<name>.inc` for every engine header (54 files in all: one per header, plus `lut_render`, which tests the LUT through the renderer). A case includes a header by its path from `src/`, for example `#include "mask/mask_brush.hpp"`; the target has `src/` on its include path, and `tests/` too so the `.inc` files can include each other as `cases/<name>.inc`. Adding a case needs no CMake edit: the `.inc` files are found by a `CONFIGURE_DEPENDS` glob, and `literoom_test.cpp` `#include`s each one.

Most cases are pure: no UI, no disk, no threads. Tests that touch the disk (`file_utils`, `virtual_copy`, `image_meta`, `preset`, `lut_library`, and also `image_codec`, `image_codec16`, `image_source`, `thumb_worker`, `export_worker`, `lut_preview_worker` and the LUT-pane program below) work in a throw-away folder under the system temp directory (`literoom_<tag>_<timestamp>`), removed again when the test ends. The metadata tests build their own synthetic JPEG files (hand-made EXIF and ICC segments), and the color-tag tests build synthetic PNG and JPEG data, so no sample photos are needed. The LUT tests write real `.cube` files into such a folder and point the LUT library at it with `lutDirOverride()` (or, where no file is needed, put a table in the library's cache with `lutCachePut`); they restore the override and empty the cache when they finish.

### 2. Cases that use LiteUI or start threads

Some headers take LiteUI's `CanvasImage` or decoders, or run a real thread. Because `literoom_test` is one executable linked with LiteUI, Catch2 and the thread library, none of this needs a build setting; it only shapes how the cases are written:

| Cases | Uses | Why |
|---|---|---|
| `raster_ops`, `seg_basic`, `render`, `render_core`, `export_render`, `wide_chain`, `image_codec`, `image_codec16`, `image_source`, `histogram`, `scopes`, `lut_render` | LiteUI | They work on a `CanvasImage` (or `Color`); `render_core`, `export_render` and `wide_chain` also build `Image16` pictures (the 16-bit side of the same pipeline). `image_codec`, `image_codec16` and `image_source` also decode what they encode with LiteUI's own decoders. |
| `detail_layer`, `neighbor_fx` | Threads | Their cache cases start a few `std::thread`s to check that concurrent requests for the same plane give the same result. |
| `latest_worker` | Threads | It runs a `std::jthread`; it is tested with plain integers and move-only types. |
| `thumb_worker` | LiteUI **and** threads | It decodes real files on a thread. The test writes those files with the app's own encoders (`image_codec.hpp`). |
| `export_worker` | LiteUI **and** threads | It decodes, renders and encodes real files on a thread. Same file setup as `thumb_worker`; the exported files are read back with the real decoder. |
| `lut_library` | Threads | Six threads ask for one LUT at once; the cache is a mutex-guarded global. |
| `lut_preview_worker` | LiteUI **and** threads | It renders previews on a thread; the test writes real `.cube` files. |

**LiteUI's implementation is compiled once.** LiteUI is one header, and its implementation (the Win32 / WIC includes, `toWide`, the decoders, ...) is only compiled when `LITEUI_IMPLEMENTATION` is defined, in exactly one translation unit. For `literoom_test` that is `tests/literoom_test.cpp`, whose first lines are `#define LITEUI_IMPLEMENTATION` and `#include "liteui.hpp"`, before any case; a case must not define it again. (The two stand-alone programs compile `src/main.cpp` and define it in their own `.cpp`.)

**Thread cases can be flaky in a way that only shows up occasionally.** Run them in a loop to shake out timing problems:

```powershell
ctest --test-dir build/release -C Release -L threaded --repeat until-fail:50
ctest --test-dir build/release -C Release -R "latest_worker|thumb_worker" --repeat until-fail:50
ctest --test-dir build/release -C Release -R "detail_layer|neighbor_fx" --repeat until-fail:50 --timeout 60
```

This repeats the tests up to 50 times and stops at the first failure. `CMakeLists.txt` gives every case a 120 s timeout (the threaded ones wait up to 10 to 20 s on purpose); `--timeout 60` makes a hung run fail sooner, which is useful for `detail_layer` and `neighbor_fx`, which have no timeouts of their own.

How `latest_worker`, `thumb_worker`, `export_worker` and `lut_preview_worker` are written so they stay reliable:

- They use **gates and latches instead of sleeps**, and every wait has a timeout (5 to 10 s), so a broken worker fails the test instead of hanging it.
- A short `grace()` sleep (100 ms) is used only where a *negative* check has no signal to wait for (for example "nothing was notified after the job returned nothing"). Too short a grace can hide a bug (a false pass) but can never fail a correct worker.
- They check the properties the code's comments promise: the newest request wins, a queued job that is replaced never starts, the notify callback runs on the worker thread and **outside** the worker's lock (the test calls `collect()` from inside notify), `shutdown()` is idempotent, and destruction with work queued does not hang.
- The `latest_worker` cases end with a stress test: 2000 submits mixed with random `invalidate()` calls, checking that a collected value is always the newest submit's.

The `detail_layer` and `neighbor_fx` thread cases are simpler: they start a handful of `std::thread`s, `join()` them, and compare the results on the main thread (equal planes for equal keys, identical output pictures). They use no gates and no timeouts, so a deadlock in a cache would hang the case until its CTest timeout instead of failing it, and a data race is only caught by a thread-sanitizer run (see [Sanitizers](#sanitizers)).

### 3. The golden render test

This is the safety net for refactoring `src/main.cpp`, and it works differently from the others.

It compiles `src/main.cpp` itself (with `main()` renamed so the test can have its own), renders **344** picture/edit combinations at the time of writing (the golden file has one line per scenario, and the test prints the exact count at the end of every run), and **fingerprints** each result. It then compares the fingerprints with a saved "golden" file.

The idea: if you restructure code and the pixels come out bit-for-bit the same, the fingerprints match and the test passes. If a refactoring changes even one pixel, you find out immediately, at the step that caused it.

Floating-point results can differ between compilers and between compiler flags, so there is **one golden file per compiler and configuration**, named `tests/golden/render_<COMPILER>_<CONFIG>.txt` (for example `render_MSVC_Release.txt`, `render_GNU_Debug.txt`). Only compare a run against a file recorded by the same build. **Commit the golden files**: without them a fresh checkout cannot run the test.

The workflow:

```powershell
# 1. Once, BEFORE refactoring: record the current behavior as the reference
cmake --build build/release --target literoom_golden_record

# 2. After every refactoring step: check nothing changed
ctest --test-dir build/release -C Release -R literoom_golden_render
```

**On a fresh checkout the golden test is skipped until you have recorded it.** If the golden file does not exist the test prints `cannot open ... (record it first with --record); skipping` and exits with code 77, which CTest reports as **Skipped** (`CMakeLists.txt` sets `SKIP_RETURN_CODE 77` on the test). A skipped golden test means the safety net is **not running**, so record the file before you start refactoring. Record once per compiler and configuration you test (`Debug`, `Release`, ...). With a single-configuration generator (Makefiles, Ninja), pass `-DCMAKE_BUILD_TYPE=Release` when you configure; without a build type the configuration part of the file name is empty (`render_MSVC_.txt`).

Recording prints the same summary as a normal run, for example `344 scenarios, 0 mismatches, 0 invariant failures, N notes`. Record only while you trust the current output, because the file becomes the reference for later refactoring. Do not re-record just to make a failing run pass.

#### What it renders

| Group (name prefix) | What is rendered |
|---|---|
| `adj/` | Every slider on its own (up, down, barely moved, full range both ends; named by label, for example `adj/Texture/pos`; LUT Amount is left out, because it does nothing without a LUT and `lut/` covers it), with nothing set, with all sliders set together, and the Filters stacked (`filters_stack`), over other color stages (`filter_after_color`) and next to the neighbor effects (`filter_with_detail`) |
| `curve/`, `mix/`, `grade/` | Master and per-channel curves, every Color Mixer channel, each grading wheel, blending and balance, a hidden wheel |
| `mask/` | Every mask type and its inverse, parameter extremes (hard and fully soft edges, smallest and largest radii, a one-dab brush, an unset Color Range), Amount, a disabled mask, overlapping masks, composites of Add / Subtract / Intersect, neighbor effects only, masks whose only effect is a Filter (`filter_only`, `filters_overlapping`), and every effect at once |
| `seg/` | The stand-in segmenter's own coverage map, which is then used as an Object mask |
| `geo/`, `geo_masks/`, `geo_film/` | All 8 orientations, straightening in both corner modes, a crop, the same mask set rendered through three different geometries, and a vignette with grain through the same three (they follow the picture as shown) |
| `film/` | Vignette and Grain on their own (dark, light, tight and hard, wide and soft; fine and crisp, big and smooth), both together, and after other sliders and a mask (they run last) |
| `lut/` | A fixed 17-point LUT built in code (no file) at full, half and a low amount, **after** other color stages and Filters, and **before** the neighbor effects, a mask and Vignette / Grain; and the things that must leave the picture exactly alone (a LUT at amount 0, a LUT the library does not have, an amount with no LUT), plus an identity LUT within one level of the picture |
| `sink/` | The "kitchen sink": every slider (the Filters at a modest 20 % and the Effects at fixed modest values, see below), Mix, Grade, curves and three masks at once, on a 200 × 150 and an odd 97 × 71 picture |
| `history/`, `sidecar/` | The undo labels and indices after a scripted sequence of edits, and the sidecar JSON text |
| `util/`, `thumb/`, `preview/` | Natural sort order, path helpers, thumbnails and the preview downsample |
| `histogram/` | The four histograms, clip flags and clipping overlay, driven through the editor's global `state` (every overlay option, strided sampling, block reduction, no image, mid-drag preview) |
| `odd/` | `odd/masks_and_detail`: three masks (radial, brush, linear) with Clarity, Dehaze and luminance noise reduction on, on the odd-sized picture |

The test picture is made with integer arithmetic only, so the **input** is identical on every platform. It has five bands (a hue sweep, a gray ramp, a pale low-contrast band, a texture band and a band with discs and color patches), and its last column has alpha 200 so a stray alpha change is caught.

**Every scenario renders an 8-bit `CanvasImage`.** The golden test never builds an `Image16`, so `renderToDisplay` and `renderToImage16` (the 16-bit entry points of `render_core.hpp`) are not pinned by fingerprints; `cases/render_core.inc` covers them with numbers.

#### Reading the result

Each line is a scenario name followed by a hash. A run ends with `N scenarios, M mismatches, K invariant failures, L notes` and `all ok` or `FAILED`.

| Output | Meaning |
|---|---|
| `MISMATCH <name>` | The scenario's output changed. Shows the expected and the new hash, size and per-channel byte sums, so you can see roughly how it changed. |
| `NEW <name>` | The scenario is not in the golden file. Counts as a failure; re-record if you added it on purpose. |
| `MISSING <name>` | It is in the golden file but was not produced now (a renamed or removed scenario). Also a failure. |
| `INVARIANT FAILED <name>` | A built-in check broke (below). Independent of the golden file. |
| `note: ...` | Information only; does **not** fail the run (below). |

Exit code: **0** every scenario matches and every invariant holds, **1** a mismatch or invariant failure, **2** bad usage, **77** no golden file recorded yet (CTest shows it as skipped).

Built-in invariants (these fail the run even when recording):

- A render with a **warm cache** gives the same bytes as the first, cold one. Every cache is emptied before each scenario so results never depend on scenario order.
- The render never changes the picture's size.
- The background render path (`renderWith` with a generation counter) gives the same bytes as the synchronous one, and a superseded render returns null.
- The sidecar text parses and loads back, and scenario names are unique.

Notes that do **not** fail the run, so read them: a scenario with no visible effect on the test picture (for example `adj/Noise_Reduction/low` at the time of writing: the scenario is still recorded, but it guards only the pixels the test picture actually has), an untouched edit set that does not reproduce the input exactly, and the sidecar round trip (a second dump differing from the first, or a render after save and load differing from the original). A refactoring that breaks save and load rendering would only show up as a note.

#### Finding where pixels changed

`golden_render_test --dump out_before/ --record x.txt` also writes one `.ppm` picture per image scenario (`/` in the name becomes `_`). Dump before and after a change, then compare the two folders with `cmp` or `fc /b` to see which scenario moved and look at it. Scenarios that are text (history, sidecar JSON, histogram counts, helpers) are fingerprinted but not dumped.

#### Refactoring `src/main.cpp`

The test includes `src/main.cpp` (with `main()` renamed so the test can have its own); the include resolves because `src/` is on the test target's include path. It reaches into it for `renderAdjusted`, `renderWith`, `clearMaskCaches`, `segmentObjectWand`, `orientImage`, `rotateArbitrary`, `cropImage`, `makeThumb`, `makePreviewBase`, `computeHistogram()`, and the editor's global `state` (the histogram and overlay fields). The engine headers already live in their own folders under `src/`; what keeps the test tied to `main.cpp` is the editor state and the helpers that still live there. When you move more code out of `src/main.cpp` into headers, those names must stay reachable.

The test target is built **without** `LITEROOM_ONNX`, whatever the option says (the ONNX settings are applied to the `literoom` target only), so it always uses the stand-in segmenter and never exercises the ONNX path.

A few habits that make this pay off:

- **Record before you touch anything**, and on a build you trust.
- **Refactor in small steps** and run the check after each, so a failure points at one small change.
- **A failure is not always a bug.** If you *meant* to change the output (a new effect, a fixed formula), re-record the golden file and review what changed in version control.
- **Adding a slider should not change the existing fingerprints** while the new slider is at 0, but the test **does** loop over the slider table, so expect this when you add one:
  - It gains new `adj/<label>/...` scenarios for the new slider. Scenarios are named by **label**, not by table position, so inserting a slider renames nothing.
  - Scenarios that set **every** slider change: `adj/all_pos`, `adj/all_neg`, the `sink/` scenarios, `mask/every_effect`, `mask/composite_every_effect` and `sidecar/sink_json`. In `fullEffect` and `buildSink` the sign a slider gets depends on its position among the sliders that are **not** Filters and **not** Levels (`sinkPos`), so inserting a Filter or a Levels slider flips no other slider (LUT Amount is skipped the same way). Inserting any other slider in the middle shifts that position for every later slider, so append it to a block `sinkPos` already skips, or extend `sinkPos`.
  - The Filters get `kSinkFilterStrength` (20) in those scenarios, not half their range. At 50 %, Negative and Solarize turn every bright pixel into the same mid-gray, and the fingerprint would stop noticing changes in the tone, color, mixer and grade stages upstream of them.
  - The Levels sliders (Black Point, White Point, Gamma) also get fixed values there, from `fixedSinkValue` (8, 8 and 20), found with `isLevels` (the block from `AdjId::BlackPoint` to `AdjId::Gamma`). At half range they would clip a fifth of the tonal range at each end and hide the stages upstream. `isFixedSink` (Filters, Levels, Effects and LUT Amount) is the one test used at all three call sites. A new slider whose half-range value would wipe out the picture gets an entry in `fixedSinkValue` the same way.
  - The Effects sliders (Vignette ... Grain Roughness) are the last block of the table, so they shift no older slider's `sinkPos`, and they get fixed values too (`fixedSinkValue`: Vignette -35, Midpoint 10, Feather 15, Grain 22, Size 30, Roughness 20), found with `isEffects` (the block from `AdjId::Vignette` to `AdjId::GrainRoughness`). At half range a vignette would black out the corners and grain would bury what the other sliders did. A new Effects slider added after `Grain Roughness` needs that upper bound moved.
  - The golden test finds the Filters with `isFilter` (the block from `AdjId::Greyscale` to `AdjId::CrossProcess`). A new Filter added after `CrossProcess` (so before `LUT Amount`) needs that upper bound moved.
  - **LUT Amount** sits right after `CrossProcess` (found with `isLut`). `sinkPos` skips it like the Filters, and `fixedSinkValue` gives it 0: the kitchen-sink scenarios have no LUT chosen, so a value would change nothing, and 0 says so. The per-slider loop skips it for the same reason. The `lut/` scenarios are where it is exercised, with a LUT held in the library's cache under a name (the golden test points the library at a folder that does not exist and never reads a file).
  - Any existing single-slider scenario that changes means the new code is affecting untouched pictures: a real bug.

#### Checking a re-record

When a change legitimately alters the kitchen-sink scenarios (a new slider, say), you can still prove that nothing *else* changed. Keep a copy of the old golden file, re-record, then run:

```bash
python tests/golden/golden_compare.py tests/golden/render_<COMPILER>_<CONFIG>.old.txt tests/golden/render_<COMPILER>_<CONFIG>.txt
```

It compares every scenario by name (fingerprint, size and channel sums) and exits 0 only if every pre-existing scenario has its old fingerprint, apart from the ones that set every slider (`adj/all_*`, `sink/*`, `mask/every_effect`, `mask/composite_every_effect`, `sidecar/sink_json`). Anything else that changed is listed as `UNEXPECTED`, anything that disappeared as `VANISHED`, and scenarios only in the new file as `NEW` (informational; they do not fail the run). The golden files are plain text: one line per scenario (`name hash width height sumR sumG sumB sumA`), `#` comment lines allowed. The last two lines of the output are a summary (`N old, M new: X unchanged, Y changed (allowed), Z UNEXPECTED, V VANISHED, W NEW`) and `ok` or `FAILED`. Exit codes: 0 ok, 1 something unexpected or vanished, 2 a file could not be read, has no scenarios, has a line without exactly 8 fields, or names a scenario twice. Options: `-v` shows the old and new fingerprint of every changed scenario, `--allow PATTERN` (repeatable, `fnmatch` style) also lets matching scenarios change, `--no-default-allow` turns the built-in list off. It also strips the index from old index-based names (`adj/14_Texture/pos` becomes `adj/Texture/pos`), so a golden file recorded before the label-based names can still be compared.

A change that adds a *group* of scenarios and alters nothing for untouched pictures (the `lut/` group) shows up as `NEW lut/...` lines and nothing else.

To prove that the only change in the kitchen-sink scenarios is the new slider, give it the value 0 in those scenarios first (for the Filters: set `kSinkFilterStrength` to `0.0f`; for the Levels and Effects sliders: make `fixedSinkValue` return `0.0f` for them) and run the normal test against the **old** golden file. There should be no `MISMATCH` lines, only `NEW` and `MISSING` for scenarios that were added or renamed. Then restore the value and re-record.

### 4. The LUT-pane test

`tests/lut_pane_test.cpp` is the other stand-alone program. Like the golden test it compiles `src/main.cpp` into its own translation unit (with `main()` renamed) and has its own `main()` on the `check.hpp` harness, so it cannot join `literoom_test`. It runs a real LUT library in a temp folder, the real preview worker and the real renderer on a `std::jthread`, and drives the LUTs pane's functions from `main.cpp`. It is the CTest entry `literoom_lut_pane` with the label `threaded`, and it is built with the sanitizers like `literoom_test` (see [Sanitizers](#sanitizers)).

```powershell
ctest --test-dir build/release -C Release -R literoom_lut_pane
ctest --test-dir build/release -C Release -L threaded --repeat until-fail:50      # timing flakiness
```

## What each test covers

Each row is one `tests/cases/<name>.inc` (the first column is its name and the prefix of its CTest case names), except `lut_pane` and `golden_render`, which are programs of their own. A name in backticks inside a row is a Catch2 case in that file, without the `<file>: ` prefix (for example `lutExport` is the case `export_worker: lutExport`).

| Test | What it pins down |
|---|---|
| `pixel_math` | `smoothStep01` (monotone, symmetric, flat at both ends), `toU8Clamped` (exact on whole levels, halves round up, clamps), `kDegToRad`. |
| `image16` | The 16-bit storage and its conversions (`core/image16.hpp`). Every byte widens to `v * 257` and narrows back; `u16ToLevels` is exact for widened 8-bit data; `narrow16` is the nearest byte and equals `toU8Clamped(u16ToLevels(s))` for **all 65,536 samples**; 16-bit to levels to 16-bit is lossless for every sample. The rounding rule of `toU16Clamped` (clamps, NaN gives 0, infinities clamp, a tie lands on one side). The row helpers (`widenSamples`, `narrowSamples`, `samplesToLevels`, `levelsToSamples`): 8-bit to float to 8-bit and 16-bit to float to 16-bit are lossless, widened 8-bit data gives exactly the same levels, and out-of-range floats and NaN clamp instead of wrapping. `Image16`'s constructor (size, zero-filled, a negative size is an empty picture rather than a huge allocation). |
| `tone_table` | The float tone table (`core/tone_table.hpp`) against the byte table it replaces (`toneLutFromValues`). A single slider, a curve alone and a channel curve alone, rounded once, give **exactly** the old bytes; the sliders are no longer rounded between each other (Contrast +57 differs from the byte table on purpose) and random slider combinations stay within 6 levels of it; the interpolation on a function that is linear there; the float chain written out by hand, Gamma included; sanitizing, the table layout and `ToneSet` (a curve that rounds to the straight line counts as none, so an untouched curve never changes a pixel). The head, for 16-bit input: Gamma +100's steep start is not interpolated across a coarse cell (`gammaLiftHasNoSteepStartError`), every other slider stays within its documented error, the head with master and channel curves is right at every 16-bit sample (under 0.02 level), 8-bit levels stay exact, and NaN, infinity and out-of-range input stay in 0..255. |
| `adjustments` | The slider table (ids match positions, ranges, stage order Tone → Color → Detail → Effects, only Tone has `apply`). Direction and reach of each tone function. `toneLutFromValues`: byte rounding after **each** slider, table order matters, curve applied last, clamping not wrapping. The Levels sliders: labels pinned (sidecar keys) and placed right after Blacks in order, exact anchor points (Black Point `v` maps to 0, White Point `255 - v` maps to 255, Gamma never moves 0 or 255 and brightens only when positive), the identity at 0, and every table non-decreasing across the whole range and at every combination of extremes, equal to the one-slider tables run in table order. The Filters block: ten Color-stage 0..100 sliders, contiguous between Colorize Amount and Texture, with their exact labels pinned (they are sidecar keys), sanitized into range, and never part of the tone table. The Effects block: six Effects-stage sliders (Vignette, Vignette Midpoint, Vignette Feather, Grain, Grain Size, Grain Roughness) contiguous right after Blur and last in the table, with their exact labels and ranges pinned (they are sidecar keys), sanitized into range, and never part of the tone table. `LUT Amount` (`lutAmount`): its label (a sidecar key, so a rename would orphan saved values), range 0..100, Color stage, no tone `apply`; it follows `Cross Process` and `Texture` follows it; at any strength it leaves the tone table alone. |
| `color_pass` | The stage flags (`active`, `mix`, `grade`, `hue`, `colorize`, and each Filter making the pass active), a neutral pass changes nothing, white-balance gains keep luma, grays stay gray under the color-only stages, saturation and vibrance, hue and colorize, the mixer and grading stages, alpha and trailing bytes untouched. Filters: sanitizing, the poster level count (2 at 100, falling smoothly), exact results for Greyscale, Negative, Posterize, Solarize, Sepia, Fade and Vintage, the properties of Bleach Bypass (black and white fixed, more contrast, grays stay gray, color mostly gone, half strength is halfway), Threshold (black or white by luma, not by the brightest channel, and it runs before Negative) and Cross Process (blacks go blue, whites go yellow, mid-gray stays, R has more contrast than G, no channel runs backwards), the fixed order (Greyscale before Sepia), and that Filters run after Color Grading. White-balance picker (`whiteBalancePicker`): a warm gray needs a cooler Temperature (the exact numbers for `(180, 150, 120)` are written out), a neutral spot gives 0 and 0, the directions (blue cast warms, green cast gives positive Tint, magenta cast gives negative), and a round trip over a grid of mild casts: the picked values run back through the real `makeColorPass` leave the spot gray to within 3 levels (and enough of the grid is `Ok` that the check really ran). A spot needing more than ±100 is `Limited`, usable, clamped, and ends closer to gray than it started; too dark, clipped and NaN/infinity inputs are refused; the boundaries `kWbMinLevel` and `kWbMaxLevel - 1` are still readable. The 3D LUT: a lone LUT turns the pass on (no table, amount 0 or an invalid table do not), an identity LUT changes no byte by more than 1 and never alpha, an inverting LUT gives the exact negative, the amount blends, the LUT runs **after** the Filters (Negative then an inverting LUT is the original) and is handed values clamped to 255, and `colorPassWithLut` equals a pass built with the LUT from the start, swaps rather than stacks, and leaves the other stages alone. The float pass (`colorPassFloatAgrees`): the float core, rounded by hand, equals `applyColorPass` over 100 random passes. Chunking (`bulkEqualsPerPixel`): one call over any length equals one call per pixel, across the 1024-pixel chunk edges, with stray trailing bytes and alpha untouched. |
| `auto_tone` | `solveAutoTone` on hand-made histograms. Too few pixels gives `NoPixels` (one short of `kAutoToneMinPixels` vs exactly enough), a single level or a range under `kAutoToneMinSpread` gives `Flat` (exactly the minimum is measurable), and both leave all four values 0. A uniform ramp is left almost alone (Exposure and Contrast within 10) and its 0.5 % and 99.5 % points, run through the real `toneLutFromValues`, land within 6 of the targets. A low-contrast range (80 to 180) gets positive Contrast, lands within 12 of both targets and widens by more than 100 levels. A bright picture gets negative Exposure and its top is not pushed into clipping; with a pile at pure white Exposure never goes positive. A dark picture gets positive Exposure and is `Limited`. Single stray pixels at 0 and 255 do not change the answer, but a pile of about 10 % does; scaling every count by 37 changes nothing; the same input always gives the same answer. A sweep over 236 ranges: always usable, whole values inside the slider ranges and never a negative zero, the table stays monotonic, an end point that was not clipped is not pushed to 0 or 255, and enough of the sweep reaches `Ok` that the check really ran. |
| `color_mix` | Band layout and names, identity table when neutral, a band's center gets exactly its own value, sign symmetry, no seams (including the Magenta-to-Red wrap) on random settings. |
| `color_grade` | Hue to RGB, neutral gives all zeros, the three ranges always add up to 1, tints are luma-neutral, hidden wheels, how Balance and Blending move the crossovers. |
| `geometry` | `rotatedDims` (expand and fit, symmetry), `GeoFrame` for all 8 orientations, straighten and crop (orthonormal, round trips, centre preserved), `geoStep` (turns, mirrors, crop following the picture), angle wrapping, `snapAngle`. |
| `curves` | Shape through the points, monotone and no overshoot on 3000 random point sets, insert / move / remove rules (minimum gap, end points), identity detection, 20,000 random edits keep the list valid. |
| `detail_layer` | `blurLuma` against a naive reference (`refBlur`: three horizontal then three vertical box passes, replicated edges, rounding after every pass) at radii 1, 2, 5, 30 and 80 (80 is larger than the 61 × 47 test picture), off by at most one 1/16 level. A flat picture stays flat at the borders. `boxRadius` scales with picture size (a 6000 px picture gets 5 to 7 times the radius of a 1000 px one), Texture < Clarity, Sharpness ≤ Texture, never below 1 or above `kMaxBoxRadius`. Cache: a repeat request returns the very same plane, another kind gives another plane, a reused address with different pixels is told apart by the fingerprint, a cancelled build returns null and caches nothing, at most 9 entries are kept (the newest survives), and 8 threads over 3 keys get equal planes and leave exactly 3 entries. `addPixels` on a step edge, for each of Texture, Clarity and Sharpness: contrast across the edge grows, flat areas and alpha do not change, negative Texture and Clarity smooth, negative Sharpness is clamped to 0, all-zero gains leave the picture untouched and build no plane, the same amount goes into R, G and B (no hue shift), and Clarity leaves a near-white edge alone (changes ≤ 1). 16-bit (`blurMatchesReference16`): the same reference check on the blur plane of a 16-bit source; the 16-bit `addPixels` has no unit test here (`render_core` covers it). |
| `neighbor_fx` | Zero gains change nothing (random bytes come back identical, `need()` is 0). The `need()` bit layout: 0 to 2 luma planes (noise reduction reads Sharpness's), 3 chroma, 4 veil, 5 to 7 blur levels. Negative Dehaze needs no map, and Blur 0 to 33 reads level 0, 34 to 66 levels 0+1, 67 to 100 levels 1+2. With only detail sliders set, `apply` equals `detail_layer::addPixels` byte for byte. Luminance noise reduction (noise ±8 beside a hard edge): flat-area noise at least halved, edge height kept above 90 %, half slider lands between, R, G and B stay equal. Color noise reduction (chroma noise at constant luma): chroma spread under 40 %, luma moves by at most 3, a hard red-to-blue edge keeps 80 % of its color difference. Dehaze on a scene veiled with known transmission 0.55 and airlight 235: error against the clear scene falls below 60 %, contrast rises 1.4 times, half strength lands between. Negative values lower contrast and raise the mean, and a haze-free picture is only mildly changed. Blur: spread falls monotonically at 20, 50 and 100, mean kept within 2 levels, slider 100 leaves under 10 % of the spread, a flat picture and slider 0 are untouched, no jump at the level boundary (33.2 vs 33.5), and a red-to-blue edge of equal luma is softened (color blur, not luma-only). Caches: a second `acquire` shares every plane, level 2 is not built at slider 50, a slider drag with the same `need()` builds nothing new, and a cancelled `acquire` returns false and leaves all three caches empty. Sizes from 1 × 1 up to 500 × 3 and 5 × 500 keep the picture size and alpha. Uneven pieces that start mid-row equal the whole picture byte for byte, and 6 threads give identical output. The float destination (`floatAgreesWithBytes`, `floatPartialRangesMatchWhole`, `floatKeepsPrecisionAndClamps`): the float instantiation of the passes agrees with the byte one (alpha untouched, results inside 0..255, and the picture really changes), pieces equal the whole, and it never rounds but does clamp to 0..255. **The test builds 8-bit sources only**: the 16-bit source path (`Map::fix` 64, the `uint16_t` `acquire`, the source type `S` of the passes) is covered only through `render_core`. |
| `film_fx` | `paramsFromValues`: all zeros is a no-op with the default midpoint, feather and roughness; every input scaled and clamped; NaN and infinity count as 0. `prepare` (a non-positive size does nothing, the ramp is never empty at any Midpoint / Feather, grain cells grow with Size in proportion to the long edge and never go below 1 px). The hash: pinned integer values (identical on every compiler), `hashUnit` stays in [-1, 1) and is near zero-mean, value noise equals the hash at lattice points. Vignette: the center is untouched, the corners go most of the way to black or white, all four corners and the middles of the four sides agree (the ellipse follows the aspect ratio), monotonic from the center out, never brighter (or darker for a positive amount) than the source, and Amount, Midpoint and Feather behave as documented; it depends only on the picture size, so a crop is centered on itself. Grain: deterministic, zero-mean within about 1 level, monochrome, bounded by the amplitude Amount allows, more Amount means more spread, a larger Size makes neighbors agree more, midtones get more than black and white, and changing one pixel's color changes no other pixel's grain. Blocks: every split of the picture (1, 2, 7, 13, 70 or 71 rows, even a cut mid-row) equals the whole picture byte for byte. Alpha is never changed and pixels with alpha 0 stay exactly as they were. |
| `mask_geom` | Radial and Linear falloff written out as formulas, feather limits, row spans cover every non-zero pixel on random shapes, round trips through random `GeoFrame`s, `applySourceEdit` leaves unrelated fields untouched. |
| `mask_raster` | Reading a coverage map at the same size (exact) and at another size (bilinear), row spans cover every non-zero pixel, empty and out-of-range rows. |
| `mask_brush` | A single dab, density, painting and erasing arithmetic (including rounding), flow build-up, map size caps, spans, incremental rasterizing equals a full one, the LRU cache. |
| `mask_rect` | The falloff written out as a formula, hard edge and feather, a quarter turn with swapped half-sizes gives the same mask, sizes are a fraction of the shorter side, the span covers every non-zero pixel and is tight (exact for axis-aligned and turned rectangles), round trip through every frame, `applySourceEdit` touches only changed fields. |
| `mask_polygon` | Geometry helpers (even-odd containment, nearest edge, simplify), fewer than three points select nothing, a hard square's area and half-pixel anti-aliasing, winding and start point do not matter, bow tie / pentagram / concave shapes, feather is centered on the outline and keeps the total weight, a polygon bigger than the picture does not fade at its edge, spans cover every non-zero pixel (including a downscaled map), frame round trip, `applySourceEdit` for a moved / added / removed vertex, the cache. |
| `mask_range` | Luminosity and Color Range written out as formulas, `setLo` / `setHi`, integer luma used for the weight, unset Color Range selects nothing, these shapes are not spatial. The float-level path (`rowWeightsF`): the fine table of 4081 entries (`fineTable`), whole-level grays give exactly the byte version's weights, luma is not rounded to a byte, 16-bit levels give the byte result, and Color Range's float rows equal its byte rows. |
| `mask_object` | Grow, shrink and feather against naive references, the bilinear warp (quarter turns, straighten corners, size cap) and its caches, shape equality by map id, prompts follow the frame. |
| `mask` | The type order matches the variants, Add / Subtract / Intersect as a fold, composite row spans, `Mask` defaults and equality, mask names, `clearMaskCaches`. |
| `sidecar` | Round trip of a rich edit set, the file layout, clamping and defaults for every field, crop validation against the turned picture, object bitmaps, a fuzz test (2,500 cases, cut to 400 when the test is built with AddressSanitizer) where every loadable result must satisfy the editor's invariants. Sync Settings (`syncLook`, `sidecarSyncLook`): a picture with no sidecar needs a size and gets a fresh file; a picture with edits gets the new sliders, curves, Mixer and grading while its crop, orientation, angle, canvas mode and masks are kept; the size comes from the file when none is given and a different size is refused; unreadable JSON, a non-object, a newer version and a file with no picture size are refused with `*outText` untouched; syncing twice gives the same text. Rating, flag and label (`meta`): `sidecarFileText` writes `meta` only when it is not the default (before `edits`) and otherwise gives the old layout byte for byte; a rated file still loads as edits; `sidecarScan` says "not edited" only for a readable file whose `edits` are exactly the neutral ones, and every other kind of file counts as edited; a hostile `meta` is clamped; Sync Settings keeps the target's own `meta` (and adds none to a file or a new sidecar that had none). Batch rating (`patchMeta`): `sidecarPatchMeta` starts a neutral file from the picture's size when there is none, changes only the named fields of an existing file and keeps its edits, checks a given size against the file's, is idempotent, drops the `meta` key when everything is cleared, refuses untrusted files without touching the output, and composes with Sync Settings in either order. Named snapshots (`snapshots`): names are cleaned (control characters and non-ASCII dropped, trimmed, capped at 32 without a trailing space) and a blank one becomes `Snapshot`; with no snapshots `sidecarFileText` gives the old layout byte for byte, and with them `snapshots` comes after `edits`; names and every edit survive a round trip, the live edits are unaffected and writing what was read gives the same text; at most `kMaxSnapshots` are written and read; a hostile `snapshots` is tolerated (an out-of-range slider is clamped, a crop that doesn't fit is dropped, non-objects and entries with no `edits` are skipped, no picture size loads nothing, a non-array gives none); Sync Settings and a batch rating keep the picture's snapshots, each with the look it captured (and add none to a file that had none), a refused file is still refused, and a hostile snapshot in a file being rewritten is validated again; `sidecarScan` ignores snapshots (neutral edits plus snapshots is not "edited", and the meta is still read). `virtualCopyNames`: `sidecarFileOf` gives a copy's own file and does not mistake a look-alike for the marker. The `lut` object: written only when there is a LUT, round trip with and without a hash, hostile values (not an object, no name, a non-string name, a blank or control-only name, a path-climbing name, a 200-character name, a short / non-hex / non-string hash, upper-case hex) never fail the file, a LUT alone makes the picture count as edited, and Sync Settings replaces or clears the LUT. |
| `render_params` | Which masks are kept (a mask whose only effect is a Filter is kept; a Filter that rounds to no effect is not), what each frozen pass holds, `filterValuesFromAdj`, `filmFromAdj` (picks the six Effects sliders and nothing else; a mask carrying only Effects sliders is dropped, and with another effect they change nothing in its pass), shapes mapped through the frame, defaults. The 3D LUT is global only: a mask whose only value is `LUT Amount` is dropped, one with other adjustments ignores it, and no mask pass ever holds a table. |
| `adjustment_set` | Undo coalescing and labels, undo / redo / jumps, the 200-step cap, geometry, curve, mixer, grading and mask steps, `version()` rules, JSON round trip, the tables built from the state (also matching mask tables), and the Filters: a History label, a sidecar round trip, a lone Filter making the global color pass active, and the global edit and a mask reading the Filters identically. The Effects sliders (`effectSliders`): the History label (`Vignette -40`), `buildRenderParams().film` follows the sliders and moves nothing else, undo takes them back one slider at a time, the sidecar holds only the non-zero ones under their labels and loads back, and a preset carries them and replaces the whole look. Presets (`presets`): `applyPreset` replaces the whole look (a slider the preset leaves at 0 is reset) but keeps crop, orientation and masks; it is one step labelled `Preset: <name>` (`Preset` for a blank name); an identical look adds no step and does not bump `version()`; the next slider touch is its own step, not folded into the preset step; the label survives undo, redo and History jumps; a hand-built preset with an out-of-range or NaN slider is sanitized; `currentPreset` returns the live look; and the result survives a JSON round trip. Auto Tone (`autoTonePick`): `setAutoTone` is one step labelled `Auto Tone`, moves only Exposure, Contrast, Whites and Blacks (Brightness, Highlights and Shadows stay), survives undo, redo and the label with it; the same values again add no step and do not bump `version()`; the next slider touch is its own step (even Exposure) and Auto Tone ends a drag in progress; one of the four changing still counts and all zeros on a fresh set is a no-op; hand-built values are clamped (NaN becomes 0) and values that sanitize to the current ones add no step; and the values survive a JSON round trip with a fresh history. White-balance pick (`whiteBalancePick`): `setWhiteBalance` is one step with the label `White Balance -67 / +14` (a zero prints `+0`), moves only Temperature and Tint, survives undo, redo and the label with it; the same values again add no step and do not bump `version()`; the next slider touch is its own step (even Temperature) and a pick ends a drag in progress; hand-built values are clamped (NaN becomes 0) and a pick that sanitizes to the current values adds no step; and the values survive a JSON round trip with a fresh history. Snapshot restore (`snapshotRestore`): `currentEdits` matches `toJson`; `restoreEdits` is one step labelled `Snapshot: <name>` that brings back sliders, crop **and masks** together, and undo puts all of them back in one press while redo restores them and the label; restoring the same state adds no step and does not bump `version()`; the next slider touch is its own step; the name is cleaned (a blank one gives `Snapshot`); a hand-built value is clamped (NaN becomes 0); a restore onto a fresh set gives the snapshot's JSON, and restoring the neutral state onto a fresh set is a no-op. The 3D LUT (`lut`): `setLut` as one labelled step (`LUT: <name>`, `Remove LUT`), the amount following the choice (0 to 100, kept when set, back to 0 on removal), no-op calls add no step, name cleaning, the library's answer (`Ok`, `HashChanged`, `Missing`), the reference reaching `buildColorPass` / `buildRenderParams` as the shared table, undo and redo restoring LUT and amount together, save and load, presets (replace, clear, equality includes the LUT), `restoreEdits` and `reset`. |
| `minijson` | Round trips (floats exactly), escapes and surrogate pairs, malformed input rejected, depth limit, base64 and run-length coding. |
| `exif` | Orientation table, re-embedding into a plain JPEG (orientation reset, thumbnail unlinked, size tags patched, ICC kept), a 100,000-case fuzz test of damaged headers, `stripGps` (coordinates zeroed not just unlinked, the rest of the EXIF intact, both byte orders, bad pointers and types, a second fuzz loop over a GPS-bearing file), and `readPhotoInfo` with the display text: every field in both byte orders (inline and offset values, SHORT and LONG ISO with the 65535 escape, DateTimeDigitized fallback), text cleaning (padding, control characters, non-ASCII, NULs, the 128-character cap, wrong types, lying counts), rejected numbers (zero denominators, absurd values, wrong types), bad or missing Exif sub-IFD pointers, the shutter / aperture / focal length / ISO / camera formatters including infinities and NaN, and a 100,000-case fuzz loop that also checks every returned value is in range and printable (run it under a sanitizer). |
| `file_utils` | Paths and UTF-8, extensions, natural order (and its deterministic tie-break), `readFilePrefix`, atomic writes (failure cases, no leftover temp file), `listImageFiles`. |
| `virtual_copy` | The copy path marker: `vcPath` / `vcSplit` round trip for every number 1 to 9, and every near-miss is *not* a copy (no name before the marker, `vc0`, `vc10`, a letter, capitals, extra characters, a missing digit); `vcImageOf` / `vcCopyOf`; the file names (`vcSidecarFileOf`, and `vcParseSidecarName` accepting the extension in any case but rejecting plain sidecars, `vc0`, `vc10`, capital `VC`, missing extension); `vcFreeId` (lowest free number, a deleted number is reused, 0 when all nine are taken); and `listVirtualCopies` on a temp folder (keyed lower-case, ascending, ignoring plain sidecars, over-long numbers and directories, a missing folder, and the entry cap). |
| `image_meta` | `exifOrientationOf` (all 8 values, both byte orders, bad input, the 256 KiB window) and `embedSourceMetadata` (layout, patched sizes, the old thumbnail erased, refusals, and `removeGps`: location kept by default, removed on request, everything else identical, source untouched), and `photoInfoOf` (both byte orders, any extension case, a virtual copy's path reading the picture behind it, a JPEG with no EXIF, refusals that reset the output, the 256 KiB window, truncated files). |
| `preset` | Names: `sanitizePresetName` drops control characters, DEL and non-ASCII, trims, caps at 32 and leaves no trailing space after the cap. `presetFileStem` replaces the characters Windows forbids, strips a trailing dot, prefixes the reserved device names (`COM1` yes, `COM10` no), and gives `preset` for a blank name. The JSON round trip (as a value and through text) is exact, and a neutral look serializes to four empty groups. Refusals: not an object, no `kind`, a `kind` other than `preset`, a `version` that is too new, 0 or a string, each leaving the output untouched. Hostile content is clamped: an out-of-range slider, a non-number, a negative value on a 0..100 slider, a junk curve (becomes the identity) and a control-character name. Files in a throw-away folder: a missing folder lists nothing, a blank name is refused, a nested folder is created, no `.tmp` is left, save then load is equal, the same name overwrites, listing is in natural order (`Cool 2` before `Cool 10`) and ignores stray files and a sub-folder named like a preset, bad files (invalid JSON, wrong kind, missing, over 1 MiB) fail with a message and leave the output untouched, a nameless preset takes its file name, and delete reports true then false. The LUT (`lut`): stored by name and hash, only when set, with `LUT Amount` in `adjust`; two presets that differ only in the LUT are different; hostile `lut` objects become no LUT while the rest loads; the file stays tiny. |
| `raster_ops` | Crop, quarter turns and mirror, `orientImage` and `cropImage` checked pixel by pixel against `GeoFrame` and the EXIF table, `rotateArbitrary`, the preview downsample, and the export resize: `fitLongEdge` (never enlarges, exact long side, short side within half a pixel), `resizeArea` (flat areas exact at any ratio, the 2 × 2 mean at half size, fractional weights, alpha-weighted color, not flipped, mean preserved) and `resizeToLongEdge` (no copy when nothing changes). |
| `sam_segmenter` | The stand-in `Segmenter` of a build without ONNX (the test body is skipped if `LITEROOM_ONNX` is defined, which the test target never does): `load` fails with exactly `built without LITEROOM_ONNX`, `encode` and `decode` fail with a non-empty error, `loaded()` and `encoded()` stay false, a null `errorOut` is fine. `sam::preprocess`, the resize / normalize / pad step `encode` feeds the model: output size for wide, tall, tiny and 1-pixel-high pictures, buffer size per layout (rank 4 the full padded square, rank 3 only the resized picture), box average when shrinking (whole and fractional factors), nearest when enlarging, ImageNet normalization in R, G, B planes, zero padding after normalization at the bottom (wide) or right (tall), the raw HWC layout checked pixel-exactly at scale 1, alpha ignored in both layouts, invalid input (null pointer, zero or negative size). |
| `seg_basic` | Preparing a picture, the color tolerance, 4-connected growth, the 5 × 5 seed patch, cancellation, the wand's prompts, reuse of the prepared picture. |
| `render` | An untouched set reproduces the input, tone is an exact table lookup, 128-row blocks and one-row renders give the same result, cancellation, masks blend as the formula says, later masks see earlier ones. Vignette and Grain (`filmEffects`): the render equals the other passes followed by `film_fx::apply` on the whole buffer, at heights that cross the 128-row blocks; alpha untouched, a transparent corner stays as it was, the background path gives the same bytes, and the effects run after the masks. |
| `render_core` | The float pipeline (`render/render_core.hpp`) against `renderWithBytes` (`render_bytes.hpp`, the old byte pipeline verbatim). One stage at a time (a tone slider, the color pass, one neighbor effect, the three detail layers together, the film effects) gives **exactly** the old bytes. Stacked stages stay close and the gap is measured: master plus channel curve within 2 levels, random stacks with a mean gap under 0.8 level for continuous effects (under 2 with Threshold / Posterize) and a small bounded share of samples more than 8 levels off. Blocks do not change the result, masks read the row before they edit it, cancellation. **16-bit:** a widened 8-bit picture gives the 8-bit result (exactly, or within 4 levels when a neighbor effect is on; a 16-bit result narrowed is within 1 more), a 16-bit edit keeps its sub-8-bit precision (the result is within 1 sample of the exact 16-bit value), and a smooth 16-bit gradient stays smooth through Texture / Clarity / Sharpness (within 16 samples). This is the only test of the neighbor effects on 16-bit sources. |
| `wide_chain` | The 16-bit twin of the picture chain (`core/wide_chain.hpp`) against the 8-bit chain (`orientImage`, `rotateArbitrary`, `cropImage`) over 60 random orientation / angle / crop combinations. Every stage has the size of its 8-bit namesake; a pure move (orientation) equals the widened 8-bit stage exactly; a resample (straighten, and the crop after it) is within 131 samples, one 8-bit rounding; identity stages hand back the very same object. An 8-bit file has no twin (every stage stays null), and a superseded straighten gives null from a non-null input (0 degrees has nothing to supersede). |
| `export_render` | The pixel half of exporting (`workers/export_render.hpp`): `renderForExport` (orient, straighten, crop, render) from an 8-bit or a 16-bit original to an 8-bit or a 16-bit result. An untouched picture keeps every sample (8 in, 8 out; 8 in, 16 out is the widened picture; 16 in, 16 out every sample; 16 in, 8 out the nearest byte). The geometry chain equals the same steps done by hand with the raster operations (exact for quarter turns and a crop; within 131 samples in 16 bits where it resamples). Precision: a strong vignette on flat gray gives the 16-bit result more than 4 times the distinct values of the 8-bit one while narrowing to it within 1, and two samples 3 apart stay apart through a Brightness edit. A superseded call returns false in every combination. `resizeToLongEdge` on 8- and 16-bit results (never enlarges, nothing below 1, an empty result is safe, 16-bit within 130 of the 8-bit result times 257). |
| `image_codec` | `flattenRow` over white, PNG round trip (lossless, alpha kept), JPEG round trip, channel order, quality and 4:4:4 chroma, the size limit (`jpegSizeOk`; the encoder itself refuses empty and oversized pictures, so no caller can skip it), metadata embedded into real encoder output. |
| `image_codec16` | 16-bit PNG reading and writing (libpng on Linux, WIC on Windows): the header parser; the decoder against PNG files made by an independent writer (distinctive 16-bit values, so a byte-order slip cannot hide); a round trip; the encoder's byte order checked through LiteUI's 8-bit decoder (each 8-bit value within one of v / 257); the fallback to the 8-bit decoder; errors. The widen / narrow conversions of `core/image16_convert.hpp` are tested in their own `cases/image16_convert.inc`. |
| `image_source` | How a file is opened (`io/image_source.hpp`), with real files. `depthOf` reads the PNG header (16-bit is wide; 8-bit, junk and a missing file are narrow). A wide file keeps its 16-bit samples untouched and its 8-bit form is the nearest byte of them (no 8-bit copy unless `want8`); a narrow file has no 16-bit twin; the orientation is applied to both. Failures (missing, junk, a cut-off 16-bit PNG) give a message and no crash, also with a null error pointer. **A truncated PNG** (cut after the signature, mid-header, mid-data, inside IEND, one byte short) is refused the same way on every platform with `kTruncatedMessage`, by `openUpright`, `uprightSize` and `decodeForThumb`; bytes after IEND are tolerated; a non-PNG, empty or missing file is left to the decoder. `uprightSize` for the orientations, and `decodeForThumb` gives the plain 8-bit decode, not turned. |
| `histogram` | Counts and luma, alpha skipped, clip bits, strided sampling, the clipping overlay in classic and per-channel mode, block reduction past 2048 px. |
| `scopes` | The view cycle (`nextKind` wraps and survives out-of-range values) and names. `sampleStep` against the histogram's stride, `density` (zero, saturated, exactly half). `rgbToCbCr` for black, white, grays, the three primaries and every corner of the RGB cube. Waveform: empty, null and fully transparent input give nothing; a solid picture lights exactly one row per channel in its own color; black left and white right land on the bottom and top rows of the right columns; a 1000-pixel-wide picture folds into 256 columns with the split exactly at column 127 / 128; a column with 1 % of its pixels on a level is half as bright as a full one; alpha skipped; strided sampling (2000 x 2000 gives 667 x 667 samples, every column filled, a pixel off the grid is invisible). Vectorscope: gray lands in the center cell, red in the top row, blue in the right column; a lone pixel among thousands is held at `kVecFloor`; `render` returns null for `Histogram` and for empty input. |
| `lut3d` | Names (`sanitizeLutName`: ASCII only, forbidden characters become `_`, capped, no path separator survives; `lutFileStem`'s reserved names), the hash hex round trip and its refusals, `valid()`'s refusals, an identity table at sizes 2 to 65 (random points, the corners, the whole gray axis stays gray), the **tetrahedral** lookup exact for an affine table with all six tetrahedra exercised, exact at the grid points of a non-affine one and continuous across cells, clamping of NaN, infinity and out-of-range input, the input domain (a 0..0.5 domain, a negative minimum), `makeLutPass` (no table, invalid table, NaN, amount 0 give the inactive pass; amount clamps to 100) and `applyLutPixel` blending. |
| `lut_cube` | The entry order (red fastest), the accepted syntax (CRLF, comments, title, case, tabs, `+`, exponents, `DOMAIN_*`, `LUT_3D_INPUT_RANGE`, the video-range flags), every refusal with its message and line number (1D, size 0 / 1 / 66, twice, data before the size, too few or too many entries, the wrong number count, `nan`, `1e999`, absurd values, a decimal comma, an unknown keyword, a keyword after the data, a bad domain), failure leaves the outputs untouched, **locale independence** (the test's own cube text is made with `std::to_chars`, so a decimal-comma locale cannot spoil the test either; this was a real bug once), the parse time of a 65³ file (printed), and the log-LUT hints (`SLog3`, `V-Log`, `LogC3`, `Log3G10`, `F-Log2` match; `Analog`, `Catalog`, `Dialog` do not; domain; mid-gray). |
| `lut_library` | The hash, names and paths (a hostile name stays inside the folder), import (bytes copied as they are, the same bytes reuse the file, `-2` / `-3`, cleaned names, a long name's suffix fits the cap, refusals: not a `.cube`, missing, empty, invalid, 1D, too large, a full library, and nothing is copied on failure), `listLutFiles` / `listLutEntries` (natural order, other files and folders ignored, only names an edit can point at again), `acquireLut` (`Ok`, `HashChanged`, `Missing`, `Unreadable`, the file's own hash, cache hits, failures remembered until `clearLutCache`, the four-entry cap, `lutCachePut`, six threads asking at once), and `loadLutFile` bypassing the cache. |
| `lut_render` | The LUT through the real `renderWith`: it gives the picture's negative, half amount is half, nothing changes for amount 0, a missing LUT, an amount with no LUT or an identity table, the LUT comes after the Filters and the other color stages and **before** the masks and Vignette / Grain (rendering the LUT first and the mask afterwards gives the same bytes as both at once), and rows cut into blocks agree with the whole picture. |
| `lut_preview_worker` | Every preview equals the real render of the small picture with that LUT at 100 %, byte for byte, with the picture's own LUT **replaced** (not stacked) and the neighbor effects left out; results come in order with the file's hash, size and warning; unusable files are error results while the rest go on; with no picture there are results without thumbnails; a new job replaces the old one (nothing of the old one arrives after `submit`); `cancel`; shutdown (twice, with work queued, never started); and previews leave the LUT cache untouched. |
| `lut_pane` | The LUTs pane's logic without a window (it compiles `main.cpp`): the list and its previews arriving, the row texts, applying a LUT (one step, the status text, and no new batch of previews), "already using", broken rows, a drag in progress, replacing a LUT keeps the amount, staleness (an edit elsewhere starts a batch, but not during a drag, with the pane closed or with no picture; another picture clears the old previews), removing and undoing, importing (good, bad, all bad, log-named, the skipped count, the same file twice), re-reading an unchanged folder keeping the previews and starting no batch, a file coming or going, and the status for a missing, unreadable or changed LUT. |
| `latest_worker` | See [kind 2](#2-cases-that-use-liteui-or-start-threads) above. |
| `thumb_worker` | `makeThumb` sizes and rounding, FIFO order, queue replacement, failed decodes, the `edited` flag, EXIF orientation, shutdown. The sidecar scan: a file holding only a rating is not "edited" and still delivers its rating, flag and label; a file with real edits is edited and delivers them too; no sidecar, unreadable JSON and a file from before `meta` give the default; a picture that can't be decoded still delivers its `meta`. Virtual copies (`virtualCopy`): a copy's path is read for its own sidecar only, so it delivers `thumbSkipped`, no thumbnail, its own `edited` and `meta`, even when its picture does not exist, while the picture itself is decoded as before. |
| `export_worker` | Batch export, with real files and the real decoder. Names: `exportStemOf` (extension dropped, `-copy2` for a virtual copy), `exportTargetPath` (`-edit`, then `-2`, `-3`; an existing file is skipped unless overwriting; a name used in this batch always wins; names compare without case). `exportPicture`: an unedited picture comes back **pixel-identical**; crop plus Exposure plus Saturation, and orient plus straighten, are compared pixel for pixel against `cropImage` / `rotateArbitrary` / `renderWith` run directly, so the worker's chain cannot drift from the editor's; Resize shrinks and never enlarges; JPEG with the metadata options; a virtual copy exports its own sidecar and its own name while the picture itself is unaffected; an unusable sidecar (wrong size, garbage), an undecodable picture and an unwritable folder each give a skipped outcome with a reason; a superseded call is `cancelled` and writes nothing. `ExportWorker`: order, one outcome per picture, the notify per result, a second `start()` refused while busy, a fresh name on the next batch (and the same name when overwriting), a failure not stopping the batch, `cancel()` (the picture in flight finishes, the rest come back `cancelled`, the worker is reusable), shutdown with work queued. The 3D LUT (`lutExport`, `missingLutIsReported`): a picture with a LUT from a real library file exports exactly what `renderWith` gives, a changed file is used, a missing or damaged one skips the picture with a reason that names the LUT (unless the amount is 0), and in a batch the others go on. |
| `photo_meta` | `PhotoMeta` JSON: defaults are left out, every rating / flag / label combination round-trips, hostile input is clamped (an out-of-range or non-number rating, unknown flag or color names, a non-object), a hand-built value is sanitized. `MetaPatch`: only the named fields change, a set-to-zero is a change, out-of-range values are forced into range. `MetaFilter`: off matches everything, "at least N stars", OR within flags and within colors, AND across the three, toggles (the value already set switches off, out-of-range ignored). |
| `color_tag` | The color-space tag code (`io/color_tag.hpp`) on synthetic PNG and JPEG data; the exact cases are in `cases/color_tag.inc`. |
| `image16_convert` | `widenImage` and `narrowImage` (`core/image16_convert.hpp`), in `cases/image16_convert.inc`. |
| `render_bytes` | The old byte pipeline kept as a reference (`render/render_bytes.hpp`), in `cases/render_bytes.inc`; `cases/render_core.inc` compares the float pipeline against it. |
| `golden_render` | See [the golden render test](#3-the-golden-render-test). 344 scenarios, all 8-bit. |

### Habits the tests share

If you write a new test, these patterns will look familiar:

- **A naive reference beside the fast code.** Blurs, morphology and rotations are checked against a slow, obvious version written in the test (`refBlur`, `refMorph`).
- **Behavioral checks with loose thresholds.** `neighbor_fx` has no naive reference for its chroma, veil and blur maps. It checks what each effect should do (noise at least halved, edges kept, error against a known clear scene, monotone blur) with generous bounds. A small numeric drift in dehaze or blur would pass; a wrong direction or a lost edge would not.
- **The old code kept as a reference.** `cases/render_core.inc` runs the byte pipeline (`render_bytes.hpp`) next to the float one: exact where a single stage does the work, bounded and *measured* (the test prints the gap) where stages are stacked. Deleting `render_bytes.hpp` would remove that reference.
- **"The span covers every non-zero pixel"** is checked on random shapes for every mask type, because a span that is too small would silently cut off part of a mask.
- **Written-out numbers.** Several tests (`adjustments`, `mask_geom`, `mask_range`, `mask_object`, `mask_brush`) repeat a formula and its constants from the design, so a changed constant or a refactored formula fails loudly.
- **Cross-module ties.** `raster_ops` is checked against `GeoFrame` and the EXIF table, and mask tables against the global table builders, so two copies of the same recipe cannot drift apart.
- **Fuzzing damaged input** (`exif`, `sidecar`, `minijson`): the loader must reject or clamp, never crash or hand back something the UI could not produce. Run these under a sanitizer (below).

## Manual checks: copy, paste and sync settings

The UI parts have no automated test (see "Known gaps"). With a folder of at least five photos, one of them a portrait phone photo (it has an EXIF rotation), and no sidecars, check:

**Copy and Paste (open picture)**
1. Edit `a`, press Ctrl+Shift+C ("Copied settings from a"), open `b`, press Ctrl+Shift+V. `b` takes the look, and History has one step, `Preset: Pasted`. Ctrl+Z and Ctrl+Y move it back and forth.
2. Paste again straight away: "Already has these settings" and no new step.
3. Crop and rotate `c`, then paste. The crop and rotation stay.
4. Paste before ever copying: "Copy settings from a photo first".

**Sync (filmstrip selection)**
1. Ctrl+click two photos (outlined), Ctrl+click one again (deselected), Shift+click for a range, Ctrl+A for all. A plain click clears the selection and opens that picture.
2. With the look set on `a`, press Ctrl+Shift+S. Expect "Syncing..." then "Synced settings to N photo(s)", a `.literoom.json` for each, no `.tmp` left over, and the "has edits" dot. Opening a synced photo shows the look, with History starting at "Original".
3. A photo that already has a crop, rotation and mask keeps all three; only the look changes.
4. The portrait phone photo (no sidecar before the sync) opens afterwards with no "size has changed" message. If it appears, the width and height swap in `runSyncJob` is wrong.
5. Nothing else selected: "Select other photos in the filmstrip first". Ctrl+A then sync: the open photo is skipped.
6. During a large sync, try to open another picture or folder: "Wait for the sync to finish", and the sync still completes.
7. A sidecar broken by hand, one with `"version": 99`, and one for a replaced picture of another size are each skipped with a reason, and their files are left untouched.
8. Close the app during a sync: it exits cleanly after finishing the batch.

## Manual checks: white-balance picker

The button, the click and the keys have no automated test (see "Known gaps"). Open a photo with a visible color cast and something that should be gray (a gray card, concrete, an overcast sky), plus one with a blown-out area.

**The pick**
1. Open the Adjust pane. **Pick White Balance (W)** is under the Color header, above Temperature. Click it: the button gets a border and the status bar says to click a neutral gray.
2. Click a spot that should be gray. Temperature and Tint jump, the cast goes, the button highlight turns off, and History has one step, `White Balance <t> / <n>`.
3. Hover the same spot: R, G and B in the readout are within about 2 or 3 of each other.
4. Ctrl+Z puts both sliders back in one step; Ctrl+Y brings them back.

**Spots that cannot be used** (each shows a message, and the picker stays armed so you can retry)
1. A blown-out area: "clipped".
2. A near-black area: "too dark".
3. Outside the picture (the canvas margin): nothing happens.
4. A strongly colored object (a red car): the pick is applied with a note that the spot cannot be made fully gray, and one slider is at ±100.
5. Pick the same spot twice: the second says "already set" and adds no step.

**Disarming** (arm it each time; the highlight must go off and a click on the picture must do nothing)
1. Esc. W again toggles it off, and W from another pane opens Adjust and arms it.
2. Switch to another pane and back: it stays off.
3. Turn on Before / After (`\`) or the split view (Y).
4. Open another picture while armed.
5. Type a "w" in the preset or mask name field: the picker must not toggle.

**Geometry and sampling**
1. Zoom in and pan, then click a gray far from the centre: that spot is neutralized.
2. Flip, rotate, crop and straighten, picking after each, and check the readout again. This is the case most likely to show a wrong mapping from click to pixel.
3. Repeat on a large photo (the background render path).

**With other edits**
1. Change Exposure, Contrast or a per-channel curve first, then pick: the spot still comes out gray, because the picker reads the picture after the tone tables.
2. A mask with its own Temperature is not changed by a pick.
3. Close and reopen the picture: Temperature and Tint are still set (and History starts at "Original"). Copy and Paste Settings, and a saved preset, carry them too.

## Manual checks: Auto Tone

The button, the A key and the menu item have no automated test (see "Known gaps"). Open four photos: a normal well-exposed one, a flat and hazy one, a very dark one (a night shot) and one with blown-out highlights. Use the Adjust pane.

**The press**
1. **Auto Tone (A)** is the first item under the Tone header. Click it on the hazy photo: Exposure, Contrast, Whites and Blacks jump, the picture gets visibly punchier, and History has one step, `Auto Tone`. The status bar lists the four values.
2. Ctrl+Z puts all four back in one step; Ctrl+Y brings them back.
3. Press it again: the status says the tone is already set and History gets no new step.
4. Change Exposure by hand, then press it again: Exposure goes back to the same value as the first press (the answer does not depend on the sliders already set), and it is a new step.

**Different pictures**
1. The well-exposed photo changes only a little (a few points on each slider).
2. The night shot stays dark: the dark areas are not turned gray. The status may say the slider ranges are not enough.
3. The blown-out photo: Exposure is not raised, and Whites lowers the top.
4. A solid-color picture (or a very small one): "almost no tonal range" and nothing changes.

**What it leaves alone**
1. Brightness, Highlights, Shadows, the curves and the color sliders keep their values. A mask's own sliders are not touched.
2. Close and reopen the picture: the four sliders are still set (and History starts at "Original"). Copy and Paste Settings, a saved preset and Sync Settings carry them.

**Keys and state**
1. The A key and Image > Auto Tone do the same as the button. Type an "a" in the preset or mask name field: Auto Tone must not run.
2. The button is dimmed with no picture open and while a slider is being dragged.
3. Rotate or crop a large photo and press it straight away: it waits ("Wait for the changes to finish applying") instead of measuring the old geometry.
4. After a crop, press it: the numbers come from the cropped picture, not the whole one.

## Manual checks: snapshots

The pane and its buttons have no automated test (see "Known gaps"). Open a photo, crop and rotate it, add a radial mask, and move a few sliders.

1. Open the Snapshots pane (the `S` button). Type `Golden`, press Save snapshot (or Enter): the status bar says "Saved snapshot: Golden", the name field empties and the row is selected. Press Save with the field blank: a second one named `Snapshot 1`.
2. Change the sliders, remove the crop, delete the mask, then click `Golden`. Everything comes back **including the crop and the mask**, as one History step labelled `Snapshot: Golden`. Ctrl+Z puts all of it back in one press.
3. Click `Golden` again straight away: "Already looks like "Golden"" and no new step.
4. Close and reopen the photo (or open another and come back): the snapshots are still listed, the History starts at "Original", and clicking one still works. Check that `photo.jpg.literoom.json` has a `snapshots` array after `edits`.
5. Delete a snapshot: select it, press Delete selected snapshot. It is gone after a reopen.
6. Add 20 snapshots: the 21st says "A picture holds at most 20 snapshots". The Save button dims.
7. With the filmstrip: select another photo, Ctrl+Shift+S (Sync Settings), then rate it with a batch rating. Open that photo afterwards: its own snapshots are untouched by both.
8. A photo with neutral edits and one snapshot shows no "has edits" dot.
9. Hand-break the file: put `"snapshots": 5` in a sidecar (the picture still opens, with no snapshots) and delete one snapshot's `edits` (that entry is skipped, the others load).

## Manual checks: virtual copies

The filmstrip code, the menu items and the pixel reuse have no automated test (see "Known gaps"). Use a folder with at least three photos, one of them a portrait phone photo (EXIF rotation), and edit the first one.

1. **Create.** Open `a` and choose Image > Create Virtual Copy (the menu shows Ctrl+Shift+N, but that key is not bound). A new cell with a `C1` badge appears right after `a`, it opens at once without a visible decode, and it has `a`'s edits. `a.jpg.vc1.literoom.json` exists next to the picture. The status bar says "Created virtual copy 1".
2. **Independence.** Change the copy (for example make it black and white). Click `a`: it still has its own look, and the copy keeps its. Both keep their thumbnails' "has edits" dots; the copy's thumbnail is `a`'s until it has been opened and left.
3. **Own rating and snapshots.** Rate the copy 4 stars and save a snapshot in it. `a` has neither. Restart the app: the copy is still next to `a`, with its rating and its snapshot.
4. **Undo history** starts at "Original" every time you switch, in both directions, and Ctrl+Z never crosses from one copy to the other.
5. **Several copies.** Create copies 2 and 3 from `a`, then from copy 1. A tenth is refused ("at most 9 virtual copies"). Delete copy 2 (Image > Delete Virtual Copy: the first press asks, the second deletes) and create another: it reuses number 2.
6. **Delete.** On the original, Delete Virtual Copy says "Only a virtual copy can be deleted". After deleting a copy the original opens, the cell is gone, the file is gone, and the other copies are still there. Switch away between the two presses: the next press asks again.
7. **Sync and ratings.** Select `b` and copy 1 of `a` (Ctrl+click) while `a` is open and press Ctrl+Shift+S: `b` and the copy's own sidecar get the look, `a` is skipped. Batch-rate the same selection: both change. Selecting a copy while *it* is open skips it, as for any open picture.
8. **Filter.** Rate copy 1 five stars and filter for five stars: the copy shows without `a`, and the open picture always stays in.
9. **Export** from a copy: the suggested name is `a-copy1-edit.jpg`, the file has the copy's edits and `a`'s EXIF.
10. **The portrait phone photo.** Create a copy of it and reopen the folder: the copy opens with no "size has changed" message (its sidecar records the picture's upright size, taken from the open picture).
11. **Folder reopen.** Open another folder and come back (File > Open Folder): the copies are listed after their pictures.
12. Delete `a.jpg`'s sidecar by hand but leave `a.jpg.vc1.literoom.json`: the copy still lists and opens.

## Manual checks: batch export

Open a folder with a few pictures: `a` edited (crop, a slider, a mask), `b` untouched, `c` a portrait phone photo, and a virtual copy of `a` with a different look.

1. **Selection.** Ctrl+click `a`, `b` and the copy, then File > Export Selected... (or Ctrl+Shift+E). Pick a folder. The status bar counts up and ends with "Exported 3 photo(s) to <folder>". With nothing selected it says to select pictures first.
2. **Each picture has its own look.** `a-edit.jpg` has `a`'s crop and edits, `a-copy1-edit.jpg` has the copy's, `b-edit.jpg` looks like `b`. All three match what the editor shows (and what single Export gives).
3. **The open picture.** Make an edit on the open picture and export without waiting: the file has that edit (the batch flushes the sidecar first). Starting a batch in the middle of a slider drag is refused.
4. **Metadata and size.** With "Keep EXIF" on, the JPEGs have the real picture's EXIF (a copy gets its picture's); with Remove location on, no GPS. With Resize on, long edges are the chosen size, and a smaller picture is not enlarged.
5. **Names.** Export the same selection again: the files get `-2`. Turn on "Overwrite existing files" and export again: the original names are rewritten, and no extra files appear. Two pictures that would get the same name (`x.jpg` and `x.png`) never overwrite each other.
6. **Export All Shown.** Filter the strip to five stars: only those are exported, in strip order.
7. **Stop.** Export about 20 large pictures and press Stop Export (File menu or Export pane): the status says "Export stopped: N exported, M not exported", the picture being rendered writes no file, and the app stays responsive throughout. A new batch can start afterwards.
8. **Skips.** Damage one picture's sidecar (or edit it for a different size): that picture is skipped with a reason in the summary ("Exported 2, skipped 1 (...)") and the others are exported. A read-only destination gives a skipped count, not a crash.
9. **The portrait phone photo.** Its export is upright, and its crop and masks land where they do in the editor.
10. **Not blocking.** While a batch runs you can open pictures and edit (export only reads sidecars). Starting a second batch is refused with "Wait for the export to finish". Closing the app mid-batch is quick.

## Manual checks: 3D LUTs

The logic is covered by `lut_pane`, `lut_render` and the others; what no test sees is how the pane looks and feels, and the path from a saved file to an export. Import a few `.cube` files first (a creative LUT or two, a camera-log one, and one 65³ file if you have it).

1. **Saved with the picture.** Apply a LUT, wait for autosave, close the picture and reopen it: the LUT and its amount are back and the pane highlights the right row.
2. **Export.** Export that picture, single and in a batch: the file shows the LUT and matches what the editor shows.
3. **Missing.** Remove that LUT's `.cube` from the library folder and reopen the picture: the status bar says it is missing, the picture shows without it, the amount slider is still there. A batch export that includes it skips that picture with a reason and carries on with the rest. Put the file back and reopen: the look is back.
4. **Changed file.** Replace the file with different content under the same name: the picture still uses it, and the status bar says the file has changed.
5. **Presets.** Save a preset with a LUT and apply it to another picture; then apply a preset without a LUT: the LUT is cleared.
6. **Copy, Paste and Sync.** Copy settings on one picture and paste on another, then Sync Settings across a few: the LUT carries over, and a picture that had another LUT loses it.
7. **Virtual copies and snapshots.** Give a virtual copy its own LUT: neither picture is affected by the other. Take a snapshot, change the LUT, restore the snapshot; then undo and redo through the LUT steps (History reads `LUT: <name>` and `Remove LUT`).
8. **Click anywhere on a row.** Click on the preview itself, not only on the name.
9. **Many LUTs.** Import about 20: the list scrolls, the current row is highlighted, previews refresh when you change the exposure or open another picture, and they wait while you drag a slider.
10. **Camera-log LUT.** Importing it shows the warning in the status bar and a note on its row; it still applies.
11. **No picture open.** The rows are names only.
12. **Large LUT.** Applying a 65³ table takes the 100 to 200 ms estimated (unoptimized builds are slower); dragging Exposure with it on, on a large picture, still feels smooth, because the render workers only hold the finished table.

## Sanitizers

Sanitizers are a CMake switch. Configure with `-DLITEUI_SANITIZE=ON` (the presets `msvc-debug-asan` and `gcc-debug-asan` in `CMakePresets.json` do this) and **`literoom_test`** and **`literoom_lut_pane_test`** are built with AddressSanitizer, with leak detection off (`NO_LEAK_CHECK`, because they link LiteUI). **`literoom_golden_render_test` is never sanitized**: its fingerprints are float-exact and a sanitizer build changes code generation and stack use. The setup lives in `cmake/LiteuiTests.cmake` (`liteui_enable_sanitizers`), shared with liteui's own tests.

| Toolchain | What `LITEUI_SANITIZE` gives |
|---|---|
| GCC / Clang (Linux, macOS, WSL) | AddressSanitizer **and** UBSan (`-fsanitize=address,undefined -fno-sanitize-recover=undefined`, so undefined behavior aborts). `ASAN_OPTIONS=halt_on_error=1:detect_leaks=0` for the two literoom targets (`NO_LEAK_CHECK`, because they link LiteUI); targets that don't link LiteUI keep `detect_leaks=1`. |
| MSVC / clang-cl | AddressSanitizer only (Visual Studio 2019 16.9 or newer). The Debug flags ASan refuses (`/RTC1`, `/ZI`) are removed once in the root, and the ASan runtime DLL is copied next to each test exe so ctest works outside a Developer Command Prompt. MSVC's STL annotations are switched off (`_DISABLE_ASAN_ANNOTATIONS` and friends) so the tests link with Catch2: use-after-free and overflows are still caught, only container-overflow checks on reserved capacity are lost. No leak detection. |
| MinGW | Not supported: the tests run unsanitized and CMake says so once. |

```powershell
# Windows (MSVC): a Developer prompt / the VS environment is needed because the presets use Ninja + cl.exe
cmake --preset msvc-debug-asan
cmake --build --preset msvc-debug-asan
ctest --preset msvc-debug-asan
```

```bash
# Linux (GCC)
cmake --preset gcc-debug-asan
cmake --build --preset gcc-debug-asan
ctest --preset gcc-debug-asan
```

The build folder is `build/debug-asan`. The test presets turn on `--output-on-failure`, use a 120 s timeout (the same as the CTest entries) and fail when no test is found. Without a preset: `cmake -S . -B build/asan -DLITEUI_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug`.

Run the thread cases and the fuzz cases this way (`latest_worker`, `thumb_worker`, `export_worker`, `lut_preview_worker`, `lut_pane`, `detail_layer`, `neighbor_fx`, and the fuzz loops in `exif` and `sidecar`).

**ThreadSanitizer has no CMake switch, and MSVC has none at all.** To check the thread cases for data races, make a separate build with GCC or Clang on Linux or under WSL (not together with `LITEUI_SANITIZE`: ASan and TSan cannot be combined):

```bash
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/tsan --target literoom_test
ctest --test-dir build/tsan -L threaded --output-on-failure
```

**GCC and UBSan.** The `static_assert` over the slider table used to compare a lambda's function pointer to `nullptr`, which GCC could not evaluate under `-fsanitize=undefined`, so anything that included `adjustments.hpp` failed to build. That check now runs at run time (`cases/adjustments.inc`) and `adjustmentSpecsValid()` no longer looks at `apply`, so the `address,undefined` build should work on GCC too.

## Platform differences

- A PNG that was cut off: WIC hands back a blank picture, libpng refuses it. `image_source` checks for the IEND chunk first so both platforms refuse it with the same message (`cases/image_source.inc`, `truncatedPngIsRefusedOnEveryPlatform`).
- `image_codec` (and `image_codec16`) use libpng and libjpeg on Linux and WIC on Windows, and the test exercises whichever one is compiled. A bug in the other path only shows up when you run the tests there. (`encodeJpegMemory`'s channel-order check exists because the WIC path flattens to BGR.)
- `file_utils`' case-only tie-break check is skipped on file systems that fold case (Windows, default macOS), since both files cannot exist.
- **Windows headers `#define` common words** (`small` is `char`, `near` and `far` are empty, `interface` is `struct`, plus `IN`, `OUT`, `ERROR`), and `liteui.hpp` pulls them in, so they apply inside any header that includes it, directly or not. A local variable named `small` compiled everywhere except under MSVC, where it became `char` and the error read "`std::shared_ptr<const CanvasImage>` followed by `char`". GCC will not tell you; to catch it before MSVC does, compile the new tests with `-Dsmall=char -Dnear= -Dfar=` (an old version of the header fails with the same errors under that flag, which is how the simulation was checked).
- **Locale.** A test that builds text with `printf`-style number formatting breaks under a decimal-comma locale, and the Windows CRT honors `setlocale`. The tests format numbers with `std::to_chars`, and the `lut_cube` cases switch to a German locale where one is installed (a Linux container often has none, which is how this stayed hidden on one platform).

## Where a new test belongs

1. **A new engine header:** put it in the `src/` folder for its layer (see [Where things live](headers.md#where-things-live)); headers are globbed, so there is nothing to add to `CMakeLists.txt`. Then add `tests/cases/<name>.inc` with `TEST_CASE("<name>: <what it checks>")` cases (the prefix is what `ctest -R <name>` matches) and one `#include "cases/<name>.inc"` in `tests/literoom_test.cpp`.
2. **One that takes `CanvasImage` or decodes files:** nothing special. `literoom_test.cpp` defines `LITEUI_IMPLEMENTATION` once for every case (see [kind 2](#2-cases-that-use-liteui-or-start-threads)); do not define it in the `.inc`.
3. **One that runs a thread:** tag its cases `[threaded]`, assert only on the main thread, give every wait a timeout, and loop it with `ctest -L threaded --repeat until-fail:50`.
4. **One that has to compile `src/main.cpp`:** it cannot join `literoom_test`'s shared translation unit. Follow `golden_render_test.cpp` or `lut_pane_test.cpp`: a separate `.cpp` with its own `main()` and its own block in `CMakeLists.txt`.
5. **A new slider, mask type or effect that changes pixels:** add a scenario to `golden_render_test.cpp` as well (sliders are picked up automatically through the table; see "Adding a slider" in [extending.md](extending.md)) and re-record the golden file for each compiler and configuration you use (and see "Checking a re-record" above).

## Known gaps and things to check

- **The real model path is not tested.** `Segmenter::load`, the encoder and decoder runs and the mask pick need ONNX Runtime and a model, and the test target is built without `LITEROOM_ONNX`. `cases/sam_segmenter.inc` covers the stand-in and `sam::preprocess`; `seg_basic` tests the stand-in object selection with the same contract, and the golden test uses it too, so nothing runs the real model automatically.
- **The 16-bit pipeline is tested thinly at the edges.** `cases/render_core.inc` is the only test that runs the neighbor effects (Dehaze, Noise Reduction, Blur, Texture / Clarity / Sharpness) on a 16-bit source, and only through random sliders on a widened 8-bit picture (within 4 levels of the 8-bit result) and one gradient test. `cases/neighbor_fx.inc` builds 8-bit sources only and `cases/detail_layer.inc` checks only the 16-bit blur plane. The golden test renders 8-bit pictures only, so `renderToDisplay` and `renderToImage16` are not pinned by fingerprints. `cases/export_worker.inc` has no 16-bit case (a 16-bit PNG export through the worker is not tested end to end; `cases/export_render.inc` covers its pixels), and `cases/histogram.inc`, `cases/scopes.inc`, `cases/thumb_worker.inc` and `cases/image_codec.inc` use 8-bit pictures only.
- **Presets are covered only below the UI.** `cases/preset.inc` and `presets` pin the file format, the helpers and the undo behavior. Not covered: the Presets pane (list, click-to-apply, save and delete buttons, the focus guard for shortcuts), `presetDir()` beyond "not empty" (the per-platform folder choice and the non-UTF-8 `%APPDATA%` case), and two names that map to the same file stem overwriting each other. The golden test has no preset scenario, so a refactor that changed what a preset does to the rendered picture would not show up there; the Presets code in `src/main.cpp` is compiled into the golden test but never run.
- **Copy, Paste and Sync Settings are covered below the UI only.** `syncLook` pins the sidecar merge, and Paste is `applyPreset`, covered by `presets`. Not covered: the clipboard, the filmstrip selection (Ctrl/Shift-click, Ctrl+A, the selection tint), the sync worker and its status messages, the `syncBusy()` guards, decoding a picture with no sidecar to learn its size (including the EXIF quarter-turn swap, the case most likely to break), and the read-only-folder failure. Like the Presets code, it is compiled into the golden test (which includes `src/main.cpp`) but never run there. Use the manual checks below after touching any of it.
- **The white-balance picker is covered below the UI only.** `whiteBalancePicker` pins the math (against the real `makeColorPass`) and `whiteBalancePick` the single undo step. Not covered: the button and its highlight, the W and Esc keys, `wbPickerActive()` disarming itself, the click-to-pixel mapping under zoom, rotation, flip, crop and straighten, the 5 × 5 patch, sampling through the tone tables, and the status messages. The golden test has no picker scenario, because a pick only sets two ordinary sliders that the existing scenarios already render. The picker code in `src/main.cpp` is compiled into the golden test but never run. Use the manual checks above after touching it, and especially after changing the white-balance gains in `makeColorPass`.
- **Auto Tone is covered below the UI only.** `cases/auto_tone.inc` pins the math (against the real `toneLutFromValues`) and `autoTonePick` the single undo step. Not covered: the button, the A key and the menu item, reading `state.source`, and the messages.
- **Snapshots are covered below the UI only.** `snapshots` pins the file form, the limits and that every rewriter keeps them; `snapshotRestore` the single undo step. Not covered: the pane (name field, Enter to save, the rows, delete), `addSnapshot`'s size guard and its messages, `applySnapshotEntry` refreshing the picture after a geometry change (the case most likely to break: a restore that changes the crop or rotation), and autosave writing when only the list changed (`snapshotsDirty`). Use the manual checks above after touching any of it.
- **Virtual copies are covered below the UI only.** `cases/virtual_copy.inc` pins the path marker and the file names, `virtualCopyNames` the sidecar name and `virtualCopy` (thumb worker) the skip of decoding. Not covered: the filmstrip bookkeeping (`filmInsertEntry`, `filmRemoveEntry`, `filmPushWithCopies`, which keep `current`, `target`, `anchor` and `byName` right when cells appear and disappear; the part most likely to break), `createVirtualCopy` and `deleteVirtualCopy` and their messages and double-press guard, the no-decode switch in `filmOpenIndex`, handing the original's thumbnail to its copies, the badge, and the real-file translations in the decode job, the Sync job and export (a missed `vcImageOf` would try to open a path ending in `|vc2`). The golden test compiles this code but does not run it.
- **The LUTs pane is covered below the view tree only.** `lut_pane` pins the pane's logic (refresh, previews, apply, remove, import, status texts) and `lut_preview_worker` the previews. Not covered: the rows' layout and highlight, the thumbnail canvases and when they repaint, click bubbling from the preview to the row, scrolling with up to 100 rows, the file picker, and the pane opening and closing (`refreshLuts` runs from the pane's display function). The log-LUT hints are heuristics; their thresholds are checked on synthetic tables, not on real camera-log LUTs. The library's cache is emptied by an import and when a picture opens, not when the pane opens, so a `.cube` edited by hand while a picture is open is seen after the next import or reopen; nothing tests that. 1D LUTs, `.3dl` and Hald files are refused, and nothing else tests more of them.
- **The `src/main.cpp` UI itself is not under test.** The golden test reaches the render and geometry pipeline, undo history, sidecar, histogram state and a few helpers, but not widgets, shortcuts, menus or the event loop.
- **`cases/render.inc`'s second mask check** compares a gray channel value against 0.4 where it means luma. This is fine for the gray ramp it uses, but would be wrong for a colored picture.
- **Timing.** The `latest_worker`, `thumb_worker`, `export_worker` and `lut_preview_worker` tests are timeout-guarded, but run them in a loop after touching `latest_worker.hpp`, `thumb_worker.hpp`, `export_worker.hpp` or `lut_preview_worker.hpp` (and `lut_pane` after touching the LUT section of `main.cpp`). The `detail_layer` and `neighbor_fx` thread tests have no timeouts (see above).
- **Input handling and guards in the neighbor effects are mostly untested.** `gainsFromValues` clamping and NaN/inf handling (in both `detail_layer` and `neighbor_fx`), `toU8`'s NaN and infinity rules, invalid arguments to `blurLuma`, `Cache::get` and the two `acquire` functions (null pixels, non-positive size, unknown kind), `apply` with `W <= 0`, and the "plane too small, skip it" guards in `addPixels`, `nrLumaPass` and `nrColorPass`.
- **Cache limits and mid-build cancellation.** Only the entry cap of `detail_layer`'s cache (9) is checked. Its 384 MB byte cap, which entry the LRU drops, and the eviction of `chromaStore` (256 MB, 6 entries) and `mapStore` (64 MB, 40 entries) are not tested. Every cancellation test sets the generation before the build starts, so only the first cancellation check fires; none cancels part-way through a blur.
- **Effect internals and interactions.** `kClarityLimit` soft limiting, the veil's airlight clamp (180 to 255) and transmission floor (0.25), `sampleMap` clamping, the order of effects inside `apply` (dehaze, detail, noise reduction, blur last) and interactions between effects (each is tested alone, or all at once on tiny pictures that check only size and alpha, never pairwise).