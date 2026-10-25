# Testing

Literoom's engine headers have no UI, so almost all of them can be tested without opening a window. This page explains the three kinds of tests, how to run them, what each one covers, and how the **golden render test** protects you when you refactor.

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

Each test is a target named `literoom_<name>_test` and a CTest entry named `literoom_<name>`, so `-R mask_brush` still matches. There are **34** literoom tests (24 plain unit tests, 4 thread tests (`detail_layer`, `neighbor_fx`, `latest_worker`, `thumb_worker`), 5 LiteUI tests and `golden_render`). A plain `ctest -N` from the liteui root lists more (178 at the time of writing, with `preset` and `virtual_copy` included), because it also includes liteui's own Catch2 cases and the `counter` example. To list only literoom's:

```powershell
ctest --test-dir build/release -N -R literoom_
```

The golden render test needs a recorded reference file before it can pass; see [The golden render test](#3-the-golden-render-test).

## The harness: `check.hpp`

Most unit tests share a tiny harness, so there is no test framework to install.

| Piece | What it does |
|---|---|
| `CHECK(cond)` | Records a failure (with file and line) and **carries on**, so one test file reports all its failures at once. |
| `CHECK_NEAR(a, b, tol)` | Passes when `|a − b| <= tol`. Use it for floats. A NaN on either side fails. |
| `return finish();` | Prints `all ok` or `N FAILED`, and sets the exit code (0 or 1) that `ctest` reads. |

`CHECK` is variadic so that braced initializers with commas, like `CHECK(crop == CropRect{1, 2, 3, 4})`, work inside it.

A test file is just a `main()` that calls some functions, makes `CHECK`s, and ends with `return finish();`.

Things worth knowing:

- **It is not thread-safe.** The failure counter (`tcheck::failures`) is a plain `int`. Tests that run threads (`latest_worker`, `thumb_worker`) therefore only call `CHECK` on the main thread; worker threads record into atomics or mutex-guarded state, and the main thread asserts afterwards.
- **The counter is readable.** `sidecar_test` uses `tcheck::failures` to stop its fuzz loop at the first failure instead of printing thousands of lines.
- **Four test files do not use it.** `minijson_test`, `exif_test`, `detail_layer_test` and `neighbor_fx_test` define their own small `CHECK` macro and print their own summary (`all ok`, `all passed` or `N FAILED`). They still return a non-zero exit code on failure, so `ctest` treats them the same. New tests should use `check.hpp`.

## The three kinds of test

### 1. Plain unit tests (no UI, no LiteUI)

One `tests/<name>_test.cpp` per header. Each is compiled on its own against the engine headers. A test includes a header by its path from `src/`, for example `#include "mask/mask_brush.hpp"`; the test targets have `src/` on their include path, and `tests/` too so that `check.hpp` is found:

`minijson`, `exif`, `pixel_math`, `adjustments`, `auto_tone`, `color_mix`, `color_grade`, `color_pass`, `geometry`, `curves`, `mask_geom`, `mask_raster`, `mask_brush`, `mask_range`, `mask_object`, `mask`, `sidecar`, `render_params`, `adjustment_set`, `file_utils`, `virtual_copy`, `image_meta`, `preset`, `sam_segmenter` (24 in all; `detail_layer` and `neighbor_fx` are also engine tests with no UI, but they start threads, so they are built with the thread library, see below).

Tests that touch the disk (`file_utils`, `virtual_copy`, `image_meta`, `preset`, and also `image_codec` and `thumb_worker` below) work in a throw-away folder under the system temp directory (`literoom_<tag>_<timestamp>`), removed again when the test ends. The metadata tests build their own synthetic JPEG files (hand-made EXIF and ICC segments), so no sample photos are needed.

Two of the engine tests, `detail_layer` and `neighbor_fx`, also start a few `std::thread`s to check that the shared caches give the same result however the race goes; they are built with the thread library, see [kind 2](#2-tests-that-need-liteui-or-threads).

### 2. Tests that need LiteUI or threads

Some headers take LiteUI's `CanvasImage` or decoders, or run a real thread, so their tests link more:

| Test | Needs | Why |
|---|---|---|
| `raster_ops`, `seg_basic`, `render`, `image_codec`, `histogram` | LiteUI | They work on a `CanvasImage` (or `Color`). `image_codec` also decodes what it encodes with LiteUI's own decoders. |
| `detail_layer`, `neighbor_fx` | The platform thread library | Their cache tests start a few `std::thread`s to check that concurrent requests for the same plane give the same result. No LiteUI. |
| `latest_worker` | The platform thread library | It runs a `std::jthread`. No LiteUI; it is tested with plain integers and move-only types. |
| `thumb_worker` | LiteUI **and** threads | It decodes real files on a thread. The test writes those files with the app's own encoders (`image_codec.hpp`). |

**A test that uses LiteUI must define `LITEUI_IMPLEMENTATION`.** LiteUI is one header, and its implementation (the Win32 / WIC includes, `toWide`, the decoders, ...) is only compiled when the macro is defined. Each test executable must define it in **exactly one** `.cpp`, before the first include that reaches `liteui.hpp`:

```cpp
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"
// ...then the literoom headers
```

Linking `liteui::liteui` (the `LITEUI` option of `literoom_add_test`) only supplies include paths and libraries; it does not define the macro. Forgetting it gives a flood of undeclared-identifier errors from `io/image_codec.hpp` (`HRESULT`, `CoInitializeEx`, `IWICImagingFactory`, `toWide`, ...) on Windows.

**Thread tests can be flaky in a way that only shows up occasionally.** Run them in a loop to shake out timing problems:

```powershell
ctest --test-dir build/release -C Release -R latest_worker --repeat until-fail:50
ctest --test-dir build/release -C Release -R "detail_layer|neighbor_fx" --repeat until-fail:50 --timeout 60
ctest --test-dir build/release -C Release -R thumb_worker --repeat until-fail:50
```

This repeats the test up to 50 times and stops at the first failure. `--timeout 60` makes a hung run fail instead of stalling ctest; use it for `detail_layer` and `neighbor_fx`, which have no timeouts of their own.

How `latest_worker` and `thumb_worker` are written so they stay reliable:

- They use **gates and latches instead of sleeps**, and every wait has a timeout (5 to 10 s), so a broken worker fails the test instead of hanging it.
- A short `grace()` sleep (100 ms) is used only where a *negative* check has no signal to wait for (for example "nothing was notified after the job returned nothing"). Too short a grace can hide a bug (a false pass) but can never fail a correct worker.
- They check the properties the code's comments promise: the newest request wins, a queued job that is replaced never starts, the notify callback runs on the worker thread and **outside** the worker's lock (the test calls `collect()` from inside notify), `shutdown()` is idempotent, and destruction with work queued does not hang.
- `latest_worker_test` ends with a stress test: 2000 submits mixed with random `invalidate()` calls, checking that a collected value is always the newest submit's.

`detail_layer_test` and `neighbor_fx_test` are simpler: they start a handful of `std::thread`s, `join()` them, and compare the results on the main thread (equal planes for equal keys, identical output pictures). They use no gates and no timeouts, so a deadlock in a cache would hang the test until ctest's own timeout instead of failing it, and a data race is only caught by a thread-sanitizer run (see [Sanitizers](#sanitizers)).

### 3. The golden render test

This is the safety net for refactoring `src/main.cpp`, and it works differently from the others.

It compiles `src/main.cpp` itself (with `main()` renamed so the test can have its own), renders roughly **250** picture/edit combinations (the test prints the exact count at the end of every run), and **fingerprints** each result. It then compares the fingerprints with a saved "golden" file.

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

Recording prints the same summary as a normal run, for example `254 scenarios, 0 mismatches, 0 invariant failures, 1 notes`. Record only while you trust the current output, because the file becomes the reference for later refactoring. Do not re-record just to make a failing run pass.

#### What it renders

| Group (name prefix) | What is rendered |
|---|---|
| `adj/` | Every slider on its own (up, down, barely moved, full range both ends; named by label, for example `adj/Texture/pos`), with nothing set, with all sliders set together, and the Filters stacked (`filters_stack`), over other color stages (`filter_after_color`) and next to the neighbor effects (`filter_with_detail`) |
| `curve/`, `mix/`, `grade/` | Master and per-channel curves, every Color Mixer channel, each grading wheel, blending and balance, a hidden wheel |
| `mask/` | Every mask type and its inverse, parameter extremes (hard and fully soft edges, smallest and largest radii, a one-dab brush, an unset Color Range), Amount, a disabled mask, overlapping masks, composites of Add / Subtract / Intersect, neighbor effects only, masks whose only effect is a Filter (`filter_only`, `filters_overlapping`), and every effect at once |
| `seg/` | The stand-in segmenter's own coverage map, which is then used as an Object mask |
| `geo/`, `geo_masks/` | All 8 orientations, straightening in both corner modes, a crop, and the same mask set rendered through three different geometries |
| `sink/` | The "kitchen sink": every slider (the Filters at a modest 20 %, see below), Mix, Grade, curves and three masks at once, on a 200 × 150 and an odd 97 × 71 picture |
| `history/`, `sidecar/` | The undo labels and indices after a scripted sequence of edits, and the sidecar JSON text |
| `util/`, `thumb/`, `preview/` | Natural sort order, path helpers, thumbnails and the preview downsample |
| `histogram/` | The four histograms, clip flags and clipping overlay, driven through the editor's global `state` (every overlay option, strided sampling, block reduction, no image, mid-drag preview) |

The test picture is made with integer arithmetic only, so the **input** is identical on every platform. It has five bands (a hue sweep, a gray ramp, a pale low-contrast band, a texture band and a band with discs and color patches), and its last column has alpha 200 so a stray alpha change is caught.

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

The test includes `src/main.cpp` today (phase 0); the include resolves because `src/` is on the test target's include path. It reaches into it for `renderAdjusted`, `renderWith`, `clearMaskCaches`, `segmentObjectWand`, `orientImage`, `rotateArbitrary`, `cropImage`, `makeThumb`, `makePreviewBase`, `computeHistogram()`, and the editor's global `state` (the histogram and overlay fields). When you move code out of `src/main.cpp` into headers, those names must stay reachable. After the split, build the test with `-DGOLDEN_USE_ENGINE_HEADERS`, which includes `engine/all.hpp` (that is, `src/engine/all.hpp`) instead of `src/main.cpp`; nothing else in the test should need to change. `CMakeLists.txt` does not set that define yet, so add it to the `golden_render_test` target when the split happens.

The test target is built **without** `LITEROOM_ONNX`, whatever the option says (the ONNX settings are applied to the `literoom` target only), so it always uses the stand-in segmenter and never exercises the ONNX path.

A few habits that make this pay off:

- **Record before you touch anything**, and on a build you trust.
- **Refactor in small steps** and run the check after each, so a failure points at one small change.
- **A failure is not always a bug.** If you *meant* to change the output (a new effect, a fixed formula), re-record the golden file and review what changed in version control.
- **Adding a slider should not change the existing fingerprints** while the new slider is at 0, but the test **does** loop over the slider table, so expect this when you add one:
  - It gains new `adj/<label>/...` scenarios for the new slider. Scenarios are named by **label**, not by table position, so inserting a slider renames nothing.
  - Scenarios that set **every** slider change: `adj/all_pos`, `adj/all_neg`, the `sink/` scenarios, `mask/every_effect`, `mask/composite_every_effect` and `sidecar/sink_json`. In `fullEffect` and `buildSink` the sign a slider gets depends on its position among the sliders that are **not** Filters (`sinkPos`), so inserting a Filter flips no other slider. Inserting any other slider in the middle shifts that position for every later slider, so append it, or extend `sinkPos`.
  - The Filters get `kSinkFilterStrength` (20) in those scenarios, not half their range. At 50 %, Negative and Solarize turn every bright pixel into the same mid-gray, and the fingerprint would stop noticing changes in the tone, color, mixer and grade stages upstream of them.
  - The golden test finds the Filters with `isFilter` (the block from `AdjId::Greyscale` to `AdjId::CrossProcess`). A new Filter added after `CrossProcess` needs that upper bound moved.
  - Any existing single-slider scenario that changes means the new code is affecting untouched pictures: a real bug.

#### Checking a re-record

When a change legitimately alters the kitchen-sink scenarios (a new slider, say), you can still prove that nothing *else* changed. Keep a copy of the old golden file, re-record, then run:

```bash
python tests/golden_compare.py render_<COMPILER>_<CONFIG>.old.txt tests/golden/render_<COMPILER>_<CONFIG>.txt
```

It compares every scenario by name and exits 0 only if every pre-existing scenario has its old fingerprint, apart from the ones that set every slider (`adj/all_*`, `sink/*`, `mask/every_effect`, `mask/composite_every_effect`, `sidecar/sink_json`). It also strips the index from the old index-based names (`adj/14_Texture/pos` becomes `adj/Texture/pos`), so a golden file recorded before the label-based names can still be compared. Anything else that changed, or vanished, is listed as `UNEXPECTED` or `VANISHED`.

To prove that the only change in the kitchen-sink scenarios is the new slider, give it the value 0 in those scenarios first (for the Filters: set `kSinkFilterStrength` to `0.0f`) and run the normal test against the **old** golden file. There should be no `MISMATCH` lines, only `NEW` and `MISSING` for scenarios that were added or renamed. Then restore the value and re-record.

## What each test covers

| Test | What it pins down |
|---|---|
| `pixel_math` | `smoothStep01` (monotone, symmetric, flat at both ends), `toU8Clamped` (exact on whole levels, halves round up, clamps), `kDegToRad`. |
| `adjustments` | The slider table (ids match positions, ranges, stage order Tone → Color → Detail, only Tone has `apply`). Direction and reach of each tone function. `toneLutFromValues`: byte rounding after **each** slider, table order matters, curve applied last, clamping not wrapping. The Filters block: ten Color-stage 0..100 sliders, contiguous between Colorize Amount and Texture, with their exact labels pinned (they are sidecar keys), sanitized into range, and never part of the tone table. |
| `color_pass` | The stage flags (`active`, `mix`, `grade`, `hue`, `colorize`, and each Filter making the pass active), a neutral pass changes nothing, white-balance gains keep luma, grays stay gray under the color-only stages, saturation and vibrance, hue and colorize, the mixer and grading stages, alpha and trailing bytes untouched. Filters: sanitizing, the poster level count (2 at 100, falling smoothly), exact results for Greyscale, Negative, Posterize, Solarize, Sepia, Fade and Vintage, the properties of Bleach Bypass (black and white fixed, more contrast, grays stay gray, color mostly gone, half strength is halfway), Threshold (black or white by luma, not by the brightest channel, and it runs before Negative) and Cross Process (blacks go blue, whites go yellow, mid-gray stays, R has more contrast than G, no channel runs backwards), the fixed order (Greyscale before Sepia), and that Filters run after Color Grading. White-balance picker (`testWhiteBalancePicker`): a warm gray needs a cooler Temperature (the exact numbers for `(180, 150, 120)` are written out), a neutral spot gives 0 and 0, the directions (blue cast warms, green cast gives positive Tint, magenta cast gives negative), and a round trip over a grid of mild casts: the picked values run back through the real `makeColorPass` leave the spot gray to within 3 levels (and enough of the grid is `Ok` that the check really ran). A spot needing more than ±100 is `Limited`, usable, clamped, and ends closer to gray than it started; too dark, clipped and NaN/infinity inputs are refused; the boundaries `kWbMinLevel` and `kWbMaxLevel - 1` are still readable. |
| `auto_tone` | `solveAutoTone` on hand-made histograms. Too few pixels gives `NoPixels` (one short of `kAutoToneMinPixels` vs exactly enough), a single level or a range under `kAutoToneMinSpread` gives `Flat` (exactly the minimum is measurable), and both leave all four values 0. A uniform ramp is left almost alone (Exposure and Contrast within 10) and its 0.5 % and 99.5 % points, run through the real `toneLutFromValues`, land within 6 of the targets. A low-contrast range (80 to 180) gets positive Contrast, lands within 12 of both targets and widens by more than 100 levels. A bright picture gets negative Exposure and its top is not pushed into clipping; with a pile at pure white Exposure never goes positive. A dark picture gets positive Exposure and is `Limited`. Single stray pixels at 0 and 255 do not change the answer, but a pile of about 10 % does; scaling every count by 37 changes nothing; the same input always gives the same answer. A sweep over 236 ranges: always usable, whole values inside the slider ranges and never a negative zero, the table stays monotonic, an end point that was not clipped is not pushed to 0 or 255, and enough of the sweep reaches `Ok` that the check really ran. |
| `color_mix` | Band layout and names, identity table when neutral, a band's center gets exactly its own value, sign symmetry, no seams (including the Magenta-to-Red wrap) on random settings. |
| `color_grade` | Hue to RGB, neutral gives all zeros, the three ranges always add up to 1, tints are luma-neutral, hidden wheels, how Balance and Blending move the crossovers. |
| `color_pass` | Which flags turn on, a neutral pass is a byte-exact no-op, white balance keeps brightness, grays stay gray, saturation +100 doubles chroma, vibrance spares vivid colors, hue direction, colorize, byte-count handling and untouched alpha. |
| `geometry` | `rotatedDims` (expand and fit, symmetry), `GeoFrame` for all 8 orientations, straighten and crop (orthonormal, round trips, centre preserved), `geoStep` (turns, mirrors, crop following the picture), angle wrapping, `snapAngle`. |
| `curves` | Shape through the points, monotone and no overshoot on 3000 random point sets, insert / move / remove rules (minimum gap, end points), identity detection, 20,000 random edits keep the list valid. |
| `detail_layer` | `blurLuma` against a naive reference (`refBlur`: three horizontal then three vertical box passes, replicated edges, rounding after every pass) at radii 1, 2, 5, 30 and 80 (80 is larger than the 61 × 47 test picture), off by at most one 1/16 level. A flat picture stays flat at the borders. `boxRadius` scales with picture size (a 6000 px picture gets 5 to 7 times the radius of a 1000 px one), Texture < Clarity, Sharpness ≤ Texture, never below 1 or above `kMaxBoxRadius`. Cache: a repeat request returns the very same plane, another kind gives another plane, a reused address with different pixels is told apart by the fingerprint, a cancelled build returns null and caches nothing, at most 9 entries are kept (the newest survives), and 8 threads over 3 keys get equal planes and leave exactly 3 entries. `addPixels` on a step edge, for each of Texture, Clarity and Sharpness: contrast across the edge grows, flat areas and alpha do not change, negative Texture and Clarity smooth, negative Sharpness is clamped to 0, all-zero gains leave the picture untouched and build no plane, the same amount goes into R, G and B (no hue shift), and Clarity leaves a near-white edge alone (changes ≤ 1). |
| `neighbor_fx` | Zero gains change nothing (random bytes come back identical, `need()` is 0). The `need()` bit layout: 0 to 2 luma planes (noise reduction reads Sharpness's), 3 chroma, 4 veil, 5 to 7 blur levels. Negative Dehaze needs no map, and Blur 0 to 33 reads level 0, 34 to 66 levels 0+1, 67 to 100 levels 1+2. With only detail sliders set, `apply` equals `detail_layer::addPixels` byte for byte. Luminance noise reduction (noise ±8 beside a hard edge): flat-area noise at least halved, edge height kept above 90 %, half slider lands between, R, G and B stay equal. Color noise reduction (chroma noise at constant luma): chroma spread under 40 %, luma moves by at most 3, a hard red-to-blue edge keeps 80 % of its color difference. Dehaze on a scene veiled with known transmission 0.55 and airlight 235: error against the clear scene falls below 60 %, contrast rises 1.4 times, half strength lands between. Negative values lower contrast and raise the mean, and a haze-free picture is only mildly changed. Blur: spread falls monotonically at 20, 50 and 100, mean kept within 2 levels, slider 100 leaves under 10 % of the spread, a flat picture and slider 0 are untouched, no jump at the level boundary (33.2 vs 33.5), and a red-to-blue edge of equal luma is softened (color blur, not luma-only). Caches: a second `acquire` shares every plane, level 2 is not built at slider 50, a slider drag with the same `need()` builds nothing new, and a cancelled `acquire` returns false and leaves all three caches empty. Sizes from 1 × 1 up to 500 × 3 and 5 × 500 keep the picture size and alpha. Uneven pieces that start mid-row equal the whole picture byte for byte, and 6 threads give identical output. |
| `mask_geom` | Radial and Linear falloff written out as formulas, feather limits, row spans cover every non-zero pixel on random shapes, round trips through random `GeoFrame`s, `applySourceEdit` leaves unrelated fields untouched. |
| `mask_raster` | Reading a coverage map at the same size (exact) and at another size (bilinear), row spans cover every non-zero pixel, empty and out-of-range rows. |
| `mask_brush` | A single dab, density, painting and erasing arithmetic (including rounding), flow build-up, map size caps, spans, incremental rasterizing equals a full one, the LRU cache. |
| `mask_range` | Luminosity and Color Range written out as formulas, `setLo` / `setHi`, integer luma used for the weight, unset Color Range selects nothing, these shapes are not spatial. |
| `mask_object` | Grow, shrink and feather against naive references, the bilinear warp (quarter turns, straighten corners, size cap) and its caches, shape equality by map id, prompts follow the frame. |
| `mask` | The type order matches the variants, Add / Subtract / Intersect as a fold, composite row spans, `Mask` defaults and equality, mask names, `clearMaskCaches`. |
| `sidecar` | Round trip of a rich edit set, the file layout, clamping and defaults for every field, crop validation against the turned picture, object bitmaps, a 2,500-case fuzz test where every loadable result must satisfy the editor's invariants. Sync Settings (`testSyncLook`, `sidecarSyncLook`): a picture with no sidecar needs a size and gets a fresh file; a picture with edits gets the new sliders, curves, Mixer and grading while its crop, orientation, angle, canvas mode and masks are kept; the size comes from the file when none is given and a different size is refused; unreadable JSON, a non-object, a newer version and a file with no picture size are refused with `*outText` untouched; syncing twice gives the same text. Rating, flag and label (`testMeta`): `sidecarFileText` writes `meta` only when it is not the default (before `edits`) and otherwise gives the old layout byte for byte; a rated file still loads as edits; `sidecarScan` says "not edited" only for a readable file whose `edits` are exactly the neutral ones, and every other kind of file counts as edited; a hostile `meta` is clamped; Sync Settings keeps the target's own `meta` (and adds none to a file or a new sidecar that had none). Batch rating (`testPatchMeta`): `sidecarPatchMeta` starts a neutral file from the picture's size when there is none, changes only the named fields of an existing file and keeps its edits, checks a given size against the file's, is idempotent, drops the `meta` key when everything is cleared, refuses untrusted files without touching the output, and composes with Sync Settings in either order. Named snapshots (`testSnapshots`): names are cleaned (control characters and non-ASCII dropped, trimmed, capped at 32 without a trailing space) and a blank one becomes `Snapshot`; with no snapshots `sidecarFileText` gives the old layout byte for byte, and with them `snapshots` comes after `edits`; names and every edit survive a round trip, the live edits are unaffected and writing what was read gives the same text; at most `kMaxSnapshots` are written and read; a hostile `snapshots` is tolerated (an out-of-range slider is clamped, a crop that doesn't fit is dropped, non-objects and entries with no `edits` are skipped, no picture size loads nothing, a non-array gives none); Sync Settings and a batch rating keep the picture's snapshots, each with the look it captured (and add none to a file that had none), a refused file is still refused, and a hostile snapshot in a file being rewritten is validated again; `sidecarScan` ignores snapshots (neutral edits plus snapshots is not "edited", and the meta is still read). `testVirtualCopyNames`: `sidecarFileOf` gives a copy's own file and does not mistake a look-alike for the marker. |
| `render_params` | Which masks are kept (a mask whose only effect is a Filter is kept; a Filter that rounds to no effect is not), what each frozen pass holds, `filterValuesFromAdj`, shapes mapped through the frame, defaults. |
| `adjustment_set` | Undo coalescing and labels, undo / redo / jumps, the 200-step cap, geometry, curve, mixer, grading and mask steps, `version()` rules, JSON round trip, the tables built from the state (also matching mask tables), and the Filters: a History label, a sidecar round trip, a lone Filter making the global color pass active, and the global edit and a mask reading the Filters identically. Presets (`testPresets`): `applyPreset` replaces the whole look (a slider the preset leaves at 0 is reset) but keeps crop, orientation and masks; it is one step labelled `Preset: <name>` (`Preset` for a blank name); an identical look adds no step and does not bump `version()`; the next slider touch is its own step, not folded into the preset step; the label survives undo, redo and History jumps; a hand-built preset with an out-of-range or NaN slider is sanitized; `currentPreset` returns the live look; and the result survives a JSON round trip. Auto Tone (`testAutoTonePick`): `setAutoTone` is one step labelled `Auto Tone`, moves only Exposure, Contrast, Whites and Blacks (Brightness, Highlights and Shadows stay), survives undo, redo and the label with it; the same values again add no step and do not bump `version()`; the next slider touch is its own step (even Exposure) and Auto Tone ends a drag in progress; one of the four changing still counts and all zeros on a fresh set is a no-op; hand-built values are clamped (NaN becomes 0) and values that sanitize to the current ones add no step; and the values survive a JSON round trip with a fresh history. White-balance pick (`testWhiteBalancePick`): `setWhiteBalance` is one step with the label `White Balance -67 / +14` (a zero prints `+0`), moves only Temperature and Tint, survives undo, redo and the label with it; the same values again add no step and do not bump `version()`; the next slider touch is its own step (even Temperature) and a pick ends a drag in progress; hand-built values are clamped (NaN becomes 0) and a pick that sanitizes to the current values adds no step; and the values survive a JSON round trip with a fresh history. Snapshot restore (`testSnapshotRestore`): `currentEdits` matches `toJson`; `restoreEdits` is one step labelled `Snapshot: <name>` that brings back sliders, crop **and masks** together, and undo puts all of them back in one press while redo restores them and the label; restoring the same state adds no step and does not bump `version()`; the next slider touch is its own step; the name is cleaned (a blank one gives `Snapshot`); a hand-built value is clamped (NaN becomes 0); a restore onto a fresh set gives the snapshot's JSON, and restoring the neutral state onto a fresh set is a no-op. |
| `minijson` | Round trips (floats exactly), escapes and surrogate pairs, malformed input rejected, depth limit, base64 and run-length coding. |
| `exif` | Orientation table, re-embedding into a plain JPEG (orientation reset, thumbnail unlinked, size tags patched, ICC kept), a 100,000-case fuzz test of damaged headers, `stripGps` (coordinates zeroed not just unlinked, the rest of the EXIF intact, both byte orders, bad pointers and types, a second fuzz loop over a GPS-bearing file). |
| `file_utils` | Paths and UTF-8, extensions, natural order (and its deterministic tie-break), `readFilePrefix`, atomic writes (failure cases, no leftover temp file), `listImageFiles`. |
| `virtual_copy` | The copy path marker: `vcPath` / `vcSplit` round trip for every number 1 to 9, and every near-miss is *not* a copy (no name before the marker, `vc0`, `vc10`, a letter, capitals, extra characters, a missing digit); `vcImageOf` / `vcCopyOf`; the file names (`vcSidecarFileOf`, and `vcParseSidecarName` accepting the extension in any case but rejecting plain sidecars, `vc0`, `vc10`, capital `VC`, missing extension); `vcFreeId` (lowest free number, a deleted number is reused, 0 when all nine are taken); and `listVirtualCopies` on a temp folder (keyed lower-case, ascending, ignoring plain sidecars, over-long numbers and directories, a missing folder, and the entry cap). |
| `image_meta` | `exifOrientationOf` (all 8 values, both byte orders, bad input, the 256 KiB window) and `embedSourceMetadata` (layout, patched sizes, the old thumbnail erased, refusals, and `removeGps`: location kept by default, removed on request, everything else identical, source untouched). |
| `preset` | Names: `sanitizePresetName` drops control characters, DEL and non-ASCII, trims, caps at 32 and leaves no trailing space after the cap. `presetFileStem` replaces the characters Windows forbids, strips a trailing dot, prefixes the reserved device names (`COM1` yes, `COM10` no), and gives `preset` for a blank name. The JSON round trip (as a value and through text) is exact, and a neutral look serializes to four empty groups. Refusals: not an object, no `kind`, a `kind` other than `preset`, a `version` that is too new, 0 or a string, each leaving the output untouched. Hostile content is clamped: an out-of-range slider, a non-number, a negative value on a 0..100 slider, a junk curve (becomes the identity) and a control-character name. Files in a throw-away folder: a missing folder lists nothing, a blank name is refused, a nested folder is created, no `.tmp` is left, save then load is equal, the same name overwrites, listing is in natural order (`Cool 2` before `Cool 10`) and ignores stray files and a sub-folder named like a preset, bad files (invalid JSON, wrong kind, missing, over 1 MiB) fail with a message and leave the output untouched, a nameless preset takes its file name, and delete reports true then false. |
| `raster_ops` | Crop, quarter turns and mirror, `orientImage` and `cropImage` checked pixel by pixel against `GeoFrame` and the EXIF table, `rotateArbitrary`, the preview downsample, and the export resize: `fitLongEdge` (never enlarges, exact long side, short side within half a pixel), `resizeArea` (flat areas exact at any ratio, the 2 × 2 mean at half size, fractional weights, alpha-weighted color, not flipped, mean preserved) and `resizeToLongEdge` (no copy when nothing changes). |
| `sam_segmenter` | The stand-in `Segmenter` of a build without ONNX (the test body is skipped if `LITEROOM_ONNX` is defined, which the test target never does): `load` fails with exactly `built without LITEROOM_ONNX`, `encode` and `decode` fail with a non-empty error, `loaded()` and `encoded()` stay false, a null `errorOut` is fine. `sam::preprocess`, the resize / normalize / pad step `encode` feeds the model: output size for wide, tall, tiny and 1-pixel-high pictures, buffer size per layout (rank 4 the full padded square, rank 3 only the resized picture), box average when shrinking (whole and fractional factors), nearest when enlarging, ImageNet normalization in R, G, B planes, zero padding after normalization at the bottom (wide) or right (tall), the raw HWC layout checked pixel-exactly at scale 1, alpha ignored in both layouts, invalid input (null pointer, zero or negative size). |
| `seg_basic` | Preparing a picture, the color tolerance, 4-connected growth, the 5 × 5 seed patch, cancellation, the wand's prompts, reuse of the prepared picture. |
| `render` | An untouched set reproduces the input, tone is an exact table lookup, 128-row blocks and one-row renders give the same result, cancellation, masks blend as the formula says, later masks see earlier ones. |
| `image_codec` | `flattenRow` over white, PNG round trip (lossless, alpha kept), JPEG round trip, channel order, quality and 4:4:4 chroma, the size limit (`jpegSizeOk`; the encoder itself refuses empty and oversized pictures, so no caller can skip it), metadata embedded into real encoder output. |
| `histogram` | Counts and luma, alpha skipped, clip bits, strided sampling, the clipping overlay in classic and per-channel mode, block reduction past 2048 px. |
| `latest_worker` | See [kind 2](#2-tests-that-need-liteui-or-threads) above. |
| `thumb_worker` | `makeThumb` sizes and rounding, FIFO order, queue replacement, failed decodes, the `edited` flag, EXIF orientation, shutdown. The sidecar scan: a file holding only a rating is not "edited" and still delivers its rating, flag and label; a file with real edits is edited and delivers them too; no sidecar, unreadable JSON and a file from before `meta` give the default; a picture that can't be decoded still delivers its `meta`. Virtual copies (`testVirtualCopy`): a copy's path is read for its own sidecar only, so it delivers `thumbSkipped`, no thumbnail, its own `edited` and `meta`, even when its picture does not exist, while the picture itself is decoded as before. |
| `photo_meta` | `PhotoMeta` JSON: defaults are left out, every rating / flag / label combination round-trips, hostile input is clamped (an out-of-range or non-number rating, unknown flag or color names, a non-object), a hand-built value is sanitized. `MetaPatch`: only the named fields change, a set-to-zero is a change, out-of-range values are forced into range. `MetaFilter`: off matches everything, "at least N stars", OR within flags and within colors, AND across the three, toggles (the value already set switches off, out-of-range ignored). |
| `golden_render` | See [the golden render test](#3-the-golden-render-test). |

### Habits the tests share

If you write a new test, these patterns will look familiar:

- **A naive reference beside the fast code.** Blurs, morphology and rotations are checked against a slow, obvious version written in the test (`refBlur`, `refMorph`).
- **Behavioral checks with loose thresholds.** `neighbor_fx` has no naive reference for its chroma, veil and blur maps. It checks what each effect should do (noise at least halved, edges kept, error against a known clear scene, monotone blur) with generous bounds. A small numeric drift in dehaze or blur would pass; a wrong direction or a lost edge would not.
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

1. **Create.** Open `a`, press Ctrl+Shift+N (Image > Create Virtual Copy). A new cell with a `C1` badge appears right after `a`, it opens at once without a visible decode, and it has `a`'s edits. `a.jpg.vc1.literoom.json` exists next to the picture. The status bar says "Created virtual copy 1".
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

## Sanitizers

Several test files say they are meant to be run under the sanitizers (`adjustments`, `latest_worker`, `thumb_worker`, `detail_layer`, `neighbor_fx`, and the fuzz tests in `exif` and `sidecar`). `CMakeLists.txt` has no switch for this, so build the test by hand. Each test's first line gives its command; add the flags:

```bash
# from the tests/ folder; -I../src is the include root (check.hpp is found next to the test)
g++ -std=c++20 -I../src -g -O1 -fsanitize=address,undefined sidecar_test.cpp -o sidecar_test && ./sidecar_test

# data races in the thread tests
g++ -std=c++20 -I../src -g -O1 -pthread -fsanitize=thread latest_worker_test.cpp -o latest_worker_test && ./latest_worker_test
```

Tests that need LiteUI also need `-I<folder with liteui.hpp>` and LiteUI's libraries, which is easier through CMake. On MSVC, `/fsanitize=address` is available for the address checks, but MSVC has **no ThreadSanitizer**: to check the thread tests for data races on Windows, build them with GCC or Clang under WSL or on Linux.

## Platform differences

- `image_codec` uses libpng and libjpeg on Linux and WIC on Windows, and the test exercises whichever one is compiled. A bug in the other path only shows up when you run the tests there. (`encodeJpegMemory`'s channel-order check exists because the WIC path flattens to BGR.)
- `file_utils`' case-only tie-break check is skipped on file systems that fold case (Windows, default macOS), since both files cannot exist.
- `detail_layer_test` and `sam_segmenter_test` carry an MSVC (`cl`) build line at the top (`detail_layer_test` also a `g++` one for sanitizer runs); the others use `g++`. All build lines are meant to be run from the repository root, with `-I src` (and `-I tests` for tests that include `check.hpp`).

## Where a new test belongs

1. **A new engine header with no UI:** put the header in the `src/` folder for its layer (see [Where things live](headers.md#where-things-live)) and add it to `LITEROOM_HEADERS` in `examples/literoom/CMakeLists.txt`. Then add `tests/<name>_test.cpp` and add the name to the first `foreach` list there (the plain unit tests, which call `literoom_add_test(<name>)`).
2. **One that takes `CanvasImage`:** add it to the `foreach` that links `liteui::liteui` (the `LITEUI` option of `literoom_add_test`), and put `#define LITEUI_IMPLEMENTATION` before the `liteui.hpp` include in the test's `.cpp` (see [kind 2](#2-tests-that-need-liteui-or-threads)).
3. **One that runs a thread:** use the `THREADS` option of `literoom_add_test` (copy the `detail_layer`/`neighbor_fx` loop, or the `latest_worker` block), assert only on the main thread, give every wait a timeout, and loop it with `--repeat until-fail` and `--timeout`.
4. Start the test file with `#include "check.hpp"`, end it with `return finish();`, and put the build command in a comment on the first line, like the others.
5. **A new slider, mask type or effect that changes pixels:** add a scenario to `golden_render_test.cpp` as well (sliders are picked up automatically through the table; see "Adding a slider" above) and re-record the golden file for each compiler and configuration you use (and see "Checking a re-record" above).

## Known gaps and things to check

- **The real model path is not tested.** `Segmenter::load`, the encoder and decoder runs and the mask pick need ONNX Runtime and a model, and the test target is built without `LITEROOM_ONNX`. `sam_segmenter_test` covers the stand-in and `sam::preprocess`; `seg_basic` tests the stand-in object selection with the same contract, and the golden test uses it too, so nothing runs the real model automatically.
- **Presets are covered only below the UI.** `preset_test` and `testPresets` pin the file format, the helpers and the undo behavior. Not covered: the Presets pane (list, click-to-apply, save and delete buttons, the focus guard for shortcuts), `presetDir()` beyond "not empty" (the per-platform folder choice and the non-UTF-8 `%APPDATA%` case), and two names that map to the same file stem overwriting each other. The golden test has no preset scenario, so a refactor that changed what a preset does to the rendered picture would not show up there; the Presets code in `src/main.cpp` is compiled into the golden test but never run.
- **Copy, Paste and Sync Settings are covered below the UI only.** `testSyncLook` pins the sidecar merge, and Paste is `applyPreset`, covered by `testPresets`. Not covered: the clipboard, the filmstrip selection (Ctrl/Shift-click, Ctrl+A, the selection tint), the sync worker and its status messages, the `syncBusy()` guards, decoding a picture with no sidecar to learn its size (including the EXIF quarter-turn swap, the case most likely to break), and the read-only-folder failure. Like the Presets code, it is compiled into the golden test (which includes `src/main.cpp`) but never run there. Use the manual checks below after touching any of it.
- **The white-balance picker is covered below the UI only.** `testWhiteBalancePicker` pins the math (against the real `makeColorPass`) and `testWhiteBalancePick` the single undo step. Not covered: the button and its highlight, the W and Esc keys, `wbPickerActive()` disarming itself, the click-to-pixel mapping under zoom, rotation, flip, crop and straighten, the 5 × 5 patch, sampling through the tone tables, and the status messages. The golden test has no picker scenario, because a pick only sets two ordinary sliders that the existing scenarios already render. The picker code in `src/main.cpp` is compiled into the golden test but never run. Use the manual checks above after touching it, and especially after changing the white-balance gains in `makeColorPass`.
- **Auto Tone is covered below the UI only.** `auto_tone_test` pins the math (against the real `toneLutFromValues`) and `testAutoTonePick` the single undo step. Not covered: the button, the A key and the menu item, reading `state.source`, and the messages.
- **Snapshots are covered below the UI only.** `testSnapshots` pins the file form, the limits and that every rewriter keeps them; `testSnapshotRestore` the single undo step. Not covered: the pane (name field, Enter to save, the rows, delete), `addSnapshot`'s size guard and its messages, `applySnapshotEntry` refreshing the picture after a geometry change (the case most likely to break: a restore that changes the crop or rotation), and autosave writing when only the list changed (`snapshotsDirty`). Use the manual checks above after touching any of it.
- **Virtual copies are covered below the UI only.** `virtual_copy_test` pins the path marker and the file names, `testVirtualCopyNames` the sidecar name and `testVirtualCopy` (thumb worker) the skip of decoding. Not covered: the filmstrip bookkeeping (`filmInsertEntry`, `filmRemoveEntry`, `filmPushWithCopies`, which keep `current`, `target`, `anchor` and `byName` right when cells appear and disappear; the part most likely to break), `createVirtualCopy` and `deleteVirtualCopy` and their messages and double-press guard, the no-decode switch in `filmOpenIndex`, handing the original's thumbnail to its copies, the badge, and the real-file translations in the decode job, the Sync job and export (a missed `vcImageOf` would try to open a path ending in `|vc2`). The golden test compiles this code but does not run it.
- **The `src/main.cpp` UI itself is not under test.** The golden test reaches the render and geometry pipeline, undo history, sidecar, histogram state and a few helpers, but not widgets, shortcuts, menus or the event loop.
- **`render_test`'s second mask check** compares a gray channel value against 0.4 where it means luma. This is fine for the gray ramp it uses, but would be wrong for a colored picture.
- **Timing.** The `latest_worker` and `thumb_worker` tests are timeout-guarded, but run them in a loop after touching `latest_worker.hpp` or `thumb_worker.hpp`. The `detail_layer` and `neighbor_fx` thread tests have no timeouts (see above).
- **Input handling and guards in the neighbor effects are mostly untested.** `gainsFromValues` clamping and NaN/inf handling (in both `detail_layer` and `neighbor_fx`), `toU8`'s NaN and infinity rules, invalid arguments to `blurLuma`, `Cache::get` and the two `acquire` functions (null pixels, non-positive size, unknown kind), `apply` with `W <= 0`, and the "plane too small, skip it" guards in `addPixels`, `nrLumaPass` and `nrColorPass`.
- **Cache limits and mid-build cancellation.** Only the entry cap of `detail_layer`'s cache (9) is checked. Its 384 MB byte cap, which entry the LRU drops, and the eviction of `chromaStore` (256 MB, 6 entries) and `mapStore` (64 MB, 40 entries) are not tested. Every cancellation test sets the generation before the build starts, so only the first cancellation check fires; none cancels part-way through a blur.
- **Effect internals and interactions.** `kClarityLimit` soft limiting, the veil's airlight clamp (180 to 255) and transmission floor (0.25), `sampleMap` clamping, the order of effects inside `apply` (dehaze, detail, noise reduction, blur last) and interactions between effects (each is tested alone, or all at once on tiny pictures that check only size and alpha, never pairwise).