# Header reference

One entry for every engine header. Each says what the file is for, the ideas worth knowing before you edit it, and what it depends on. For the bigger picture of how they fit together, read [architecture.md](architecture.md) first.

"No UI" means the header does not include `liteui.hpp` and can be tested alone. A few headers need LiteUI only for its `CanvasImage` (an RGBA8 picture) or its decoders; those are marked. `Image16` (an RGBA picture with 16 bits per channel) is Literoom's own type, in `image16.hpp`, and needs no UI.

**How an entry is laid out.** Each entry gives: what the file is for; what it **depends on** and which **test** covers it; its **constants** (name, type, value, meaning); its **types** (fields with type, default and range); and every **function** with its signature, inputs (type and valid range), return value, and details (rounding, cancellation, caching, what happens on bad input).

**Tests.** Every header's unit tests are in `tests/cases/<header>.inc`; the `**Test:**` line of an entry names the file. All of those files are `#include`d into one translation unit, `tests/literoom_test.cpp`, built as the single Catch2 executable `literoom_test` (C++23, linked with LiteUI, Catch2 and the thread library). It defines `LITEUI_IMPLEMENTATION` once for all cases, so the tests that need `CanvasImage` or the decoders live in the same executable as the pure ones; adding a test is a new `.inc` plus one `#include` there, with nothing to change in `CMakeLists.txt`. CTest runs each Catch2 case on its own as `literoom_<case name>` (case names look like `pixel_math: toU8Clamped`, so `ctest -R pixel_math` selects one file's cases). Catch2 tags become CTest labels: the timing-sensitive worker cases are tagged `[threaded]` (`ctest -L threaded --repeat until-fail:50`). Every case also carries a Catch2 tag with its file's name (for example `[color_pass]`), so `ctest -L color_pass` selects one file's cases as well. Two programs stand apart, because each compiles `src/main.cpp` into its own translation unit (with `main()` renamed) and has its own `main()`; neither is Catch2. `tests/golden_render_test.cpp` ("the golden test") renders fixed scenarios and compares their fingerprints with `tests/golden/render_<compiler>_<config>.txt`; it is skipped (exit code 77) until a file for that compiler and configuration exists, `--record` (or the `literoom_golden_record` target) writes one, its CTest label is `golden` (`ctest -LE golden` leaves it out), and it is never built with sanitizers because the fingerprints are float-exact. `tests/lut_pane_test.cpp` drives the LUTs pane's logic from `main.cpp` against a real temp library, the real preview worker and the real renderer, on the small `check.hpp` harness (the golden test keeps its own tally of mismatches and invariant failures); its CTest label is `threaded`. On MSVC both need the same 8 MB stack as the app, whose `main()` builds the whole UI in one large frame. See [testing.md](testing.md).

**Conventions used throughout.**
- A picture is RGBA8 (4 bytes per pixel, row-major, alpha last), or, for the 16-bit twin of the pipeline, `Image16`: the same layout with `uint16_t` samples (8 bytes per pixel, straight alpha). Functions that take a count `n` mean samples/bytes, not pixels, unless stated. The float passes work in "levels" (0..255 as floats); a 16-bit sample is 257 times a level's byte value, so 8-bit data widened with `widen8` is exact in levels.
- **Cancellation contract:** long routines take `const std::atomic<uint64_t> *latest, uint64_t gen` and stop early (returning false or `nullopt`) when `latest->load() != gen`. A null `latest` means "never cancel". See `latest_worker.hpp`.
- **Rounding rule:** every float-to-byte conversion goes through `toU8Clamped` (`pixel_math.hpp`) so results stay bit-identical; the 16-bit counterpart is `toU16Clamped` (same file, used by `levelsToSamples` in `image16.hpp`). In the float pipeline a value is rounded **once**, by whoever consumes it (see `tone_table.hpp`).
- **Sanitizing rule:** slider values are clamped to their range and NaN/infinity become 0 (the neutral value) before they reach any table index or cast.

## Contents

- [Where things live](#where-things-live)
- [Basics](#basics)
- [Adjustments and the edit state](#adjustments-and-the-edit-state)
- [Color](#color)
- [Detail effects](#detail-effects)
- [Geometry](#geometry)
- [Masks](#masks)
- [Rendering](#rendering)
- [Saving and files](#saving-and-files)
- [Background work](#background-work)
- [Test support](#test-support)

---

## Where things live

Every header sits under `src/`, which is the include root, so a header is always included by its path from `src/` (for example `#include "mask/mask.hpp"`). The entries below use bare file names; this table gives the folder for each. Header names are unique, so a bare name is enough to find one.

| Folder | Headers |
|---|---|
| `core/` | `pixel_math`, `geometry`, `adjustments`, `auto_tone`, `raster_ops`, `image16`, `image16_convert`, `tone_table`, `wide_chain` |
| `color/` | `color_mix`, `color_grade`, `color_pass`, `curves`, `lut3d` |
| `detail/` | `detail_layer`, `neighbor_fx`, `film_fx` |
| `mask/` | `mask`, `mask_geom`, `mask_range`, `mask_rect`, `mask_polygon`, `mask_raster`, `mask_brush`, `mask_object`, `seg_basic`, `sam_segmenter` |
| `edit/` | `adjustment_set`, `render_params` |
| `render/` | `render`, `render_core`, `render_bytes`, `histogram`, `scopes` |
| `io/` | `sidecar`, `photo_meta`, `preset`, `virtual_copy`, `minijson`, `file_utils`, `image_codec`, `image_codec16`, `image_source`, `exif`, `image_meta`, `color_tag`, `lut_cube`, `lut_library` |
| `workers/` | `latest_worker`, `thumb_worker`, `export_render`, `export_worker`, `lut_preview_worker` |
| `tests/` | `check.hpp` (used by `lut_pane_test.cpp`), `literoom_test.cpp` with `cases/*.inc`, `golden_render_test.cpp` with `golden/`, and `lut_pane_test.cpp` |

The folders are layers, lowest first: `core`, then `color`, `detail` and `mask`, then `io`, then `edit`, then `render`. `workers` sits beside `render` and uses `core` and `io`; `export_render`, `export_worker` and `lut_preview_worker` are the exceptions that also use `edit` and `render`, because they run the pixel pipeline. A header may include headers from folders below its own, never from folders above it, so the "Depends on" lines in the entries below always point down this list. Two exceptions to know about: `core/tone_table.hpp` includes `color/curves.hpp` (it composes the Tone sliders with the curves), and `core/image16_convert.hpp` includes `liteui.hpp` for `CanvasImage` (declarations only, no platform code).

---

## Basics

### `pixel_math.hpp`
The small numeric helpers used everywhere: a smoothstep, the rounding rules from float to a byte and to a 16-bit sample, the 16-bit-sample-to-level conversion, and the degree constant.

**Depends on:** standard library only (`<algorithm>`, `<cassert>`, `<cmath>`, `<cstdint>`). No UI. **Test:** `cases/pixel_math.inc`.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kDegToRad` | `constexpr double` | π / 180 | Degrees to radians. It is a `double`; code that needs a float casts it (`static_cast<float>(kDegToRad)`) |
| `kU16PerLevel` | `constexpr float` | 257 | 16-bit samples per level: a 16-bit sample `v` is `v / 257` levels ("levels" are the float pipeline's 0..255 scale), so an 8-bit value `k` widened to `k * 257` is exactly `k` levels again |

**Functions**

**`inline float smoothStep01(float e0, float e1, float x)`**
- Inputs: `e0` lower edge, `e1` upper edge (**must be greater than `e0`**), `x` the value.
- Returns: `t²(3 − 2t)` where `t = clamp((x − e0) / (e1 − e0), 0, 1)`, so 0 at or below `e0`, 1 at or above `e1`, smooth between.
- Edge cases: NaN `x` returns 0; ±infinity saturates to 0 or 1.
- Gotcha: the `e1 > e0` requirement is only an `assert`, which is compiled out in Release builds. With `e1 <= e0` a Release build divides by zero.
- Used for the tonal-range weights of Highlights, Shadows, Whites and Blacks, the soft edges of masks, and the Color Mixer and Color Grading blends.

**`inline uint8_t toU8Clamped(float v)`**
- Input: any float. Returns: a byte.
- Rule (**the** rounding rule of the whole pipeline): NaN → 0; the value is clamped to 0..255 (so infinities saturate); then `(uint8_t)(v + 0.5f)`, i.e. round to nearest with halves rounding up (within float precision of the `+ 0.5` step).
- Anything that turns a float into a byte should use it so results stay bit-identical. `neighbor_fx::toU8` is the same rule written without `std::` calls.

**`inline uint16_t toU16Clamped(float v)`**: levels to a 16-bit sample, with the 16-bit twin of the rounding rule: NaN gives 0, `v` is clamped to 0..255, then `(uint16_t)(v * 257 + 0.5f)`. `255.0f` gives exactly 65535.

**`inline float u16ToLevels(uint16_t v)`**: a 16-bit sample as float levels, `float(v) / 257`. It is a **division**, not a multiply by `1/257`, because only a division makes every widened 8-bit value (`k * 257`) come back as the exact integer `k`.

---

### `image16.hpp`
`Image16`, the 16-bit-per-channel RGBA picture the float pipeline starts from, and the row conversions between it, the 8-bit samples of a `CanvasImage`, and the float "levels" (0..255) the pixel passes work in. Plain buffers, no editor state.

Why 16 bits and not float for storage: a 24 MP picture is 192 MB at 16 bits and 384 MB as float, and the editor keeps several stages of it (original, oriented, rotated, source). 16 bits holds everything a 16-bit PNG or a 12-14 bit RAW can say; the passes themselves compute in float, a block of rows at a time (`render_core.hpp`).

**Depends on:** `pixel_math.hpp` (`u16ToLevels`, `toU8Clamped`, `toU16Clamped`). No UI. **Test:** `cases/image16.inc`.

**Types**

- `struct Image16 { int width = 0, height = 0; std::vector<uint16_t> pixels; }`: R, G, B, A interleaved, **straight** (not premultiplied) alpha, row-major, no padding; the same layout as `CanvasImage` with `uint16_t` samples. Alpha is 16-bit too (0 = transparent, 65535 = opaque). `pixels.size()` is `width * height * 4`.
  - `Image16()`: empty. `Image16(int w, int h)`: a zero-filled (transparent black) picture; a negative `w` or `h` counts as 0.
  - `bool empty() const`: true when `width <= 0 || height <= 0`. `size_t pixelCount() const`: `width * height`.

**Functions**

**`inline uint16_t widen8(uint8_t v)`**: `v * 257`, which spreads 0..255 over 0..65535 exactly (255 × 257 = 65535).

**`inline uint8_t narrow16(uint16_t v)`**: `(v + 128) / 257`, the nearest byte (halfway rounds up). `narrow16(widen8(x)) == x` for every byte.

**`inline void widenSamples(const uint8_t *src, uint16_t *dst, size_t n)`** / **`inline void narrowSamples(const uint16_t *src, uint8_t *dst, size_t n)`**: `widen8` / `narrow16` over `n` **samples** (not pixels). No bounds check; the caller sizes both buffers.

**`inline void samplesToLevels(const uint8_t *src, float *dst, size_t n)`** / **`(const uint16_t *src, float *dst, size_t n)`**: loads `n` samples as float levels. The 8-bit form is a plain cast; the 16-bit form goes through `u16ToLevels`. Both are exact for 8-bit data and for its widened form.

**`inline void levelsToSamples(const float *src, uint8_t *dst, size_t n)`** / **`(const float *src, uint16_t *dst, size_t n)`**: stores with the rounding rule (`toU8Clamped` / `toU16Clamped`): round to nearest, clamp to the sample's range, NaN gives 0.

---

### `image16_convert.hpp`
`CanvasImage` (RGBA8, LiteUI) <-> `Image16`. Kept apart from `image16.hpp` so that one stays UI-free. It needs `liteui.hpp` only for the `CanvasImage` struct (declarations, no `LITEUI_IMPLEMENTATION`), so any translation unit may include it. The codecs in `image_codec16.hpp` are the part that needs the platform libraries.

**Depends on:** `image16.hpp`, `liteui.hpp`. **Test:** `cases/image16_convert.inc`. `cases/wide_chain.inc` does not use this header; it widens with `widenSamples` from `image16.hpp`.

**`inline Image16 widenImage(const CanvasImage &src)`**: every sample `v` becomes `v * 257`. A new `Image16` of the same size.

**`inline CanvasImage narrowImage(const Image16 &src)`**: every sample to the nearest byte. `narrowImage(widenImage(x))` equals `x` exactly. Both copy `min(destination size, source size)` samples, so a source whose `pixels` is shorter than `width * height * 4` leaves the rest zero instead of reading past the end.

---

## Adjustments and the edit state

### `adjustments.hpp`
The Adjust sliders. `AdjId` names every slider, `kAdjustmentSpecs` is the table (label, range, per-channel function, stage), and `AdjValues` is one value per slider (used by the global edit and by every mask). No UI, no pixels.

**Depends on:** `pixel_math.hpp` (`smoothStep01`, `toU8Clamped`). **Test:** `cases/adjustments.inc`.

**Four stages**
- **Tone**: pointwise per channel (0..255 in, float out). Composed into one lookup table by `toneLutFromValues`. Runs first.
- **Color**: needs cross-channel math. `apply` is `nullptr`; read by `makeColorPass` / `applyColorPass` (`color_pass.hpp`). This stage also holds the **Filters** (Greyscale, Sepia, Vintage, Fade, Negative, Posterize, Solarize, Bleach Bypass, Threshold, Cross Process): 0..100 strengths that run as the last block of the color pass. **LUT Amount** follows them: the strength of the picture's 3D LUT (0 = off; the LUT itself is the edit's `LutRef`, see `lut3d.hpp`), the very last step of the color pass. It is global only: the Adjust pane and the mask lists skip it by id, and `buildMaskPasses` never reads it.
- **Detail**: needs neighbor pixels. `apply` is `nullptr`; read by `detail_layer.hpp` and `neighbor_fx.hpp`.
- **Effects**: needs the pixel's *position* (Vignette, Grain). `apply` is `nullptr`; read by `film_fx.hpp`, the very last step of `renderWith` (after the masks). Global only: the mask slider lists skip this stage. Last in the table.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kExposureMin` / `kExposureMax` | `int` | -100 / 100 | Exposure range (percent; scales each channel) |
| `kBrightnessMin` / `kBrightnessMax` | `int` | -100 / 100 | Brightness range (flat offset in levels) |
| `kBipolarMin` / `kBipolarMax` | `int` | -100 / 100 | The shared range; 0 is "untouched" |
| `kToneMin` / `kToneMax` | `int` | = `kBipolar*` | Older names for the same range, kept so old code compiles |
| `kBlackPointMax` / `kWhitePointMax` | `int` | 100 / 100 | Upper limits of Black Point and White Point (both start at 0). Capped well below 255 so the stretch denominator `255 - v` stays at 155 or more |
| `kGammaOctaves` | `float` | 2 | Gamma's exponent is `2^(-v / 100 * kGammaOctaves)`, so the exponent runs from 0.25 (+100) to 4 (-100) |

**Types**
- `enum class AdjId : int`: `Exposure, Brightness, Contrast, Highlights, Shadows, Whites, Blacks, BlackPoint, WhitePoint, Gamma, Temperature, Tint, Vibrance, Saturation, Hue, ColorizeHue, ColorizeAmount, Greyscale, Sepia, Vintage, Fade, Negative, Posterize, Solarize, BleachBypass, Threshold, CrossProcess, LutAmount, Texture, Clarity, Sharpness, Dehaze, NoiseLuma, NoiseColor, Blur, Vignette, VignetteMidpoint, VignetteFeather, Grain, GrainSize, GrainRoughness, Count`. The order **must** match `kAdjustmentSpecs` (it is the index).
- `struct AdjustmentSpec`

  | Field | Type | Meaning |
  |---|---|---|
  | `id` | `AdjId` | Must equal the entry's position in the table |
  | `label` | `std::string_view` | Display name (a string literal, so the table is constant-initialized) |
  | `min`, `max` | `float` | Slider range; `min < max` is enforced at compile time |
  | `apply` | `float (*)(float channelValue, float value)` | Tone only: one channel 0..255 and the slider value in, **unclamped, unrounded** float out. `nullptr` for Color, Detail and Effects |
  | `stage` | `Stage` (`Tone`, `Color`, `Detail`, `Effects`) | Which pass consumes it. Defaults to `Tone` |
- `using AdjValues = std::array<float, adjIndex(AdjId::Count)>`: 41 floats, indexed by `adjIndex(id)`.

**The table `kAdjustmentSpecs`** (`inline constexpr std::array<AdjustmentSpec, 41>`). Declaration order is the sidebar order and, within Tone, the processing order.

| Slider | Range | Stage | Per-channel function (x = channel 0..255, v = value) |
|---|---|---|---|
| Exposure | -100..100 | Tone | `x * (1 + v/100)` |
| Brightness | -100..100 | Tone | `x + round(v)` |
| Contrast | -100..100 | Tone | `128 + (x - 128) * (1 + v/100)` |
| Highlights | -100..100 | Tone | `x + v * 0.6 * smoothStep01(0.4, 1, x/255)` |
| Shadows | -100..100 | Tone | `x + v * 0.6 * (1 - smoothStep01(0, 0.6, x/255))` |
| Whites | -100..100 | Tone | `x + v * 0.4 * smoothStep01(0.75, 1, x/255)` |
| Blacks | -100..100 | Tone | `x + v * 0.4 * (1 - smoothStep01(0, 0.25, x/255))` |
| Black Point | 0..100 | Tone | `(x - v) * 255 / (255 - v)`; `x` unchanged when `v == 0` |
| White Point | 0..100 | Tone | `x * 255 / (255 - v)`; `x` unchanged when `v == 0` |
| Gamma | -100..100 | Tone | `255 * (max(x, 0)/255) ^ e` with `e = 2^(-v/100 * kGammaOctaves)`; `x` unchanged when `v == 0` |
| Temperature, Tint, Vibrance, Saturation, Hue | -100..100 | Color | none (see `color_pass.hpp`) |
| ColorizeHue | 0..360 (degrees) | Color | none |
| ColorizeAmount | 0..100 | Color | none |
| Greyscale, Sepia, Vintage, Fade, Negative, Posterize, Solarize, Bleach Bypass, Threshold, Cross Process | 0..100 | Color | none (the Filters block of `color_pass.hpp`; 0 = off). Their labels are sidecar keys, so never rename them |
| LUT Amount | 0..100 | Color | none (the strength of the 3D LUT, last step of the color pass; 0 = off. Global only. Its label is a sidecar key, so never rename it) |
| Texture, Clarity, Dehaze | -100..100 | Detail | none |
| Sharpness, NoiseLuma ("Noise Reduction"), NoiseColor ("Color Noise"), Blur | 0..100 | Detail | none |
| Vignette | -100..100 | Effects | none (`film_fx.hpp`; negative darkens the edges, positive lightens them, 0 = off) |
| Vignette Midpoint, Vignette Feather | -100..100 | Effects | none (offsets from the default look: 0 = midpoint and feather halfway; + moves the ramp outward / widens it) |
| Grain | 0..100 | Effects | none (0 = off) |
| Grain Size | 0..100 | Effects | none (0 = finest) |
| Grain Roughness | -100..100 | Effects | none (offset from an even blend of smooth and crisp grain; -100 = smooth, +100 = crisp) |

The Effects labels (`Vignette`, `Vignette Midpoint`, `Vignette Feather`, `Grain`, `Grain Size`, `Grain Roughness`) are sidecar keys, so never rename them.

- The Highlights/Shadows/Whites/Blacks scales (0.6 / 0.4) were chosen so the curve stays monotonic. Whites and Blacks have the thinnest margin (about 0.94); raising their 0.4 much past 0.42 breaks it.
- **Levels** (Black Point, White Point, Gamma) come last in the Tone block, in that order, so each runs on the previous one's rounded output. Black Point and White Point are linear with slope `255 / (255 - v)`, at least 1 and finite because `v <= 100`, so they are monotonic. Gamma is monotonic for any positive exponent, never moves 0 or 255, and floors its input at 0 so `pow` never sees a negative base. Each returns `x` untouched at 0 (no `pow(x, 1)` rounding noise). Black Point returns negative values below `v` and White Point values above 255; the table builder clamps them.
- These four act per channel through the shared table, so they are a tone-curve approximation, not a luminance-preserving recovery.

**Functions**

**`constexpr size_t adjIndex(AdjId id)`**
- Input: `id` (`AdjId`). Returns: its index as `size_t` (the position in `kAdjustmentSpecs` and `AdjValues`).

**`constexpr bool adjustmentSpecsValid()`**
- Returns `true` if entry *i* has `id == i` and every `min < max`.
- Evaluated by a `static_assert` right after the table, so a missing or misordered entry fails the build. That every Tone entry has a non-null `apply` is **not** checked here (GCC cannot constant-evaluate that null check under `-fsanitize=address,undefined`); `cases/adjustments.inc` checks it at run time.

**`inline AdjValues defaultAdjValues()`**
- Returns: all sliders at 0 (value-initialized array).

**`inline float sanitizedValue(const AdjustmentSpec &spec, float v)`**
- Inputs: `spec` (supplies `min`/`max`), `v` (raw slider value, may be NaN or infinite).
- Returns: `v` clamped to `[spec.min, spec.max]`; NaN/infinity give `0`.
- Why: the monotonicity of the tone functions was only worked out inside the range, so a corrupt sidecar or UI bug must not get past this.

**`inline std::array<uint8_t, 256> toneLutFromValues(const AdjValues &values, const std::array<float, 256> *curveLut)`**
- Inputs:
  - `values`: one value per slider; only Tone entries are read.
  - `curveLut`: optional float table from `curveToLut`, or `nullptr` for no curve.
- Returns: a 256-entry table; input level *i* maps to `lut[i]`. The same table serves R, G and B.
- Details:
  - For each input level, runs every Tone spec in table order. Specs whose sanitized value is 0 are skipped.
  - **Rounds and clamps (`toU8Clamped`) after each spec**, not once at the end, so the result is bit-identical to separate per-pixel passes that each stored a byte.
  - The optional curve is applied last, indexed by the already-rounded level, and rounded again.
  - Shared by `AdjustmentSet::buildLut` and by masks.

### `tone_table.hpp`
The Tone stage of the **float** pipeline: the same sliders and curves as `toneLutFromValues`, composed into one table of floats instead of 256 bytes, **without the rounding after every slider**.

`toneLutFromValues` rounds to a byte after each Tone slider, so that one table equals separate 8-bit passes. In float that rounding is gone: each slider's function is applied to the previous one's float result, clamped to 0..255 (the clamp stays; it is what makes Highlights / Whites / Contrast behave at the ends), and the answer is rounded once, by whoever consumes it. The old table stays as it is: it is the reference the tests compare against, and Auto Tone still reasons with it.

**Depends on:** `adjustments.hpp`, `curves.hpp` (`curveSample`, `curveToLut`, `curveIsIdentity`). No UI. **Test:** `cases/tone_table.inc` (it also covers the head: `gammaLiftHasNoSteepStartError`, `otherSlidersStayWithinTheirDocumentedError`, `headWithCurves`, `eightBitLevelsAreExact`, `edgesAreSafe`).

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kToneSub` | `int` | 16 | Grid points per level: the table samples the curve every 1/16 level |
| `kToneSize` | `int` | 4081 | `255 * kToneSub + 1`: entries for 0, 1/16, ..., 255 |
| `kToneHeadPerLevel` | `float` | 257 | head cells per level (one per 16-bit sample) |
| `kToneHeadLevels` | `int` | 2 | the head covers 0..2 levels |
| `kToneHeadCells` / `kToneHeadSize` | `int` | 514 / 515 | cells / entries of the head |
| `kToneTableSize` | `int` | 4596 | `kToneSize + kToneHeadSize`: a `ToneTable`'s `y` holds the main grid, then the head |

An integer level lands exactly on a grid point, so an 8-bit input gets the exact value (no interpolation error at all for 8-bit pictures). For 16-bit input the 1/16-level grid is too coarse right above black for a curve with a steep start (Gamma +100 is off by up to 15 levels in the first cell), so each table also carries a **head**: the same curve at every 1/257 level (every 16-bit sample) over the first `kToneHeadLevels` levels, read by `toneLookupWide`. Worst error over all 65536 samples: about 0.002 level for Gamma +100 (under one 16-bit step, 0.0039), at most about 0.03 level for any other slider (Blacks −100, the steepest band; a smooth deviation, no banding). Integer levels stay exact.

**Types**

- `struct ToneTable { std::array<float, kToneTableSize> y; }`: the composed curve: `y[0, kToneSize)` the main grid, `y[kToneSize, kToneTableSize)` the head.
  - `float at(float x) const`: `toneLookupWide(y.data(), x)`.
  - `bool isIdentity() const`: true when every entry is **exactly** its own input level (`i / 16`), meaning the stage can be skipped.
- `struct ToneSet { std::array<ToneTable, 3> ch; bool shared = true; bool identity = true; }`: what the render loop holds, the R, G and B tables. With no per-channel curve all three are the same table, so only `ch[0]` is built and `shared` is set (the loop reads one table, like the byte pipeline's shared-LUT fast path). `identity` is true when the whole stage does nothing. `const ToneTable &at(int c) const` returns `ch[0]` when `shared`, else `ch[c]` (`c` 0..2, not range-checked).

**Functions**

**`inline float toneClamp(float v)`**: `v` clamped to 0..255; NaN gives 0.

**`inline float toneLookup(const float *tbl, float x)`**: linear interpolation in a `kToneSize` table at `x` levels (clamped to 0..255 first, NaN gives 0). One multiply, one float-to-int and one lerp. `tbl` must hold `kToneSize` floats. Plain grid only: used for the curve tables; a `ToneTable` is read with `toneLookupWide`.

**`inline float toneLookupWide(const float *tbl, float x)`**: reads a `kToneTableSize` table (`ToneTable::y.data()`): below `kToneHeadLevels` through the head (exact at every 16-bit sample), above through the main grid (`toneLookup`). `x` is clamped to 0..255 first (NaN gives 0); the head index is clamped because `x * 257` can round up to the cell count.

**`inline ToneTable toneTableFromValues(const AdjValues &values, const CurvePoints *master, const CurvePoints *channel)`**
- Inputs: the sliders; the master curve (null for none); one channel's own curve (null for none).
- Order per grid point: every Tone slider of `kAdjustmentSpecs` that has an `apply` function and a non-zero value, in table order (each result `toneClamp`ed); then the master curve; then the channel curve (the order of `AdjustmentSet::buildChannelLuts`). So the R, G and B tables are three calls that differ only in `channel`.
- Slider values go through `sanitizedValue` first (clamped, NaN/infinity become 0). A curve that is not evaluable gives the identity, like `curveToLut`. The curves are sampled at 1/16-level steps with `curveSample`.

**`inline std::shared_ptr<const ToneSet> makeToneSet(const AdjValues &values, const CurveSet &curves)`**
- `curves` is `{master, R, G, B}`. A curve is **used** only when `curveIsIdentity(curveToLut(curve))` is false: the **same rule as the byte pipeline** for deciding a curve is a straight line, so an untouched curve never changes a pixel (even though the float table would sample it more finely).
- No per-channel curve in use: builds one table, `shared = true`, `identity = ch[0].isIdentity()`. Otherwise builds all three (a channel without its own curve gets just the master), `shared = false`, and `identity` is true only if all three are.
- Returns a new `ToneSet` each call; never null. Cost is about 4081 points times the active sliders, per table.

### `auto_tone.hpp`
Auto Tone: turns a luma histogram into values for Exposure, Contrast, Whites and Blacks. A pure function of 256 counts. No UI, no pixels, no editor state. See [architecture.md](architecture.md#auto-tone).

**Depends on:** `adjustments.hpp` (it checks its own result through the real `toneLutFromValues`). **Test:** `cases/auto_tone.inc`. **In the app** (`main.cpp`, not this header): the Adjust pane's **Auto Tone (A)** button, the A key and Image > Auto Tone measure `state.source` with `histogram::compute`, call `solveAutoTone` and apply the result with `AdjustmentSet::setAutoTone`.

**How it works.** The tonal range is the 0.5 % and 99.5 % points of the luma histogram (so a few stray pixels cannot decide). Exposure and Contrast are solved together so that the black point lands on `kAutoToneTargetLo` and the white point on `kAutoToneTargetHi`; Whites and Blacks then close what is left. The steps follow the tone table's own order (Exposure, Contrast, Whites, Blacks) and round the way it does. It stretches the *measured range* rather than aiming the median at mid-gray, so a night scene stays dark and a snow scene stays bright, and a picture that already uses the whole range is left nearly alone. Brightness, Highlights, Shadows, the Levels sliders (Black Point, White Point, Gamma) and the curves are neither read nor changed. The check runs through the byte table (`toneLutFromValues`, which rounds after each slider), while the float pipeline's table (`tone_table.hpp`) does not, so a render can land a fraction of a level off what was checked.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kAutoToneMinPixels` | `uint64_t` | 64 | Fewer samples than this give `NoPixels` |
| `kAutoToneClipFraction` | `double` | 0.005 | The black / white point are the levels below / above which this fraction of pixels lie |
| `kAutoToneMinSpread` | `int` | 16 | A range narrower than this (levels) is `Flat` |
| `kAutoToneTargetLo` / `kAutoToneTargetHi` | `float` | 6 / 248 | Where the black and white points are sent (a little room before clipping) |
| `kAutoToneExposureFactorMin` / `Max` | `float` | 0.5 / 2.0 | Limits on Exposure's gain (the slider gives `1 + v/100`, so -50..+100) |
| `kAutoToneContrastFactorMin` / `Max` | `float` | 0.5 / 2.0 | Limits on Contrast's gain |
| `kAutoToneMinBandWeight` | `float` | 0.1 | Whites / Blacks only act in a narrow band at the ends. If the point sits where the band's weight is below this, the slider is left at 0 instead of being pushed to its limit for no effect |
| `kAutoToneLimitedTolerance` | `float` | 24 | After solving, if either point ends up further than this from its target, the status is `Limited` |

**Types**
- `enum class AutoToneStatus`: `Ok` (both points reach their targets), `Limited` (usable, but the slider ranges stopped short of a target, for example a very dark picture that would need more than +100 Exposure), `Flat`, `NoPixels` (both of these change nothing).
- `struct AutoTone`: `exposure`, `contrast`, `whites`, `blacks` (whole slider values within their ranges, never a negative zero; all 0 unless usable), `status` (default `NoPixels`), and `usable()` (true for `Ok` and `Limited`).

**Functions**

**`inline AutoTone solveAutoTone(const std::array<uint32_t, 256> &luma)`**
- Input: counts per luma level 0..255 of the picture **before** the tone tables (`histogram::Counts::l` of the source picture, not of the shown picture, which already has the tone sliders applied). Only the shape matters, so the number of pixels does not change the answer, and the same histogram always gives the same answer.
- Returns: the four values and a status. Never changes anything itself.
- Details:
  - Exposure is applied first and rounds and clamps, so it is never allowed to push the white point past 255 (a picture with clipped highlights never gets a positive-gain boost from it; Whites may still pull the top down).
  - Contrast pivots on 128, so its gain is limited so that neither end of the measured range is clipped.
  - The result is judged by running it through `toneLutFromValues`: `Limited` means a point ended more than `kAutoToneLimitedTolerance` from its target.
  - Rounding uses `lround`, so a zero is never `-0` (a History label would print it).

---

### `adjustment_set.hpp`
`AdjustmentSet`: the live value of every edit, the undo/redo history, the History labels, the sidecar JSON and the `RenderParams`. No UI, no pixels. See [undo-and-history.md](undo-and-history.md).

**Depends on:** `adjustments.hpp`, `color_grade.hpp`, `color_mix.hpp`, `color_pass.hpp`, `curves.hpp`, `geometry.hpp`, `lut3d.hpp`, `lut_library.hpp`, `mask.hpp`, `minijson.hpp`, `neighbor_fx.hpp`, `preset.hpp`, `render_params.hpp` (and through it `tone_table.hpp`, for `makeToneSet`), `sidecar.hpp`. **Test:** `cases/adjustment_set.inc`. **Thread model:** UI thread only. Even some `const` members write `mutable` state (`version_`, the label cache), so don't call any member from two threads. Other threads get a `RenderParams` copy instead.

**Constants:** `kMaxUndoSteps` (`size_t`, 200): the history holds the baseline plus at most this many steps (so `historySize()` is at most 201); older steps are dropped as new edits come in.

**How undo coalescing works.** A slider drags continuously (many `set()` calls per second) and LiteUI sends no drag-end event, so repeated calls for the **same control** overwrite the newest history entry instead of adding one. Dragging Exposure from -20 to +40 is one undo step. Rules:
- Coalescing only happens while the newest step is the current one (no redo tail pending).
- Touching a different control, any discrete edit, an undo/redo, or a jump in the History panel starts a fresh step.
- The "current drag" is tracked by `lastEdited_`. Besides the real sliders (`AdjId`) it uses pseudo-ids just above `AdjId::Count`: `Count` itself means "no drag in progress", then `Count + 1` angle, `+ 2` curve, `+ 3` Color Mixer, `+ 4` Color Grading, `+ 5` mask.
- Controls that need to say *which* one is being dragged keep an extra key: the curve channel, the Mixer slot index, and an integer `key` for grading and masks.

**What one undo step restores** (the private `Snapshot`): slider values, crop, orientation, straighten angle, corner mode, the four curves, the 24 Mixer values, the grade, the whole mask list and the LUT reference (`LutRef`). A snapshot also has a `std::string label`. It is **empty for every step except a preset step, a white-balance pick, an Auto Tone, a snapshot restore and a LUT choice or removal**, and it is not part of the edit state (it is never compared and never restored); see "How a step is labelled" below.

**Read access**

| Member | Returns |
|---|---|
| `float get(AdjId id) const` | The slider value (no range check on `id`) |
| `const CropRect &crop() const` | The committed crop (empty = none) |
| `const Orient &orient() const` | Mirror and quarter turns |
| `float angle() const` | Straighten angle, degrees clockwise |
| `bool expand() const` | Straighten keeps the full canvas (true) or auto-crops (false) |
| `const CurvePoints &curve(CurveChannel ch = Master) const` | One tone curve |
| `float mix(MixChan c, size_t band) const`, `const MixValues &mixValues() const` | Color Mixer values |
| `const ColorGrade &grade() const` | Color Grading |
| `const MaskSet &masks() const` | The masks |
| `const LutRef &lut() const` | The picture's 3D LUT (empty = none). How much of it is used is the `LUT Amount` slider |
| `GeoFrame geoFrame() const` | `makeGeoFrame(origW_, origH_, orient_, angle_, expand_, crop_)`: the identity frame until `setOriginalSize` has been called |
| `uint64_t version() const` | Bumped by **every** change to the edit state (edit, undo, redo, jump, reset, load). The app compares it with its value at the last save/load to know whether the sidecar is stale. Not part of the undo history |

**Editing members** (each call that changes something creates or updates an undo step as described above)

**`void set(AdjId id, float value)`**
- Sets one slider. The value is **stored as given** (not clamped here; `toneLutFromValues`, `makeColorPass` and the other passes sanitize it later). It does not check for "no change": a call with an unchanged value still counts as an edit (it overwrites the current step if it is the same slider, otherwise it adds one).

**`void setCrop(const CropRect &c)`**
- Commits a crop (an empty `CropRect` removes it). Always its own undo step, and a slider drag never coalesces into it. A no-op if equal to the current crop. The rectangle is in pixels of the **oriented, straightened** picture, which is what `makeGeoFrame` and `geoStep` use.

**`void setGeometry(const Orient &o, float angle, const CropRect &c)`**
- Commits a new orientation, straighten angle and crop together (the crop is stored relative to the oriented, straightened image, so all three must change together). One undo step. No-op if all three are unchanged.

**`void setAngle(float deg, bool coalesce)`**
- Sets the straighten angle. It changes the picture's size, so **any committed crop is dropped** (undo brings it back). `coalesce = true` for a slider drag (repeated calls fold into one step); `false` for a discrete action (nudge button, reset). No-op if unchanged.

**`void setExpand(bool expand)`**
- Sets the corner mode (false = auto-crop the empty corners, true = grow the canvas). Always its own step. If the angle is non-zero the crop is dropped, since the picture just changed size. No-op if unchanged.

**`void setCurve(CurveChannel ch, const CurvePoints &c, bool coalesce)`** and **`void setCurve(const CurvePoints &c, bool coalesce)`** (master shorthand)
- Sets one tone curve. `coalesce = true` while a point is added or dragged: repeated calls for the **same channel** fold into one step until `endCurveEdit()` (or any other edit). `false` for a discrete action. No-op if unchanged.

**`void resetAllCurves()`**: all four curves back to straight lines, as one step (no-op if they already are). **`void endCurveEdit()`**: the point drag ended; the next curve edit starts a fresh step.

**`void setMix(MixChan c, size_t band, float value)`**
- Sets one Color Mixer slider. A drag on the same slider folds into one step; no-op if the value is unchanged. `band` is not range-checked (it must be below `kMixBandCount`). **`void resetMix()`**: zeroes all 24 sliders as one step (no-op if already 0). Its History label comes from the usual diff, so it reads "Color Mix: Reset" only when **two or more** values were non-zero; if exactly one was, the step reads like a single slider edit (for example `Color Mix: Red Hue +0`).

**`void setGrade(const ColorGrade &g, int key)`**
- Commits a grade. Repeated calls with the same non-negative `key` (the control being dragged) fold into one step until `endGradeEdit()`. `key < 0` is a discrete action (eye toggle, reset) and always its own step. No-op if unchanged. **`void endGradeEdit()`** closes the drag.

**`void setMasks(const MaskSet &m, int key)`**
- Commits a new mask list (add, delete, move, edit a slider...). `key` works as in `setGrade`; callers pack the mask index into it so two masks never share a step. No-op if equal (mask equality is cheap: object bitmaps compare by id). **`void endMaskEdit()`** closes the drag.

**3D LUT** (see [`lut3d.hpp`](#lut3dhpp) and [`lut_library.hpp`](#lut_libraryhpp); the reference is look data, the strength is the ordinary `LUT Amount` slider)

**`bool setLut(const LutRef &ref)`**
- Chooses the LUT, or removes it (an empty `ref`, or a name with nothing left after `sanitizeLutName`), as **one undo step**. The name is cleaned on the way in, so no caller can store an unsafe file-name stem.
- The amount follows the choice so the result is visible at once and a LUT-less picture stays untouched: choosing a LUT while `LUT Amount` is 0 sets it to 100; choosing another one while an amount is set keeps that amount; removing the LUT sets the amount back to 0. Both changes are in the same step, so one undo brings back both.
- Returns false, adding **no step** and not bumping `version()`, if neither the reference nor the amount would change. The amount counts: removing "the LUT" from a picture that has none, but whose `LUT Amount` is above 0, still returns true, because the amount goes back to 0 (one `Remove LUT` step). Choosing the LUT the picture already has, with an amount above 0, returns false.
- Like `applyPreset` it goes through `pushStep()`, so it ends any slider drag in progress. Its label is stored in the step: `"LUT: <name>"`, or `"Remove LUT"`.
- Does not look at the library: a reference to a file that does not exist is accepted (see `lutStatus`).

**`LutStatus lutStatus() const`**: where the LUT stands in the library right now: `None` (no LUT), `Ok`, `HashChanged` (the file's bytes differ from when it was chosen, still used), `Missing` or `Unreadable` (no effect). Resolves through `acquireLut`, so it may parse the file on the first call (the UI thread, about 150 ms for the largest tables) and is cheap after that.

**Presets** (see [`preset.hpp`](#presethpp); a preset is the *look* only, so crop, orientation, straighten angle and masks are never part of it)

**`Preset currentPreset(const std::string &name) const`**
- Returns the current slider values, the four curves, the 24 Mixer values, the grade and the LUT as a `Preset`. `name` goes through `sanitizePresetName`, so it may come back shorter, trimmed or empty. Does not touch the history or `version()`.

**`bool applyPreset(const Preset &p)`**
- **Replaces the whole look** with `p` as **one undo step**. A slider the preset does not set (its value is 0) is reset to 0, not left alone; this is "replace", not "merge". Crop, orientation, angle, corner mode and masks are not touched. The LUT is part of the look: a preset **without** one clears the picture's LUT (its `LUT Amount` comes from the preset's slider values like any other slider). The preset's LUT name is cleaned again; a preset that differs from the picture only in its LUT is a change.
- Each slider value is run through `sanitizedValue` (clamped to its range, NaN/infinity become 0), so a hand-built `Preset` cannot put an out-of-range slider in. The curves, Mixer values and grade are taken **as given**: a `Preset` that came from `presetFromJson` already satisfies the editor's rules (see `sidecarCurveFromJson`), but a hand-built one must keep them itself.
- Returns false, and adds **no step** and does not bump `version()`, if the sanitized sliders, the curves, the Mixer values, the grade **and the (cleaned) LUT reference** all equal the current ones. Otherwise returns true.
- The step is always its own: it goes through `pushStep()`, which ends any slider/curve/Mixer/grade/mask drag, so the next slider touch starts a fresh step rather than overwriting the preset step. Its label is `"Preset: <name>"` (the name sanitized again), or just `"Preset"` if the name is blank. The label is stored in the new snapshot, so it survives undo, redo and History jumps, and it stays even if the preset *file* is later deleted: history holds a full copy of the look, never a link to the file.

**White balance picker** (the eyedropper's edit; the math is `solveWhiteBalance` in [`color_pass.hpp`](#color_passhpp))

**`bool setWhiteBalance(float temperature, float tint)`**
- Sets **Temperature and Tint together as one undo step**. Two `set()` calls would be two steps, and `applyPreset` would replace the whole look, so the picker needs its own call.
- Both values go through `sanitizedValue` (clamped to the slider range, NaN/infinity become 0), like a preset's.
- Returns false, and adds **no step** and does not bump `version()`, if both sanitized values already equal the current ones. Otherwise returns true.
- Like `applyPreset` it goes through `pushStep()`, so it ends any drag in progress and the next slider touch (even Temperature) starts a fresh step instead of overwriting the pick. Its label is `"White Balance <temperature> / <tint>"` with signed numbers (for example `White Balance -67 / +14`; a zero prints as `+0`). The label is stored in the snapshot, so it survives undo, redo and History jumps.
- Nothing else changes: the other sliders, curves, Mixer, grade, crop, orientation and masks are left alone. The values are ordinary slider values, so the sidecar, presets, Copy / Paste and Sync Settings carry them with no extra code. The label is History-only and is not saved.

**Auto Tone** (the button's edit; the math is `solveAutoTone` in [`auto_tone.hpp`](#auto_tonehpp))

**`bool setAutoTone(float exposure, float contrast, float whites, float blacks)`**
- Sets **Exposure, Contrast, Whites and Blacks together as one undo step**. Four `set()` calls would be four steps.
- All four values go through `sanitizedValue`, like a preset's.
- Returns false, and adds **no step** and does not bump `version()`, if all four sanitized values already equal the current ones. Otherwise returns true.
- Like `applyPreset` it goes through `pushStep()`, so it ends any drag in progress and the next slider touch (even Exposure) starts a fresh step. Its label is `Auto Tone`, stored in the snapshot, so it survives undo, redo and History jumps.
- Nothing else changes: Brightness, Highlights, Shadows, the curves, the other sliders, Mixer, grade, crop, orientation and masks are left alone. The values are ordinary slider values, so the sidecar, presets, Copy / Paste and Sync Settings carry them with no extra code. The label is History-only and is not saved.

**Named snapshots** (a saved copy of the *whole* edit state; the list lives in `main.cpp`, see [undo-and-history.md](undo-and-history.md#named-snapshots) and [`sidecar.hpp`](#sidecarhpp))

**`SidecarEdits currentEdits() const`**
- The whole edit state as a plain value: sliders, crop, orientation, angle, corner mode, curves, Mixer, grade, masks and the LUT. This is what `toJson()` writes and what *Save snapshot* stores. Does not touch the history or `version()`. It builds the struct field by field, so a new field of `SidecarEdits` must be added here.

**`bool restoreEdits(const SidecarEdits &e, const std::string &name)`**
- Puts the picture back exactly as `e` describes it as **one undo step**. Unlike `applyPreset` it replaces the geometry (crop, orientation, angle, corner mode) and the masks too, so the caller must refresh the picture the way it does after an undo.
- Slider values go through `sanitizedValue` (clamped, NaN/infinity become 0). Everything else is taken **as given** (the LUT's name is made safe like any other name), so `e` must come from `currentEdits()` or `sidecarSnapshotsFromJson` (which checks the crop against the picture's size).
- Returns false, and adds **no step** and does not bump `version()`, if the sanitized sliders and every other field already equal the current ones. Otherwise returns true.
- Like `applyPreset` it goes through `pushStep()`, so it ends any drag in progress. Its label is `"Snapshot: <name>"` (the name sanitized again with `sanitizeSnapshotName`), or `"Snapshot"` for a blank name; it is stored in the history step, so it survives undo, redo and History jumps, and it is not saved.

**Loading, saving and resetting**

**`void setOriginalSize(int w, int h)`**: the untouched picture's size, which masks are stored against and `geoFrame()` and the sidecar loader need. Call it once per opened file, **after `reset()`**.

**`void reset()`**: a fresh baseline for a newly opened image: every slider, Mixer value, grade, mask, the LUT, crop, orientation and angle back to neutral, curves to identity, history reduced to that one baseline step.

**`mj::Json toJson() const`**: everything the user has done, as the sidecar JSON (the undo history is not included). Calls `sidecarEditsToJson(currentEdits())`.

**`bool loadJson(const mj::Json &j)`**
- Replaces every edit with the saved ones (call after `reset()` and `setOriginalSize()`). Returns false with **nothing changed** if `j` isn't usable (not an object, or the original size isn't set). Otherwise the undo history restarts from the loaded state. Everything else is validated and clamped by `sidecarEditsFromJson`.

**Undo, redo and History panel**
- `bool canUndo() const`, `bool canRedo() const`; **`void undo()`**, **`void redo()`**: move one step and restore it; both end any drag coalescing. No-ops if impossible.
- `size_t historySize() const`, `size_t historyIndex() const`.
- **`const std::string &historyLabel(size_t i) const`**: what step `i` changed compared with step `i − 1` (`"Original"` for step 0; an empty string for an out-of-range `i`). Labels are cached per `version()`, so polling every frame is cheap. The reference is only valid until the next change.
- **`void goToHistory(size_t i)`**: jumps to step `i` (the same as pressing Undo/Redo until you get there). No-op if `i` is out of range or already current.

**How a step is labelled** (`describeStep` diffs a step against the one before it, so the setters don't know about labels and a coalesced drag can never leave a stale one). One exception comes first: a step whose snapshot carries a non-empty `label` (only `applyPreset`, `setWhiteBalance`, `setAutoTone`, `restoreEdits` and `setLut` set one) is described by that label, with no diffing. Next, before any slider is looked at, a changed LUT reference is described as `LUT: <name>` (or `Remove LUT`); that is a safety net, because `setLut` always stores a label. Otherwise the **first** difference found wins, checked in this order:
1. A global slider: `"<label> <signed value>"` in `kAdjustmentSpecs` order (e.g. `Exposure +20`). Values within 0.05 of a whole number print as `%+d`, others as `%+.1f`.
2. `Rotate / Flip`, then `Straighten ±x.x°`, then `Straighten: Keep Canvas` / `Straighten: Auto-Crop`, then `Crop` / `Remove Crop`.
3. `Tone Curve: RGB / Red / Green / Blue`.
4. Color Mixer: more than one value changed → `Color Mix: Reset`; one value → `Color Mix: <band> <Hue|Saturation|Luminance> <value>`.
5. `Color Grading: <Shadows|Midtones|Highlights|Global>`, or `Blending`, or `Balance`.
6. Masks: `Add <type> Mask`, `Delete Mask`, `Reorder Masks` (exactly two positions differ and each mask now sits where the other was; checked before the per-mask loop), then per mask `Mask <n>: ` followed by `Rename`, a slider and value, `Amount <n>`, `Color Mix`, `Color Grading`, `Curve`, `On`/`Off`, `Invert`, `Parts`, or `Edit`.
7. Anything else: `Edit`.

**Building render input** (no pixels; these are what the worker thread gets copies of)

**`std::array<uint8_t, 256> buildLut() const`**
- The Tone sliders composed into one 256-entry table, then the **master** curve on top (Lightroom's order). The curve is skipped when it rounds to identity (`curveIsIdentity`), so an untouched curve gives bit-identical results. O(256) whatever the image size.

**`std::array<std::array<uint8_t, 256>, 3> buildChannelLuts() const`**
- Per-channel R, G, B tables: `buildLut()` and then each channel's own curve on top (`lut[k] = clamp(lround(curve[base[k]]), 0, 255)`). A channel whose curve is a straight line reuses the shared table, so with no per-channel curves all three are identical (which is what the byte pipeline's, `renderWithBytes`, fast path checks). These are the **byte** tables; the float pipeline uses `toneF`, built in `buildRenderParams()`.

**`ColorPass buildColorPass() const`**: `makeColorPass` from Temperature, Tint, Vibrance, Saturation, Hue, Colorize Hue and Amount, the Mixer, the grade, the ten Filters (via `filterValuesFromAdj(values_)`) and the 3D LUT: `makeLutPass(acquireLut(lut_).lut, LUT Amount)`, so the pass holds a shared pointer to the cached table and an unavailable LUT is simply inactive.

**`RenderParams buildRenderParams() const`**
- Freezes the whole state into a self-contained value: `luts` from `buildChannelLuts()`, `toneF` from `makeToneSet(values_, curves_)` (the float tone tables `renderWith` uses; always set, even when nothing is on, in which case `toneF->identity` is true and the stage is skipped), `color` from `buildColorPass()`, `fx` from `neighbor_fx::gainsFromValues` (Texture, Clarity, Sharpness, Dehaze, Noise Reduction, Color Noise, Blur), `masks` from `buildMaskPasses(masks_, geoFrame())` (only if there are masks), and `film` from `filmFromAdj(values_)` (Vignette and Grain: global, applied after the masks).

### `render_params.hpp`
`RenderParams` and `MaskPass`: everything the per-pixel pipeline needs, frozen into one small copyable value. Built from the edit state in O(256), so the UI thread builds one and hands a copy to a worker; the worker never touches the `AdjustmentSet`, which the UI thread keeps mutating. No pixels, no UI.

**Depends on:** `adjustments.hpp`, `color_pass.hpp`, `curves.hpp`, `film_fx.hpp`, `geometry.hpp`, `mask.hpp`, `neighbor_fx.hpp`, `tone_table.hpp`. **Test:** `cases/render_params.inc`.

**Types**

`struct MaskPass`: one mask frozen for the render thread.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `parts` | `std::vector<MaskPassPart>` | empty | The mask's shapes, **already mapped into the current picture**, combined left to right |
| `invert` | `bool` | false | Adjust everything outside the shapes |
| `amount` | `float` | 1 | 0..1; multiplies the weight |
| `tone` | `bool` | false | Any Tone slider or curve is non-neutral, so `luts` and `toneF` are valid |
| `luts` | `std::array<std::array<uint8_t, 256>, 3>` | zeros | R, G, B tone tables, the **byte** pipeline's (only filled when `tone`) |
| `toneF` | `std::shared_ptr<const ToneSet>` | null | The same tone stage as **float** tables, the float pipeline's (set with `tone`; null means `render_core` falls back to `luts`) |
| `color` | `ColorPass` | inactive | The mask's own color pass |
| `fx` | `neighbor_fx::Gains` | all 0 | The mask's Texture, Clarity, Sharpness, Dehaze, Noise and Blur gains |

`struct RenderParams`: `luts` (three 256-entry byte tables: tone sliders plus tone curves, per channel; what `renderWithBytes` reads), `toneF` (`std::shared_ptr<const ToneSet>`: the same stage as float tables, what `renderWith` / `render_core` reads; **null = use `luts`**, so a hand-built `RenderParams` that only sets `luts` still renders, rounding after the tone stage like the byte pipeline), `color` (`ColorPass`, runs after the tables; it holds the picture's 3D LUT as a shared pointer, so copying a `RenderParams` never copies a table), `fx` (`neighbor_fx::Gains`, after the color pass), `masks` (`std::vector<MaskPass>`, applied after all of it), `film` (`film_fx::Params`: Vignette and Grain, the very last step, after the masks).

**Functions**

**`inline FilterValues filterValuesFromAdj(const AdjValues &a)`**: picks the ten Filter sliders (Greyscale, Sepia, Vintage, Fade, Negative, Posterize, Solarize, Bleach Bypass, Threshold, Cross Process) out of an `AdjValues` in the form `makeColorPass` takes. It must be passed at **both** `makeColorPass` call sites (`AdjustmentSet::buildColorPass()` and `buildMaskPasses()`); a site that forgets it silently ignores the Filters.

**`inline film_fx::Params filmFromAdj(const AdjValues &a)`**: picks the six Effects sliders (Vignette, Vignette Midpoint, Vignette Feather, Grain, Grain Size, Grain Roughness) out of an `AdjValues` and sanitizes them through `film_fx::paramsFromValues`. Its only call site is `AdjustmentSet::buildRenderParams()`: masks never read these sliders. The result feeds `film_fx::prepare` inside the renderer.

**`inline std::vector<MaskPass> buildMaskPasses(const MaskSet &masks, const GeoFrame &frame)`**
- Inputs: the user's masks (stored against the original picture) and the `GeoFrame` of the picture being rendered.
- Returns: one `MaskPass` per mask that is **on** and would **change something**, in list order. Shapes are mapped into the current picture here, once, on the UI thread.
- A mask is skipped if it is disabled, has no parts, has `amount <= 0` (`amount` is clamped to 0..100 first), or has no effect (no tone, no active color pass, no neighbor effects).
- How each part is built:
  - `tone` is set if any Tone slider is non-zero or any of the four curves is not the identity (checked with `curveIsIdentity`).
  - Float tables: when `tone` is set, `toneF = makeToneSet(adj, curves)` (see `tone_table.hpp`; it uses the same "is this curve a straight line" rule as below, so both table sets agree on which curves count).
  - Byte tables: `toneLutFromValues(adj, masterCurve or null)` gives the base table. A channel with its own non-identity curve then maps through it: `lut[k] = clamp(lround(channelCurve[base[k]]), 0, 255)`. Order is sliders, master curve, channel curve (same as `AdjustmentSet::buildChannelLuts`).
  - `color` comes from `makeColorPass` (Temperature, Tint, Vibrance, Saturation, the mask's own mixer and grade, Hue, Colorize Hue, Colorize Amount, and the Filters through `filterValuesFromAdj`). A mask with only a Filter slider set counts as having an active color pass, so it is kept.
  - `fx` comes from `neighbor_fx::gainsFromValues` with the seven detail sliders.
  - The Effects sliders (Vignette, Grain) and **LUT Amount** are **not read**: a mask that carries one has no adjustment from it, and one with nothing else is dropped. A mask's color pass never gets a LUT.
- A second use besides `AdjustmentSet::buildRenderParams()`: `main.cpp` calls it with a **prefix** of the mask list (`MaskSet(ms.begin(), ms.begin() + sel)`) to render "the picture just before mask `sel`", which is what the Luminosity / Color Range masks sample. There it also sets `rp.film = {}`, because Vignette and Grain run after the masks and would not be in that picture. Skipped masks are dropped from the result as usual, so the result can be shorter than the prefix.

---

## Color

### `color_pass.hpp`
The float per-pixel color pass: white-balance gains → vibrance → saturation → hue → Color Mixer → colorize → Color Grading → Filters → 3D LUT. `applyColorPassF` runs it in place on float levels (what the float pipeline in `render_core.hpp` uses, **not rounded or clamped**); `applyColorPass` is the same pass on RGBA8 bytes, rounding to 8 bits once at the end. Works on gamma-encoded values directly (no linearization). Also holds `solveWhiteBalance`, the inverse of the white-balance gains, used by the app's eyedropper.

**Depends on:** `color_grade.hpp`, `color_mix.hpp`, `lut3d.hpp`, `pixel_math.hpp`. No UI. **Test:** `cases/color_pass.inc`, which also holds the float-vs-byte agreement test (`colorPassFloatAgrees`: 100 random passes), the chunking test (`bulkEqualsPerPixel`: buffer lengths from 1 up to 3000, around the 1024-pixel chunk edge), the white-balance picker (`whiteBalancePicker`) and the LUT tests (`lutActiveFlag`, `lutIdentityChangesNothing`, `lutAppliesAndBlends`, `lutRunsLast`, `colorPassWithLut`). The order of the LUT among the stages is also checked through the real renderer in `cases/lut_render.inc` and the golden test's `lut/` scenarios.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kHueShiftDegrees` | `float` | 90 | The Hue slider at ±100 turns colors by ±90° |
| `kColorizeStrength` | `float` | 0.6 | Colorize Amount 100 multiplies the luma-neutral tint (hue color minus its own luma, 0..1 units) by this × 255 |
| `kThresholdLevel` | `float` | 128 | Threshold filter: luma at or above this goes white, below goes black |
| `kWbMinLevel` | `float` | 6 | White-balance picker: every channel of the sampled spot must be at least this (0..255), or the spot is `TooDark` |
| `kWbMaxLevel` | `float` | 254 | White-balance picker: a channel at or above this counts as clipped (`Clipped`) |

**Types**

`struct ColorPass`: everything the loop needs, built once per render by `makeColorPass`.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `active` | `bool` | false | False = skip the whole pass (fast path) |
| `gain[3]` | `float[3]` | {1,1,1} | White-balance gains R, G, B (already luma-normalized) |
| `vibrance` | `float` | 0 | -1..+1 |
| `saturation` | `float` | 1 | Multiplier: 0 gray, 1 untouched, 2 double |
| `hue` | `bool` | false | Hue shift on; `hueM` is valid |
| `hueM[9]` | `float[9]` | identity | Row-major 3×3 rotation about the gray axis |
| `colorize` | `bool` | false | Colorize on; `tintVec` is valid |
| `tintVec[3]` | `float[3]` | {0,0,0} | Luma-neutral RGB offset at full midtone weight (0..255 units) |
| `mix` | `bool` | false | Color Mixer on; `mixLut` is valid |
| `mixLut` | `MixLut` | zeros | Only filled when `mix` (`MixLut`'s three arrays are zero-initialized) |
| `grade` | `bool` | false | Color Grading on; `gradeLut` is valid |
| `gradeLut` | `GradeLut` | zeros | Only filled when `grade` |
| `filters` | `FilterPass` | all off | The Filters block; `filters.any` = on. Counts toward `active` |
| `lut` | `LutPass` | none | The 3D LUT (shared pointer to an immutable table, plus the 0..1 amount); `lut.any()` = on. Counts toward `active`. Only the global color pass ever has one: masks do not |

`struct FilterValues`: the ten Filter slider values as given by the UI (0..100 each, default 0): `greyscale`, `sepia`, `vintage`, `fade`, `negative`, `posterize`, `solarize`, `bleachBypass`, `threshold`, `crossProcess`.

`struct FilterPass`: the Filters frozen for the loop, built by `makeFilterPass`.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `any` | `bool` | false | False = skip the whole block |
| `grey`, `sepia`, `vintage`, `fade`, `negative`, `solarize`, `bleach`, `threshold`, `cross` | `float` | 0 | Strength 0..1 (slider / 100) |
| `posterLevels` | `int` | 0 | 0 = off, otherwise 2..255 levels per channel |
| `posterScale`, `posterInv` | `float` | 0 | `(levels − 1) / 255` and its inverse, so the loop has no division |

**Functions**

**`inline FilterPass makeFilterPass(const FilterValues &v)`**: freezes the ten Filter sliders. Each value goes NaN/infinity → 0, else clamped to 0..100 and divided by 100. Posterize becomes a level count `round(256 · (2/256)^p)` (`p` = strength 0..1), at least 2; 256 or more means off, otherwise `posterLevels`, `posterScale` and `posterInv` are set. `any` is true if any strength is above 0 or `posterLevels != 0`.

**`inline void applyFilterPixel(const FilterPass &f, float &r, float &g, float &b)`**: one pixel through the Filters, float in and out, not rounded (the caller rounds). Clamps to 0..255 first and again after Sepia, because Vintage, Fade, Greyscale and Sepia can overshoot. Every step after that second clamp (Solarize, Bleach Bypass, Cross Process, Threshold, Posterize, Negative) keeps values inside 0..255, so the result is always within 0..255 (up to float rounding, about 1e-5; checked by brute force over millions of random pixels and slider mixes). The code comment still says it "may leave the range"; it cannot. Fixed order and formulas are listed in step 8 of `applyColorPassF` below. Helpers: `filterLerp`, `filterClamp255`, `filterBleachChannel` (overlay blend of a channel with the luma, `k` fading from multiply to screen around luma 0.45..0.55), `filterSCurve255` (smoothstep S-curve blended in by an amount). No `std::` helpers, for the Debug-build reason below.

**`inline ColorPass makeColorPass(float temperature, float tint, float vibrance, float saturation, const MixValues &mix, const ColorGrade &grade, float hueShift = 0.0f, float colorizeHue = 0.0f, float colorizeAmount = 0.0f, const FilterValues &filters = FilterValues{}, const LutPass &lut = LutPass{})`**
- Inputs (all UI slider values):
  - `temperature`, `tint`, `vibrance`, `saturation`, `hueShift`: -100..100, 0 = untouched.
  - `mix`: the 8 × 3 Color Mixer sliders (`MixValues`).
  - `grade`: the four-wheel `ColorGrade`.
  - `colorizeHue`: degrees; only checked for finiteness (it wraps, so any value works).
  - `colorizeAmount`: 0..100.
  - `filters`: the ten Filter strengths, 0..100 (default: all off). Sanitized by `makeFilterPass` (NaN/infinity → 0, clamped).
  - `lut`: the picture's LUT, already vetted and frozen by `makeLutPass` (default: none). Only the global call site (`AdjustmentSet::buildColorPass`) passes one; `buildMaskPasses` does not.
- Returns: a `ColorPass`. If nothing is non-neutral, `active` is false and the rest is left at defaults.
- Sanitizing: each slider is clamped to its range and NaN/infinity become 0, so the loop only ever sees finite values and its table indexes are in range. `grade` is sanitized inside `buildGradeLut`.
- Details:
  - Gains: `gR = (1 + 0.3 t)(1 + 0.1 n)`, `gG = 1 - 0.2 n`, `gB = (1 - 0.3 t)(1 + 0.1 n)` with `t = temperature/100`, `n = tint/100`. Then divided by the gains' luma so white balance changes color, not brightness.
  - `saturation` field = `1 + slider/100`; `vibrance` field = `slider/100`.
  - Hue matrix is the SVG `hueRotate` matrix with angle = `hueShift/100 × 90°`.
  - `tintVec` = (hue color − its own luma) × `colorizeAmount/100 × 0.6 × 255`.
  - The Mixer and Grade tables are built here (`buildMixLut`, `buildGradeLut`) only when those stages are on.
  - `cp.filters = makeFilterPass(filters)` is built before `active` is computed, and `filters.any` is part of `active`; without that the fast path would skip a lone Filter.
  - `cp.lut` is stored (only when `lut.any()`) before `active` is computed, and `lut.any()` is part of `active`; without that the fast path would skip a lone LUT. A LUT alone also computes the (neutral) white-balance gains: they are within a float rounding of 1, so a LUT-only pass need not be bit-exact with a default pass, which is why `colorPassWithLut` below rebuilds through `makeColorPass` in that case.

**`inline void applyColorPassF(const ColorPass &cp, float *p, std::size_t n)`**
- Inputs:
  - `cp`: the frozen pass.
  - `p`: pointer to RGBA **floats** (levels, R, G, B in 0..255), modified in place.
  - `n`: number of **floats** (4 per pixel), not pixels. Trailing floats that don't make a whole pixel are left alone.
- Returns: nothing. Alpha is never touched. Returns at once if `!cp.active`.
- **The result is neither rounded nor clamped**, so it can lie outside 0..255 (for example with only Vibrance or Saturation active; the Mixer, Grading, Filters and the LUT each clamp their own **input**; the Mixer and the Filters also leave their output inside 0..255, while Grading's offsets and a LUT's table values can take it outside). The caller decides: the float pipeline clamps R, G, B with `render_core::clampRgb`, `applyColorPass` rounds.
- Per-pixel order and rules:
  1. Multiply by the white-balance gains.
  2. **Vibrance**: HSV saturation `s = (max − min)/max`. Positive vibrance scales chroma by `1 + vib(1 − s)` (muted colors most); negative by `1 + vib·s` (saturated colors most). Chroma is measured from Rec. 601 luma.
  3. **Saturation**: chroma scaled about luma by `cp.saturation`.
  4. **Hue**: multiply by `hueM`.
  5. **Mixer**: clamp to 0..255, RGB → HSL, apply `hueShift`/`satMul`/`lumAdj` from `mixLut`, HSL → RGB. Pure grays (max = min) are skipped. The effect fades in with chroma (smoothstep up to `kMixChromaFull`), so near-gray noise does not become colored speckle. Luminance moves toward white (positive) or black (negative) by `lumAdj × w × kMixLumAmount`.
  6. **Colorize**: adds `tintVec × 4·l·(1 − l)` where `l` is luma 0..1 (clamped to 0..1 first; strongest in midtones, zero at black and white).
  7. **Grading**: clamp to 0..255, `yi = int(luma + 0.5)`, add `gradeLut[c][yi]`.
  8. **Filters** (only if `filters.any`): `applyFilterPixel`, still in float. Each filter is `lerp(pixel, filtered, strength)`, so strength 0 is exactly the identity (existing renders do not change). Fixed order: Vintage (desaturate 35%, warm cast, lifted blacks, contrast × 0.92) → Fade (blacks lifted to 40, white stays 255) → Greyscale (`kGradeLuma*`) → Sepia (the classic 3×3 matrix) → clamp to 0..255 → Solarize (invert channel values above 128) → Bleach Bypass (half-desaturate, then overlay the luma: more contrast, less color, black and white fixed) → Cross Process (S-curve on R at 0.9, on G at 0.5, B lifted from 0 to 32 and lowered from 255 to 225) → Threshold (luma at or above `kThresholdLevel` = 128 goes white, below goes black; the slider is the blend strength) → Posterize → Negative (`255 − x`). The input is clamped to 0..255 first, because earlier stages can overshoot. Posterize is a hard quantize whose level count falls smoothly from 256 (slider 0) to 2 (slider 100); 256 levels counts as off.
  9. **3D LUT** (only if `lut.any()`): clamp R, G and B to 0..255 (NaN to 0), then `applyLutPixel`: the table's tetrahedral lookup, blended with the pixel by the amount, `lerp(pixel, lut(pixel), amount)`. Last, so the LUT sees the finished color; before the neighbor effects, the masks and Vignette / Grain (see `renderPipeline` in `render_core.hpp`).
  10. Write the floats back as they are.
- Notes: raw pointers and no `std::` helpers in the loop, on purpose, for Debug-build speed. The Mixer index is computed inline (not via `mixLutIndex`) with the same formula.

**`inline void applyColorPass(const ColorPass &cp, std::uint8_t *p, std::size_t n)`**
- The same pass on RGBA8 bytes, in place. `n` is the number of **bytes** (4 per pixel); trailing bytes that don't make a whole pixel are left alone. Alpha is never touched; returns at once if `!cp.active`.
- How: converts blocks of 1024 pixels to float (a 16 KiB stack buffer), runs `applyColorPassF`, and writes each channel back with `toU8Clamped`, once. Its output is pinned by `cases/color_pass.inc`: `colorPassFloatAgrees` (the float core, rounded by hand, equals this function over 100 random passes), `bulkEqualsPerPixel` (one call over any length, across the 1024-pixel chunk edges, equals one call per pixel; stray trailing bytes and alpha untouched). Used by the byte pipeline (`render_bytes.hpp`) and the tests.

**`inline ColorPass colorPassWithLut(const ColorPass &cp, const LutPass &lut)`**
- Returns a copy of `cp` whose LUT is `lut`; an inactive `lut` (a default `LutPass{}`) removes it. Everything else in the pass is kept exactly. This is how the LUT previews swap one LUT for another on the picture's own `RenderParams` without the sliders that made the pass (`lutPreviewParams`).
- Equivalence: when nothing else is in play (no Mixer, grade, hue, colorize, Filters, gains, vibrance or saturation change) and a LUT is given, the result is **`makeColorPass`'s own pass for a LUT alone**, so a preview matches a real render byte for byte. Otherwise it is `cp` with `lut` replaced and `active` recomputed (`cp.active` is false only when no stage is on, so removing the only active stage turns the pass off).
- Does not change `cp`.


**White-balance picker types**

`enum class WbPickStatus`: `Ok` (solved exactly, to slider rounding), `Limited` (the spot needs more than ±100, so the answer is clamped and the spot ends up close to gray, not exactly gray), `TooDark` (a channel is below `kWbMinLevel`, or an input is NaN/infinity), `Clipped` (a channel is at or above `kWbMaxLevel`, so its real value is lost).

`struct WbPick`: `temperature` and `tint` (`float`, whole slider values in -100..100, 0 unless the status is usable), `status` (starts as `TooDark`), and `bool usable() const` (true for `Ok` and `Limited`).

**`inline WbPick solveWhiteBalance(float r, float g, float b)`**
- Finds the Temperature and Tint whose gains (the ones `makeColorPass` builds) turn the color `(r, g, b)` gray. The three inputs are 0..255 and may be an average over a small patch.
- **What to pass in:** the color as it *reaches the color pass*: the picture after the tone tables (tone sliders and curves) and **before** any white balance. That value does not depend on the current Temperature or Tint, so the answer is absolute: picking twice at the same spot gives the same result. The final picture (what the RGB readout shows) is the wrong input, because it has already been through white balance, saturation, the Mixer, grading and the Filters.
- Math: gray means `R·gR = G·gG = B·gB`. The luma normalization in `makeColorPass` scales all three gains alike, so it does not change ratios, and the system solves in closed form with no iteration: `t = (B − R) / (0.3 (R + B))` from `R(1 + 0.3t) = B(1 − 0.3t)`, then `n = (G − m) / (0.2 G + 0.1 m)` from `G(1 − 0.2n) = m(1 + 0.1n)`, where `m` is the average of red and blue after the temperature gains (the two are equal unless `t` was clamped, and the average then splits the leftover error). Both are scaled by 100, **rounded to whole numbers** (the sliders show integers) and clamped to ±100.
- Status: NaN/infinity → `TooDark`. Then any channel ≥ `kWbMaxLevel` → `Clipped`, then any channel < `kWbMinLevel` → `TooDark` (checked in that order, so a spot that is both reads as clipped). Needing more than ±100 on either slider → `Limited` (still usable). Otherwise `Ok`.
- **Keep it in step with `makeColorPass`.** The constants 0.3, 0.1 and 0.2 are repeated from the gain formulas there. If the gains change, change this function too; the `color_pass` test runs the answer back through `makeColorPass` and fails if they drift apart.

**In the app** (`main.cpp`, not this header): the Adjust pane's **Pick White Balance (W)** button arms a one-click picker. See [architecture.md](architecture.md#white-balance-picker). The edit goes through `AdjustmentSet::setWhiteBalance`.

### `color_mix.hpp`
The Color Mixer (HSL): eight hue bands × (Hue, Saturation, Luminance), baked into three per-hue tables (`MixLut`) of 720 entries (half-degree resolution). Pure math; the sliders' display colors stay with the UI.

**Depends on:** `pixel_math.hpp`. No UI. **Test:** `cases/color_mix.inc`.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kMixMin` / `kMixMax` | `int` | -100 / 100 | Slider range |
| `kMixHueRangeDeg` | `float` | 30 | Hue slider ±100 = ±30° |
| `kMixLumAmount` | `float` | 0.5 | Luminance +100 = halfway to white |
| `kMixChromaFull` | `float` | 0.10 | Chroma (0..1) at which a pixel gets the full effect |
| `kMixLutSize` | `int` | 720 | Table resolution (0.5°) |
| `kMixBandCount`, `kMixChanCount` | `std::size_t` | 8, 3 | Bands and channels |
| `kMixCenters` | `std::array<float, 9>` | 0, 30, 60, 120, 180, 240, 270, 320, 360 | Hue (degrees) at the middle of each band; the last entry closes the circle |

**Types**
- `enum class MixBand : int`: `Red, Orange, Yellow, Green, Aqua, Blue, Purple, Magenta, Count`.
- `enum class MixChan : int`: `Hue, Saturation, Luminance, Count`.
- `using MixValues = std::array<float, 24>`: channel-major, index = `chan * 8 + band`.
- `struct MixLut`: three `std::array<float, 720>`:
  - `hueShift`: degrees to add to the hue.
  - `satMul`: multiplier on HSL saturation, always 0..2.
  - `lumAdj`: luminance amount, -1..+1.

**Functions**

**`constexpr std::size_t mixIndex(MixChan c, std::size_t band)`**: flat index into `MixValues` (`c * 8 + band`). No range check on `band`.

**`inline MixValues defaultMixValues()`**: all 24 sliders at 0.

**`constexpr bool mixCentersValid()`**: true if `kMixCenters` starts at 0, ends at 360 and is strictly increasing (a zero-length segment would divide by zero). Checked by a `static_assert`.

**`inline const char *mixBandName(std::size_t band)`**: "Red" … "Magenta"; empty string for `band >= 8`.

**`inline bool mixAnyNonZero(const MixValues &m)`**: true if any of the 24 values is non-zero.

**`inline bool mixChanAnyNonZero(const MixValues &m, MixChan c)`**: true if any band of channel `c` is non-zero.

**`inline float mixClamped(float v)`**: clamp to `kMixMin..kMixMax`; NaN/infinity → 0.

**`inline std::size_t mixLutIndex(float hueDeg)`**
- Input: hue in degrees (any finite value; wraps).
- Returns: nearest table index, always `0..719`. NaN/infinity → 0.
- Always use this instead of rounding by hand: `round(hue/360 × 720)` reaches 720 for hues near 360. Here 359.9° and -0.1° (the same hue) both give 0, and -0.3° gives 719. (The comment in `color_mix.hpp` says -0.1 → 719; that is off by one step, the code returns 0.)

**`inline MixLut buildMixLut(const MixValues &m)`**
- Input: the 24 slider values (sanitized inside with `mixClamped`).
- Returns: the three tables. For table entry *i* (hue = *i* × 0.5°): find the segment `[kMixCenters[j], kMixCenters[j+1])`, blend band *j* and band *j+1* (Magenta's next is Red) with `t = smoothStep01(0, 1, position in segment)`, divide by 100, then
  - `hueShift = blend(Hue) × 30`,
  - `satMul = 1 + blend(Saturation)`,
  - `lumAdj = blend(Luminance)`.
- A pixel exactly at a band center gets that band's value alone; there are no seams.

### `color_grade.hpp`
Color Grading: Shadows, Midtones, Highlights and Global wheels, baked into one per-luma table of RGB offsets (`GradeLut`). Pure math.

**Depends on:** `pixel_math.hpp`. No UI. **Test:** `cases/color_grade.inc`.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kGradeRangeCount` | `size_t` | 4 | Number of wheels |
| `kGradeTintStrength` | `float` | 0.30 | A saturation-100 tint is the hue's color minus its own luma, times this × 255: the most-shifted channel moves by up to about 27% of 255 (blue or yellow; red only about 21%, green 18%) |
| `kGradeLumAmount` | `float` | 0.30 | Luminance ±100 = ±30% of 255 |
| `kGradeLumaR/G/B` | `float` | 0.299 / 0.587 / 0.114 | Rec. 601 luma. **Shared with `color_pass.hpp`**: the tint is made luma-neutral with them and the table is indexed by luma computed with them, so both sides must agree |

**Types**
- `enum class GradeRange : int`: `Shadows, Midtones, Highlights, Global, Count`.
- `struct GradeWheel`

  | Field | Type | Default | Range | Meaning |
  |---|---|---|---|---|
  | `hue` | `float` | 0 | 0..359° (any value wraps) | Wheel puck angle |
  | `sat` | `float` | 0 | 0..100 | Wheel puck distance |
  | `lum` | `float` | 0 | -100..100 | Luminance offset |
  | `visible` | `bool` | true | | The eye icon; hidden wheels contribute nothing |

  Methods: `neutral()` (`sat == 0 && lum == 0`), `operator==`.
- `struct ColorGrade`: `wheel` (`std::array<GradeWheel, 4>`), `blending` (`float`, 0..100, default 50: how softly ranges overlap), `balance` (`float`, -100..100, default 0: positive moves the crossovers toward the shadows, so the highlights wheel covers more). Methods: `anyActive()` (any visible, non-neutral wheel), `operator==`.
- `using GradeLut = std::array<std::array<float, 256>, 3>`: per-channel (R, G, B) offsets in 0..255 units, indexed by 8-bit luma.

**Functions**

**`inline void gradeHueToRgb(float h, float *rgb)`**
- Inputs: `h` hue in degrees (any value; wraps; NaN/infinity treated as 0); `rgb` pointer to **3 floats** to fill.
- Output: the fully saturated color of that hue, channels 0..1. Also used by `makeColorPass` for Colorize.

**`inline float gradeClampOr(float v, float lo, float hi, float def)`**: `v` clamped to `[lo, hi]`; NaN/infinity give `def` (the neutral value).

**`inline GradeLut buildGradeLut(const ColorGrade &g)`**
- Input: the grade. Every field is sanitized with `gradeClampOr` (NaN gives the neutral value: sat 0, lum 0, blending 50, balance 0).
- Returns: the per-luma offset table.
- How it is built:
  1. Per visible wheel: `tint = (hue color − its luma) × sat/100 × 0.30 × 255`, and `lumOff = lum/100 × 0.30 × 255`.
  2. `pivot = 0.5 − 0.2 × balance/100`; crossovers at `pivot ∓ 0.25`; edge half-width `hw = 0.05 + 0.30 × blending/100` (never below 0.05, so edges don't collapse).
  3. For each luma `y`: `wS = 1 − smoothStep01(lo − hw, lo + hw, y)`, `wH = smoothStep01(hi − hw, hi + hw, y)`, `wM = max(0, 1 − wS − wH)`; if the three sum above 1 they are normalized. Global always has weight 1.
  4. Offsets are the weighted sums of the wheels' tints, plus the weighted luminance offsets added to all three channels.
  5. **Monotonic guard**: the luminance part is held at a plateau wherever `i + offset` would drop below an earlier output level, so large offsets with low Blending can't invert tones. Settings that were already monotonic are copied through unchanged.

### `curves.hpp`
Tone curves: control points, the monotone cubic (PCHIP) that turns them into a table (`curveSample` for any grid, `curveToLut` for the 256-entry byte-level one), and the operations that keep the point list valid.

**Depends on:** `pixel_math.hpp`. No UI. **Test:** `cases/curves.inc`. `tone_table.hpp` samples curves on a finer grid with `curveSample`.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kCurveMinGap` | `float` | 2 | Minimum x distance between points (0..255 scale) |
| `kCurveMaxPoints` | `std::size_t` | 16 | Most points per curve |
| `kCurveChannelCount` | `std::size_t` | 4 | Curves per set |

**Types**
- `struct CurvePoint { float x = 0, y = 0; }`: input level and output level, both 0..255. Has `operator==`.
- `using CurvePoints = std::vector<CurvePoint>`: sorted by `x`. The first and last points sit at x = 0 and x = 255 (their `y` may move, e.g. to lift blacks).
- `enum class CurveChannel : int`: `Master, Red, Green, Blue, Count`. Master applies to all three channels first; Red/Green/Blue then apply to their own channel.
- `using CurveSet = std::array<CurvePoints, 4>`.

**Functions**

**`inline CurvePoints identityCurve()`**: `{(0,0), (255,255)}`. **`inline CurveSet identityCurves()`**: four identity curves.

**`inline bool curveEvaluable(const CurvePoints &pts)`**
- Returns true if there are at least 2 points, all coordinates are finite, and `x` is strictly increasing. This is the minimum `curveSample` / `curveToLut` need; the edit functions below keep stricter invariants.

**`inline void curveSample(const CurvePoints &pts, float *lut, std::size_t count, float step)`**
- Inputs: control points; `lut`, room for `count` floats; `count` the number of samples; `step` the distance between sample inputs (**must be >= 0**: the segment search only walks forward).
- Output: `lut[i]` = the curve at input level `x = i * step`, for `i = 0..count-1`, **unrounded** and clamped to 0..255. The 256-entry, step-1 case is `curveToLut`; the float pipeline samples a finer grid (`kToneSub` = 16 per level, `tone_table.hpp`).
- Details: Fritsch–Butland PCHIP (the weighted-harmonic-mean slope). It passes through every point and never overshoots. Interior slope is the weighted harmonic mean of neighboring secants, or 0 at a local extremum or flat spot. End slopes are shape-preserving one-sided estimates; with exactly two points the curve is the straight line between them. Interval widths are floored at 1e-3. Outside the first/last point the value is held flat.
- Gotcha: if the points are not evaluable (see `curveEvaluable`) the output is the identity `lut[i] = i * step`, which is **not clamped**, so it can pass 255 if `count * step` does.

**`inline std::array<float, 256> curveToLut(const CurvePoints &pts)`**
- Returns: the curve evaluated at every level 0..255 (`curveSample(pts, ..., 256, 1.0f)`), **unrounded**, clamped to 0..255. If the points are not evaluable, returns the identity table. Details as for `curveSample`.

**`inline bool curveIsIdentity(const std::array<float, 256> &lut)`**
- Returns true if every entry rounds to its own index with `toU8Clamped`. The pipeline then skips the curve stage. It must use the same rounding as the code that applies the curve, which is why it uses `toU8Clamped`.

**`inline int curveInsert(CurvePoints &pts, float x, float y)`**
- Inputs: the list (modified), and the new point (`x`, `y` are clamped to 0..255).
- Returns: the new point's index, or **-1** if a value is not finite, the list already has `kCurveMaxPoints` points, the point would be at or beyond an end (an end point can't be duplicated), or it is closer than `kCurveMinGap` to a neighbor.

**`inline void curveMovePoint(CurvePoints &pts, int idx, float x, float y)`**
- Inputs: the list, the point index, and the target position.
- Behavior: out-of-range `idx` or non-finite coordinates are ignored. `y` is always clamped to 0..255 and applied. End points keep their `x`. Interior points move to `x` clamped between their neighbors ± `kCurveMinGap`, so order never changes (if there is no room, `x` is left unchanged).

**`inline bool curveRemove(CurvePoints &pts, int idx)`**
- Removes an interior point and returns true. Returns false (nothing removed) for either end point or an out-of-range index.

### `lut3d.hpp`
3D color LUTs as plain values: the table (`Lut3D`), the name that points at one in the library (`LutRef`), the per-pixel lookup (tetrahedral interpolation) and the frozen form the render loop holds (`LutPass`). No files, no UI, no editor state. See [3D LUTs](architecture.md#3d-luts).

**Depends on:** standard library only. **Tests:** `cases/lut3d.inc`; the LUT through the real pipeline is checked by `cases/lut_render.inc` (numbers) and the golden test's `lut/` scenarios (fingerprints). **Thread model:** no shared state. A `Lut3D` is built once and then only read, so a `std::shared_ptr<const Lut3D>` is shared freely between threads.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kLutMinSize` / `kLutMaxSize` | `int` | 2 / 65 | Grid points per axis a table may have |
| `kLutNameMax` | `size_t` | 48 | Longest LUT name kept |

**Types**

- **`struct LutRef { std::string name; uint64_t hash = 0; }`** with `empty()` (no name) and a defaulted `==`. What an edit stores to say "this picture uses that LUT": the library name and the hash of the file's bytes when it was chosen. Hash 0 means unknown (accept any file by that name).
- **`struct Lut3D`**: `int size`; `domainMin` / `domainMax` (`std::array<float, 3>`, the input range of the file, default 0..1 per channel); `std::vector<float> rgb`, `size³` RGB triples in the picture's own **0..255 units**, the red index varying fastest, then green, then blue (the order of a `.cube` file); and `scale` / `offset` (derived) so that a pixel value maps to a grid position as `pos = value · scale + offset`. `void finalize()` computes `scale` / `offset` (call it after `size` and the domain are set; the parser and `identityLut` do). `bool valid() const`: size in range, exactly `size³ · 3` entries, a finite domain with max above min, finite positive `scale`, finite `offset`. Nothing else in the program looks up a table that is not `valid()`.
- **`struct LutPass { std::shared_ptr<const Lut3D> lut; float amount = 0; }`** with `any()` (a table and an amount above 0). The LUT frozen for the render thread: shared and immutable, so copying a `ColorPass` or a `RenderParams` copies a pointer, however big the table is.

**Functions**

**`inline std::string sanitizeLutName(const std::string &s)`**
- Returns: `s` reduced to printable ASCII (the `TextInput` widget is ASCII), with each of `< > : " / \ | ? *` replaced by `_`, trimmed, cut to `kLutNameMax` and stripped of trailing dots and spaces. May be empty. The result is always safe as a file-name stem, so a name read from a hand-edited sidecar can never point outside the library folder (`"../x"` becomes `".._x"`).

**`inline std::string lutFileStem(const std::string &name)`**: the stem a LUT is stored under: `sanitizeLutName(name)`, `"lut"` if that is empty, and the Windows device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1..9`, `LPT1..9`, any case) prefixed with `_` (`COM10` is not reserved). A name that sanitizes to nothing, such as `".."`, gives `"lut"`. Gotcha: only a name that is **exactly** a device name is caught; Windows also treats `CON.v2` (device name plus a dot and more) as reserved, and `lutFileStem("CON.v2")` returns it unchanged, so `CON.v2.cube` cannot be created on Windows (an import under that name would fail on Windows).

**`inline std::string lutHashToHex(uint64_t h)`** / **`inline bool lutHashFromHex(std::string_view s, uint64_t *out)`**: 16 lowercase hex digits and back. `FromHex` accepts upper case and **only exactly 16 digits**; anything else returns false and leaves `*out` untouched.

**`inline Lut3D identityLut(int n)`**: a table that changes nothing (up to float rounding), `n` clamped to `kLutMinSize..kLutMaxSize`. Used by tests and previews.

**`inline void lutEval(const Lut3D &L, float r, float g, float b, float *out)`**
- Inputs: a **valid** table, and a pixel in 0..255 units (values outside that are clamped first: NaN and -infinity become 0, +infinity 255). `out` receives three floats in the same units (not clamped: a table may hold values outside 0..255).
- Method: the pixel value `v` is read as `v / 255` in the file's domain units and mapped to a grid position through the domain (`scale` / `offset`): with `DOMAIN_MAX` 0.5, pixel value 127.5 is the **last** grid point, and with a domain of -1..1 pixel value 0 sits at the middle of the grid. The position is clamped to the grid, and the cell it falls in is cut into **six tetrahedra** along its main diagonal. The output is linear inside the tetrahedron the fractional parts select: the path from the cell's origin to its far corner visits the axes in order of decreasing fractional part. Compared with trilinear this keeps the gray axis exactly gray (no color cast between grid points) and reproduces any table that is affine inside a cell exactly (`cases/lut3d.inc` checks the affine case on a 5-point grid with 2000 random pixels, counting that every one of the six tetrahedra was hit, and checks that an identity table keeps every gray 0..255 gray).
- Raw pointers and no `std::` helpers, for Debug-build speed, like `applyColorPass`.

**`inline LutPass makeLutPass(std::shared_ptr<const Lut3D> lut, float slider)`**
- `slider` is LUT Amount, 0..100. Returns the **inactive** pass (no table, amount 0) for a null table, an invalid table, a NaN/infinite slider, or a slider that clamps to 0; otherwise the table and `clamp(slider, 0, 100) / 100`.

**`inline void applyLutPixel(const LutPass &p, float &r, float &g, float &b)`**: one pixel in place, `lerp(pixel, lut(pixel), p.amount)`. Requires `p.any()`. The caller clamps to 0..255 first (the color pass does), because the blend starts from the pixel as given.

---

## Detail effects

### `detail_layer.hpp`
Texture, Clarity and Sharpness, which share one idea, the **detail layer**: `detail = luma − blur(luma)`, then `result = pixel + gain × detail`. The three differ only in blur size. All of it lives in `namespace detail_layer`.

**Depends on:** standard library only. No UI (plain RGBA8 or 16-bit pointers). **Test:** `cases/detail_layer.inc`. For 16-bit sources it checks only the blur plane against a reference (`blurMatchesReference16`) and a flat plane; the 16-bit `addPixels` and `SrcTraits<uint16_t>::lumaMid` are exercised only through `cases/render_core.inc` (see that entry). The planes are built from, and each pixel's own luma is measured on, the **source at its own precision**: RGBA8 (`uint8_t`) or 16-bit (`uint16_t`), through `SrcTraits<S>` (below). For 8 bits every result is bit-identical to what it always was; only the destination may be float.

**Design rules worth knowing**
- The blur is of the **source** picture's luma, so a plane depends only on the image: it is built once and cached. No slider drag recomputes it (Texture's own slider only changes `gain`).
- Trade-off: detail is measured before tone mapping, so a big Exposure boost does not amplify it.
- The plane covers the whole image, so no halo rows are needed and masks work for free.
- Only luma is blurred and the same amount is added to R, G and B, so edges get no color fringes.
- Blur sizes are fractions of the long edge, so the 1024 px preview and the full render look alike. The box radius is an integer ≥ 1, so the smallest blurs (Sharpness on anything under about 6000 px) are wider than nominal.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kKindCount` | `int` | 3 | Number of layers |
| `kSigmaFrac[3]` | `float[3]` | 0.0012, 0.006, 0.0003 | Gaussian sigma as a fraction of the long edge (Texture, Clarity, Sharpness) |
| `kSigmaMin[3]` | `float[3]` | 1, 3, 0.6 | Floor on sigma, in pixels |
| `kGain[3]` | `float[3]` | 0.8, 1.0, 1.5 | What slider ±100 adds |
| `kClarityLimit` | `float` | 64 | Soft limit on Clarity's detail, in levels |
| `kMaxBoxRadius` | `int` | 1000 | Keeps every running sum exact in float for 8-bit sources (at most about 8.2 million, below 2^24). With 16-bit sources a sum can pass 2^24 (the sums are `uint32_t`, still exact); only its conversion to float rounds, by under 0.01 plane count |
| `kLumaR/G/B` | `int` | 54, 183, 19 | 8-bit luma weights, /256, summing to exactly 256 (`static_assert`). Used both to build planes and to measure each pixel; they must agree |

**Types**
- `enum Kind : int { Texture = 0, Clarity = 1, Sharpness = 2 }`: index of a layer (unscoped, inside the namespace).
- `using Plane = std::vector<uint16_t>`: `width × height` values, each **luma × `kFix`**: 16 (4 fractional bits) for an 8-bit source, 64 (6 fractional bits) for a 16-bit one, so the blur is not rounded to whole levels before it is subtracted.
- `struct Gains`: `float g[3]` (per kind; default 0). Methods: `any()` (any gain non-zero), `need()` (bitmask; bit *k* set if layer *k*'s gain is non-zero), `operator==`.
- `struct Planes`: `Cache::PlanePtr p[3]`; a null entry means that layer isn't needed.
- `class Cache`: see below.

**Functions**

**`inline int lumaOf(const uint8_t *p)`**: `p` points at an RGBA pixel. Returns 0..255, `(54R + 183G + 19B + 128) >> 8`.

**`template <class S> struct SrcTraits`** (specialized for `uint8_t` and `uint16_t`): the one place that knows how the planes and passes read one source pixel. Members: `kFix` / `kInvFix` (plane units per level and its inverse), `level(v)` (a sample in levels), `lumaFix(p)` (luma in plane units), `lumaMid(p, &lumaFix, &mid)` (that plus Clarity's midtone weight `4 l (1 − l)`), `chroma(p, &cr, &cb)` (R − luma, B − luma in levels), `fixFromSum(sum, cnt)` (a block average in plane units) and `darkFix(m)` (a sample in plane units).
- `SrcTraits<uint8_t>`: `kFix` 16 (4 fractional bits). The integer arithmetic these passes always used: luma is rounded to a whole level, the midtone weight comes from the 256-entry table. Bit-identical to before.
- `SrcTraits<uint16_t>`: `kFix` 64 (6 fractional bits). A sample is `v / 257` levels (a division, so a widened 8-bit value `k * 257` is exactly `k`). Luma is **not** rounded (the weighted sum of samples is below 2^24, exact in float, times `1 / (256 * 257)`), and the midtone weight is computed from it. Rounding luma to a level was a ±0.5 level sawtooth on every gradient that Texture, Clarity and Sharpness multiplied; on a smooth 16-bit gradient the measured detail is now about 0.01 level (about 0.3 to 0.6 for the same gradient squeezed to 8 bits).

**`inline int boxRadius(int kind, int w, int h)`**
- Inputs: layer `kind` (0..2, **not validated**), picture size.
- Returns: box radius `r` (box width 2r + 1), clamped to `1..1000`.
- Details: `sigma = max(kSigmaMin, kSigmaFrac × longEdge)`. Three box passes approximate a Gaussian when `width = sqrt(4 sigma² + 1)`, and `r = round((width − 1)/2)`.

**`inline bool cancelled(const std::atomic<uint64_t> *latest, uint64_t gen)`**: true if `latest` is non-null and no longer equals `gen` (relaxed load). A null `latest` never cancels.

**`inline void hboxRow(uint16_t *row, size_t W, int r, uint16_t *pad, float inv)`**
- Horizontal box blur of one row, **in place**. `pad` is scratch for `W + 2r` values; `inv = 1/(2r+1)`. Edges replicate the border pixel, so a flat picture stays flat. Does nothing if `W == 0`.

**`inline bool vbox(const uint16_t *in, uint16_t *out, size_t W, size_t H, int r, uint32_t *col, float inv, const std::atomic<uint64_t> *latest, uint64_t gen)`**
- Vertical box blur `in` → `out` (different buffers). `col` is scratch for `W` running column sums, so memory is read row by row. Edges replicate. Polls cancellation every 16 rows.
- Returns: false if cancelled (`out` is then incomplete).

**`inline bool blurLuma(const uint8_t *rgba, ...)`** and **`blurLuma(const uint16_t *rgba, ...)`** (both `(int w, int h, int r, Plane &out, const std::atomic<uint64_t> *latest, uint64_t gen)`; one template, `blurLumaT<S>`)
- Inputs: RGBA8 picture, size, box radius `r` (≥ 1), the output plane (resized), and the cancellation pair.
- Returns: true on success. False if `rgba` is null, a size is non-positive, `r < 1`, or it was cancelled; `out` is then garbage.
- Details: luma × `kFix` (16 for 8-bit, 64 for 16-bit sources), then 3 horizontal passes, then 3 vertical passes.

**`template <class S> inline uint64_t fingerprint(const S *rgba, int w, int h)`** (S = `uint8_t` or `uint16_t`; the sample size is mixed in, so a 16-bit picture never shares a key with an 8-bit one)
- Returns: a 64-bit FNV-1a hash seeded with the size, over about 1024 samples (bytes, or 16-bit values) spread across the picture plus the last one. **Not a full hash**: a small edit between the samples goes unnoticed, which is why the cache assumes pixels are never edited in place.

**`class Cache`** (shared by the UI thread and render workers; internally locked)
- `using PlanePtr = std::shared_ptr<const Plane>`.
- `PlanePtr get(const uint8_t *rgba, ...)` / `get(const uint16_t *rgba, ...)` (`int w, int h, int kind, const std::atomic<uint64_t> *latest, uint64_t gen`): the plane for this picture and layer, built if absent (from the source at its own precision). Returns **null** if cancelled (nothing is cached then) or if an argument is invalid (null pixels, non-positive size, kind outside 0..2). The key is pixel pointer + size + kind + fingerprint. The blur runs outside the lock; two threads may rarely build the same plane and one wins.
- `void clear()`: drops everything. Call it after any in-place edit of pixels.
- `size_t size() const`: number of cached planes.
- Limits: 9 entries and 384 MiB. The least recently used are evicted, never the newest. A plane still held by a render stays alive through its `shared_ptr`.

**`inline Cache &cache()`**: the one process-wide `Cache`.

**`inline Gains gainsFromValues(float texture, float clarity, float sharpness)`**
- Inputs: slider values; texture and clarity -100..100, sharpness 0..100. Each is clamped; NaN/infinity count as 0.
- Returns: `g = value/100 × kGain[kind]`. Always finite.

**`inline bool acquire(Planes &out, const uint8_t *rgba, int w, int h, unsigned need, const std::atomic<uint64_t> *latest, uint64_t gen)`** and its twin with `const uint16_t *rgba` (both are one-line wrappers of `template <class S> acquireT`)
- Inputs: `need` is the bitmask from `Gains::need()`. Fills `out.p[k]` for each requested layer from the cache.
- Returns: false if cancelled. Returns **true** with the planes left null if `rgba` is null or a size is non-positive (nothing to do).

**`inline const float *midWeights()`**: pointer to a 256-entry table of `4 l (1 − l)` (l = index/255). Clarity uses it so it leaves the ends alone and can't clip highlights or crush shadows.

**`inline void putLevel(uint8_t *d, float v)`** / **`inline void putLevel(float *d, float v)`**: stores one channel of a finished pixel. A byte gets `v` clamped to 0..255 and rounded to nearest; a float gets it clamped to 0..255 and **not rounded** (the float pipeline rounds once, at the very end). Plain comparisons, no `std::` calls (NaN is not handled here; gains are always finite).

**`template <class Dst, class S> inline void addPixels(const Planes &pl, const Gains &gn, const S *src, Dst *dst, size_t count, size_t first)`** (`S` = `uint8_t` or `uint16_t`, deduced; `Dst` stays the first parameter so `addPixels<Dst>(...)` keeps working)
- Inputs:
  - `pl`, `gn`: the planes and gains for this render.
  - `src`: the **source** pixels for these positions (they give each pixel's own luma).
  - `dst`: the pixels to modify, 4 samples per pixel, alpha untouched. `Dst` is `uint8_t` (RGBA8 bytes) or `float` (levels 0..255, what the float pipeline passes). They may already be tone/color adjusted.
  - `count`: number of pixels.
  - `first`: index (`y × width + x`) of the first pixel in the whole picture, used to index the planes.
- Behavior: for each pixel, `d = luma − blur` in levels, then one offset `k` is added to R, G and B through `putLevel` (clamped; rounded to a byte only for a `uint8_t` destination, left as a float for a `float` one):
  - Texture: `k += gTexture × d`.
  - Clarity: `d` is soft-limited, `d / (1 + |d|/64)`, then `k += gClarity × mid[luma] × d`.
  - Sharpness: `k += gSharpness × d`.
- A plane that doesn't cover `[first, first + count)` (built for another picture size) is skipped, not read out of range. Returns at once if all effective gains are 0. Raw pointers and no `std::` helpers in the loop, for Debug-build speed.

### `neighbor_fx.hpp`
Dehaze, noise reduction (luminance and color) and Blur, built on the same idea as `detail_layer.hpp`: every effect is a per-pixel formula that reads precomputed, cached planes of the **source** picture, so it needs no halo rows, works the same inside masks, and never recomputes while a slider is dragged. Read the top of `detail_layer.hpp` first. Everything is in `namespace neighbor_fx` (alias `dl` = `detail_layer`).

**Depends on:** `detail_layer.hpp`. No UI. **Test:** `cases/neighbor_fx.inc`, which also holds the float-destination tests (`floatAgreesWithBytes`, `floatPartialRangesMatchWhole`, `floatKeepsPrecisionAndClamps`). It builds only 8-bit sources: the 16-bit source path of this header (`Map::fix` = 64, the `uint16_t` `acquire`, the `S` of the passes) is covered only through `cases/render_core.inc`.

**How each effect works**
- **Luminance noise reduction** takes away only the *small* part of the finest detail layer (the Sharpness plane: pixel minus a roughly 1.4 px blur). Per pixel, with `d` = that difference in levels and `s` = the slider (0..1): `k = −s·d / (1 + d²/T²)` is added to R, G and B, where `T = 2 + 14·s` levels. Small differences (noise) are removed, large ones (real edges) are spared.
- **Color noise reduction** moves each pixel toward its blurred chroma (R − luma and B − luma; G follows because chroma has zero luma). With `dr`, `db`, `dg` the moves, the weight is `s / (1 + (dr² + db² + dg²)/20²)`, so a genuine color edge (a big move) is left alone.
- **Dehaze (positive)**: dark-channel prior. A tiny "veil" map gives `t = 1 − omega·dark/A` (`omega = 0.85 × amount`, clamped to `t >= 0.25`), then `J = A + (I − A)/t`. **Dehaze (negative)** needs no map: a flat blend toward gray 235 by `0.6 × |amount|`.
- **Blur**: a color blur stored at three fixed strengths at reduced resolution; the slider interpolates between source → level 0 → level 1 → level 2.
- Noise reduction and blur are applied as a **delta measured on the source** ("blurred minus source") added to the current pixel, so they sit on top of the tone and color sliders instead of undoing them.
- Order inside `apply()`: Dehaze, detail layers (`addPixels`), luminance NR, color NR, Blur last (so it wins).

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kChromaSigmaFrac`, `kChromaSigmaMin` | `float` | 0.0025, 2 | Chroma blur sigma: fraction of the long edge, floored at 2 px |
| `kNrLumaTMin`, `kNrLumaTMax` | `float` | 2, 16 | Luminance NR threshold `T` (levels) at slider 0 / 100 |
| `kNrColorT` | `float` | 20 | Chroma move that gets half weight |
| `kVeilEdge` | `int` | 384 | Long edge of the veil map, px |
| `kVeilPatch` | `int` | 3 | Erosion radius (and blur radius) on the veil map |
| `kDehazeStrength` | `float` | 0.85 | "omega": how much of the veil to remove |
| `kDehazeMinT` | `float` | 0.25 | Transmission floor (limits blow-up) |
| `kLightMin` | `float` | 180 | Estimated airlight is clamped to `[180, 255]` |
| `kHazeAdd` | `float` | 0.6 | Negative Dehaze: blend amount at -100 |
| `kHazeLight` | `float` | 235 | ...toward this gray |
| `kBlurSigmaFrac[3]` | `float[3]` | 0.004, 0.012, 0.035 | Blur sigma of each level, times the long edge |
| `kBlurSigmaMin` | `float` | 1.5 | Floor on blur sigma, px |
| `kBlurTargetSigma` | `float` | 2.5 | Sigma, in reduced px, of the stored blur (sets the reduction factor) |

**Types**
- `struct Map { int w, h, ch = 1; float scale = 1; float aux = 0; float fix = 16; std::vector<uint16_t> v; }`: a small multi-channel map at reduced resolution. Values are stored × `fix` (16 for an 8-bit source, 64 for a 16-bit one; 4 or 6 fractional bits), so a value in levels is `v / fix`. `scale` = reduced px per source px; `aux` holds the estimated airlight `A` for a veil map. `using MapPtr = std::shared_ptr<const Map>`.
- `template <class T> class Store`: a keyed, byte-capped LRU cache for any plane type (`Ptr` = `std::shared_ptr<const T>`).
  - `Store(size_t maxBytes, size_t maxEntries)`.
  - `template <class S, class Build> Ptr get(const S *rgba, int w, int h, int kind, Build &&build)` (`S` = `uint8_t` or `uint16_t`): returns the cached item or builds it. `build` is a callable returning `std::shared_ptr<T>` or null if cancelled; a cancelled build is never cached. Key = pixel pointer + size + `detail_layer::fingerprint` + `kind`. The build runs outside the lock.
  - `void clear()`, `size_t size() const`. Eviction drops the least recently used, never the newest.
- `struct Gains`: slider values as multipliers.

  | Field | Type | Range | Meaning |
  |---|---|---|---|
  | `detail` | `dl::Gains` | | Texture / Clarity / Sharpness |
  | `dehaze` | `float` | -1..+1 | -1 adds haze, +1 removes it |
  | `nrLuma`, `nrColor`, `blur` | `float` | 0..1 | |

  Methods:
  - `bool any() const`: any gain non-zero.
  - `unsigned blurLevels() const`: bitmask over blur levels 0..2 this setting reads. `pos = min(blur, 1) × 3`; segment 0 → `1` (level 0), segment 1 → `3` (levels 0, 1), segment 2 → `6` (levels 1, 2). 0 when `blur <= 0`.
  - `uint32_t need() const`: which planes a render needs. **Bits 0..2** the luma detail planes (Texture, Clarity, Sharpness; luminance NR uses Sharpness's), **bit 3** the two chroma planes, **bit 4** the veil (only for positive Dehaze), **bits 5..7** blur levels.
  - `operator==`.
- `struct Planes`: `dl::Planes luma`, `dl::Cache::PlanePtr chroma[2]` (R − luma, B − luma), `MapPtr veil`, `MapPtr blur[3]`. Null = not needed.
- `struct Walker`: steps through consecutive pixels of a picture `W` wide keeping `(x, y)`. `Walker(size_t first, int width)` (width must be positive), `next()`.

**Stores (process-wide)**

| Function | Holds | Limits | `kind` keys |
|---|---|---|---|
| `Store<dl::Plane> &chromaStore()` | full-size chroma planes | 256 MiB, 6 entries | 10, 11 |
| `Store<Map> &mapStore()` | veil and blur maps (reduced size) | 64 MiB, 40 entries | veil 20; blur 30..32 |

With `detail_layer`'s own cache (384 MiB) the worst case is about 700 MB of cached planes on very large pictures. **`inline void clearCaches()`** empties `dl::cache()`, `chromaStore()` and `mapStore()`.

**Functions**

**`inline int boxRadiusForSigma(float sigma)`**: box radius whose 3-pass blur approximates a Gaussian of `sigma` (same derivation as `detail_layer::boxRadius`), clamped to `1..dl::kMaxBoxRadius`.

**`inline bool blurPlane(dl::Plane &p, size_t W, size_t H, int r, const std::atomic<uint64_t> *latest, uint64_t gen)`**: 3 box passes each way, **in place**. Returns false if cancelled (then `p` is garbage).

**`inline float chromaSigma(int w, int h)`**: `max(2, 0.0025 × long edge)`.

**`template <class S> inline bool buildChroma(const S *rgba, int w, int h, int which, dl::Plane &out, const std::atomic<uint64_t> *latest, uint64_t gen)`**
- `which`: 0 = blurred (R − luma), 1 = blurred (B − luma). Stored as `(chroma + 255) × kFix` (16 for 8-bit, 64 for 16-bit sources) so the signed value fits an unsigned plane. Returns false if cancelled.

**`template <class V> inline void erode(std::vector<V> &m, int w, int h, int r)`**: separable min filter of radius `r` on a small map (8- or 16-bit values), in place, edges clamped.

**`template <class S> inline std::shared_ptr<Map> buildVeil(const S *rgba, int w, int h, const std::atomic<uint64_t> *latest, uint64_t gen)`**
- Returns the veil map, or null if cancelled. Steps: min-pool `min(R, G, B)` over `f × f` blocks (`f = ceil(longEdge / 384)`), erode by `kVeilPatch`, set airlight `A` = the brightest dark-channel value clamped to `[180, 255]` (stored in `aux`), then blur the map.

**`template <class S> inline std::shared_ptr<Map> buildBlurLevel(const S *rgba, int w, int h, int level, const std::atomic<uint64_t> *latest, uint64_t gen)`**
- `level` 0..2 (**not validated**). Builds a 3-channel color blur of sigma `max(1.5, kBlurSigmaFrac[level] × longEdge)`, stored at reduced size (box average by `f = round(sigma / 2.5)`, then a small blur). Null if cancelled.

**`inline void sampleMap(const Map &m, int x, int y, float *out)`**: bilinear sample of a map at **source** pixel `(x, y)`; `out` receives `m.ch` values (still × `m.fix`: 16 for an 8-bit source, 64 for a 16-bit one). Coordinates are clamped to the map.

**`inline Gains gainsFromValues(float texture, float clarity, float sharpness, float dehaze = 0, float noiseLuma = 0, float noiseColor = 0, float blur = 0)`**
- Inputs are UI values: texture, clarity, dehaze in -100..100; the rest 0..100. Each is clamped and NaN/infinity count as 0, so every gain is finite. Returns the `Gains` (values divided by 100; the three detail values go through `dl::gainsFromValues`).

**`inline bool acquire(Planes &out, const uint8_t *rgba, int w, int h, uint32_t need, const std::atomic<uint64_t> *latest, uint64_t gen)`** and its twin with `const uint16_t *rgba` (both call `template <class S> acquireT`; the 8-bit one is a plain function so a null pointer still finds it)
- Fetches (or builds, the first time) the planes in `need`. Returns false if cancelled. With null pixels or a non-positive size it returns **true** and leaves the planes null.

**`inline uint8_t toU8(float v)`**: the `toU8Clamped` rule without `std::` calls (NaN → 0, over 255 → 255, otherwise `v + 0.5` truncated).

**`inline void put(uint8_t *d, float v)`** / **`inline void put(float *d, float v)`**: stores one channel of a finished pixel. A byte gets `toU8(v)` (rounded and clamped); a float gets `v` clamped to 0..255 (NaN to 0) and **not rounded**, so the float pipeline keeps its precision between passes and rounds once at the end.

**`inline bool covers(const dl::Plane *p, size_t first, size_t count)`**: true if the plane exists and covers pixels `[first, first + count)`.

**Per-pixel passes** (all are templates over the destination sample type `Dst`, `uint8_t` or `float`, and take `count` consecutive pixels of `dst`, 4 samples per pixel, alpha untouched; every channel is stored through `put`; `src` is the **source** pixels at the same positions (RGBA8 or 16-bit: a second template parameter `S`, deduced, which must be the type the planes were built from), whatever `Dst` is; `first` is the index in the whole picture; the loops use raw pointers for Debug-build speed)
- **`template <class Dst> dehazePass(const Map *veil, float amount, Dst *dst, size_t count, size_t first, int W)`**: see above. A positive amount with a null `veil` does nothing.
- **`template <class Dst, class S> nrLumaPass(const dl::Plane *sharp, float s, const S *src, Dst *dst, size_t count, size_t first)`**: skipped if the plane doesn't cover the range.
- **`template <class Dst, class S> nrColorPass(const dl::Plane *cr, const dl::Plane *cb, float s, const S *src, Dst *dst, size_t count, size_t first)`**: skipped unless both planes cover the range. `dg` is derived so the luma-weighted moves sum to zero.
- **`template <class Dst, class S> blurPass(const MapPtr *levels, float amount, const S *src, Dst *dst, size_t count, size_t first, int W)`**: `pos = min(amount, 1) × 3`, `seg = min(int(pos), 2)`, `t = pos − seg`. Blends level `seg−1` (or the source itself when `seg == 0`) toward level `seg`, then adds `blurred − source` to `dst`. Skipped if a needed level is missing.

**`template <class Dst, class S> inline void apply(const Planes &pl, const Gains &g, const S *src, Dst *dst, size_t count, size_t first, int W)`**
- Runs all the effects in the order above for `count` consecutive pixels. `W` is the picture width and must be positive, otherwise nothing is done. Each effect runs only when its gain is non-zero.

### `film_fx.hpp`
Vignette and Grain: the two effects that depend on **where a pixel is**, which no other stage knows. Plain functions on plain values, no UI and no editor state. Both are the last step of the pixel pipeline (after the masks, see `render_core::renderPipeline`) and global only. `applyF` runs them on float levels (what the float pipeline uses); `apply` is the same on RGBA8 bytes.

**Depends on:** `pixel_math.hpp` (`toU8Clamped`). **Test:** `cases/film_fx.inc` (and `cases/render.inc`, `cases/render_params.inc`, `cases/adjustment_set.inc`, the golden scenarios `film/` and `geo_film/`; `cases/render_core.inc` checks that the film effects alone give exactly the old byte pipeline's bytes).

- **Vignette** is measured against the picture's own frame, i.e. it is "post-crop": centered on the picture as shown or exported and following its aspect ratio, so a crop, rotation or flip just gives the vignette of the new frame. The distance `d` is 0 at the center and 1 at the corners (an ellipse through all four corners). A smoothstep ramp centered on `midpoint` with a half-width set by `feather` turns `d` into a weight `t` (0 untouched, 1 full). Negative amount darkens, `p' = p (1 - k|a|t)`; positive lightens, `p' = p (1 - k a t) + 255 k a t`. No `GeoFrame` is needed because the renderer is handed the already-geometried picture.
- **Grain** is a deterministic function of the pixel's (x, y) in the picture being rendered: an integer hash, with no random number generator, no state, and no dependence on render order, block size, thread or compiler. It is added equally to R, G and B (monochrome), scaled by a weight that is 1 at mid-gray and 0.4 at black and white (`4·l·(1 − l)` blended with `kGrainEndWeight`; `l` is the pixel's luma `(54R + 183G + 19B) / (256 · 255)`, clamped to 0..1, measured after the vignette). `size` sets the grain cell as a fraction of the long edge (at least 1 px), so the preview and the full picture look alike (not the same pixels: their grids differ); `roughness` blends smooth value noise (per cell) with crisp per-pixel noise.
- Both are computed in float on the finished pixel. `applyF` leaves the result **unrounded and unclamped** (the caller rounds once); `apply` rounds once with `toU8Clamped`. Pixels with alpha 0 (the empty corners of a straightened picture) are left exactly as they are; alpha is never changed.
- Order inside `applyF()` / `apply()`: vignette, then grain.

**Constants** (namespace `film_fx`)

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kVignetteStrength` | `float` | 0.9 | At |amount| = 1 and full weight a channel moves 90 % of the way to black (or white), never all the way |
| `kMidpointSpan` | `float` | 0.45 | Midpoint ±100 moves the ramp center, in corner-distance units, around 0.5 by ±0.45 |
| `kRampHalfMin`, `kRampHalfMax` | `float` | 0.05, 0.50 | Half-width of the ramp at Feather 0 / 100 (so the ramp is never empty) |
| `kGrainLevels` | `float` | 48 | Grain 100 adds up to about this many levels |
| `kCellFracMin`, `kCellFracSpan` | `float` | 0.0004, 0.0026 | Grain cell = long edge × (min + span × size), at least 1 px |
| `kSmoothGainMax` | `float` | 1.5 | Gain on the smooth (value-noise) part for wide cells, so Size does not also mean "weaker" |
| `kGrainEndWeight` | `float` | 0.4 | Grain weight at black and white (1 at mid-gray) |
| `kSeedCrisp`, `kSeedSmooth` | `uint32_t` | `0x6C697465`, `0x726F6F6D` | Seeds of the two noise layers |

**Types**
- `struct Params`: `vignette` (-1..1), `midpoint` (0.05..0.95), `feather` (0..1), `grain` (0..1), `size` (0..1), `roughness` (0..1). `hasVignette()`, `hasGrain()`, `any()`, `operator==`. A default `Params` does nothing.
- `struct Prepared`: a `Params` plus everything the per-pixel loop needs for one picture size (`W`, `H`, `vig`, `grain`, `invHalfW/H`, the ramp `inner` / `invRamp`, the grain `cell` / `invCell` / `smoothGain`). `smoothGain = 1 + 0.5 × min(1, cell − 1)` (`kSmoothGainMax` = 1.5 is the cap), so a 1 px cell gets no extra gain and cells 2 px or wider get the full 1.5. `any()`.

**Functions**
- **`inline Params paramsFromValues(float vignette, float midpoint, float feather, float grain, float size, float roughness)`**: from the six slider values (Vignette, Midpoint, Feather, Roughness -100..100; Grain, Size 0..100). Midpoint, Feather and Roughness are 0 at their default, so a file that only sets "Vignette" is a normal vignette. Every input is clamped, and NaN or infinity count as 0.
- **`inline Prepared prepare(const Params &p, int W, int H)`**: resolves the params for one picture size. A non-positive size gives a `Prepared` that does nothing.
- **`inline uint32_t hash2(uint32_t x, uint32_t y, uint32_t seed)`**: the 32-bit integer hash (unsigned arithmetic only, so identical on every compiler; a few values are pinned in the test). **`hashUnit`** turns it into a float in [-1, 1).
- **`inline float valueNoise(float u, float v, uint32_t seed)`**: smooth value noise at grain-cell coordinates (`u`, `v` ≥ 0); at integer coordinates it is exactly the hash there.
- **`inline float grainAt(const Prepared &f, int x, int y)`**: the grain value at a pixel (roughly -1.5..1.5, before Amount and the tone weight). Depends only on (x, y), size and roughness.
- **`inline float vignetteWeight(const Prepared &f, int x, int y)`**: the vignette weight 0..1 of a pixel.
- **`inline float finiteClamp(float v, float lo, float hi)`**: `v` clamped to `[lo, hi]`; NaN/infinity give 0 (used by `paramsFromValues`).
- **`inline void applyF(const Prepared &f, float *p, size_t count, size_t first)`**: applies both to `count` consecutive RGBA pixels of **float levels** (4 floats per pixel, 0..255) of a picture `W` wide, in place. `first` is the index (`y * W + x`) of the first pixel in the **whole** picture, so any split of the picture into blocks (even mid-row) gives the same result. A pixel whose alpha is exactly 0 is left alone; alpha is never changed. The result is **not rounded or clamped**. Does nothing when `any()` is false. The loop uses raw pointers and no `std::` helpers (apart from `sqrt`) for Debug-build speed.
- **`inline void apply(const Prepared &f, uint8_t *p, size_t count, size_t first)`**: the same on `count` consecutive RGBA8 pixels, in place: `applyF` on blocks of 1024 pixels (a 16 KiB stack buffer), each channel rounded and clamped once with `toU8Clamped`. Gives the bytes the old byte loop gave. Same `first` rule; does nothing when `any()` is false.

---

## Geometry

### `geometry.hpp`
`CropRect`, `Orient`, the rotated size, the `GeoFrame`, and the rotate/flip algebra. Pure math, no pixels.

**Depends on:** `pixel_math.hpp` (`kDegToRad`). No UI. **Test:** `cases/geometry.inc`.

**Constants:** `kAngleMax` (`float`, 45): the Straighten range in degrees; positive turns the picture clockwise. With the 90° buttons it reaches any angle.

**Order of operations** (as `syncSource` runs them): orient (mirror, then quarter turns) → straighten → crop. Masks are stored against the **original** picture and mapped through `GeoFrame`, so no geometry change ever has to touch a mask.

**Types**
- `struct CropRect { int x = 0, y = 0, w = 0, h = 0; }`: a region in pixels of the oriented, straightened image. `empty()` is true when `w <= 0 || h <= 0`; a default `CropRect` means "no crop". Has `operator==`.
- `struct Orient { int rot = 0; bool flip = false; }`: an optional left-right mirror, then `rot` clockwise quarter turns (0..3). Covers all 8 ways to rotate/flip. `identity()` is `rot == 0 && !flip`.
- `struct Dims { int w, h; }`.
- `enum class GeoOp { RotateCW, RotateCCW, FlipH, FlipV }`.
- `struct GeoFrame`: one similarity transform `p_source = L · p_original + t`, in continuous pixel coordinates (pixel centers at +0.5).

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `m00, m01, m10, m11` | `double` | 1, 0, 0, 1 | The 2×2 matrix `L` (orthogonal: a rotation, or a mirror then a rotation) |
  | `tx, ty` | `double` | 0 | Translation |
  | `origW, origH` | `double` | 1 | Size of the untouched picture, px |
  | `srcW, srcH` | `double` | 1 | Size after orient / straighten / crop, px |

  Methods:
  - `double det() const`: determinant of `L` (negative when mirrored).
  - `void toSrc(double x, double y, double &ox, double &oy) const`: original → source.
  - `void toOrig(double x, double y, double &ox, double &oy) const`: source → original.
  - `double radiusScale() const`: `min(origW, origH) / min(srcW, srcH)`. Radii are stored as a fraction of the original's shorter side but shown against the current picture's.
  - `static float wrapAxis(float a)`: an axis angle folded into `[-90, 90]`. NaN/infinity → 0; never returns -0. Safe for huge angles (an older subtract-180 loop never terminated).
  - `static float wrapDir(float a)`: an arrow angle kept in `(-180, 180]`. NaN/infinity → 0.
  - `float axisToSrc(float deg) const` / `float axisToOrig(float deg) const`: direction of an **axis** (degrees clockwise, no head or tail) between original and source; result in `[-90, 90]`. A mirror flips the sign.
  - `float dirToSrc(float deg) const` / `float dirToOrig(float deg) const`: direction of an **arrow** (degrees clockwise from +x), result in `(-180, 180]`. The unit vector is pushed through the linear part and the angle re-read, so a mirror or half turn does not fold the arrow onto its opposite.

**Functions**

**`inline Dims rotatedDims(int w, int h, float deg, bool expand)`**
- Inputs: picture size, turn angle `deg` (either direction), and the mode.
- Returns: the size after turning:
  - `expand = true`: the bounding box of the turned picture (nothing lost, empty corners).
  - `expand = false` (fit): the largest rectangle with the picture's own aspect ratio inside the turned picture (no empty corners).
- Edge cases: a non-positive size comes back as at least 1 × 1; `deg == 0` or NaN/infinity returns `{w, h}`. A tiny epsilon keeps exact quarter-turns from rounding up. Both formulas are symmetric in (w, h), so a later quarter turn gives exactly the transposed size.

**`inline GeoFrame makeGeoFrame(int ow, int oh, const Orient &o, float angle, bool expand, const CropRect &crop)`**
- Inputs: original size `ow × oh`, orientation, straighten `angle` (degrees), `expand` mode, and crop.
- Returns: the frame. With no known size (`ow <= 0` or `oh <= 0`) it returns the identity frame, so masks pass through unchanged.
- Details: `o.rot` is taken modulo 4; a NaN/infinite angle counts as 0. Mirror is `(x, y) → (w − x, y)`; a quarter turn is `(x, y) → (h − y, x)`. Straighten rotates clockwise about the center into the `rotatedDims` size. The crop is clamped exactly as `cropImage` does it (sums done in 64 bits, so a huge crop can't overflow). `srcW`/`srcH` are at least 1.

**`inline void geoStep(Orient &o, float &angle, CropRect &c, int &W, int &H, GeoOp op)`**
- Inputs/outputs (all modified in place):
  - `o`: the orientation. An out-of-range `rot` is normalized first.
  - `angle`: the straighten angle.
  - `c`: the crop; it moves with the picture so the same part stays selected. Ignored if empty.
  - `W`, `H`: size of the straightened image **before** the op; updated to the size after (swapped by quarter turns).
  - `op`: which button.
- Rules: a mirror reverses the rotation direction (flip toggled, `rot` negated); `FlipV` is `FlipH` plus a half turn. A mirror also negates the straighten angle; a quarter turn leaves it alone.

**`inline float snapAngle(float deg)`**
- Returns `deg` rounded to 0.1° and clamped to `±kAngleMax`; exactly `0` near zero (never -0); NaN/infinity → 0.

### `raster_ops.hpp`
Pixel-buffer operations on a `CanvasImage` (RGBA8): crop, rotate by quarter turns, mirror, orient, straighten by any angle, the box downsample behind the preview, and the area-average resize behind Export's "Resize to long edge". (Needs LiteUI for `CanvasImage`.)

**Depends on:** `geometry.hpp`, `image16.hpp`, `liteui.hpp`. **Test:** `cases/raster_ops.inc`.

**Templates.** Every operation is a template over the picture type `Img`, so the same code serves `CanvasImage` (RGBA8, what the screen shows) and `Image16` (RGBA 16-bit, what the float pipeline starts from). The signatures below are written for `CanvasImage`; for `Image16` read `Image16` / `uint16_t`. A picture type needs `width`, `height`, a `pixels` vector of unsigned samples (R, G, B, A interleaved) and a `(w, h)` constructor. For `CanvasImage` the arithmetic is exactly what it was before these were templates; where an operation clamps or rounds (`resizeArea`, `rotateArbitrary`), the limit is the sample type's maximum (255 or 65535), taken from `raster_detail::kMaxSample<Img>`. Two non-template overloads take a literal `nullptr` (`resizeToLongEdge(nullptr, n)`, `orientImage(nullptr, o)`) and return null, because a bare `nullptr` names no picture type to deduce.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kPreviewMaxEdge` | `int` | 1024 | Long side of the preview, at most |
| `kPreviewMinRatio` | `float` | 1.5 | Images whose long side is within `1024 × 1.5` skip the preview |

**Functions**

**`inline bool isLargeImage(const CanvasImage &img)`**: true if the long side is greater than `int(1024 × 1.5) = 1536`. Decides whether a full-resolution geometry rebuild is moved off the UI thread. It is gated on the **original** (decoded file), not on the cropped working image, so one editing session always takes the same path.

**`inline std::shared_ptr<CanvasImage> makePreviewBase(const CanvasImage &src)`**
- Returns: null unless `isLargeImage(src)` (also null if the result would be empty). Otherwise a box-downsampled copy by the smallest integer factor `k = ceil(longEdge / 1024)`, with size `(width / k) × (height / k)` (integer division, so up to `k − 1` edge pixels are dropped). Each output channel, alpha included, is the rounded mean of its `k × k` block. Runs once per opened image.

**`inline Dims fitLongEdge(int w, int h, int longEdge)`**
- Returns: the size `w × h` becomes when scaled so its long side is exactly `longEdge`, aspect ratio kept (the short side is rounded to the nearest pixel and is at least 1). **Never enlarges**: a picture already `longEdge` or smaller, a `longEdge` below 1 and a non-positive size all come back unchanged. Example: 6000 × 4000 at 2048 gives 2048 × 1365.

**`inline std::shared_ptr<CanvasImage> resizeArea(const CanvasImage &src, int dw, int dh)`**
- Returns: `src` resampled to `dw × dh` by **area averaging**: each output pixel is the average of the part of the source it covers, with fractional weights at the edges, so any ratio works and nothing aliases. The right filter for shrinking; enlarging works but is blocky.
- Details: color is averaged **weighted by alpha**, so a transparent corner (left by Straighten) never darkens its neighbors, and a region with nothing visible comes out as `(0, 0, 0, 0)`. A fully opaque, flat area keeps its exact value; at exactly half size the result is the rounded 2 × 2 mean. Same size, or an invalid size (`dw`/`dh` below 1, empty `src`), returns a copy of `src`. Works one output row at a time with `double` sums (memory is one source row), so a 24 MP photo takes a fraction of a second.

**`inline std::shared_ptr<CanvasImage> resizeToLongEdge(const std::shared_ptr<CanvasImage> &src, int longEdge)`**: `resizeArea` to the size `fitLongEdge` gives. When nothing changes (already small enough, or `longEdge < 1`) it returns `src` **itself**, so no resize costs no copy; null stays null. Used by `exportImage`.

**`inline std::shared_ptr<CanvasImage> cropImage(const CanvasImage &src, const CropRect &r)`**
- Returns: a **new** image of the `r` region, clamped to `src`'s bounds (a rectangle outside the picture gives a 0-pixel dimension). Never null.

**`inline std::shared_ptr<CanvasImage> rotate90cw(const CanvasImage &src)`**: a new `h × w` image, the source rotated 90° clockwise (source `(x, y)` lands at column `h − 1 − y`, row `x`).

**`inline std::shared_ptr<CanvasImage> flipHorizontal(const CanvasImage &src)`**: a new image mirrored left-right.

**`inline std::shared_ptr<CanvasImage> orientImage(const std::shared_ptr<CanvasImage> &src, const Orient &o)`**
- Applies `o`: the mirror first (if `flip`), then `o.rot` quarter turns. The identity orientation returns **`src` itself** (no copy), and a null `src` comes back null.
- Gotcha: `o.rot` is **not** normalized here (unlike `makeGeoFrame` and `geoStep`, which take it modulo 4). It must already be 0..3; a negative value turns nothing.

**`inline std::shared_ptr<CanvasImage> rotateArbitrary(const CanvasImage &src, float deg, bool expand, const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)`**
- Inputs: the picture; `deg` the clockwise angle about the center; `expand` mode (see `rotatedDims`); optional cancellation pair.
- Returns: the rotated picture in a canvas of `rotatedDims(...)` size, or **null only if cancelled**. `deg == 0`, NaN or infinity returns an unchanged copy.
- Details:
  - Bilinear resampling in **premultiplied alpha**, so transparent pixels contribute no color.
  - *Fit*: every output pixel lands inside the picture; samples that graze the edge repeat the edge pixel.
  - *Expand*: empty corners come out fully transparent and the rotated edge is anti-aliased by a one-pixel coverage falloff.
  - Cancellation is polled every 64 rows (the most expensive stage of an async geometry rebuild).

---

### `wide_chain.hpp`
The 16-bit twin of the editor's picture chain. The editor holds original -> oriented -> rotated (straightened) -> source (cropped), built by `orientImage` / `rotateArbitrary` / `cropImage`, all RGBA8. A picture that really has more than 8 bits (a 16-bit PNG) also gets a twin of every stage as `Image16`, built from the 16-bit original by the **same operations with the same arguments**, so each twin stage has exactly the size of its 8-bit namesake. The 8-bit chain stays what every tool measures against (the histogram, scopes, white-balance picker, Auto Tone, the mask tools). The twin only feeds the render: the full-resolution render and the drag preview (`previewBase16`) start from it, and the neighbor effects read it directly (see `render_core.hpp`). A twin stage is **null when the file is 8-bit**.

These functions mirror, line for line, what `EditorState::syncSource` and `runGeometryJob` do for the 8-bit chain, so the two cannot drift apart.

**Depends on:** `geometry.hpp`, `image16.hpp`, `raster_ops.hpp` (its `Image16` forms of `orientImage`, `rotateArbitrary` and `cropImage`). Needs LiteUI only through `raster_ops.hpp`. **Test:** `cases/wide_chain.inc` (checks sizes and content against the 8-bit chain).

**`namespace wide_chain`**, with `using Wide = std::shared_ptr<Image16>`:

**`inline Wide oriented(const Wide &original, const Orient &o)`**: `orientImage(original, o)`. The identity orientation returns `original` **itself** (no copy). Null in, null out.

**`inline Wide rotated(const Wide &oriented, float angle, bool expand, const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)`**
- An angle of exactly 0 returns `oriented` itself (no copy); otherwise `rotateArbitrary`. Null in, null out.
- Gotcha: with `latest` / `gen` it can be superseded, which gives **null from a non-null input**. A caller that needs to tell "no twin" from "cancelled" checks its input first (`runGeometryJob` does).

**`inline Wide cropped(const Wide &rotated, const CropRect &crop)`**: an empty crop returns `rotated` itself; otherwise `cropImage`. Null in, null out.

---

## Masks

### `mask.hpp`
The mask data model: `MaskType`, the `MaskShape` and `MaskEval` variants, the Add/Subtract/Intersect composition, and `Mask` / `MaskSet`. Includes every shape header, and the top of the file contains the long explanation of how masks work.

**Depends on:** `adjustments.hpp`, `color_grade.hpp`, `color_mix.hpp`, `curves.hpp`, `geometry.hpp`, `mask_brush.hpp`, `mask_geom.hpp`, `mask_object.hpp`, `mask_polygon.hpp`, `mask_range.hpp`, `mask_rect.hpp`. No UI. **Test:** `cases/mask.inc`.

**How a mask is applied:** the picture is rendered as usual first. Then for each enabled mask in list order, every pixel is run through **that mask's** adjustments and blended back: `out = base + (adjusted(base) − base) × weight`. Weight 1 = full effect, 0 = untouched; later masks stack on earlier ones.

**Where geometry lives:** shapes are stored against the **original** picture (centers as a fraction of its width/height, radii as a fraction of its shorter side) and mapped by a `GeoFrame` into the current one when drawn, edited or rendered. Rotating, flipping, straightening or cropping never has to touch a mask, and the same mask renders identically on the preview and the full image.

**Adding another kind of mask:** (1) a shape struct with its parameters and `toSource(GeoFrame)` (a mask that isn't spatial just returns `*this`); a shape that is edited on the canvas also needs `fromSource` and `applySourceEdit`. (2) An `Eval` struct: the shape resolved for one image size, with `rowSpan` and `rowWeights`; a mask that depends on the picture's pixels is handed them there, and also needs `rowWeightsF` (float levels, for the float pipeline) and a branch for it in `maskRowWeightsF`. (3) One more alternative in `MaskShape` and `MaskEval` (same order as `MaskType`), a line in `maskTypeName`, and an entry in `maskTypeUsesPixels` if it reads the picture (`maskShapeToSource` and `prepareMask` are `std::visit`: no edit). (4) Its overlay and handles in the UI. For a mask that doesn't read the picture, the pipeline, undo, preview, workers, export and the per-mask sliders need no change.

**Constants:** `kMaxMasks` (`size_t`, 16): rows in the Masks list. `kMaxMaskParts` (`size_t`, 6): shapes combined inside one mask. `kMaskNameMax` (`size_t`, 32).

**Types**
- `enum class MaskType : int`: `Radial, Linear, Brush, Luminosity, ColorRange, Object, Rectangle, Polygon, Count`. **Same order as the alternatives of `MaskShape` / `MaskEval`**: `MaskPart::type()` is the variant index.
- `using MaskShape = std::variant<RadialShape, LinearShape, BrushShape, LuminosityShape, ColorRangeShape, ObjectShape, RectShape, PolygonShape>`; `using MaskEval = std::variant<RadialEval, LinearEval, BrushEval, LuminosityEval, ColorRangeEval, ObjectEval, RectEval, PolygonEval>`.
- `enum class MaskOp : int { Add, Subtract, Intersect }`. With `c` the new part's weight and `m` the running weight:
  - Add: `m + c − m·c` (union; never exceeds 1).
  - Subtract: `m·(1 − c)`.
  - Intersect: `m·c`.
  - A **fold, left to right**, so order matters, exactly like the mask's own Invert (applied last).
- `struct MaskPassPart { MaskShape shape; MaskOp op = Add; }`: a part with its shape already mapped into the picture being rendered.
- `struct MaskComposite`: the parts of one mask resolved for one image size. Fields `std::vector<MaskEval> evals`, `std::vector<MaskOp> ops` (`ops[0]` is ignored). Methods below.
- `struct MaskPart { MaskShape shape; MaskOp op = Add; }`: `op` is ignored on the first part. `type()` returns the `MaskType`. Has `operator==`.
- `struct Mask`: one mask as the user sees it.

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `parts` | `std::vector<MaskPart>` | empty | Never empty for a mask made by `addMask` |
  | `name` | `std::string` | empty | The user's name; empty = show its kind |
  | `enabled` | `bool` | true | |
  | `invert` | `bool` | false | Adjust everything **outside** the shapes |
  | `amount` | `float` | 100 | 0..100: how strongly the adjustments apply |
  | `adj` | `AdjValues` | zeros | The mask's own sliders (same table as the global ones) |
  | `mix` | `MixValues` | zeros | Its own Color Mixer |
  | `grade` | `ColorGrade` | neutral | Its own Color Grading |
  | `curves` | `CurveSet` | identity | Its own tone curves (RGB, R, G, B) |

  `type()` is the first part's kind (Radial if there are no parts). Has `operator==`.
- `using MaskSet = std::vector<Mask>`.

**Functions**

**`inline const char *maskTypeName(MaskType t)`**: "Radial", "Linear", "Brush", "Luminosity", "Color Range", "Object", "Rectangle", "Polygon"; "Mask" for anything else.

**`inline bool maskTypeUsesPixels(MaskType t)`**: true for Luminosity and ColorRange, whose weights read the picture's own pixels (so the overlay must be built from the picture just before them).

**`inline const char *maskOpName(MaskOp op)`**: "Add", "Subtract", "Intersect".

**`inline MaskShape maskShapeToSource(const MaskShape &shape, const GeoFrame &f)`**: maps any shape into the current picture (calls the alternative's `toSource`).

**`inline MaskEval prepareMask(const MaskShape &shape, int W, int H)`**: resolves a shape for one image size (calls `prepare`).

**`inline bool maskRowSpan(const MaskEval &e, int y, int W, int &x0, int &x1)`**: which pixels `[x0, x1)` of row `y` can have non-zero weight. A superset. False if none.

**`inline void maskRowWeights(const MaskEval &e, int y, int x0, int x1, float *out, const uint8_t *pix)`**: fills `out[0 .. x1 − x0)` with weights 0..1. `pix` is the RGBA row from `x0` on **as it is when the mask is reached** (after the global adjustments and earlier masks); only the pixel-reading kinds use it.

**`inline void maskRowWeightsF(const MaskEval &e, int y, int x0, int x1, float *out, const float *pix)`**: the same from a row of **float levels** (0..255, 4 per pixel), as the float pipeline holds it. Luminosity and Color Range call their `rowWeightsF`, so a 16-bit picture is not rounded to bytes before it is selected; every other kind ignores the pixels and is called as before. `pix` may be null for a mask that doesn't read the picture.

**`MaskComposite::rowSpan(int y, int W, int &x0, int &x1) const`**
- Folds the parts' spans: **Add widens** the union, **Intersect narrows** (and drops the span if nothing is left or the span is empty), **Subtract** can only remove weight so it leaves the span alone. Part 0 counts as Add. Returns false if nothing is left. The result is a superset, so it is safe to cut work with it.

**`MaskComposite::rowWeights(int y, int x0, int x1, float *out, const uint8_t *pix, float *tmp) const`** and **`rowWeightsF(..., const float *pix, float *tmp) const`**
- Weights for pixels `[x0, x1)` of row `y`. `tmp` is scratch for one row of weights (at least `x1 − x0` floats). An empty composite gives zeros. Plain loops, no `std::` helpers. `rowWeightsF` is the same with the row as float levels (0..255, 4 per pixel): Luminosity and Color Range call their `rowWeightsF` (via `maskRowWeightsF`), every other kind ignores the pixels and is called as before. Both share one private fold.

**`inline MaskComposite prepareComposite(const std::vector<MaskPassPart> &parts, int W, int H)`**: resolves every part for one image size.

**`inline std::string sanitizeMaskName(const std::string &s)`**: keeps only printable ASCII (0x20..0x7E) and caps the result at 32 characters (the TextInput widget is ASCII with a byte cursor).

**`inline bool maskNameBlank(const std::string &s)`**: true if the name is empty or only spaces.

**`inline void clearMaskCaches()`**: empties the brush cache, the polygon cache and the object warp and eval caches.

### `mask_geom.hpp`
The two gradient masks: **Radial** (a rotated ellipse) and **Linear** (graduated). Each has a stored shape, its mapping through a `GeoFrame`, and an `Eval` resolved for one image size with exact row spans. No UI, no pixels of its own.

**Depends on:** `geometry.hpp`. **Test:** `cases/mask_geom.inc`.

**Constants:** `kMaskMinRadius` (`float`, 0.02): smallest radius / half-size of the Radial, Linear and Rectangle masks, applied in `prepare`. `kMaskMaxRadius` (`float`, 1.5): the largest radius (not referenced inside this header; the UI uses it). Radii are a fraction of the picture's **shorter** side (the original's when stored, the current picture's once mapped).

**Common shape contract** (all mask shape structs follow it)
- `toSource(const GeoFrame &f) const`: stored → in the current picture.
- `static fromSource(const T &s, const GeoFrame &f)`: the inverse, for a shape drawn on the current picture.
- `applySourceEdit(const T &before, const T &after, const GeoFrame &f)`: folds an edit made on the in-view form into the stored shape, **touching only fields that changed**, so an unrelated slider never nudges a position through float round-off and an unchanged mask compares equal (no undo step).
- `prepare(int W, int H) const`: resolves the shape for one image size into an `Eval`.
- Every `Eval` has `bool rowSpan(int y, int W, int &x0, int &x1) const` and `void rowWeights(int y, int x0, int x1, float *out, const uint8_t *pix = nullptr) const` (weights for pixels `[x0, x1)` into `out[x − x0]`; `pix` is unused by these two).

**Radial**
- `struct RadialShape`

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `cx`, `cy` | `float` | 0.5 | Center, fraction of image width / height |
  | `rx`, `ry` | `float` | 0.3 | Half-axes, fraction of the shorter side |
  | `angle` | `float` | 0 | Rotation of the `rx` axis, degrees clockwise |
  | `feather` | `float` | 0.5 | 0 = hard edge .. 1 = fades from the center |

  - `float innerFrac() const`: `1 − feather` clamped to `[0, 0.99]`, so a "hard" edge is still a one-percent ramp and never divides by zero.
  - `toSource` / `fromSource` map the center through the frame, scale radii by `f.radiusScale()` (and back), and map the angle with `axisToSrc` / `axisToOrig`.
  - `prepare`: radii below `kMaskMinRadius` are raised to it; `inner = innerFrac()`.
- `struct RadialEval`: `cx, cy` (px), `c, s` (cos/sin of the rotation), `invRx, invRy` (1/radius, px), `inner`, `invFall` (`1/(1 − inner)`), `qa, qb` (`1/rx²`, `1/ry²`).
  - `rowSpan`: exact for the rotated ellipse (solves the outline quadratic for the row), padded one pixel each side, clamped to `[0, W]`. False when the ellipse doesn't reach the row.
  - `rowWeights`: weight 1 while normalized distance `d <= inner`, 0 at `d >= 1`, and `1 − smoothstep(t)` with `t = (d − inner)/(1 − inner)` between.

**Linear**
- `struct LinearShape`

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `cx`, `cy` | `float` | 0.5 | Center, fraction of width / height |
  | `len` | `float` | 0.25 | Half length of the ramp, fraction of the shorter side |
  | `angle` | `float` | 90 | Direction of fade-**out**, degrees clockwise from +x, in (-180, 180]. 90 = full at the top, fading toward the bottom |

  - The ramp runs from the line `len` behind the center (weight 1) to the line `len` ahead (weight 0), perpendicular to the direction. `toSource` / `fromSource` use `dirToSrc` / `dirToOrig` (an arrow, not an axis). `prepare` raises `len` to at least `kMaskMinRadius`.
- `struct LinearEval`: `cx, cy` (px), `ux, uy` (unit fade-out direction), `len`, `invLen`.
  - `rowSpan`: the weight is non-zero exactly where the ramp coordinate `s < 1`, which is a half-line of the row (all or none of it when the axis is vertical, `|ux| < 1e-6`), padded one pixel.
  - `rowWeights`: `s = ((dx·ux + dy·uy)/len)` in -1..+1; weight 1 for `s <= −1`, 0 for `s >= 1`, otherwise `1 − smoothstep((s + 1)/2)`.

### `mask_range.hpp`
The two masks that select by the picture's own values: **Luminosity** and **Color Range**. Neither is spatial, so neither maps through a `GeoFrame` (`toSource` returns the shape unchanged and `applySourceEdit` just copies `after`). Both are evaluated on the picture as it stands when the mask is reached (after the global adjustments and earlier masks) but before the mask's own adjustments, so darkening the shadows doesn't change which pixels are "shadows".

**Depends on:** `geometry.hpp`. No UI. **Test:** `cases/mask_range.inc`: both the byte path (`rowWeights`) and the float-level path (`rowWeightsF`: `fineTable`, `wholeLevelGraysMatchTheByteVersionExactly`, `lumaIsNotRoundedToAByte`, `sixteenBitLevelsGiveTheByteResult`, `colorRangeFloatEqualsByte`). See `mask_geom.hpp` for the common shape contract.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kLumMaxRamp` | `float` | 0.5 | Soft edge width at Feather = 100%, as a fraction of 0..1 luminance |
| `kLumSub` | `int` | 16 | Entries per level of the fine luminance table (1/16 level), read by `rowWeightsF` |
| `kLumFineSize` | `int` | 4081 | `255 * kLumSub + 1`: size of that table |
| `kColorRangeMaxDist` | `float` | 0.75 | Distance reached at Range = 100% |
| `kColorRangeLumWeight` | `float` | 0.35 | How much brightness differences count in Color Range |

**Luminosity**
- `struct LuminosityShape`: `lo` (`float`, default 0): luminance 0..1 where full weight begins; `hi` (0.35): where it ends (a new mask picks out the shadows); `feather` (0.4): 0 = hard cut .. 1 = widest soft edge.
  - `void setLo(float v)` / `void setHi(float v)`: clamp to 0..1 and push the other end so **Min can never pass Max** (the end being dragged wins).
  - `prepare`: `a = min(lo, hi)`, `b = max(lo, hi)`, ramp width `f = feather × 0.5`. For each of the 256 luminance levels, with `f <= 1e-4` a hard cut (1 inside `[a, b]`), otherwise `smooth((t − (a − f))/f) × (1 − smooth((t − b)/f))` (a ramp **outside** each end).
- `struct LuminosityEval`: `std::array<float, 256> lut` (weight per 8-bit luminance, built once per render request) and `fine` (`std::shared_ptr<const std::vector<float>>`, `kLumFineSize` entries of the same curve at 1/16 level; shared and immutable so copying an eval is cheap; null on a default-constructed eval, which makes `rowWeightsF` return zeros).
  - `rowSpan`: every row, the whole width (`W > 0`).
  - `rowWeights`: luminance per pixel is the integer `(54R + 183G + 19B + 128) >> 8` (about Rec. 709), then a table read. With `pix == nullptr` (can't happen through the pipeline) it returns zeros.
  - `rowWeightsF(y, x0, x1, out, const float *pix)`: the float pipeline's version: luma `(54R + 183G + 19B) / 256` in float levels, not rounded, read from a second table `fine` (`kLumFineSize` = 4081 entries, 1/16 level apart, same `weightAt` function as `lut`) by linear interpolation. For a pixel whose luma is a whole level (every 8-bit gray) it equals `rowWeights` exactly; a hard edge (Smoothness 0) now falls where the real luma is, so a pixel within half a level of the edge can be classified differently than the byte version.

**Color Range**
- `struct ColorRangeShape`: `set` (`bool`, false until a color is clicked), `r, g, b` (`float` 0..1, the sample as it was when clicked), `range` (0.30, 0..1 tolerance), `feather` (0.5).
  - Distance is measured in an opponent space: `y` = Rec. 709 luma (0.2126, 0.7152, 0.0722), `cb = b − y`, `cr = r − y`, all 0..1.
  - `prepare`: `outer = clamp(range) × 0.75`, `inner = outer × (1 − clamp(feather))`, `invFall = 1 / max(1e-4, outer − inner)`.
- `struct ColorRangeEval`: `set`, sample `y0, cb0, cr0`, `inner`, `outer`, `invFall`.
  - `rowSpan`: the whole width if `set`, otherwise none.
  - `rowWeights`: `d = sqrt((0.35·(y − y0))² + (cb − cb0)² + (cr − cr0)²)`; weight 1 for `d <= inner`, 0 for `d >= outer`, otherwise `1 − smoothstep`. Zeros if `!set` or `pix == nullptr`. `rowWeightsF` takes float levels through the same private template body, so for byte-valued input it is identical.

### `mask_raster.hpp`
`RasterEval`: a 0-to-255 coverage map resolved for one image size and read back with bilinear interpolation. The Brush, Object and Polygon masks all use it; they differ only in how the map is made. It keeps a per-row `[lo, hi)` range of non-zero pixels so empty areas are skipped. Standard library only; no UI.

**Depends on:** standard library only. **Test:** `cases/mask_raster.inc` (also exercised through the brush and object tests).

**Types**

`struct RasterEval`

| Field | Type | Meaning |
|---|---|---|
| `W`, `H` | `int` | The image this is resolved for |
| `mw`, `mh` | `int` | The map's own size: `W × H`, or smaller for a big image (Brush, Polygon). An Object map keeps its own size, which can exceed a small preview's |
| `map` | `std::shared_ptr<const std::vector<uint8_t>>` | 0..255 per map pixel, row-major `mw × mh`. Shared, so copying an Eval (they live in a `std::variant`) is cheap |
| `lo`, `hi` | `std::shared_ptr<const std::vector<int>>` | Per **map** row, `[lo, hi)` covers every non-zero pixel. A superset: an erase stroke does not shrink it |

**Functions** (members)

**`void mapRows(int y, int &r0, int &r1, float &t) const`**: the two map rows image row `y` interpolates between (`r0`, `r1`, clamped to the map) and the blend factor `t`. The image row's center maps to map row `(y + 0.5) × mh / H − 0.5`.

**`bool rowSpan(int y, int imgW, int &x0, int &x1) const`**
- Returns false if there is no map, `y` is outside `[0, H)`, or the rows' range is empty. Otherwise `[x0, x1)` is the union of the two map rows' ranges, scaled to image pixels and padded by `ceil(W/mw) + 1` for the interpolation's reach, clamped to `[0, imgW]`. It assumes `lo` and `hi` are set whenever `map` is.

**`void rowWeights(int y, int x0, int x1, float *out, const uint8_t *pix = nullptr) const`**
- Fills `out[x − x0]` with weights 0..1 (map value / 255, bilinear in both axes). `pix` is unused. Zeros if there is no map or `y` is out of range.
- Fast path: when `mw == W` there is no horizontal interpolation and it reads `row0[x]` directly, so `x0..x1` must lie inside `[0, W)`.

### `mask_rect.hpp`
The **Rectangle** mask: a rotated rectangle (center, half-sizes as a fraction of the picture's shorter side, angle, feather), laid out like the Radial mask in `mask_geom.hpp` but with a rectangular outline. The weight is 1 inside the `inner` fraction of the rectangle and eases (smoothstep) to 0 at its sides. `RectEval::rowSpan` is exact up to a tiny rounding tolerance: for a row, each pair of opposite sides is an interval of `dx`, and the span is their intersection (padded by a pixel). No UI, no pixels of its own.

**Depends on:** `geometry.hpp`, `mask_geom.hpp` (for `kMaskMinRadius` / `kMaskMaxRadius`). **Test:** `cases/mask_rect.inc`. See `mask_geom.hpp` for the common shape contract.

**Constants:** none of its own. Half-sizes below `kMaskMinRadius` (0.02, from `mask_geom.hpp`) are raised to it in `prepare`.

**Types**
- `struct RectShape`

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `cx`, `cy` | `float` | 0.5 | Center, fraction of image width / height |
  | `rx`, `ry` | `float` | 0.3, 0.2 | Half-sizes, fraction of the shorter side |
  | `angle` | `float` | 0 | Rotation of the `rx` axis, degrees clockwise |
  | `feather` | `float` | 0.2 | 0 = hard edge .. 1 = fades from the center (the Radial default is 0.5) |

  - `float innerFrac() const`: `1 - feather` clamped to `[0, 0.99]`, so a "hard" edge is still a one-percent ramp and never divides by zero.
  - `toSource` / `fromSource` / `applySourceEdit`: exactly the Radial mask's: the center goes through the frame, `rx` / `ry` scale by `f.radiusScale()`, the angle uses `axisToSrc` / `axisToOrig`, and `applySourceEdit` touches only the fields that changed.
  - `prepare(int W, int H)`: raises each half-size to at least `kMaskMinRadius`, converts to pixels (times the shorter side), `inner = innerFrac()`.
- `struct RectEval`: `cx, cy` (px), `c, s` (cos / sin of the clockwise rotation), `rx, ry` (half-sizes, px), `invRx, invRy`, `inner`, `invFall` (`1 / (1 - inner)`).

**Functions**

**`bool RectEval::rowSpan(int y, int W, int &x0, int &x1) const`**
- With `u = (dx·c + dy·s)/rx` and `v = (-dx·s + dy·c)/ry` the weight is non-zero exactly where `|u| < 1` and `|v| < 1`. Each is linear in `dx`, so each pair of sides gives an interval; the span is their intersection, padded one pixel each side and clamped to `[0, W]`. A row parallel to a pair of sides (`|a| < 1e-6`) is either entirely inside that pair or entirely outside it. False if nothing is left.
- The span must be a **superset** of the pixels `rowWeights` gives weight to (`MaskComposite::rowSpan` and the renderer rely on it). The edges are solved here in a different order of operations than per pixel (FMA, vectorization), so a pixel exactly on an edge could get a ~1e-13 weight while the span said "outside"; the half-size is therefore widened by `R × 1.00001 + 1e-4` before solving.

**`void RectEval::rowWeights(int y, int x0, int x1, float *out, const uint8_t *pix = nullptr) const`**: weights for pixels `[x0, x1)` into `out[x - x0]`. The normalized distance is `d = max(|u|, |v|)`: weight 1 for `d <= inner`, 0 for `d >= 1`, otherwise `1 - smoothstep((d - inner)/(1 - inner))`. `pix` is unused. Plain loop, no `std::` helpers.

### `mask_polygon.hpp`
The **Polygon** mask (click-placed polygon or freehand lasso): a closed outline of `PolyPt` vertices (fractions of the original picture), filled with the even-odd rule and optionally feathered. Like the Brush it has no closed form, so it is rasterized into a coverage map (`PolygonEval` is a `RasterEval`) and the last few maps are memoized (`polygonCache()`, emptied by `clearMaskCaches()`). `rowSpan` is the first to the last non-zero pixel of each map row, so it is as tight as the feathered shape really is. Fewer than three points select nothing. The unit-independent geometry helpers live here so they can be tested; the UI uses them on document pixels. No UI.

**Depends on:** `geometry.hpp`, `mask_raster.hpp`. **Test:** `cases/mask_polygon.inc`. **Thread model:** `polygonCache()` is a mutex-guarded global, so the UI thread and the render threads can all call `prepare`.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kPolyMaxPoints` | `size_t` | 2048 | Vertices per polygon (not checked in this header; the sidecar loader enforces it) |
| `kPolyMaxFeather` | `float` | 0.08 | Soft-edge half width at Feather = 100%, as a fraction of the shorter map side |
| `kPolyMapEdge` | `int` | 2048 | Longest side the coverage map is rasterized at; bigger images upsample it |
| `kPolyCacheSize` | `size_t` | 6 | Memoized coverage maps |
| `kPolySubRows` | `int` | 4 | Scanlines per map row (vertical anti-aliasing) |

**Types**
- `struct PolyPt { float x = 0, y = 0; }`: a vertex, as a fraction of the **original** picture's width / height when stored, of the current picture once mapped. Has `operator==`.
- `struct PolygonEval : RasterEval {}`.
- `struct PolygonShape`

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `pts` | `std::vector<PolyPt>` | empty | The outline, closed implicitly (last point back to the first). Fewer than 3 select nothing |
  | `feather` | `float` | 0.1 | 0 = hard edge .. 1 = widest soft edge |

- `struct PolygonCacheEntry { PolygonShape shape; int W, H; PolygonEval eval; }`, `struct PolygonCache { std::mutex mutex; std::vector<PolygonCacheEntry> entries; clear(); size(); }`.

**Geometry helpers** (pure, in whatever unit the points are in)

**`inline bool polyContains(const std::vector<PolyPt> &pts, float x, float y)`**: even-odd point-in-polygon, the same rule the rasterizer fills with. False for fewer than 3 points.

**`inline float polySegDist(float px, float py, const PolyPt &a, const PolyPt &b, float *cx = nullptr, float *cy = nullptr)`**: distance from the point to the segment `a`-`b`; `*cx`, `*cy` (optional) receive the closest point on it. A zero-length segment gives the distance to `a`.

**`inline float polyNearestEdge(const std::vector<PolyPt> &pts, float x, float y, int *edge, float *cx = nullptr, float *cy = nullptr)`**: the distance to the closest edge of the **closed** outline; `*edge` is its index (edge `i` joins point `i` and point `i + 1`; the last joins back to point 0), and `*cx`, `*cy` the closest point. With exactly two points there is one edge (no closing edge). Returns -1 and sets `*edge = -1` with fewer than two points.

**`inline std::vector<PolyPt> polySimplify(const std::vector<PolyPt> &pts, float tol)`**: Ramer-Douglas-Peucker on an **open** polyline (a freehand lasso): keeps the first and last point and every point that sticks out more than `tol` from the line through its kept neighbours. Iterative (no recursion). Fewer than 3 points come back unchanged.

**Shape members**

**`PolygonShape::toSource(const GeoFrame &f) const`**: every vertex goes through the frame. **`static PolyPt PolygonShape::pointFromSource(const PolyPt &p, const GeoFrame &f)`**: the inverse for one vertex drawn on the current picture.

**`void PolygonShape::applySourceEdit(const PolygonShape &before, const PolygonShape &after, const GeoFrame &f)`**: folds an edit of the in-view form into the stored shape, touching only what changed (so an unrelated slider never nudges a vertex through float round-off, a dragged vertex moves alone, and an unchanged shape compares equal: no undo step). `feather` is copied if it changed. If the number of points differs (a vertex was added or removed), **every** point is mapped back from `after`; otherwise only the points that differ between `before` and `after`.

**`PolygonEval PolygonShape::prepare(int W, int H) const`** (memoized): looks for the same shape and size in the cache (most recent first; a hit moves to the front) and otherwise rasterizes outside the lock and inserts at the front, keeping `kPolyCacheSize` entries.

**`PolygonEval PolygonShape::rasterize(int W, int H) const`**
- `W <= 0` or `H <= 0` gives an empty Eval (null `map`). The map is the image size shrunk (aspect kept) so its long side is at most 2048. With fewer than 3 points, or nothing left to draw, the Eval has `mw` / `mh` but no map, so `rowSpan` is false everywhere.
- **Feather:** `r = clamp(feather) × 0.08 × shorter map side`. The edge is blurred only when `r >= 0.5` px: three box blurs (`polydetail::boxRadii(r/2, ...)`, the standard "boxes for gauss" recipe), each run along rows and then columns, so the reach is about two sigma = `r`.
- **Canvas:** the outline is drawn and blurred on a canvas wider than the map (the part of the plane the outline's bounding box plus the blur padding can reach), so a polygon that runs off the picture does not fade out at the picture's edge.
- **Fill:** even-odd, with `kPolySubRows` (4) scanlines per map row and exact fractional x coverage on each, summed into a float coverage plane.
- **Map:** coverage is clamped to 0..1 and quantized with `v × 255 + 0.5`; each map row's `[lo, hi)` is exactly its first to last non-zero pixel.

**`polydetail::boxRadii(float sigma, int out[3])`** gives the three box radii; **`polydetail::boxBlur(const float *src, float *dst, int w, int h, int r, bool horizontal)`** is one running-sum pass (window `2r + 1`, zero outside), so its cost does not depend on `r`.

### `mask_brush.hpp`
The **Brush** mask: strokes of round dabs rasterized into a coverage map. A brush's size is its radius as a fraction of the picture's shorter side. A memo of the last few maps means a slider drag, or one more stroke, doesn't start over.

**Depends on:** `geometry.hpp`, `mask_raster.hpp`. No UI. **Test:** `cases/mask_brush.inc`.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kBrushMinSize` / `kBrushMaxSize` | `float` | 0.005 / 0.50 | Smallest / largest brush radius |
| `kBrushDefaultSize` | `float` | 0.05 | Radius of a fresh brush |
| `kBrushSpacing` | `float` | 0.20 | Render dab spacing × radius |
| `kBrushPointGap` | `float` | 0.08 | Minimum gap between recorded points × radius (used by the UI when recording; not referenced here) |
| `kBrushMaxStrokes` | `size_t` | 512 | Per mask (not checked in this header; the sidecar loader enforces it) |
| `kBrushMaxPoints` | `size_t` | 20000 | Per stroke (not checked in this header; the sidecar loader enforces it) |
| `kBrushLutSize` | `size_t` | 1024 | Falloff table resolution |
| `kBrushMapEdge` | `int` | 2048 | Longest side the coverage map is rasterized at; bigger images upsample it |
| `kBrushCacheSize` | `size_t` | 6 | Memoized coverage maps |

**Types**
- `struct BrushPt { float x = 0, y = 0; }`: a point as a fraction of the picture's width / height.
- `struct BrushStroke`

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `erase` | `bool` | false | Erases from the mask instead of adding |
  | `size` | `float` | 0.05 | Radius, fraction of the shorter side |
  | `feather` | `float` | 0.5 | 0 = hard edge .. 1 = fades from the center |
  | `density` | `float` | 1 | 0..1, the most a stroke can add or remove |
  | `flow` | `float` | 1 | 0..1: 1 = a dab sets coverage (max); lower = dabs build up toward `density` (one pass ≈ flow) |
  | `pts` | `std::vector<BrushPt>` | empty | The polyline |
- `struct BrushEval : RasterEval {}`.
- `struct BrushShape { std::vector<BrushStroke> strokes; }`.
- `struct BrushCacheEntry { BrushShape shape; int W, H; BrushEval eval; }`, `struct BrushCache { std::mutex mutex; std::vector<BrushCacheEntry> entries; clear(); size(); }`.

**Functions**

**`BrushShape::toSource(const GeoFrame &f) const`**: every point goes through the frame, every `size` is multiplied by `f.radiusScale()`. (There is no `applySourceEdit`: strokes are only ever appended, never edited in place.)

**`static BrushPt BrushShape::pointFromSource(const BrushPt &p, const GeoFrame &f)`** and **`static float BrushShape::sizeFromSource(float size, const GeoFrame &f)`**: the inverse, for the pieces of a new stroke drawn on the current picture.

**`BrushEval BrushShape::prepare(int W, int H) const`** (memoized)
- Returns the coverage map for this shape at this size. Looks in the cache (most recent first): an identical shape and size returns the same map. Otherwise, the **first** cached entry (most recent first) of the same size whose strokes are a prefix of this brush's becomes the `base`, and only the new strokes are stamped on top of a copy of it. The result is inserted at the front; the cache keeps `kBrushCacheSize` entries. The rasterizing runs outside the lock. A hit, or an entry that becomes the `base`, is moved to the front, so the list stays most-recently-used first.

**`BrushEval BrushShape::rasterize(int W, int H, const BrushEval *base = nullptr, size_t first = 0) const`**
- Inputs: image size; optionally `base` (the finished map of this brush's first `first` strokes, same map size) to build on.
- Returns: the map (`BrushEval`). `W <= 0` or `H <= 0` gives an empty Eval (null `map`).
- Details:
  - Map size: the image size shrunk (aspect kept) so its long side is at most 2048.
  - Per stroke, radius = `max(size, 0.005) × shorter map side`; dab step = `max(1, radius × 0.20)`; the falloff table is 1 inside `inner = clamp(1 − feather, 0, 0.99)`, easing (smoothstep) to 0 at the rim, scaled by `density`.
  - **Flow ≥ 0.999**: one stroke's coverage is the **max** over its dabs (a stroke crossing itself doesn't double up).
  - **Flow < 0.999** (clamped to at least 0.01): dabs **accumulate** toward `255 × density`, each adding `(cap − cov) × falloff × aF`, with `aF` chosen so one straight pass adds up to about `flow`. Dabs are placed at exact spacing along the path (carried across segments, so build-up doesn't depend on how often points were recorded); a single-point stroke gets one pass worth of dabs.
  - Strokes fold into the map in order: paint `m = m + (1 − m)·c`, erase `m = m·(1 − c)` (in bytes, rounded). Row spans widen only for painted strokes, so an erase never shrinks them.
  - Safety caps: at most 65536 dabs per segment.

### `mask_object.hpp`
The **Object** mask: a coverage bitmap from a segmentation, stored against the original picture and warped into the current one, then grown/shrunk and feathered. Maps are immutable and shared; the warp and the grown/feathered result are memoized (the last 6 of each).

**Depends on:** `geometry.hpp`, `mask_raster.hpp`. No UI. **Test:** `cases/mask_object.inc`.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kObjectMapEdge` | `int` | 1024 | Long side of a stored / warped map |
| `kObjectMaxGrow` | `float` | 0.03 | Grow = ±1 means 3% of the long side |
| `kObjectMaxFeather` | `float` | 0.02 | Feather = 1 means 2% of the long side |
| `kObjectCacheSize` | `size_t` | 6 | Entries in each cache |

**Types**
- `struct ObjectMap { int w = 0, h = 0; std::vector<uint8_t> a; uint64_t id = 0; }`: coverage 0..255, row-major. `id` is unique per result (and per warp). **Equality of shapes compares this id**, so undo snapshots and `setMasks`' `m == masks_` stay cheap.
- `struct ObjectPrompt { float x = 0, y = 0; bool fg = true; }`: a click as a fraction of the picture; `fg` false = background.
- `struct ObjectEval : RasterEval {}`.
- `struct ObjectShape`

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `map` | `std::shared_ptr<const ObjectMap>` | null | Stored: original orientation. In-view form: warped |
  | `prompts` | `std::vector<ObjectPrompt>` | empty | The clicks, for refinement |
  | `feather` | `float` | 0 | 0..1 |
  | `grow` | `float` | 0 | -1 (shrink) .. +1 (grow) |

  `operator==` compares map ids (a null map counts as id 0), prompts, feather and grow.
- `ObjectWarpCache` / `ObjectEvalCache` (each `{ mutex, entries, clear(), size() }`) and `ObjectCacheEntry { uint64_t id; float grow, feather; ObjectEval eval; }`.

**Functions**

**`inline uint64_t nextObjectMapId()`**: a process-wide unique id (atomic counter, starts at 1).

**`inline uint64_t hashMix(uint64_t h, double v)`**: mixes a double's bits into a hash (boost-style combine).

**`inline void objAxisMorph(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, int w, int h, int r, bool dilate, bool horizontal)`**: box-shaped max (`dilate`) or min filter of radius `r` along one axis; `out` is resized. Edges are clamped. Cost is O(w·h·r).

**`inline void objAxisBlur(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, int w, int h, int r, bool horizontal)`**: running-sum box blur of radius `r` along one axis (edges replicated, rounded mean).

**`inline ObjectWarpCache &objectWarpCache()`**, **`inline ObjectEvalCache &objectEvalCache()`**: the process-wide caches (emptied by `clearMaskCaches()`).

**`inline std::shared_ptr<const ObjectMap> warpObjectMap(const std::shared_ptr<const ObjectMap> &src, const GeoFrame &f)`**
- Warps a stored map (original orientation) into the picture on screen. `src` must be non-null.
- The result's `id` mixes the source id with every number of the frame, so anything that caches on the in-view shape is invalidated by a rotate, flip, straighten or crop, not just by a new segmentation.
- Output size: `srcW × srcH` scaled by `min(1, 1024 / longEdge)`, at least 1 × 1. Each output pixel is mapped back through `f.toOrig`; pixels that fall outside the original (straighten's empty corners) stay 0; the rest are bilinearly sampled (edges clamped). The last 6 warps are cached.

**`ObjectShape::toSource(const GeoFrame &f) const`**: prompts are mapped through the frame; a non-empty map is warped with `warpObjectMap`.

**`ObjectShape::applySourceEdit(const ObjectShape &before, const ObjectShape &after, const GeoFrame &)`**: only the sliders edit an object, so only `feather` and `grow` are copied from `after`. The bitmap and prompts are replaced wholesale by `setSelectedObjectMap`.

**`ObjectEval ObjectShape::prepare(int W, int H) const`** (memoized)
- Returns the resolved Eval (with `W`, `H` set). A null or empty map gives an Eval with no map (nothing selected).
- Cache key: map id + `grow` + `feather`. It does not depend on `W`/`H` (the map keeps its own size), so the preview and the full-size render share one result.
- Steps: grow/shrink by `rg = round(|grow| × 0.03 × longSide)` with a box dilate (grow > 0) or erode (grow < 0), then feather with `rf = round(feather × 0.02 × longSide)` as two box-blur passes in each axis (close to a Gaussian), then per-row `[lo, hi)` spans of non-zero pixels.

### `seg_basic.hpp`
The stand-in segmenter: a magic wand that grows over similar colors. `segmentObject` in `main.cpp` falls back to it when the SAM model cannot be loaded (a build without ONNX, or the model files are missing), and only when there are prompts. It has the same contract as the real model (picture and prompts in, coverage map out, cancellable, runs on the worker thread), so everything around it works the same. (Needs LiteUI for `CanvasImage`.)

**Depends on:** `liteui.hpp`, `mask_object.hpp`. **Test:** `cases/seg_basic.inc`.

**Constants:** `kSegPrepEdge` (`int`, 512): long side the stand-in works at. `kSegTolerance` (`int`, 40): RGB distance that counts as "same color".

**Types:** `struct SegPrepared { int w = 0, h = 0; std::vector<uint8_t> rgb; }`: what a model would cache per image (an embedding): a box-averaged RGB copy (`w × h × 3`).

**Functions**

**`inline std::shared_ptr<const SegPrepared> prepareSegImage(const CanvasImage &s)`**: box-averages the picture down by `k = max(1, ceil(longEdge / 512))`; the result is `max(1, width/k) × max(1, height/k)`. Alpha is ignored.

**`inline bool segGrow(const SegPrepared &im, int sx, int sy, std::vector<uint8_t> &mark, const std::atomic<uint64_t> *latest, uint64_t gen)`**
- Inputs: the prepared image, the seed `(sx, sy)` in its pixels, a `mark` buffer (`w × h`, 1 = included; modified in place), and the cancellation pair.
- Returns: false if cancelled.
- Details: the seed color is the mean of a 5 × 5 patch. A 4-neighbor flood fill adds pixels whose squared RGB distance from that color is at most `40²`. Cancellation is polled every 4096 steps. Pixels already marked are not revisited. Gotcha: the seed pixel itself is only included if it is within tolerance of that patch mean (at an edge it may not be).

**`inline std::shared_ptr<const ObjectMap> segmentObjectWand(const CanvasImage &img, uint64_t key, const std::vector<ObjectPrompt> &prompts, std::string *err, const std::atomic<uint64_t> *latest, uint64_t gen)`**
- Inputs: the picture, a `key` identifying it (the prepared copy is reused while the key is unchanged), the prompts (fractions, clamped into the picture), an error string (never set by this stand-in), and the cancellation pair.
- Returns: a new `ObjectMap` (a fresh `id`; size = the prepared image's) with coverage 255 where a foreground region covers the pixel and no background region does, else 0. **Null if cancelled.**
- Gotcha: **worker thread only**. The cache of the prepared image is a plain `static`, not locked.

### `sam_segmenter.hpp`
Segment-Anything-style segmentation through ONNX Runtime (MobileSAM). Everything is in `namespace sam`. `Point`, `kInputSize`, `Prepared` and `preprocess` are always compiled (no ONNX needed); the real `Segmenter` is only compiled in with `LITEROOM_ONNX`, and without it the same `Segmenter` class exists (same method names) but is never available: `loaded()` and `encoded()` return false, and `load`, `encode` and `decode` return false with the error "built without LITEROOM_ONNX".

**Depends on:** ONNX Runtime (`onnxruntime_cxx_api.h`) when enabled; `toWide` from LiteUI on Windows; `preprocess` needs the standard library only. **Test:** `cases/sam_segmenter.inc` (the stand-in `Segmenter` of a build without ONNX, and `preprocess`; the real model path is **not** covered). **Not thread-safe**: use it from one thread, the segmentation worker.

**Types**
- `struct Point { float x = 0, y = 0; bool fg = true; }`: a prompt as a fraction of the picture's width / height; `fg` false = background.
- `inline constexpr int kInputSize` = 1024: SAM's longest-side input size. `Segmenter::kInput` is the same value (ONNX build only; the stand-in class has no such constant).
- `struct Prepared { std::vector<float> data; int nw = 0, nh = 0; }`: what `encode` feeds the encoder. `nw` x `nh` is the picture as resized (long side `kInputSize`); `ok()` is `nw > 0 && nh > 0`.
- `class Segmenter`.

**`inline Prepared preprocess(const uint8_t *rgba, int w, int h, int rank)`**: the resize / normalize / pad step of `encode`, a plain function so it can be tested without ONNX Runtime or a model.
- Returns an empty result (`nw == nh == 0`, no data) for a null picture or `w <= 0` / `h <= 0`.
- Size: scale `1024 / max(w, h)`, each side `lround`ed and at least 1 (2000 x 1000 gives 1024 x 512; 4000 x 1 gives 1024 x 1). Resampling is a **box average when shrinking** (a fractional factor such as 1.5 is handled; each box reaches its last partly covered pixel) and **nearest when enlarging**. Alpha is ignored.
- `rank == 4`: normalized `(v - mean) / std` per channel (the constants under *Encoder layouts*), planar R, G, B, in a **zero-padded `1024 x 1024` square** (`data.size() == 3 * 1024 * 1024`). The padding is zero *after* normalization, not the normalized value of black.
- any other rank (the encoder only allows 3): raw 0..255 R, G, B, HWC, `nh x nw x 3` floats, no padding.

**Encoder layouts** (detected from the model's input rank; `preprocess` builds either): **rank 4**: NCHW `1 × 3 × 1024 × 1024`, normalized here (mean 123.675 / 116.28 / 103.53, std 58.395 / 57.12 / 57.375) and zero-padded at the bottom/right. **Rank 3**: HWC raw 0..255 RGB at the resized size (`nh × nw × 3`); the model normalizes and pads itself (samexporter-style export).

**Members**

**`bool loaded() const`**: both models are loaded.

**`bool load(const std::string &encoderPath, const std::string &decoderPath, std::string *err)`**
- Loads both ONNX models (intra-op threads = hardware concurrency, at least 1; full graph optimization). Returns false with `*err` set if a session can't be created, the encoder has no inputs/outputs, the decoder lacks any of the inputs `image_embeddings, point_coords, point_labels, mask_input, has_mask_input, orig_im_size` or the output `masks`, or the encoder input rank is neither 3 nor 4. `iou_predictions` is optional. Logs the model names to `stderr`.

**`bool encode(const uint8_t *rgba, int w, int h, std::string *err)`**
- Encodes an RGBA8 picture (alpha ignored). **Slow** (about 0.5 to 2 s on a CPU), so do it once per opened image. The picture is resized so its long side is 1024 (box average) and the embedding is kept. Returns false if not loaded, the pixels are null or a size is non-positive (`"segmenter not ready"`), or the run fails. Any earlier embedding is cleared first.

**`bool encoded() const`**: an embedding is held.

**`bool decode(const std::vector<Point> &pts, std::vector<float> *logits, int *outW, int *outH, std::string *err)`**
- Cheap (tens of ms). Needs an embedding and at least one prompt. Prompts are converted to the resized picture's coordinates and a padding point (label -1) is appended, as SAM expects; no previous mask is passed.
- Output: `*logits` (row-major, **> 0 means inside**) of size `*outW × *outH`, at the encoded picture's aspect, long side 1024.
- Mask choice: if the model returns several masks and there is **exactly one** prompt and IoU scores are available, the best of masks 1..K−1 is used; otherwise mask 0.

---

## Rendering

### `render.hpp`
The synchronous convenience entry for rendering an `AdjustmentSet`. The pixel pass itself lives in [`render_core.hpp`](#render_corehpp) (the float pipeline the app uses) and [`render_bytes.hpp`](#render_byteshpp) (the old byte pipeline, kept as a test reference); `renderWith` and `applyMaskToBlock` are documented there. (Needs LiteUI for `CanvasImage`.)

**Depends on:** `adjustment_set.hpp`, `render_core.hpp`. **Tests:** `cases/render.inc`; `cases/lut_render.inc` renders everything through `renderAdjusted`.

**Functions**

**`inline std::shared_ptr<CanvasImage> renderAdjusted(const AdjustmentSet &set, const CanvasImage &original)`**
- The simple synchronous entry point: `renderWith(original, set.buildRenderParams())`. Non-destructive; always starts from `original`'s untouched pixels. Big images go through the render worker instead.

### `render_core.hpp`
The pixel pass **in float**: tone tables → color pass → neighbor effects → masks → Vignette and Grain, in blocks of rows that a background render can abandon. This is the pipeline the app, the workers and the golden test call (through `renderWith`). (Needs LiteUI for `CanvasImage`.)

A block of rows is loaded from the source (RGBA8 or RGBA 16-bit) into float **levels** (0..255, the unit every constant of the passes is written in), goes through every stage as floats, and is **rounded once**, when it is stored into the result. The byte pipeline (`render_bytes.hpp`) rounded after the tone sliders, the color pass, every neighbor pass, every mask and the film effects. Where that code clamped to 0..255 so does this (after the color pass, inside every neighbor pass, after each mask's color pass), so highlights cannot run past white between stages.

For a picture that started as 8-bit the result is the old result up to the rounding that is no longer done between stages: **exactly equal** when only one stage did anything (a single tone slider, the color pass alone, one neighbor effect, the film effects), and within a few levels when stages are stacked. A 16-bit picture keeps its extra precision through every stage. Each pixel is independent, so blocking does not change the result: preview, background render and batch export agree.

**Nothing is measured on a rounded copy any more.** The neighbor effects read the source at its own precision (RGBA8 or 16-bit), and a Luminosity / Color Range mask picks its weights from the row as float levels. A picture that is a *widened* 8-bit one therefore gives the 8-bit result exactly when no neighbor effect is on, and within a few levels (measured: about 1 for one effect at 100, about 2 for all of them stacked) when one is, because a 16-bit source's luma is no longer rounded to a level.

**Depends on:** `liteui.hpp`, `color_pass.hpp`, `image16.hpp`, `pixel_math.hpp`, `tone_table.hpp`, `detail_layer.hpp`, `film_fx.hpp`, `neighbor_fx.hpp`, `render_params.hpp`, `mask.hpp`. **Tests:** `cases/render_core.inc` (the float pipeline against `renderWithBytes`: single stages exactly equal, stacked stages within a few levels, blocks, masks reading the row before they edit it, cancellation; and the 16-bit paths: a widened 8-bit picture gives the 8-bit result, 16-bit precision, a smooth gradient through Texture / Clarity / Sharpness), `cases/render.inc`, `cases/export_render.inc`, `cases/lut_render.inc` and the golden test (the golden test renders 8-bit pictures only, so it does not pin the 16-bit entry points). **16-bit neighbor effects are tested only here**, through random sliders on a widened picture (within 4 levels of the 8-bit result when an effect is on) and the Texture / Clarity / Sharpness gradient test; there is no unit test of them in `cases/neighbor_fx.inc` or `cases/detail_layer.inc`. Everything but the three public entry points is in `namespace render_core`.

**Constants:** `render_core::kBlockRows` (`int`) = 128, the same as the byte pipeline, so cancellation latency is unchanged.

**Functions in `render_core`**

**`inline void clampRgb(float *p, size_t n)`**: clamps R, G and B of `n` floats (4 per pixel) to 0..255; NaN gives 0. Alpha is left alone. Plain comparisons, no `std::` helpers, for Debug-build speed like the other per-pixel loops.

**`inline void applyTone(const ToneSet &ts, float *p, size_t n)`**: the tone stage on `n` floats: each R, G, B through its channel's table (`ts.at(c)`, `toneLookupWide`). Not rounded.

**`inline void applyByteLuts(const std::array<std::array<uint8_t, 256>, 3> &luts, float *p, size_t n)`**: the fallback for a `RenderParams` that has no float tables (built by hand): each value is first rounded to a byte with `toU8Clamped`, then looked up, exactly as the byte pipeline does.

**`inline bool partsReadPixels(const std::vector<MaskPassPart> &parts)`**: true if any part is a mask type whose weights read the picture's own pixels (`maskTypeUsesPixels`: Luminosity, Color Range).

**`inline void applyMaskToBlock(const MaskPass &mp, const MaskComposite &ev, bool readsPixels, float *q, int rows, int y0, int W, float *scratch, float *wbuf, float *wbuf2, const S *ref, const neighbor_fx::Planes &dp)`** (`template <class S>`, `S` = `uint8_t` or `uint16_t`)
- Inputs:
  - `mp`, `ev`: the frozen mask and its composite resolved for this image size. `readsPixels`: `partsReadPixels(mp.parts)`.
  - `q`, `rows`, `y0`: `rows` whole rows of floats (4 per pixel) to edit; the first is image row `y0`. `W`: picture width.
  - `scratch`: one row of RGBA floats. `wbuf`, `wbuf2`: one row of weights each. A pixel-reading mask is shown the row itself, as float levels (no byte rounding).
  - `ref`: the **source samples** of the same rows, of the type the planes in `dp` were built from (RGBA8 or 16-bit); only read when `mp.fx.any()` (may be null otherwise). `dp`: the whole-image planes.
- Per row:
  1. Find the span the shape touches (`rowSpan`). **Inverted masks use the whole row** (weight is `1 − w`), so no span cut applies.
  2. Compute the weights (`rowWeightsF`) from the row **before** this mask edits it, so a mask's own adjustment never changes what it selects; the row is passed only when `readsPixels` (no other shape reads it).
  3. Copy the span to `scratch`; run the mask's tone (`mp.toneF`, else the byte tables `mp.luts` via `applyByteLuts`; only if `mp.tone`), its color pass (`applyColorPassF`, then `clampRgb` if `mp.color.active`) and its neighbor effects (planes from `dp`).
  4. Blend back **in float**: `w = (invert ? 1 − w : w) × amount`; each channel becomes `a + (b − a)·w`. Pixels with `w <= 0` are skipped. No rounding or clamp here.
- Replaces the byte version `applyMaskToBlockBytes`.

**`template <class SrcImg, class OutImg> inline std::shared_ptr<OutImg> renderPipeline(const SrcImg &src, const CanvasImage *view8, const RenderParams &rp, const std::atomic<uint64_t> *latest, uint64_t gen)`**
- Inputs: `SrcImg` is `CanvasImage` or `Image16`; `OutImg` is what is produced (`CanvasImage` for the screen, `Image16` for a 16-bit export). `src` is **never modified**. `latest` / `gen`: the usual cancellation pair (`latest` may be null).
- Returns: a new `W × H` picture, or **null if cancelled** (between blocks, or while building the neighbor planes). An empty source, or one whose `pixels` is shorter than `W * H * 4`, returns an empty / zero-filled picture of the same size, not null.
- **`view8`:** **not used any more.** The neighbor effects used to measure an 8-bit view of a 16-bit source; they now read the source itself, at its own precision (RGBA8 or 16-bit, `SrcTraits`). Their planes are cached per buffer address, so a source that lives as long as a slider drag keeps them cached (a null `view8` is no longer slower). The parameter stays so callers that pass a view keep compiling.
- Order per block of 128 rows: load samples to levels (`samplesToLevels`) → tone (float tables `rp.toneF`, else `rp.luts` through `applyByteLuts`; skipped when `rp.toneF->identity`; with no float tables the byte tables are **always** applied, even if they are the identity) → color pass (`applyColorPassF` then `clampRgb`, only if `rp.color.active`) → neighbor effects (`neighbor_fx::apply`, if `rp.fx.any()`) → each mask (`applyMaskToBlock`) → Vignette and Grain (`film_fx::applyF`, if any; global only, on the finished pixel) → round once into the output (`levelsToSamples`).
- How it works:
  - Masks are resolved against **this** image's size (`prepareComposite`), so preview and full-size renders agree. Vignette and Grain likewise (`film_fx::prepare`), and are handed the index of a block's first pixel (`off / 4`), so a block gives the same pixels as the whole picture in one go.
  - Neighbor planes for the global effects **and every mask** are acquired once up front, from the source itself, only if some effect needs planes (`fxNeed != 0`). A negative Dehaze needs no planes but still gets the source pointer, which is why the pointer is set whenever any effect is in use.
  - One float buffer of up to 128 rows is reused for every block.

**Entry points** (global, not in `render_core`)

**`inline std::shared_ptr<CanvasImage> renderWith(const CanvasImage &src, const RenderParams &rp, const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)`**: an 8-bit picture in, an 8-bit picture out. The signature the app, the workers and the golden test have always called. Null if cancelled.

**`inline std::shared_ptr<CanvasImage> renderToDisplay(const Image16 &src, const CanvasImage *view8, const RenderParams &rp, const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)`**: a 16-bit picture to RGBA8 for the screen. `view8` is not used (may be null).

**`inline std::shared_ptr<Image16> renderToImage16(const Image16 &src, const CanvasImage *view8, const RenderParams &rp, const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)`**: a 16-bit picture to 16 bits (a 16-bit export): the same pipeline, rounded to 16 bits once.

**`inline std::shared_ptr<Image16> renderToImage16(const CanvasImage &src, const RenderParams &rp, const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)`**: an RGBA8 picture to 16 bits. A heavy edit of an 8-bit photo then comes out as smooth gradients instead of re-quantized steps (a 16-bit PNG export).

### `render_bytes.hpp`
The **original byte pipeline** (RGBA8 in, a byte table per slider stage, every pass rounding to a byte), kept verbatim as `renderWithBytes`. **The app does not use it any more**: `renderWith` in `render_core.hpp` is the float pipeline. It stays as the reference the tests compare the float pipeline against, and can be deleted once the new golden file has been recorded. (Needs LiteUI for `CanvasImage`.)

**Depends on:** `liteui.hpp`, `color_pass.hpp`, `pixel_math.hpp`, `detail_layer.hpp`, `film_fx.hpp`, `neighbor_fx.hpp`, `render_params.hpp`, `mask.hpp`. **Test:** `cases/render_bytes.inc`; `cases/render_core.inc` also compares the float pipeline with it (it is the reference, so deleting this header removes that check). It reads only the byte tables `rp.luts` and `mp.luts`; the float tables (`toneF`) are ignored.

**`inline std::shared_ptr<CanvasImage> renderWithBytes(const CanvasImage &src, const RenderParams &rp, const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)`**
- Inputs: the source picture (**never modified**), the frozen parameters, and the optional cancellation pair. Returns: a new rendered picture, or **null if cancelled**.
- Order per pixel, **rounding to a byte after every stage**: tone LUTs → color pass (`applyColorPass`) → neighbor effects → masks, each doing the same three steps on its own copy → Vignette and Grain (`film_fx::apply`, global only).
- How it works: blocks of 128 rows, `latest` re-read before each; masks and film resolved against this image's size; neighbor planes acquired once up front from `src`'s pixels; when all three channel tables are identical (no per-channel curves) a single-table fast path is used. Raw pointers and no `std::` helpers in the inner loops (Debug-build speed, like `applyColorPass`).

**`inline void applyMaskToBlockBytes(const MaskPass &mp, const MaskComposite &ev, uint8_t *q, size_t n, int y0, int W, uint8_t *scratch, float *wbuf, float *wbuf2, const uint8_t *ref, const neighbor_fx::Planes &dp)`**
- The byte version of `render_core::applyMaskToBlock`: `q`, `n` are whole rows of bytes (the first is image row `y0`), `scratch` one row of RGBA bytes, `ref` the source bytes of the same rows.
- Per row: span (`rowSpan`; inverted masks use the whole row) → weights from the row **before** the edit → copy to `scratch` → tone tables (if `tone`), color pass, neighbor effects → blend back, each channel `toU8Clamped(a + (b − a)·w)` with `w = (invert ? 1 − w : w) × amount`; pixels with `w <= 0` are skipped.

### `histogram.hpp`
Counting the pixels: R/G/B/luma histograms, which channels are clipped, and the clipping overlay. Pure functions of an image and a few flags; the editor's state is not touched. Everything is in `namespace histogram` except the constants. (Needs LiteUI for `CanvasImage` and `Color`.)

**Depends on:** `liteui.hpp`. **Test:** `cases/histogram.inc`. `src/main.cpp`'s `computeHistogram()` picks the image on screen, calls these and stores the results.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kHistMaxSamples` | `int` | 300000 | About this many pixels are visited per recompute |
| `kClipOverlayMaxEdge` | `int` | 2048 | Overlay resolution cap (long edge) |
| `kColorClipShadow` | `Color` | (60, 110, 255, 255) | Classic-mode shadow clipping color (blue) |
| `kColorClipHighlight` | `Color` | (235, 50, 50, 255) | Classic-mode highlight clipping color (red) |
| `kChanR`, `kChanG`, `kChanB` | `uint8_t` | 1, 2, 4 | Channel bits used in clipping masks |

**Types**
- `struct Counts`: `r`, `g`, `b`, `l` (each `std::array<uint32_t, 256>`, zero by default), and `clipLo`, `clipHi` (`uint8_t`): the `kChan*` bits of channels that have any pixel at 0 / at 255. They are read from the end bins, so on a picture bigger than `kHistMaxSamples` they reflect only the sampled pixels (the overlay looks at every pixel).
- `struct ClipOptions { bool shadows = false, highlights = false; bool perChannel = false; }`: which overlays are on, and whether to color by exactly which channels clip.

**Functions**

**`inline Counts compute(const CanvasImage *img)`**
- Input: the image (may be null or empty, which gives all zeros).
- Returns: the four histograms and the clip flags.
- Details: visits a regular grid with `step = max(1, int(sqrt(w·h / 300000)))`. Pixels with alpha 0 are skipped. Luma is integer Rec. 601, `(77R + 150G + 29B) >> 8`.

**`inline uint8_t channelsAt(const uint8_t *px, uint8_t level)`**: `px` points at an RGBA pixel. Returns the `kChan*` bits of the R/G/B values equal to `level`.

**`inline Color clipChannelColor(uint8_t mask, bool pastel)`**
- Inputs: a channel set (1 = R, 2 = G, 4 = B; only the low 3 bits are used) and whether to lighten the color.
- Returns: the additive mix color from one palette table (R red, G green, R+G yellow, B blue, R+B magenta, G+B cyan, all three light gray; `mask == 0` is fully transparent). `pastel` moves each channel halfway to 255. This table is the one place to tweak the palette.

**`inline std::shared_ptr<CanvasImage> clipOverlay(const CanvasImage *img, const ClipOptions &opt)`**
- Input: the image and the options.
- Returns: an RGBA overlay, or **null** if both overlays are off or the image is null/empty.
- Details:
  - Size: `step = ceil(max(w, h) / 2048)`, overlay is `ceil(w/step) × ceil(h/step)`. A block is marked if **any** pixel in it is clipped, so small highlights don't vanish.
  - Classic mode: red where any channel is 255, blue where any is 0.
  - Per-channel mode: colored by exactly which channels clip. Pixels clipped in all three are left untinted.
  - Where a block holds both kinds, highlights win. Fully transparent pixels never count. Marked blocks get alpha 255; others stay transparent.

### `scopes.hpp`
The two views that can replace the histogram in the pinned canvas above the panes: a **waveform** (every picture column drawn as a column of levels) and a **vectorscope** (every pixel's chroma drawn as a point on a Cb / Cr plane). Pure functions of an image: the editor's `state` is not touched. Each scope is built in two steps, counting and then drawing, so the counts can be tested without looking at pixels. Everything is in `namespace scopes`. See [architecture.md](architecture.md#waveform-and-vectorscope).

**Depends on:** `histogram.hpp` (for `kHistMaxSamples`), `liteui.hpp` (`CanvasImage`). **Test:** `cases/scopes.inc`. **In the app** (`main.cpp`, not this header): `computeHistogram()` calls `scopes::render` for the current view and stores the picture in `state.scopeImage`; a click on the pinned canvas calls `cycleScope()`; `paintScope` stretches the picture over the canvas and adds the graticule.

**Constants** (namespace `scopes`)

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kWaveMaxCols` | `int` | 256 | Most scope columns of the waveform (fewer for a narrow picture) |
| `kWaveLevels` | `int` | 256 | Height of the waveform, one row per level |
| `kVecSize` | `int` | 128 | The vectorscope is `kVecSize x kVecSize` cells; Cb and Cr (0..255) are halved |
| `kWaveSat` | `float` | 0.04 | A level holding this share of its column is drawn at full brightness |
| `kVecSat` | `float` | 0.01 | A cell holding this share of all pixels is drawn at full brightness |
| `kVecFloor` | `float` | 0.25 | A vectorscope cell with any pixel at all is at least this bright |
| `kVecCellLuma` | `float` | 180 | The luma at which a vectorscope cell's own color is shown |
| `kBgR`, `kBgG`, `kBgB` | `uint8_t` | 34, 34, 38 | The plot background (the histogram canvas's dark gray) |

**Types**
- `enum class Kind : int`: `Histogram, Waveform, Vectorscope, Count`. What the pinned canvas shows.
- `struct Waveform`: `cols` (0 = nothing counted), `total` (samples counted), `r`, `g`, `b` (`std::vector<uint32_t>`, index `col * kWaveLevels + level`) and `colTotal` (samples counted per column).
- `struct Vector`: `total` and `n` (`kVecSize * kVecSize` cells, index `y * kVecSize + x` with `x = Cb / 2` and `y = Cr / 2`; empty when `total == 0`).

**Functions**

**`inline const char *kindName(Kind k)`**: "Histogram", "Waveform" or "Vectorscope" (anything else gives "Histogram"). **`inline Kind nextKind(Kind k)`**: the next view in the order above, wrapping from `Vectorscope` to `Histogram`; a value out of range gives `Histogram`.

**`inline int sampleStep(int w, int h)`**: the stride of the sample grid, `max(1, int(sqrt(w * h / kHistMaxSamples)))`: the same formula as `histogram::compute`, so a scope never visits more pixels than the histogram.

**`inline float density(uint32_t count, uint32_t total, float saturation)`**: brightness 0..1 of a point holding `count` of `total` samples: `min(1, sqrt(share / saturation))` with `share = count / total`; 0 when either is 0. A share is used, not a count, so a small preview and the full picture look alike.

**`inline uint8_t blendTo(uint8_t bg, float target, float v)`**: `bg` moved toward `target` (clamped to 0..255) by `v`, rounded.

**`inline Waveform waveformCounts(const CanvasImage *img)`**
- Counts the R, G and B levels of every picture column on the same sample grid as the histogram. Fully transparent pixels are skipped. A null or empty picture, or one with only transparent pixels, gives `cols == 0`.
- The grid has `nx = ceil(width / step)` sample columns; they are folded into `cols = min(kWaveMaxCols, nx)` scope columns by sample index (`col = ix * cols / nx`), so every scope column gets at least one sample and a narrow picture never leaves gaps.

**`inline std::shared_ptr<CanvasImage> waveformImage(const Waveform &wf)`**: the waveform as a picture, `cols` wide and `kWaveLevels` tall, level 255 at the top. Each channel lights only its own color component on the background, so where R, G and B coincide the point is white. Brightness is `density(count, colTotal, kWaveSat)`. Null if `cols == 0`.

**`inline void rgbToCbCr(int r, int g, int b, int *cb, int *cr)`**: Rec. 601 full-range chroma in integer math, `Cb = 128 + ((-43 R - 85 G + 128 B) >> 8)` and `Cr = 128 + ((128 R - 107 G - 21 B) >> 8)`, clamped to 0..255. Any gray is exactly (128, 128); pure blue is (255, 107), pure red (85, 255), pure green (43, 21). The right shifts are of negative numbers, which C++20 defines as arithmetic shifts.

**`inline Vector vectorCounts(const CanvasImage *img)`**: counts every sampled, non-transparent pixel into the cell of its Cb / Cr. An empty picture or one with only transparent pixels gives `total == 0`.

**`inline std::shared_ptr<CanvasImage> vectorImage(const Vector &v)`**: the vectorscope as a `kVecSize x kVecSize` picture: Cb grows to the right and Cr upward, so red is at the top. A cell with pixels is drawn in its own hue (its Cb / Cr at luma `kVecCellLuma`) with brightness `max(kVecFloor, density(count, total, kVecSat))`; empty cells are the background. Null if `total == 0`.

**`inline std::shared_ptr<CanvasImage> render(Kind kind, const CanvasImage *img)`**: the scope picture for `kind`: `waveformImage(waveformCounts(img))`, `vectorImage(vectorCounts(img))`, and null for `Histogram` (which is drawn from the counts, not from a picture) or for an empty picture.

---

## Saving and files

### `sidecar.hpp`
The saved edits as JSON. Pure functions on plain values; **loading validates and clamps everything**, so a hand-edited or damaged file can never put the editor in a state its own UI couldn't reach. The undo history is not saved. See [sidecar-format.md](sidecar-format.md). `AdjustmentSet::toJson` / `loadJson` call these functions; the file handling lives next to `openImage`. `preset.hpp` reuses the per-group functions (`sidecarAdjToJson` / `FromJson`, the curve, Mixer and grade ones), so a preset is validated and clamped exactly like a saved edit. `sidecarSyncLook` (last entry below) is how Sync Settings puts a look into another picture's sidecar. The wrapper also carries the picture's **named snapshots** (see below), and `sidecarFileOf` understands a virtual copy's path.

**Depends on:** `adjustments.hpp`, `color_grade.hpp`, `color_mix.hpp`, `curves.hpp`, `geometry.hpp`, `lut3d.hpp`, `mask.hpp`, `minijson.hpp`, `photo_meta.hpp`, `virtual_copy.hpp`. No UI. **Test:** `cases/sidecar.inc`.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kSidecarVersion` | `int` | 1 | Written as `version` by `sidecarFileText`; `sidecarReadForRewrite` (so `sidecarSyncLook` and `sidecarPatchMeta`) refuses a file whose `version` is missing, not a number or higher ("from a newer Literoom"). `sidecarEditsFromJson` itself does not look at it |
| `kSidecarExt` | `const char *` | `".literoom.json"` | Appended to the picture's path. Defined in `virtual_copy.hpp` (which holds the rest of the file-name rules) and visible here |
| `kMaxSnapshots` | `size_t` | 20 | Named snapshots kept per picture (written and read) |
| `kSnapshotNameMax` | `size_t` | 32 | Longest snapshot name |
| `kSnapshotsMaxBytes` | `size_t` | 24 MiB | Largest the snapshot list may grow to (the app refuses to add one past it) |
| `kSidecarMaxPrompts` | `size_t` | 256 | Object prompts kept per mask part |
| `kSidecarMaxMapPixels` | `int64_t` | 2^26 | Largest object bitmap (8192 × 8192) |

**Types:** `struct SidecarEdits { AdjValues values{}; CropRect crop; Orient orient; float angle = 0; bool expand = false; CurveSet curves = identityCurves(); MixValues mix{}; ColorGrade grade; MaskSet masks; LutRef lut; }`: everything the user did to a picture.

**`struct NamedSnapshot { std::string name; SidecarEdits edits; }`** and **`using SnapshotList = std::vector<NamedSnapshot>`**: a saved, named copy of the whole edit state.

**File layout** (written by `sidecarEditsToJson`; the wrapper's `snapshots` array is described under the snapshot functions below)
- `adjust`: object keyed by **slider label** (e.g. `"Exposure"`, `"Noise Reduction"`, `"Color Noise"`, `"Colorize Hue"`); zero sliders are left out.
- `curves`: keys `rgb`, `r`, `g`, `b`, each an array of `[x, y]` pairs; identity curves left out.
- `colorMix`: keys `hue`, `sat`, `lum`, each an array of 8 numbers; `{}` if all zero.
- `colorGrading`: `wheels` (4 objects with `hue`, `sat`, `lum`, `visible`), `blending`, `balance`; `{}` if default.
- `lut`: `{ "name": "...", "hash": "0123456789abcdef" }`, **only when the picture has a LUT** (the key is absent otherwise, so a picture without one writes the same file as before). The strength is the `LUT Amount` entry of `adjust`. `hash` is 16 hex digits and left out when unknown. The table itself is never stored (see `lut_library.hpp`).
- `geometry`: `flip`, `rotate` (quarter turns), `angle`, `keepFullCanvas`, and `crop` as `[x, y, w, h]` (only if there is a crop).
- `masks`: array of masks. Each has `enabled`, `invert`, `adjust`, `parts`, and only when non-default: `name`, `amount`, `colorMix`, `colorGrading`, `curves`. Each part is the shape object plus `op` (`"add"`, `"subtract"`, `"intersect"`).
- Shape objects by `type`: `radial` (`cx, cy, rx, ry, angle, feather`); `linear` (`cx, cy, len, angle`); `brush` (`strokes` of `erase, size, feather, density, flow` (only if not 1), and `pts` as a flat `x0, y0, x1, y1, …` list); `luminosity` (`lo, hi, feather`); `colorRange` (`set, r, g, b, range, feather`); `object` (`feather, grow, prompts` as `[x, y, fg]`, and `map` = `{w, h, rle}` where `rle` is base64 of the run-length-coded bytes); `rect` (`cx, cy, rx, ry, angle, feather`: the same fields as `radial`); `polygon` (`feather`, and `pts` as a flat `x0, y0, x1, y1, …` list).

**Functions**

**`inline std::string sidecarFileOf(const std::string &imagePath)`**: `"photo.jpg"` → `"photo.jpg.literoom.json"`. For a virtual copy's path (`"photo.jpg|vc2"`, see [`virtual_copy.hpp`](#virtual_copyhpp)) it is that copy's own file, `"photo.jpg.vc2.literoom.json"`. Implemented by `vcSidecarFileOf`.

**`inline float sidecarNum(const mj::Json &o, const char *key, float def, float lo, float hi)`**: the number under `key` in object `o`, clamped to `[lo, hi]`; `def` (**not clamped**) if the key is missing or not a number.

**`inline bool sidecarFlag(const mj::Json &o, const char *key, bool def)`**: the bool under `key`, or `def` if missing or not a bool.

**`inline float sidecarClampNum(const mj::Json &x, float lo, float hi)`**: a JSON value as a float clamped to `[lo, hi]` (a non-number counts as 0).

**`inline mj::Json sidecarAdjToJson(const AdjValues &v)`** / **`inline AdjValues sidecarAdjFromJson(const mj::Json *j)`**
- Write: label-keyed object of non-zero sliders. Read: each slider clamped to its own range; a null pointer, a non-object, or a missing or non-numeric key gives 0.

**`inline mj::Json sidecarCurvesToJson(const CurveSet &cs)`** / **`inline CurvePoints sidecarCurveFromJson(const mj::Json &arr)`**
- Read enforces the editor's invariants: 2 to 16 points, each an `[x, y]` pair of numbers (clamped to 0..255), first point at x = 0 and last at x = 255, interior points at least `kCurveMinGap` apart. **Anything else becomes the identity curve.**

**`inline mj::Json sidecarMixToJson(const MixValues &m)`** / **`inline MixValues sidecarMixFromJson(const mj::Json *j)`**: read clamps each value to -100..100; missing rows or entries stay 0.

**`inline mj::Json sidecarGradeToJson(const ColorGrade &g)`** / **`inline ColorGrade sidecarGradeFromJson(const mj::Json *j)`**: read clamps `hue` to 0..359, `sat` to 0..100, `lum` to -100..100, `blending` to 0..100, `balance` to -100..100.

**`inline mj::Json sidecarLutToJson(const LutRef &r)`** / **`inline LutRef sidecarLutFromJson(const mj::Json *j)`**: the `lut` object above (also used by `preset.hpp`). Write: the name goes through `sanitizeLutName`, `hash` only if non-zero. Read: not an object, no string `name`, or a name with nothing left after cleaning gives **no LUT** (never an error: a bad `lut` cannot make the file unreadable); the name is cleaned (printable ASCII, `<>:"/\|?*` become `_`, at most 48 characters, no trailing dot or space), so a hand-edited name can never point outside the library folder; a `hash` that is not exactly 16 hex digits is dropped (read as unknown).

**`inline const char *sidecarOpKey(MaskOp op)`**: `"subtract"`, `"intersect"`, otherwise `"add"`.

**`inline std::string sidecarMapData(const ObjectMap &m)`**: base64 of the run-length-coded bitmap, memoized by the map's id (maps are immutable) so autosave doesn't re-encode them. The memo holds up to 16 entries. **UI thread only.**

**`inline mj::Json sidecarShapeToJson(const MaskShape &s)`**: one shape as above. An object's bitmap is included only if its size matches `w × h`.

**`inline bool sidecarShapeFromJson(const mj::Json &o, MaskShape *out)`**
- Returns false if `type` is missing or unknown. Load limits:
  - radial: `cx, cy` -1..2; `rx, ry` 0.001..4; `angle` -360..360; `feather` 0..1. linear: `len` 0.001..4.
  - brush: at most 512 strokes and 20000 points per stroke; `size` within `kBrushMinSize..kBrushMaxSize`; point coordinates -1..2; a stroke with no points is dropped; `flow` 0.01..1.
  - luminosity: `lo` and `hi` set through `setLo` / `setHi`, so Min never passes Max.
  - object: `feather` 0..1, `grow` -1..1; prompts need 3 items (`[x, y, fg]`), at most 256, coordinates 0..1. The bitmap is accepted only if `w`, `h` are 1..16384, `w × h <= 2^26`, the base64 and run-length decode succeed and give exactly `w × h` bytes; it gets a **new `id`**.
  - rect: the same limits as radial. polygon: `feather` 0..1; point coordinates -1..2; at most `kPolyMaxPoints` (2048) points (an odd trailing coordinate is ignored); **fewer than three points is kept** (it just selects nothing).

**`inline mj::Json sidecarMaskToJson(const Mask &m)`** / **`inline bool sidecarMaskFromJson(const mj::Json &o, Mask *out)`**
- Read: false if `o` isn't an object, `parts` is missing, or **no valid part** remains (a mask without a shape can't be shown or edited). Parts with an unknown or invalid shape are skipped; at most `kMaxMaskParts` are kept. The name is passed through `sanitizeMaskName`; `amount` is clamped to 0..100 (default 100).

**`inline mj::Json sidecarEditsToJson(const SidecarEdits &e)`**: the whole file's content (layout above).

**Named snapshots** (the wrapper's `snapshots` array, see [sidecar-format.md](sidecar-format.md#named-snapshots))

**`inline std::string sanitizeSnapshotName(const std::string &s)`**: printable ASCII only, trimmed, at most `kSnapshotNameMax` characters (no trailing space after the cut). May return an empty string. The same rules as a preset name. **`inline std::string snapshotNameOrDefault(const std::string &s)`**: the sanitized name, or `"Snapshot"` when it is empty; the writer and the loader both use it, so a blank name reads and writes back the same.

**`inline mj::Json sidecarSnapshotsToJson(const SnapshotList &list)`**: `[ { "name": ..., "edits": { ... } }, ... ]`, at most `kMaxSnapshots` entries, each `edits` written by `sidecarEditsToJson`.

**`inline SnapshotList sidecarSnapshotsFromJson(const mj::Json *j, int origW, int origH)`**
- Inputs: the `snapshots` value of a parsed sidecar (a null pointer if the key is missing) and the picture's original size, which each snapshot's crop is checked against.
- Never fails. A value that isn't an array gives an empty list; an entry that isn't an object, or whose `edits` `sidecarEditsFromJson` refuses (this includes a non-positive size), is skipped; at most `kMaxSnapshots` are kept. Each kept snapshot is validated and clamped like the live edits, and its name goes through `snapshotNameOrDefault`.

**`inline bool sidecarEditsFromJson(const mj::Json &o, int origW, int origH, SidecarEdits *out)`**
- Inputs: the parsed JSON, the picture's original size (needed to check the crop), and the output.
- Returns: false (and leaves `*out` untouched) only if `o` isn't an object or `origW`/`origH` aren't positive. Anything wrong inside is dropped or clamped instead.
- Details: `rotate` is rounded into 0..3; `angle` clamped to ±`kAngleMax`. The crop is kept only if it has four numbers, positive `w`/`h`, and fits inside the oriented, straightened size (a quarter turn swaps width and height first, then `rotatedDims` with the angle and `keepFullCanvas`). At most `kMaxMasks` masks are loaded.

**`inline bool sidecarSyncLook(const std::string &oldText, const std::string &imageName, int imgW, int imgH, const AdjValues &values, const CurveSet &curves, const MixValues &mix, const ColorGrade &grade, const LutRef &lut, std::string *outText, std::string *err)`**: puts a look into **another picture's** sidecar text without opening it (Image > Sync Settings to Selected). See [sidecar-format.md](sidecar-format.md#syncing-a-look-onto-other-pictures).
- Inputs: `oldText` is the picture's current sidecar file (`""` if it has none). `imageName` is written to `image.name` (informational). `imgW` / `imgH` are the picture's upright size: **required only when `oldText` is empty** (a new file must record it); otherwise the size recorded in the file is used, and if `imgW` and `imgH` are both positive they must match it. The look is the four groups a preset holds plus the LUT (`lut` is **required**, not defaulted, so a caller must say what a synced look does to the target's LUT: an empty one clears it). `outText` receives the whole new file (wrapper plus edits); `err` receives a short reason.
- Returns: true with `*outText` set. False, with a reason in `*err` and `*outText` untouched, when the old file can't be trusted: not valid JSON or not an object, a missing, non-numeric or higher `version`, no `image.width` / `image.height`, a different size than the one given, or an `edits` part that `sidecarEditsFromJson` refuses. Or when there is no old file and `imgW` / `imgH` aren't both positive. The caller skips that picture rather than overwrite edits it can't read.
- Details: the old `edits` go through `sidecarEditsFromJson` (so they are validated and clamped as on open), **only** `values`, `curves`, `mix`, `grade` and `lut` are replaced, and the result is written by `sidecarEditsToJson`. The picture's own rating, flag and color label (`meta`) are read from the old file and written back as they were. Crop, orientation, angle, `keepFullCanvas` and masks therefore survive (a crop that no longer fits is dropped by the loader, as on open). The values passed in are written as given, so they should come from `AdjustmentSet::currentPreset` or `presetFromJson`. The output always has `app: "literoom"` and the current `kSidecarVersion`. Syncing the same look twice gives the same text. The picture's named snapshots are read from the old file and written back as they were (each keeps the look it captured). Pure: no file access, safe from any thread; the app calls it from the sync worker (`runSyncJob` in `main.cpp`), which does the reading and the temp-file-and-rename write.

**Rating, flag and color label** (the wrapper's `meta` object; see [sidecar-format.md](sidecar-format.md)). `sidecar.hpp` includes `photo_meta.hpp`.

**`inline bool sidecarReadForRewrite(const std::string &oldText, int imgW, int imgH, SidecarEdits *e, PhotoMeta *meta, SnapshotList *snaps, int *W, int *H, std::string *err)`**: the trust rules shared by every function that rewrites another picture's sidecar. With no text, the edits are neutral and the size must be given. With text, the file must be a JSON object, not from a newer version, with a picture size that matches `imgW x imgH` (when given) and loadable `edits`; otherwise it is refused with a reason in `*err`. On success it returns the file's edits, `meta`, named snapshots (validated by `sidecarSnapshotsFromJson`, empty if there are none) and size. A rewriter must write `meta` and `snaps` back through `sidecarFileText`, or it would drop them.

**`inline bool sidecarPatchMeta(const std::string &oldText, const std::string &imageName, int imgW, int imgH, const MetaPatch &patch, std::string *outText, PhotoMeta *outMeta, std::string *err)`**: the batch rating. Applies `patch` to the picture's rating / flag / color label in its sidecar text and keeps everything else; same trust rules as `sidecarSyncLook` (both are built on `sidecarReadForRewrite`). `*outMeta` is the meta after the patch. A caller can skip a picture with no sidecar when `patch.applyTo(PhotoMeta{})` is the default, since there is nothing to record. Pure.

**`inline PhotoMeta sidecarMetaFromRoot(const mj::Json &root)`**: the `meta` object of a parsed sidecar, clamped; the default `PhotoMeta` if it is missing or malformed.

**`inline std::string sidecarFileText(const std::string &imageName, int w, int h, mj::Json edits, const PhotoMeta &meta, const SnapshotList &snapshots = {})`**: the whole sidecar file: `app`, `version`, `image`, `meta` (only if it is not the default), `edits`, and `snapshots` (only if there are any). The app's own save, `sidecarSyncLook` and `sidecarPatchMeta` all write through it, so no writer can drop `meta` or `snapshots`. With a default `meta` and no snapshots the output is exactly the layout the app wrote before either existed. `imageName` is the real picture's name, also for a virtual copy.

**`inline const std::string &sidecarNeutralEditsDump()`**: the text `sidecarEditsToJson` gives for an untouched `SidecarEdits{}`.

**`struct SidecarScan { bool edited = true; PhotoMeta meta; }`** and **`inline SidecarScan sidecarScan(const std::string &text)`**: what the filmstrip wants from a sidecar without opening the picture. `edited` is false only for a file that parses as an object whose `edits` dump equals the neutral dump (a file that exists because the picture was rated, or only has snapshots: `snapshots` is not looked at); an unreadable file, a non-object, or one with no `edits` counts as edited, as any existing file always did. `meta` is the file's rating, flag and label (the default if the file can't be read). Pure; the thumbnail worker calls it on its own thread.

### `photo_meta.hpp`
A picture's **rating, flag and color label**, their JSON form, and the filmstrip filter. Pure values: no UI, no files, no editor state. They are **not edits**: they never go through `AdjustmentSet`, so there is no undo step or History label. They live in the sidecar's top-level `meta` object (see [sidecar-format.md](sidecar-format.md)).

**Depends on:** `minijson.hpp`. **Test:** `cases/photo_meta.inc` (the sidecar's `meta` object is covered in `cases/sidecar.inc`). **Thread model:** no shared state; every function is safe from any thread.

**Constants:** `kMaxRating` = 5; `kLabelCount` = 5; `kLabelNames` = `red`, `yellow`, `green`, `blue`, `purple` (label numbers are 1 to 5, 0 is none).

**`enum class PhotoFlag : uint8_t { None, Pick, Reject }`**

**`struct PhotoMeta { int rating = 0; PhotoFlag flag = PhotoFlag::None; int label = 0; }`**: `==` and `isDefault()`.

**`inline PhotoMeta photoMetaSanitized(PhotoMeta m)`**: rating clamped to 0..5, an unknown flag becomes `None`, label clamped to 0..5.

**`inline mj::Json photoMetaToJson(const PhotoMeta &m)`**: `{"rating": 3, "flag": "pick", "label": "red"}` with every default field left out, so the default gives an empty object. The value is sanitized first.

**`inline PhotoMeta photoMetaFromJson(const mj::Json &o)`**: a non-object gives the default; a non-number rating is 0 and others are clamped into 0..5; a flag or label that isn't one of the exact names (case matters) is "none".

**`struct MetaPatch { std::optional<int> rating; std::optional<PhotoFlag> flag; std::optional<int> label; }`**: "set these fields": what one rating / flag / label command changes. `empty()`; `applyTo(m)` changes only the fields that are set and sanitizes the result. A field set to 0 / `None` is a change (a clear), not an absent field.

**`struct MetaFilter { int minRating; unsigned flags; unsigned labels; }`**: what the filmstrip shows. `minRating` is "at least this many stars" (0 = off); `flags` has a bit `1 << PhotoFlag value` per accepted flag and `labels` a bit `1 << label` per accepted color (0 = off for each). `active()` is true when anything is on. `matches(m)`: every switched-on criterion must hold (AND), and within `flags` or `labels` any chosen value matches (OR). A picture with no label doesn't match a color filter. `toggleRating(n)` (n is clamped; the value already set switches it off), `toggleFlag(f)`, `toggleLabel(c)` (1..5; anything else is ignored).

### `preset.hpp`
Presets: a saved **look** (slider values, the four tone curves, the Color Mixer, Color Grading and the 3D LUT, by name) that can be applied to any picture. Crop, orientation, straighten angle and masks are **not** part of a preset, because they belong to one picture. Pure functions on plain values plus small file helpers; no UI, no editor state. Loading goes through the sidecar readers, so a hand-edited or damaged preset can never put the editor in a state its own UI couldn't reach. `AdjustmentSet::currentPreset` / `applyPreset` are the bridge to the edit state; the Presets pane in `main.cpp` is the UI. See [undo-and-history.md](undo-and-history.md) for how applying one appears in History.

**Depends on:** `adjustments.hpp`, `color_grade.hpp`, `color_mix.hpp`, `curves.hpp`, `file_utils.hpp`, `lut3d.hpp`, `minijson.hpp`, `sidecar.hpp` (all at or below `io/`). No UI. **Test:** `cases/preset.inc`; the undo behavior of applying a preset is covered in `cases/adjustment_set.inc`. **Thread model:** no shared state, so every function is safe from any thread; the app calls them from the UI thread only.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kPresetVersion` | `int` | 1 | Written to every file; a file with a missing, lower-than-1 or higher version is refused |
| `kPresetExt` | `const char *` | `".literoom-preset.json"` | Appended to the file-name stem |
| `kPresetNameMax` | `size_t` | 32 | Longest preset name kept |
| `kPresetMaxFileBytes` | `size_t` | 2^20 (1 MiB) | Larger files are refused (a real preset is a few hundred bytes) |
| `kPresetMaxFiles` | `size_t` | 500 | Most files `listPresetFiles` returns |

**Types:** `struct Preset { std::string name; AdjValues values{}; CurveSet curves = identityCurves(); MixValues mix{}; ColorGrade grade; LutRef lut; }` with a defaulted `operator==` (all six fields: two looks that differ only in the LUT are different presets). A default `Preset` is the neutral look with no name and no LUT.

**File layout** (written by `presetToJson`): `{ "app": "literoom", "kind": "preset", "version": 1, "name": "...", "adjust": {...}, "curves": {...}, "colorMix": {...}, "colorGrading": {...}, "lut": {"name": "...", "hash": "..."} }`. The four groups use exactly the sidecar's forms (see `sidecar.hpp`): sliders keyed by **label** with zero sliders left out (so `LUT Amount` is in `adjust`), identity curves left out, and so on; `lut` is `sidecarLutToJson`'s object and is **only written when the look has a LUT**. A neutral look therefore has four empty groups and no `lut`. The LUT is a name and a hash, never a table, so a preset stays a few hundred bytes (the 1 MiB limit is not touched). Because `adjust` is keyed by slider label, **renaming a slider label orphans it in saved presets** just as it does in sidecars.

**Functions**

**`inline std::string sanitizePresetName(const std::string &s)`**
- Returns: `s` reduced to printable ASCII (bytes `0x20..0x7E` only, so control characters, DEL and every non-ASCII byte are dropped), trimmed of leading and trailing spaces, and cut to `kPresetNameMax` characters (re-trimmed after the cut). May return an empty string. The app's `TextInput` is ASCII-only, which is why non-ASCII is not kept.

**`inline bool presetNameBlank(const std::string &s)`**: true if `sanitizePresetName(s)` is empty.

**`inline std::string presetFileStem(const std::string &name)`**
- Returns: a safe file-name stem for `name`: the sanitized name with each of `< > : " / \ | ? *` replaced by `_`, trailing dots and spaces removed, and the Windows device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1..9`, `LPT1..9`, any case) prefixed with `_`. Blank input gives `"preset"`.
- Gotcha: the mapping is not one-to-one. `"a/b"` and `"a_b"` share a stem, so saving one overwrites the other's file (the name stored *inside* the file stays as given), and on a case-insensitive file system so do `"Warm"` and `"warm"`. Also, only a name that is **exactly** a device name is caught: `"CON.v2"` comes back unchanged although Windows treats it as reserved (the same gap as `lutFileStem`).

**`inline mj::Json presetToJson(const Preset &p)`**: the file layout above. The name is sanitized on the way out.

**`inline bool presetFromJson(const mj::Json &o, Preset *out)`**
- Inputs: a parsed JSON value and the output.
- Returns: false (and leaves `*out` untouched) if `o` isn't an object, `kind` is missing or not the string `"preset"`, or `version` is not a number from 1 to `kPresetVersion` (a string `"1"` is refused). The `app` key is not checked. Otherwise true.
- Details: nothing inside is trusted. Sliders are clamped to their ranges, a non-number becomes 0, a curve that breaks the editor's rules becomes the identity curve, Mixer and grade values are clamped, and a bad `lut` object becomes no LUT (all by the `sidecar*FromJson` functions). A missing or invalid `name` gives an empty name; `loadPresetFile` fills it from the file name.

**`inline std::string presetDir()`**
- Returns: where presets are kept: `%APPDATA%\Literoom\presets` on Windows; `$XDG_CONFIG_HOME/literoom/presets` or `~/.config/literoom/presets` elsewhere; the relative folder `presets` if no home can be found. Does not create the folder.
- Gotcha: on Windows `%APPDATA%` comes from the environment in the system code page, not UTF-8, so a profile path with non-ASCII characters may not round-trip through `fsPath`.

**`inline std::string presetPath(const std::string &dir, const std::string &name)`**: `dir + "/" + presetFileStem(name) + kPresetExt`.

**`inline bool savePresetFile(const std::string &dir, const Preset &p, std::string *err, std::string *pathOut = nullptr)`**
- Inputs: the folder, the preset, an optional error string (may be null), and an optional output for the path written.
- Returns: true on success. Refuses (false, `*err` = `"give the preset a name"`) if the name is blank after sanitizing.
- Details: creates the folder (and parents) if needed, then writes with `writeFileAtomic` (temp file and rename), so an interrupted save never leaves half a file. A preset whose stem matches an existing file **overwrites it** without asking. Other failures set `*err` to the file system's message or `"couldn't write <name>"`.

**`inline bool loadPresetFile(const std::string &path, Preset *out, std::string *err)`**
- Returns: true and fills `*out`; on any failure returns false with `*out` untouched and `*err` set to one of: `"couldn't read <file>"` (unreadable or empty), `"<file> is too large for a preset"` (over `kPresetMaxFileBytes`), `"<file>: <parse message>"` (not valid JSON), or `"<file> is not a Literoom preset"` (`presetFromJson` refused it).
- Details: if the file has no usable name, the name becomes the file name without `kPresetExt`, sanitized. Gotcha: it cuts off the last `strlen(kPresetExt)` characters of the file name **without checking** that they are the extension. `listPresetFiles` only returns files that end with it, so the app is safe; a caller passing some other path gets a shortened name.

**`inline std::vector<std::string> listPresetFiles(const std::string &dir)`**
- Returns: file names (UTF-8, no directory part) of regular files directly in `dir` whose lower-cased name is longer than, and ends with, `kPresetExt`, in natural order (ties broken by plain comparison, like `listImageFiles`). Sub-folders and other files are skipped. A missing or unreadable folder gives `{}`.
- Cap behavior: at most `kPresetMaxFiles` names are collected **before** sorting, so a folder with more yields an arbitrary subset, sorted (the same rule as `listImageFiles`). The app shows at most 24 anyway (`kPresetRows` in `main.cpp`).

**`inline bool deletePresetFile(const std::string &path)`**: true if the file was removed; false if it did not exist or couldn't be removed.

**In the app** (`main.cpp`, not this header): the Presets activity keeps a cache of the folder's presets, re-read when the pane is opened and after each save or delete (never per frame). Clicking a row calls `AdjustmentSet::applyPreset`; "Save current look" calls `savePresetFile` with `currentPreset(name)`; unreadable files are skipped silently. `typingInTextField()` is true while the preset-name field has focus (and also while the mask-name or snapshot-name field does), so the plain-key shortcuts (Y, J, Left, Right, Delete...) stand down. The list shows at most `kPresetRows` (24) presets, and a damaged file is skipped.

### `color_tag.hpp`
What a picture file says about its **color space**, and carrying that tag into a PNG export. The editor converts nothing: it edits the file's numbers as they are and assumes sRGB. That is fine while the export says what the source said, and wrong when the tag is lost (a Display P3 picture exported as an untagged PNG looks dull everywhere else). So the tag travels with the picture. A source with no tag stays untagged: nothing is ever invented, and **no pixel is converted anywhere**. Everything is in `namespace color_tag`.

| Source | Export | What happens |
|---|---|---|
| PNG | PNG | its `iCCP` / `sRGB` / `gAMA` / `cHRM` chunks are copied (`carryColorToPng`) |
| JPEG | PNG | its ICC profile becomes an `iCCP` chunk |
| JPEG | JPEG | already done by `image_meta.hpp` (`embedSourceMetadata`) |
| PNG | JPEG | **not carried**: the PNG's profile is deflate-compressed and reading it would need an inflater (the app's status line says so) |

The PNG work is a byte splice like `exif::embed`: no libpng, no zlib, no LiteUI, so it behaves the same on Windows and Linux. The chunks go right after IHDR, before PLTE and IDAT, as the PNG specification wants.

**Depends on:** `exif.hpp` (`JpegMeta`, `readJpegMeta`), `file_utils.hpp` (`fsPath`, `readFilePrefix`, `isJpegPath`). No UI. **Test:** `cases/color_tag.inc` (synthetic PNG and JPEG buffers, as the header says). **In the app** (`main.cpp`, not this header): `currentColorTag()` calls `colorTagOf` once per opened picture (the Info pane) and `announceColorTag()` puts `Color: <text>` in the status bar for every file that could be read (with ". Shown as sRGB, not converted" appended when the file is tagged as something else); the single-picture `exportImage` calls `carryColorToPng` after a PNG export (the "Exported" status then carries ` [<description> kept]`) and, for a JPEG export of a non-JPEG source tagged as not sRGB, appends ` (color profile not carried into JPEG)`. The batch `exportPicture` (`export_worker.hpp`) calls `carryColorToPng` after every PNG it writes, 8- or 16-bit, and ignores its result; it adds no "not carried into JPEG" note.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kInfoPrefixBytes` | `size_t` | 2 MiB | How much of a file's start `colorTagOf` reads (a ProPhoto profile is about 0.8 MB over many JPEG APP2 segments). **Not** the same constant as `kInfoPrefixBytes` (256 KiB) in `image_meta.hpp`: that one is global, this one is in `color_tag` |
| `kCarryPrefixBytes` | `size_t` | 8 MiB | How much `colorOfSource` reads (same limit as `kMetaPrefixBytes` for `embedSourceMetadata`) |
| `kMaxIccBytes` | `size_t` | 16 MiB | Largest ICC profile or `iCCP` payload accepted |
| `kPngHeadBytes` | `size_t` | 1 MiB | How much of an exported PNG's start `insertColorIntoFile` searches for the first IDAT |
| `kProfileNameMax` | `size_t` | 79 | The PNG limit for an `iCCP` profile name |

**Types**
- `struct PngColor { std::vector<uint8_t> iccp, srgb, gama, chrm; }`: the color chunks of a PNG, **payloads only** (CRCs are made again on the way out). `iccp` = profile name, NUL, compression method, compressed profile; `srgb` = 1 byte (rendering intent); `gama` = 4 bytes (gamma × 100000); `chrm` = 32 bytes (white point and primaries). `bool any() const` is true if any of the four is non-empty.
- `struct ColorTag { std::string text; bool srgb = true; }`: for the Info pane. `text` is empty when the file could not be read. `srgb` is false when the file is tagged as something else (the editor still treats it as sRGB).

**Functions: checksums and bytes**

**`inline uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0)`**: CRC-32 as PNG uses it (polynomial `0xEDB88320`); `crc` is the value so far, **0 to start** (like zlib's `crc32()`). This differs from `pngCrcUpdate` in `image_codec16.hpp`, which wants `0xFFFFFFFF` as the start and a final xor.

**`inline uint32_t adler32(const uint8_t *p, size_t n)`**: Adler-32 (the zlib checksum), in blocks of 5552 so the sums cannot overflow.

**`inline uint32_t be32(const uint8_t *p)`** / **`inline void putBe32(std::vector<uint8_t> &v, uint32_t x)`**: read / append a big-endian 32-bit value.

**`inline std::vector<uint8_t> zlibStored(const uint8_t *data, size_t n)`**: `data` as a valid zlib stream made of **stored (uncompressed) deflate blocks** of up to 65535 bytes, so no compressor is needed. An `iCCP` made from a JPEG profile is therefore a little larger than the profile. `n == 0` gives one empty block.

**Functions: PNG chunks**

**`inline bool pngSignature(const uint8_t *p, size_t n)`**: true if `p` is non-null and starts with the 8-byte PNG signature. **`inline bool isType(const uint8_t *type, const char *name)`**: 4-byte compare. **`inline bool isColorChunk(const uint8_t *type)`**: true for `iCCP`, `sRGB`, `gAMA`, `cHRM`.

**`template <class F> inline void forEachPngChunk(const uint8_t *p, size_t n, F &&f)`**
- Calls `f(type, data, length, offsetOfChunk, fits)` for each chunk after the signature whose 8-byte header is in the buffer, in order, until `f` returns false or the buffer ends. It does **not** check that the first chunk is IHDR.
- `fits` says whether the whole chunk (data and CRC) is in the buffer. When it is not, `f` is called once more for that chunk (it may still want the type, as for a huge IDAT) and the walk stops: `f` must not read `data` then. A length above `0x7FFFFFFF` ends the walk without a call.

**`inline std::vector<uint8_t> makeChunk(const char *type, const uint8_t *data, size_t n)`**: a whole chunk: length, type, data, CRC (over the type and the data).

**`inline bool readPngColor(const uint8_t *p, size_t n, PngColor *out)`**
- Returns false (with `*out` reset) if the buffer does not start with a PNG signature; true otherwise, even with no color chunks.
- Reads only the color chunks **before the first IDAT** (or IEND). Only the first **valid** chunk of each kind counts: a chunk with a wrong size or a bad CRC is skipped, so a later good one can still be used. Sizes: `iCCP` 4 to `kMaxIccBytes` bytes, `sRGB` 1, `gAMA` 4, `cHRM` 32. A color chunk that does not fit in the buffer ends the walk.

**`inline std::vector<uint8_t> colorChunkBytes(const PngColor &c)`**: the chunks to write, in the specification's order: `iCCP` **or** `sRGB` (`iCCP` wins), then `gAMA`, then `cHRM`. `gAMA` and `cHRM` are written even next to `iCCP` / `sRGB` if the source had them. Empty chunks are left out.

**`inline bool buildColoredHead(const uint8_t *p, size_t n, const PngColor &c, std::vector<uint8_t> *head, size_t *idatAt)`**
- From the first `n` bytes of a PNG builds its start: signature, IHDR, then `c`'s chunks, then every other chunk that came before the first IDAT (**without** any color chunk of its own: the old tags are dropped). `*idatAt` is the offset of the first IDAT in `p`; everything from there on is copied as it is.
- Returns false if `p` does not start with a signature and a 13-byte IHDR, if a chunk before the IDAT is cut off by the end of the buffer, or if no IDAT starts inside it (also when IEND comes first).

**`inline bool insertColor(std::vector<uint8_t> *png, const PngColor &c)`**: the whole PNG in memory with `c`'s chunks **in place of any it had**. False, with `*png` untouched, if it is not a PNG with an IHDR and an IDAT. An empty `c` therefore **strips** the tags.

**`inline bool insertColorIntoFile(const std::string &path, const PngColor &c)`**
- The same for a file, without holding the image data (it can be hundreds of MB): reads the first `kPngHeadBytes`, builds the new head, writes it to `<path>.colortag.tmp`, streams the rest from the first IDAT in 1 MiB blocks, then renames the temp file over the original (both files are closed first: a rename over an open file fails on Windows).
- Returns false with the file untouched if anything fails, including when **the first IDAT does not start within the first 1 MiB** (for example a PNG with a very large text chunk before its image data). The temp file is removed on failure. It does not `fsync`.

**`inline std::string sanitizeName(const std::string &s)`**: for a profile name: at most `kProfileNameMax` characters, each byte outside printable ASCII becomes **`?`** (unlike the other `sanitize*Name` functions, which drop such bytes), trailing then leading spaces trimmed. May return an empty string.

**Functions: ICC**

**`inline std::string iccDescription(const uint8_t *icc, size_t n)`**
- A readable name for an ICC profile: its `desc` tag, through `sanitizeName`. Empty if it cannot be read: fewer than 132 bytes, no `acsp` signature at offset 36, more than 1000 tags, a tag table or tag outside the data, or a `desc` of any type other than the two below.
- v2 `textDescription`: the ASCII count (which includes the NUL) then the text. v4 `multiLocalizedUnicode`: the **first record only**, UTF-16BE, characters outside printable ASCII become `?`.

**`inline std::vector<uint8_t> assembleIcc(const std::vector<std::vector<uint8_t>> &chunks)`**: a JPEG's whole ICC profile from `exif::JpegMeta::icc`: each APP2 payload without its 14-byte `"ICC_PROFILE\0"` + sequence header, joined in order (a payload of 14 bytes or fewer adds nothing).

**Functions: describing**

**`inline bool containsNoCase(const std::string &hay, const char *needle)`**: ASCII case-insensitive substring test.

**`inline ColorTag describeProfile(const std::string &name)`**: if the name contains `srgb` (any case): `"sRGB (embedded profile)"`, `srgb = true`. Otherwise `"<name> (embedded profile, not sRGB)"` (`"unnamed profile"` for an empty name), `srgb = false`. Gotcha: it is a substring test, so any profile whose name merely mentions sRGB counts as sRGB.

**`inline std::string profileNameOf(const PngColor &c)`**: the profile name inside `c.iccp` (up to the NUL, at most 80 bytes read), through `sanitizeName`.

**`inline ColorTag describePng(const PngColor &c)`**: first match wins:
1. `iccp` present: `describeProfile(profileNameOf(c))`.
2. `srgb` present: `"sRGB"`.
3. `gama` present: the gamma `g = value / 100000`. If `|g − 0.45455| <= 0.005` (about 2.18 to 2.22): `"gamma 2.2 (assumed sRGB)"`, `srgb = true`. Otherwise `"gamma <1/g, 2 decimals>, not sRGB"` (0 if `g <= 0`), `srgb = false`, with `" (with primaries)"` added if `cHRM` is also there. Gotcha: in the sRGB-like gamma branch `cHRM` is **not looked at**, so a file with a 2.2 gamma and non-sRGB primaries reads as sRGB.
4. only `chrm`: `"custom primaries, not sRGB"`, `srgb = false`.
5. nothing: `"untagged (assumed sRGB)"`.

**`inline ColorTag colorTagOf(const std::string &path)`**
- What the file at `path` is tagged as. Reads the first `kInfoPrefixBytes`. An unreadable or empty file gives an **empty** `text`; anything else starts as `"untagged (assumed sRGB)"`.
- A **PNG is recognized by its signature, not its extension**; a **JPEG by its extension** (`isJpegPath`) and then its ICC profile (named by `iccDescription`, or `"ICC profile"` if that is empty). Any other file, or a picture with no tag, stays untagged. A JPEG profile that is cut off by the 2 MiB prefix fails validation in `readJpegMeta` and reads as untagged.

**`inline PngColor colorOfSource(const std::string &sourcePath, std::string *text)`**
- The tags `sourcePath` has, as the chunks a PNG export gets: its own for a PNG; for a JPEG, an `iCCP` made from the profile (name, NUL, method 0, `zlibStored` data). Reads the first `kCarryPrefixBytes`. Empty (`any()` false) for an untagged or unreadable source, or a JPEG profile that is empty or over `kMaxIccBytes`.
- `*text` (optional) is set to the description **only when tags were found**; otherwise it is left as it was.

**`inline std::string carryColorToPng(const std::string &pngPath, const std::string &sourcePath)`**
- Writes the source's tags into the PNG just exported at `pngPath` (`colorOfSource`, then `insertColorIntoFile`). Returns the description of what was written (for example `Display P3 (embedded profile, not sRGB)`), or `""` when the source has no tag **or** the file could not be rewritten: the two cases are not told apart. The export stays valid either way.

### `lut_cube.hpp`
Reads a 3D LUT in the Adobe / DaVinci Resolve **`.cube`** format, and says whether a LUT looks made for another kind of picture. Pure text in, `Lut3D` out: no files, no UI. See [3D LUTs](architecture.md#3d-luts).

**Depends on:** `lut3d.hpp`. **Test:** `cases/lut_cube.inc` (including parse time for a 65³ file, which it prints). **Thread model:** no shared state; safe from any thread.

**Types:** `struct LutCubeInfo { std::string title; bool domainDefault = true; bool videoRange = false; }`: what a file said besides its table. `title` is the `TITLE` line (printable ASCII, at most 100 characters); `domainDefault` is false when the input domain is not 0..1 on every channel; `videoRange` is set by `LUT_IN_VIDEO_RANGE` / `LUT_OUT_VIDEO_RANGE`.

**`inline bool parseCube(std::string_view text, Lut3D *out, LutCubeInfo *info, std::string *err)`**
- Returns: true and fills `*out` (and `*info`, which may be null). On failure returns false with a reason in `*err` (which may be null) and **leaves both outputs untouched**. Errors name the line: `"line 12: expected three finite numbers"`.
- Accepted: `LUT_3D_SIZE n` (2..65), `DOMAIN_MIN` / `DOMAIN_MAX` (three numbers each, max above min on every channel), `LUT_3D_INPUT_RANGE lo hi` (the Resolve spelling), `TITLE`, `LUT_IN_VIDEO_RANGE`, `LUT_OUT_VIDEO_RANGE`, `#` comments (also after a data line), blank lines, CRLF or LF, keywords in any case, a leading `+`, exponents. Table lines are exactly three numbers, the red index changing fastest.
- Refused: a 1D LUT (`LUT_1D_SIZE`), a size outside 2..65 or given twice, an unknown keyword, **any keyword after the table data has begun**, table data before the size, a line with fewer or more than three numbers, a non-finite number (`nan`, `inf`, `1e999`), a value with magnitude above 16, a number that is not a plain decimal (a decimal comma is never read), and a table with fewer or more than `size³` entries.
- Numbers are read with `std::from_chars`, **independent of the system locale**: a German or French locale cannot change what a file means.
- Values are stored scaled to 0..255 (`Lut3D::rgb`); the domain is kept as given and `finalize()` is called.

**`inline bool lutNameLooksLog(std::string_view s)`**: true if a word of `s` (split at anything that is not a letter or digit, lower-cased, trailing digits dropped) is a camera-log curve name: `log`, `slog`, `vlog`, `clog`, `logc`, `flog`, `nlog`, `dlog`, `dlogm`, `redlog`, `log3g`, `acescct`, `acescc`. So `"SLog3 to 709"`, `"V-Log_to_709"`, `"ARRI LogC3"` and `"RED Log3G10"` match, and `"Analog Film"`, `"Catalog"` and `"Dialog"` do not.

**`inline std::string lutExpectationWarning(const std::string &label, const Lut3D &L, const LutCubeInfo &info)`**
- Returns `""` when nothing looks off, otherwise one sentence: *"<label> may expect camera-log input (<reasons>). Literoom applies LUTs to sRGB / Rec.709 pictures, so it may look wrong."* The reasons: the name or the title is log-like, the input range is not 0 to 1, and/or the LUT moves mid-gray (127.5) by more than a quarter of the range. Any one is enough. It only **suggests**: nothing in a `.cube` file says for certain, and the app never blocks a LUT because of it.

### `lut_library.hpp`
The LUT library: `.cube` files kept in one folder, found by name, with a small cache of the ones in use. Standard library plus `file_utils` / `lut_cube` / `lut3d`: no UI, no editor state, so everything here is tested against a temp folder. See [3D LUTs](architecture.md#3d-luts).

**Depends on:** `lut3d.hpp`, `lut_cube.hpp`, `file_utils.hpp`. **Tests:** `cases/lut_library.inc` (several threads resolve one LUT at once); `cases/lut_render.inc` (resolves LUTs through `lutCachePut`); `tests/lut_pane_test.cpp` (a real temp library driven through the LUTs pane's functions in `main.cpp`: refresh, import, apply, remove, status texts). **Thread model:** the cache is a mutex-guarded global (`acquireLut`, `clearLutCache`, `lutCachePut` are safe from any thread; the UI thread and the export worker both resolve LUTs). `lutDirOverride()` is a plain global meant to be set by tests before any thread uses the library. Everything else is a pure function. **This is the only place a render-side LUT is ever read from a file**; the render loop and the previews' renders see frozen tables.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kLutExt` | `const char *` | `".cube"` | Extension of a library file |
| `kLutMaxFileBytes` | `size_t` | 16 MiB | Larger files are refused (a 65³ cube is about 7 MB) |
| `kLutMaxFiles` | `size_t` | 100 | Most files listed or kept (the pane builds this many rows) |
| `kLutCacheEntries` | `size_t` | 4 | Tables (and remembered failures) the cache holds; the oldest is dropped |

**Types**

- **`struct LutEntry { std::string name; std::string path; uint64_t hash = 0; int size = 0; }`**: a library file. `listLutEntries` leaves `hash` and `size` at 0; `importLutFile` fills both.
- **`enum class LutStatus { None, Ok, HashChanged, Missing, Unreadable }`**: `None` = the edit has no LUT; `Ok` = found, same bytes as when it was chosen; `HashChanged` = found and **used**, but not the file that was chosen; `Missing` = no such file; `Unreadable` = there, but unreadable or not a valid cube. Only `Ok` and `HashChanged` give a table.
- **`struct LutResolved { std::shared_ptr<const Lut3D> lut; LutStatus status; uint64_t hash; }`**: the answer of `acquireLut`. `hash` is the hash of the file's bytes **as it is now** (what a new `LutRef` should store), whatever the reference asked for; 0 when there is no table.

**Functions**

**`inline uint64_t lutHashBytes(const uint8_t *p, size_t n)`**: FNV-1a, 64 bit. Never returns 0 (0 means "unknown" in a `LutRef`).

**`inline std::string lutDir()`** / **`inline std::string &lutDirOverride()`**: where the library lives: `%APPDATA%\Literoom\luts` on Windows; `$XDG_CONFIG_HOME/literoom/luts` or `~/.config/literoom/luts` elsewhere; the relative folder `luts` if no home can be found. Does not create it. A non-empty `lutDirOverride()` wins (tests). On Windows the same non-UTF-8 `%APPDATA%` gotcha as `presetDir` applies.

**`inline std::string lutPath(const std::string &dir, const std::string &name)`**: `dir + "/" + lutFileStem(name) + ".cube"`. **`inline std::string lutNameOfFile(const std::string &fileName)`**: a file name without its `.cube` (any case), cleaned by `sanitizeLutName`.

**`inline bool readLutBytes(const std::string &path, std::vector<uint8_t> *out, std::string *err)`**: all bytes of a file. False with *"couldn't read <file>"*, *"<file> is empty"* or *"<file> is too large for a LUT (16 MiB at most)"* (checked before reading). **`parseLutBytes`**: the same bytes through `parseCube`, error prefixed with the file name. **`inline bool loadLutFile(const std::string &path, Lut3D *out, LutCubeInfo *info, uint64_t *hash, std::string *err)`**: both, bypassing the cache; `*hash` (optional) is the hash of the bytes. The LUT previews use this.

**`inline std::vector<std::string> listLutFiles(const std::string &dir)`**: names (UTF-8, no directory) of the regular files in `dir` ending in `.cube` (any case), natural order; sub-folders and other files skipped; a missing folder gives `{}`. At most `kLutMaxFiles` are collected **before** sorting (the same rule as `listPresetFiles`).

**`inline std::vector<LutEntry> listLutEntries(const std::string &dir)`**: `listLutFiles` as entries (name and path only), **leaving out files an edit could not point at again**: a file whose name has a character a LUT name cannot hold (non-ASCII, a trailing dot) would not be found by `lutPath(name)`, so it is not offered.

**`inline bool importLutFile(const std::string &src, const std::string &dir, LutEntry *out, std::string *warning, std::string *err)`**
- Copies the `.cube` at `src` into the library after checking that it parses. The name is the file's name, cleaned; a taken name gets `-2`, `-3`, ... (the name is cut so the suffix fits `kLutNameMax`); **a taken file with the very same bytes is reused** and nothing is written. The bytes are copied unchanged (`writeFileAtomic`), so the hash of the library file is the hash of the source.
- `*warning` (optional) is `lutExpectationWarning`'s sentence or `""`. Failure (nothing written, `*err` says why): not a `.cube`, unreadable, empty, too large, not a valid cube (with the line), the library already holds `kLutMaxFiles`, or no free name up to `-99`.

**`inline bool deleteLutFile(const std::string &path)`**: true if the file was removed. (The app has no Delete button; this is for tests and tools.)

**`inline LutResolved acquireLut(const LutRef &ref)`**
- Turns a reference into a shared table. An empty `ref` returns `{}` (status `None`) without touching the cache. Otherwise: the cache is consulted by library folder and file stem (compared without case); on a miss the file is read and parsed **outside the lock, on the calling thread** (so the first use of a big table costs its parse time), and the result, **success or failure**, is cached. Two threads that miss at once both parse; the first to finish wins.
- `status`: `Ok` if `ref.hash` is 0 or equals the file's hash, `HashChanged` if not, `Missing` if there is no file, `Unreadable` if it does not parse. A failure is remembered, so a missing file costs one check, not one per slider frame.
- **`inline void clearLutCache()`**: empties the cache. Call it whenever the folder's content may have changed: after an import, when a picture is opened (a file may have been edited since). **`inline void lutCachePut(const std::string &name, std::shared_ptr<const Lut3D> lut, uint64_t hash)`**: puts a table in the cache by hand, so tests need no file.

### `minijson.hpp`
A tiny JSON value, writer and parser (UTF-8, no dependencies), plus base64 and byte run-length helpers. Everything is in `namespace mj`. Used for the edit sidecar and presets.

**Depends on:** standard library only. No UI. **Test:** `cases/minijson.inc`.

**Number text is locale-independent:** the C library's decimal separator is swapped for `.` on write and back on read, so a comma-decimal locale can neither corrupt a file nor make a valid one unreadable. Save files in binary mode (`dump()` uses `\n`), ideally with `writeFileAtomic`.

**`class Json`**

Constants: `Json::kMaxInputBytes` = 64 MiB (larger inputs are rejected before parsing: each value is about 140 bytes in memory).

`enum class Type { Null, Bool, Number, String, Array, Object }`. A default-constructed `Json` is Null.

**Building values** (static)
- `Json::flag(bool b)`; `Json::str(std::string s)`; `Json::array()`; `Json::object()`.
- `Json::num(double v)`: NaN and infinity become 0 (JSON has none), and `-0` becomes `+0`.
- `Json::num(float v)`: written with the **fewest digits that read back as the same float** (`0.3f` → `0.3`, not `0.300000012`).
- `Json::num(T v)` for any integer type except `bool`: converted to `double`. Values beyond 2^53 lose precision; store hashes or mtimes as strings.

**Inspecting**
- `type()`, `isNull()`, `isBool()`, `isNumber()`, `isString()`, `isArray()`, `isObject()`.
- `double asNum(double def) const`, `bool asFlag(bool def) const`: the value, or `def` if it is not that type. `const std::string &asStr() const`: the string (empty if not a string).
- `int asInt(int def, int lo = INT_MIN, int hi = INT_MAX) const` and `float asFloat(float def, float lo, float hi) const`: clamped, so safe to cast even for hostile values like 1e300.

**Arrays**
- `Json &push(Json v)`: appends (a Null becomes an array first; `assert` if it is some other type). Chainable.
- `const std::vector<Json> &items() const`; `const Json &at(size_t i) const` (throws `std::out_of_range`); `size_t size() const` (member count for an object, else item count).

**Objects** (insertion order is kept, so files diff cleanly)
- `Json &set(const std::string &key, Json v)`: adds or **replaces** (a Null becomes an object first). Chainable.
- `const Json *find(std::string_view key) const` and `Json *find(...)`: pointer to the member or null.
- `const Json &get(std::string_view key) const`: like `find` but returns a shared Null when missing, so lookups chain: `j.get("a").get("b").asNum(0)`.
- `keys()`, `values()`: parallel vectors, so unknown keys can be round-tripped.

**Text**
- `std::string dump() const`: pretty-printed with 2-space indent and a trailing `\n`. Arrays that hold only numbers/bools are packed 16 per line; other arrays and objects get one item per line. Integral numbers below 1e15 are written without a decimal point; others use the fewest digits (up to 17) that read back exactly.
- `static bool parse(std::string_view text, Json *out, std::string *err)`: returns true and sets `*out`, or returns false with `*err` (optional) saying where (`... at line L, column C (byte N)`; an input over the size limit just says `input too large`). Rules:
  - A leading UTF-8 BOM is skipped; input over 64 MiB is rejected; trailing characters are an error; nesting deeper than 64 is an error.
  - Strings must be valid UTF-8 (no overlongs, surrogates or code points above U+10FFFF); control characters are rejected; `\uXXXX` escapes with proper surrogate pairs are decoded; a lone surrogate or `\u0000` is an error.
  - Numbers are scanned leniently and then checked with `strtod` (must consume the whole token and be finite), so a few non-standard forms such as leading zeros are accepted.
  - A duplicate object key keeps the **last** value (at the first one's position).

**Base64** (RFC 4648, padded)
- **`inline std::string base64Encode(const std::vector<std::uint8_t> &d)`**.
- **`inline bool base64Decode(std::string_view s, std::vector<std::uint8_t> *out)`**: strict. Length must be a multiple of 4; `=` only at the very end (one or two); no whitespace; any other character fails. `*out` is cleared on failure.

**Run-length coding** (byte pairs: count 1..255, then value)
- **`inline std::vector<std::uint8_t> rleEncode(const std::vector<std::uint8_t> &in)`**: worst case (no repeated bytes) doubles the size.
- **`inline bool rleDecode(const std::vector<std::uint8_t> &in, std::size_t expected, std::vector<std::uint8_t> *out)`**: true only if the runs add up to exactly `expected` bytes. Fails on an odd length, a zero count, or a total that overshoots or falls short; `*out` is empty on failure. **Safe on untrusted input**: it refuses at once if `expected` exceeds what the stream could produce (`pairs × 255`), so it never allocates more than the data can fill, whatever `expected` claims.

### `file_utils.hpp`
Paths, extensions, folder listing, natural sort and atomic writes. Standard library only, so every function is tested against a temp directory. Paths are UTF-8 `std::string` at the edges of the program.

**Depends on:** standard library only. No UI. **Test:** `cases/file_utils.inc`.

**Functions**

**`inline std::filesystem::path fsPath(const std::string &utf8)`**: converts a UTF-8 string to a path without going through the system code page.

**`inline std::string u8ToString(const std::filesystem::path &p)`**: the way back, to UTF-8.

**`inline std::string baseName(const std::string &path)`**: the part after the last `/` or `\`; all of it if there is none.

**`inline std::string fileExtLower(const std::string &path)`**
- Returns: the extension after the last `.`, lower-cased (ASCII), without the dot. Empty if there is no dot or the last dot lies before the last slash.
- Gotcha: a name that is only an extension (`".jpg"`) returns `"jpg"`.

**`inline bool isJpegPath(const std::string &path)`**: true if the extension is `jpg` or `jpeg` (any case).

**`inline std::string filmLower(std::string s)`**: ASCII lower-case copy; the filmstrip's case-insensitive name key.

**`inline bool naturalLess(const std::string &a, const std::string &b)`**
- Returns: true if `a` sorts before `b` in natural order (`IMG_2` before `IMG_10`).
- Rules: runs of digits compare by numeric value (leading zeros ignored, one digit kept; a longer run is larger). Other characters compare lower-cased. If one string is a prefix of the other, the shorter is less.
- Not a total order on its own: `"a01"` and `"a1"` are equivalent, which `listImageFiles` resolves with a plain comparison.

**`inline bool readFilePrefix(const std::string &path, size_t maxBytes, std::vector<uint8_t> *out)`**
- Inputs: UTF-8 path, the most bytes to read, output buffer.
- Returns: true if the file opened and at least one byte was read; false if unreadable or empty. `*out` holds the bytes actually read (all of the file if smaller than `maxBytes`). Note it allocates `maxBytes` up front.

**`inline bool writeFileAtomic(const std::string &path, const std::vector<uint8_t> &bytes, std::string *errorOut)`**
- Inputs: UTF-8 target path, the bytes, and an optional (may be null) error string.
- Returns: true on success.
- Details: writes to `<path>.tmp`, then renames over the target, so an interrupted export never leaves a half-written picture over a good one. On failure the temp file is removed and `*errorOut` is set (`"couldn't write <name>"`, or the filesystem's message for a failed rename). It does not `fsync`.

**`inline std::vector<std::string> listImageFiles(const std::string &folder, size_t maxFiles)`**
- Inputs: folder path (UTF-8), cap on the number of names.
- Returns: file names (UTF-8, no directory part) of regular files ending in `.jpg`, `.jpeg` or `.png` (any case) that lie **directly** in `folder`, in natural order. Equal names under natural order are ordered by plain comparison, so the result never depends on directory order. A missing or unreadable folder gives `{}`; unreadable entries are skipped.
- Cap behavior (on purpose, pinned by a test): the cap is applied **before** sorting, so a folder over the cap yields an arbitrary (directory-order) subset, sorted.

### `virtual_copy.hpp`
**Virtual copies**: more than one set of edits for one picture file. Pure string and folder helpers: no UI, no editor state, no pixels. See [sidecar-format.md](sidecar-format.md#virtual-copies) for the design and what the UI does with it.

A copy is an extra sidecar (`photo.jpg.vc2.literoom.json`). The program identifies a picture by one string, its path, so a copy gets its own path by appending a marker no file name can contain: `/photos/photo.jpg|vc2`. Code keyed by path tells copies apart with no change; code that needs the real file calls `vcImageOf()`.

**Depends on:** `file_utils.hpp`. No UI. **Test:** `cases/virtual_copy.inc`. **Thread model:** no shared state; every function is safe from any thread.

**Constants:** `kSidecarExt` (`".literoom.json"`; moved here from `sidecar.hpp`, which includes this header), `kMaxVirtualCopies` (`int`, 9: copies are numbered 1 to 9, a single digit in the marker), `kVcMark` (`"|vc"`).

**Functions**

**`inline std::string vcPath(const std::string &imagePath, int n)`**: the path of copy `n`; `imagePath` itself for `n < 1`. Does not check `n` against `kMaxVirtualCopies` (such a path just isn't recognized by `vcSplit`).

**`inline bool vcSplit(const std::string &p, std::string *image, int *n)`**
- True if `p` is exactly `"<image>|vc<1..9>"`. Then `*image` is the real picture's path and `*n` the number (both outputs optional). False for everything else: no marker, a marker not at the very end, `|vc0`, `|vc10`, a non-digit, capital letters, an empty name before the marker (`"|vc1"`, `"dir/|vc1"`).

**`inline std::string vcImageOf(const std::string &p)`**: the real picture file behind a path; `p` itself if it isn't a copy's path. **`inline int vcCopyOf(const std::string &p)`**: 0 for a picture, else the copy number.

**`inline std::string vcSidecarFileOf(const std::string &p)`**: `"photo.jpg"` → `"photo.jpg.literoom.json"`, `"photo.jpg|vc2"` → `"photo.jpg.vc2.literoom.json"`. `sidecarFileOf` calls it.

**`inline bool vcParseSidecarName(const std::string &fileName, std::string *imageName, int *n)`**: the other direction. True if `fileName` (no directory) is `"<image>.vc<1..9>"` plus the sidecar extension (the extension in any case, `.vc` lower-case); gives the picture's file name and the copy number. A plain `photo.jpg.literoom.json` is false.

**`inline std::map<std::string, std::vector<int>> listVirtualCopies(const std::string &folder, size_t maxFiles)`**
- The copies found in `folder`: lower-case picture file name → copy numbers, ascending and unique. The picture need not exist (the caller matches names against the pictures it lists). Looks at no more than `maxFiles` folder entries (a cap on entries, not on matches); directories and unreadable entries are skipped; a missing folder gives `{}`. The filmstrip calls it with `kFilmMaxFiles` when it builds a strip.

**`inline int vcFreeId(const std::vector<int> &used)`**: the lowest number in `1..kMaxVirtualCopies` that isn't in `used`; 0 if all are taken. A deleted copy's number is reused.

### `image_codec.hpp`
Encoding a `CanvasImage` as PNG or JPEG: libpng and libjpeg on Linux, WIC on Windows. (Needs LiteUI because it reuses the libraries LiteUI already pulls in.) Everything that touches the editor (file pickers, state, status line) stays in `main.cpp`'s `exportImage`; `saveJpegFile` lives in `export_worker.hpp` (shared with the batch export).

**Depends on:** `liteui.hpp`, `color_tag.hpp` (included on both platforms; only the Windows PNG path calls it). **Test:** `cases/image_codec.inc`.

**Constants**

| Name | Type | Value | Meaning |
|---|---|---|---|
| `kJpegQualityDefault` | `int` | 92 | Default JPEG quality |
| `kJpegMaxSide` | `int` | 65500 | Largest side allowed (the format's limit is 65535). Enforced by `jpegSizeOk`, which `encodeJpegMemory` calls before anything else. PNG has no such limit |

**Functions**

**`inline bool encodePngFile(const std::string &path, const CanvasImage &img, std::string *errorOut)`**
- Inputs: output path, the picture (RGBA8, straight alpha), and an optional (may be null) error string.
- Returns: true if the file was written; otherwise false with `*errorOut` set.
- Details: 8-bit RGBA PNG, alpha kept. Linux uses libpng (a failure `longjmp`s to the cleanup path); Windows uses WIC, swaps R and B if the encoder substitutes BGRA, and fails on any other pixel format. **WIC always writes its own `gAMA` and `cHRM` chunks**, so after a successful encode the Windows version rewrites the file with `color_tag::insertColorIntoFile(path, color_tag::PngColor{})`, which strips every color chunk (best effort: the PNG is valid either way). libpng writes none, so on both platforms a fresh export is untagged, and `carryColorToPng` is what adds the source's tags (nothing is ever invented). The Linux version opens the file with `fopen(path.c_str())`.

**`inline bool jpegSizeOk(int w, int h)`**: true when both sides are at least 1 and at most `kJpegMaxSide` (JPEG stores each side in 16 bits). `encodeJpegMemory` uses it as its first step; a caller can use it to explain the refusal before encoding.

**`inline void flattenRow(const CanvasImage &img, int y, uint8_t *dst, bool bgr)`**
- Inputs: the picture, row index `y` (not range-checked), destination for `width × 3` bytes, and the channel order (`true` = BGR for WIC, `false` = RGB for libjpeg).
- Output: that row as packed 8-bit color, with alpha composited **over white**: `(c·a + 255·(255 − a) + 127) / 255`; fully opaque pixels are copied exactly.

**`inline bool jpegFullChroma(int quality)`**: true when `quality >= 90`, meaning 4:4:4 (no chroma subsampling), to keep colored edges crisp.

**`inline bool encodeJpegMemory(const CanvasImage &img, int quality, std::vector<uint8_t> *out, std::string *errorOut)`**
- Inputs: the picture, quality (about 1..100; libjpeg clamps it, WIC uses `quality/100`), the output buffer, and an optional error string.
- Returns: true on success, with the whole JPEG in `*out`; otherwise false and `*errorOut` is set.
- Details: encoded **to memory** so the source photo's EXIF and ICC can be spliced in before the file is written (`embedSourceMetadata`, then `writeFileAtomic`). Alpha is flattened onto white. Linux installs a custom libjpeg error handler (`JpegEncErr`, `jpegEncErrExit`) that `longjmp`s back instead of letting libjpeg `exit()` the app. Windows writes in chunks of 64 rows and asks for 4:4:4 as a best-effort property. The size is checked first, on both platforms: a picture that fails `jpegSizeOk` (empty, negative, or a side over `kJpegMaxSide`) is refused with the error "JPEG can't hold a picture this size", before any pixel is read and with `*out` untouched. So no caller can skip the limit.

### `image_codec16.hpp`
Reading and writing 16-bit pictures (`Image16`): libpng on Linux, WIC on Windows, the libraries `liteui.hpp` already pulls in (which is why it needs `liteui.hpp`). LiteUI's decoders reduce every PNG to 8 bits (libpng `png_set_strip_16`; WIC `32bppRGBA`), so a 16-bit PNG needs its own read path; that is all this adds. TIFF and RAW would be more branches of `decodeImage16` (they need libtiff / LibRaw, which Literoom does not link yet).

A translation unit that includes it must define `LITEUI_IMPLEMENTATION` before `liteui.hpp`, like `image_codec.hpp`'s users. `widenImage` / `narrowImage` live in `image16_convert.hpp`, which needs no platform code.

**Depends on:** `image16.hpp`, `image16_convert.hpp`, `color_tag.hpp`, `file_utils.hpp` (`fsPath`), `liteui.hpp` (`toWide`, libpng / WIC). **Test:** `cases/image_codec16.inc` (the PNG test vectors are embedded in the test file). **Platform note:** both halves are built and run (the Windows WIC half is exercised by the same test under MSVC); the WIC code follows `decodeFile` / `encodePngFile` line for line with `64bppRGBA` in place of `32bppRGBA`.

**Types**

- `struct PngHeader { uint32_t width = 0, height = 0; int bitDepth = 0; int colorType = 0; }`: `bitDepth` is 1, 2, 4, 8 or 16 (0 = not a PNG / unreadable); `colorType` is 0 gray, 2 RGB, 3 palette, 4 gray + alpha, 6 RGBA. `bool ok() const` is `bitDepth != 0`.

**Functions**

**`inline PngHeader parsePngHeader(const uint8_t *d, size_t n)`**: the IHDR of a PNG held in memory (at least its first 33 bytes). A default `PngHeader` (`ok()` false) for null `d`, `n < 33`, a wrong signature, an IHDR that is not 13 bytes or not first, a zero width or height, or a bit depth / color type outside the PNG set. It does **not** check that the depth fits the color type.

**`inline PngHeader readPngHeader(const std::string &path)`**: reads the first 33 bytes of the file at `path` (UTF-8, through `fsPath`) and parses them. `ok()` false if it can't be opened or isn't a PNG.

**`inline bool hostIsLittleEndian()`**: PNG samples are big-endian, so on a little-endian machine (every machine Literoom runs on) the 16-bit samples are byte-swapped with `png_set_swap` on read and write.

**`inline uint32_t pngCrcUpdate(uint32_t crc, const uint8_t *d, size_t n)`**: CRC-32 as PNG uses it (polynomial `0xEDB88320`), continued across calls. Start from `0xFFFFFFFF` and xor the final value with `0xFFFFFFFF`.

**`inline bool validatePngChunks(const std::string &path, std::string *errorOut)`**
- Walks every chunk and checks the file is whole: signature, IHDR first, each chunk's data and CRC present and correct, at least one IDAT, and a closing IEND. Errors: *"failed to open file"*, *"not a valid PNG file"*, *"PNG is truncated"*, *"PNG chunk length is invalid"*, *"PNG has no IHDR first"*, *"PNG is corrupt (chunk CRC mismatch)"*, *"PNG has no image data"*.
- Why: libpng enforces this itself; WIC does not (a cut-off or corrupted PNG can decode "fine" with zeroed rows), so the Windows decoder calls it first. Reads in 64 KiB pieces.

**`inline std::optional<Image16> decodePng16File(const std::string &path, std::string *errorOut)`**
- Reads a **16-bit** PNG of any color type at full precision as straight-alpha RGBA. Returns `nullopt` with `*errorOut` set otherwise.
- Linux (libpng): setup is liteui's `decodePngFile` (gray expanded to RGB, tRNS to alpha, an opaque alpha added when there is none; the palette branch is carried over but never taken, since PNG limits palettes to 8 bits), minus `png_set_strip_16`, plus `png_set_swap`. A file that is not 16-bit gives *"not a 16-bit PNG"* (so does a zero size or one beyond `0x7FFFFFFF / 8` wide); other errors: *"failed to open file"*, *"not a valid PNG file"*, *"libpng init failed"*, *"libpng decode error"*, *"unexpected PNG layout"*. The `Image16` and row table are declared **before** `setjmp` on purpose: libpng's error `longjmp` skips destructors of anything created after it, which would leak a whole picture. The path is opened with `fopen(path.c_str())`.
- Windows (WIC): converts frame 0 to `64bppRGBA` with no dither; error *"WIC: 16-bit decode failed"* (and the ones of `CoCreateInstance` / open).

**`inline bool encodePng16File(const std::string &path, const Image16 &img, std::string *errorOut)`**
- Writes `img` as an RGBA PNG, 16 bits per channel, straight alpha, no interlace. Returns false with *"nothing to write"* for an empty picture or one whose `pixels` is shorter than `pixelCount() * 4`. Linux errors: *"failed to open file for writing"*, *"libpng: failed to create write struct"*, *"libpng: error during write"*; Windows: *"WIC: 16-bit PNG encode failed"* (it fails if the encoder wants a pixel format other than `64bppRGBA`; unlike `encodePngFile` there is no BGRA swap).
- Writes straight to `path`, not atomically (use `writeFileAtomic` on a buffer if a caller needs that). On Windows, WIC's own `gAMA` / `cHRM` chunks are then stripped by rewriting the file through `color_tag::insertColorIntoFile(path, PngColor{})` (best effort), the same as in `encodePngFile`, so a 16-bit export is untagged until `carryColorToPng` adds the source's tags.

**`inline std::optional<Image16> decodeImage16(const std::string &path, std::string *errorOut = nullptr)`**
- The one entry point: a 16-bit PNG (`readPngHeader` says depth 16) goes through `decodePng16File` at full precision; anything else (JPEG, 8-bit PNG, ...) goes through `liteui_image::decodeFile` and is widened with `widenImage` (`v * 257`), so the 16-bit pipeline starts from **exactly** the pixels the editor shows for it. `nullopt` with `*errorOut` on failure.

### `image_source.hpp`
The one place that knows how a picture file is opened. The editor's decode job, the batch export and the sidecar-size read each used to decide for themselves whether a file is a 16-bit PNG; now they ask here, so a new kind of source (TIFF, later RAW) is one more branch of `depthOf` / `openUpright`, not three edits that have to agree. The orientation is passed in (`exifOrientationOf(path)` for now), so the header does not depend on `image_meta.hpp` and the filmstrip worker keeps injecting its own. A translation unit that includes it needs `LITEUI_IMPLEMENTATION` defined before `liteui.hpp` (it includes `image_codec16.hpp`).

**Depends on:** `liteui.hpp`, `file_utils.hpp` (`fsPath`), `geometry.hpp` (`Orient`), `image16.hpp`, `image16_convert.hpp`, `raster_ops.hpp` (`orientImage`), `image_codec16.hpp`. **Test:** `cases/image_source.inc` (real files).

**Types**
- `enum class Depth { Narrow, Wide }`: 8 bits per channel (the 8-bit chain is the whole picture) or more (an `Image16` twin carries the precision).
- `struct Opened { std::shared_ptr<CanvasImage> image; std::shared_ptr<Image16> wide; }`: `image` is the 8-bit form, upright (null only for a Wide file opened with `want8 = false`); `wide` is set for a Wide file only.

**Functions**
- `bool pngIsTruncated(const std::string &path)`: true for a file that starts with the PNG signature but does **not** end with its `IEND` chunk (a failed copy or download). Why: libpng refuses such a file, but WIC (Windows) hands back a picture with the missing rows blank, which would be edited and exported as if whole, so every PNG is checked here, the same way on both platforms. Only the last 4096 bytes are searched, for `IEND` plus its CRC, so bytes after `IEND` are tolerated. A file that is not a PNG, or can't be read, is **not** truncated as far as this check can tell (the decoder reports what is wrong with it). `openUpright`, `uprightSize` and `decodeForThumb` all call it first and fail with `kTruncatedMessage`.
- `kTruncatedMessage`: `"the PNG file is incomplete (it ends before its last chunk)"`, the `*errorOut` text of those failures.
- `Depth depthOf(const std::string &realPath)`: from the file's header, not its extension. Today only a PNG whose `readPngHeader` says 16 bits is Wide. Unreadable or unrecognized files are Narrow (the 8-bit decoder then reports what is wrong).
- `std::optional<Opened> openUpright(const std::string &realPath, const Orient &orient, bool want8, std::string *errorOut = nullptr)`: a Wide file is read at full precision (`decodeImage16`) and turned upright into `wide`; with `want8` its 8-bit form (`narrowImage`) is made too. A Narrow file gives `image` only. **A Wide file whose full-precision read fails is retried through the 8-bit decoder** (which says what is wrong if it fails as well), for the editor and the export alike, so the two always agree on what a file is. `nullopt` with `*errorOut` set when nothing could be read. `realPath` is a virtual copy's *picture file* (`vcImageOf`), not the copy's own path.
- `bool uprightSize(const std::string &realPath, const Orient &orient, int *w, int *h, std::string *errorOut = nullptr)`: the size once upright (a quarter turn swaps the sides). It decodes the file today; the place for a header-only read for JPEG, TIFF or RAW.
- `std::optional<CanvasImage> decodeForThumb(const std::string &realPath, std::string *errorOut = nullptr)`: what a thumbnail is made from: always 8-bit (a Wide file's plain 8-bit decode), **not** turned (`ThumbWorker` turns it with its own orientation function).

**Callers:** `runDecodeJob` (`main.cpp`: `want8 = true`), `exportPicture` (`export_worker.hpp`: `want8 = false`, only the twin that is needed is kept alive), `runSyncJob` (`uprightSize`), `ThumbWorker` (`decodeForThumb`).

### `exif.hpp`
JPEG metadata with no dependencies: the EXIF orientation tag, carrying a source JPEG's EXIF block and ICC profile into a re-encoded one, and the camera data the Info pane shows (make, model, lens, ISO, shutter, aperture, focal length, date taken) with the functions that turn it into display text. Everything is in `namespace exif`. Every read is bounds-checked, so a damaged or hostile file yields "no metadata" rather than reading out of range.

**Depends on:** standard library only. No UI. **Test:** `cases/exif.inc`.

**What happens on export**
- The orientation is reset to upright (the pixels already are), the embedded thumbnail is erased (its data zeroed, not just unlinked), and the picture size fields are updated.
- **Not carried over:** XMP and IPTC (captions, copyright fields and any develop settings stored in XMP are dropped), and thumbnails stored anywhere other than IFD1 (e.g. inside a MakerNote).
- **Carried over:** GPS data inside the EXIF block, unless `stripGps` is called on the `JpegMeta` before `embed` (Export's "Remove location (GPS)"). Only the standard GPS IFD is handled: location a camera hides inside its own MakerNote is not touched.
- Embedding the source ICC profile is only right if the pixels are still in that profile's color space.

**Two readers, on purpose.** `readJpegMeta` returns an EXIF block **rewritten** for an upright re-encode (orientation 1, thumbnail erased), which is right for export and wrong for showing the file's own data. `readPhotoInfo` reads the first EXIF APP1 of the file as it is and never produces a block to write back, so the two never share a buffer.

**Types**
- `struct Transform { int rot = 0; bool flip = false; }`: how to turn a picture upright: mirror left-right first (if `flip`), then `rot` clockwise quarter turns. Same convention as `Orient` in `geometry.hpp`.
- `struct JpegMeta`

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `orientation` | `int` | 1 | The stored EXIF orientation, 1..8 (1 if absent or invalid) |
  | `exif` | `std::vector<uint8_t>` | empty | The EXIF APP1 payload (`"Exif\0\0"` + TIFF data), **already rewritten** for an upright re-encode: orientation 1, thumbnail erased and unlinked. Empty if the file had none |
  | `icc` | `std::vector<std::vector<uint8_t>>` | empty | The ICC APP2 payloads (`"ICC_PROFILE\0"` + chunk header + data), verbatim, in sequence order. Empty if none, or if the chunks were incomplete, inconsistent or over 16 MiB (a broken profile is worse than none) |
- `struct PhotoInfo`: what the Info pane shows. Every field is optional: text is empty and numbers are 0 when the file does not say, or says something implausible.

  | Field | Type | Meaning |
  |---|---|---|
  | `make` | `std::string` | IFD0 Make (0x010F), e.g. `Canon` |
  | `model` | `std::string` | IFD0 Model (0x0110), e.g. `Canon EOS R5` |
  | `lens` | `std::string` | LensModel (Exif 0xA434); empty if not recorded |
  | `dateTaken` | `std::string` | `YYYY-MM-DD HH:MM:SS` from DateTimeOriginal (0x9003), else DateTimeDigitized (0x9004); empty for a missing, unset (`0000:00:00 ...`) or invalid date |
  | `iso` | `int` | 1..`kInfoMaxIso`, else 0. From PhotographicSensitivity (0x8827); when that is missing, 0 or 65535 (the "see ISOSpeed" escape) from ISOSpeed (0x8833, a SHORT or LONG) |
  | `exposureTime` | `double` | Shutter in seconds, (0, 86400], else 0 |
  | `fNumber` | `double` | (0, 1000], else 0 |
  | `focalLength` | `double` | Millimetres, (0, 10000], else 0 |

  `bool empty() const` is true when every field is unset. **GPS is deliberately not read:** the Info pane never shows a location.
- Constants: `kInfoMaxText` (`size_t`, 128): text fields are cut to this many characters; `kInfoMaxIso` (`int`, 10,000,000): larger ISO values are not believed.

**Functions**

**`inline Transform orientationToTransform(int o)`**
- Input: EXIF orientation value.
- Returns: the upright transform. Any value outside 2..8 gives `{0, false}`.

  | Value | Meaning | `{rot, flip}` |
  |---|---|---|
  | 1 | normal | `{0, false}` |
  | 2 | mirror | `{0, true}` |
  | 3 | turn 180 | `{2, false}` |
  | 4 | mirror vertically | `{2, true}` |
  | 5 | transpose | `{3, true}` |
  | 6 | turn 90 cw | `{1, false}` |
  | 7 | transverse | `{1, true}` |
  | 8 | turn 270 cw | `{3, false}` |

**`inline bool readJpegMeta(const uint8_t *data, size_t size, JpegMeta *out)`**
- Inputs: bytes of a JPEG (a prefix is fine; only the header up to the first scan is read), its length, and the output (always reset first).
- Returns: false if it isn't a JPEG (null, under 4 bytes, or no `FF D8` start); true otherwise, **even when there is no metadata**.
- Details: walks the marker segments until SOS or EOI, stopping quietly if it loses sync or hits a bad length. Uses the **first EXIF APP1 whose TIFF header is valid** (an earlier malformed one is skipped, not fatal; `readPhotoInfo` takes the first EXIF APP1 as it is). ICC chunks are collected and validated (all agree on the count, sequence numbers exactly 1..count once each). `out->exif` is only set if the TIFF header (byte order, magic 42, IFD0 offset ≥ 8) is valid.

**`inline std::vector<uint8_t> embed(const std::vector<uint8_t> &jpeg, const JpegMeta &meta, int w, int h)`**
- Inputs: a freshly encoded JPEG, the metadata from `readJpegMeta`, and the new picture size.
- Returns: a copy of `jpeg` with the EXIF segment and ICC segments inserted right after SOI (after a leading APP0 if the file starts with one, as JFIF requires). A buffer that isn't a JPEG is returned unchanged.
- Details: the EXIF segment is left out if `meta.exif` is malformed or `w`/`h` aren't positive (its size fields would be stale). Size fields written: IFD0 ImageWidth/ImageLength and the EXIF sub-IFD's PixelXDimension/PixelYDimension, where present, each rewritten as a LONG. Invalid ICC chunks and any segment over 65533 bytes of payload are skipped.

**`inline bool stripGps(JpegMeta *meta)`**
- Removes the GPS location from `meta->exif` and leaves everything else in it alone (size, orientation, Make, the Exif sub-IFD and so on). Returns true if there was a GPS block; false, with nothing changed, for an empty or malformed EXIF block or one without GPS (so calling it twice is harmless).
- Details: it works on a copy of the TIFF part and copies it back. The GPS IFD that IFD0's GPSInfo tag (0x8825) points to is **zeroed**, including every value it keeps outside its entries (the latitude and longitude rationals), and the tag is then cut out of IFD0 with the entries after it, and the next-IFD pointer, moving up by 12 bytes. The block keeps its size. Dropping only the tag would leave the coordinates in the file. A GPSInfo tag that points out of range or has the wrong type is still removed, and nothing is read or written outside the block.
- Use: call it between `readJpegMeta` and `embed` (`embedSourceMetadata` does this for you with `removeGps`).

**`inline bool readPhotoInfo(const uint8_t *data, size_t size, PhotoInfo *out)`**
- Inputs: bytes of a JPEG (a prefix is fine), its length, and the output (always reset first).
- Returns: false if it isn't a JPEG; true otherwise, **even when there is no EXIF or none of the fields** (check `out->empty()`).
- Details: finds the first EXIF APP1 with the same marker walk as `readJpegMeta` and reads Make and Model from IFD0, the rest from the Exif sub-IFD (the pointer must be a LONG, count 1, offset ≥ 8; without one only Make and Model are read). Works on a copy of the TIFF part. Every read is bounds-checked and every value range-checked (see the table above), so a damaged or hostile file gives empty fields, never an out-of-range read or a value the UI could not show.
- **Text is made safe to show:** reading stops at the first NUL; spaces, tabs and line breaks become single spaces (none at either end, which removes the usual space padding); other control characters are dropped; anything outside 7-bit ASCII becomes `?`; at most 128 characters. Lens or camera names with accented letters therefore show `?` for them.

**Display text.** All return an empty string for "unknown", so the caller can hide the row. Plain ASCII, no locale.
- `describeShutter(double seconds)`: `1/250 s` when the time is within 3 % of 1/N (N ≥ 2), `0.4 s` otherwise, `2.5 s` / `30 s` from one second up. Empty for 0, negative, NaN, infinite or under a microsecond.
- `describeAperture(double f)`: `f/2.8`, `f/11`. `describeFocalLength(double mm)`: `85 mm`, `24.5 mm`. `describeIso(int iso)`: `ISO 400`.
- `describeExposure(const PhotoInfo &)`: `1/250 s, f/2.8, ISO 400`, leaving out the parts that are unknown.
- `describeCamera(const PhotoInfo &)`: the model alone when it begins with the make's first word, ignoring case (`Canon` + `Canon EOS R5` gives `Canon EOS R5`, `NIKON CORPORATION` + `NIKON D850` gives `NIKON D850`), otherwise make and model joined (`SONY ILCE-7M3`). Either may be missing.

**`namespace exif::detail`** (internal; listed so you can find things)
- `kMaxIccBytes` (`size_t`, 16 MiB).
- `struct Tiff`: a bounds-checked, endian-aware view over a TIFF block with `ok`, `rd16`, `rd32`, `wr16`, `wr32`, `zero`. Every read or write that would fall outside the block fails or is skipped.
- `findEntry(t, ifd, tag, &entry, &count, &end)`: finds a tag in an IFD; `count` and `end` are filled whenever the IFD is readable, even if the tag is absent.
- `tiffHeader(t, &ifd0)`: reads byte order and IFD0's offset (rejects offsets under 8).
- `dropThumbnail(t, nextIfdPtr)`: zeroes the IFD1 thumbnail's data and IFD, then unlinks IFD1.
- `resetOrientation(t, ifd0)`: sets tag 0x0112 to 1 (only if SHORT, count 1), drops the thumbnail, and returns the original value.
- `setLong(t, ifd, tag, value)` and `setDimensions(t, ifd0, w, h)`: rewrite size tags.
- `tiffTypeSize(type)`: bytes per value for a TIFF type (0 for an unknown one).
- `dropGps(t, ifd0)`: the work behind `stripGps` (see there). Everything it needs is read before anything is zeroed.
- `isExifPayload`, `isIccPayload`: header checks. `normalizeIcc(icc)`: validates and orders the chunks.
- For `readPhotoInfo`: `findExifSegment` (the first EXIF APP1 payload, pointing into the input), `cleanText` (the text rules above), `valueOffset` (where an entry's values live: inline if they fit in 4 bytes, else at the stored offset, range-checked), `readText`, `readRational` (zero denominators rejected), `readFirstUInt` (SHORT or LONG), the one-call `textTag` / `rationalTag` / `uintTag`, `normalizeDate`, and `trimmedFixed` (fixed-point text without trailing zeros).

### `image_meta.hpp`
Glue between `file_utils.hpp`, `virtual_copy.hpp`, `exif.hpp` and `geometry.hpp`: reads a file's camera rotation as an `Orient`, reads its camera data for the Info pane, and copies metadata into an export. No UI, no editor state, so all three functions are tested with synthetic JPEG files in a temp folder.

**Depends on:** `exif.hpp`, `file_utils.hpp`, `virtual_copy.hpp`, `geometry.hpp`. **Test:** `cases/image_meta.inc`.

**Constants:** `kMetaPrefixBytes` (`size_t`, 8 MiB): how much of the source file is searched for its EXIF/ICC segments when exporting. `kInfoPrefixBytes` (`size_t`, 256 KiB): how much is searched for the Info pane's camera data. (`exifOrientationOf` also looks at only the first 256 KiB.)

**Functions**

**`inline Orient exifOrientationOf(const std::string &path)`**
- Input: UTF-8 path of a picture file.
- Returns: the `Orient` that makes the picture upright. Identity for anything that isn't a `.jpg`/`.jpeg`, can't be read, isn't a JPEG, or has no valid orientation tag.
- Why: decoders hand back the stored pixels, so a portrait shot would otherwise open on its side. It is applied **once**, as the file is opened: the "original" the editor (and its saved edits and masks) work on is the upright picture.

**`inline bool embedSourceMetadata(std::vector<uint8_t> *jpeg, const std::string &sourcePath, int width, int height, bool removeGps = false, bool iccOnly = false)`**
- Inputs: `jpeg` (a freshly encoded JPEG, modified in place), the source photo's path, the new picture size (the exported size, so after a resize it is the smaller one), `removeGps` (default false) and `iccOnly` (default false).
- Returns: true when the source is a `.jpg`/`.jpeg` whose header could be read (i.e. `exif::embed` ran). `*jpeg` is then rewritten, and stays byte-identical if the source had no EXIF or ICC data. Returns false with `*jpeg` untouched for a non-JPEG, missing or empty source, or one that doesn't start like a JPEG.
- Effects: orientation reset to upright, thumbnail erased, size fields updated (see `exif::embed`). GPS is carried over unless `removeGps` is true, in which case it is cut out first (`exif::stripGps`); the result is otherwise byte-identical to the default, and a source without a GPS block gives the same bytes either way. The source file is only read, never changed.
- **`iccOnly`**: the EXIF block is left out altogether (no EXIF segment is written) and only the ICC profile is carried, so `removeGps` then has nothing to do. The profile says how the picture's numbers are to be read, so even an export without metadata must keep it (a Display P3 picture without its profile looks dull everywhere). The return value and the "byte-identical if the source had no ICC data" rule are the same as above.

**`inline bool photoInfoOf(const std::string &path, exif::PhotoInfo *out)`**
- Input: UTF-8 path of a picture or of a virtual copy (`photo.jpg|vc2`: the data is read from the real file behind it through `vcImageOf`, since a copy has no file of its own), and the output (always reset first).
- Returns: true when the file is a `.jpg`/`.jpeg` that could be read and starts like a JPEG; `*out` is then filled in and may still be `empty()` if the file has no EXIF. False, with `*out` empty, for another extension, a missing or empty file, or one that is not a JPEG. Only the first 256 KiB are searched; GPS is never read.
- Use: the Info pane calls it once per opened picture (see `currentPhotoInfo()` in `main.cpp`), on the UI thread: it is one small read and touches neither the render pipeline nor any worker.

---

## Background work

### `latest_worker.hpp`
`LatestWorker<Job, Value>`: one background thread that runs "latest wins" jobs. It replaces the hand-written render, geometry, decode and segment workers, which all implemented the same protocol. The Sync Settings batch also runs on one, though it never cancels it (see [threading.md](threading.md#the-sync-worker-is-a-batch-that-is-never-superseded)). See [threading.md](threading.md). No UI.

**Depends on:** standard library only (`std::jthread`). **Test:** `cases/latest_worker.inc` (timing-sensitive: run it in a loop to shake out flakiness).

**The protocol**
- Every `submit()` and `invalidate()` bumps a generation number. Only the newest request can win: a newer submit replaces a job still waiting, and a running job polls the number to abandon itself.
- A finished value is parked under a lock and re-checked against the newest generation, so a result invalidated while it finished is never delivered.
- The UI thread is woken by the notify callback **outside** the lock, so it can't deadlock on the worker's mutex.
- **Thread model:** every public member is called from **one** thread (the UI thread). The worker thread, started lazily by the first `submit()`, only runs the job function and touches the parked slot.

**Types**
- `struct WorkerCancel`
  - `std::atomic<uint64_t> *latest` and `uint64_t gen`: the raw atomic and this job's generation, so existing cancellable routines (`renderWith`, `rotateArbitrary`, `segmentObjectWand`) keep their `(const std::atomic<uint64_t>* latest, uint64_t gen)` parameters. Pass `c.latest, c.gen`.
  - `bool cancelled() const`: true once a newer `submit()`/`invalidate()`/`shutdown()` has superseded this job.
- `template <class Job, class Value> class LatestWorker`, with aliases `JobType`, `Result`, `Cancel` (= `WorkerCancel`), and
  `using JobFn = std::function<std::optional<Value>(Job &&, Cancel)>`: runs on the worker thread. Return `std::nullopt` to say "abandoned" (nothing is parked or notified). A job that can't be cancelled simply always returns a value (a failure value, if need be).

**Members**

**`explicit LatestWorker(JobFn fn)`**: stores the job function. No thread yet. Non-copyable.

**`~LatestWorker()`**: marks shutdown so a running job bails at its next poll; the `std::jthread` (declared last, so destroyed first) requests stop and joins.

**`void setNotify(std::function<void()> fn)`**: callback run **on the worker thread** each time a result has been parked. Call it **before the first `submit()`**; the thread is created by `submit`, so it then sees the value without extra synchronization.

**`void shutdown()`**: stops the worker and joins it; afterwards the notify callback never runs again. Blocks until a running job that ignores `Cancel` has finished.

**`void submit(Job job)`**: starts the thread if needed, queues `job`, and supersedes any earlier one (waiting or running). Sets `outstanding()` to true.

**`void invalidate()`**: forgets whatever is waiting or running; its result will never be taken. Sets `outstanding()` to false.

**`bool outstanding() const`**: true from `submit()` until its result has been taken or `invalidate()`.

**`std::optional<Value> collect()`**
- Non-blocking. Returns the finished result of the newest request if it is ready (and clears `outstanding()`); otherwise `std::nullopt`. Always `nullopt` when nothing is outstanding. A parked result from an older generation is discarded.

**`Value waitForLatest()`**
- Blocking. Waits for the newest request to finish and returns its value. **Only call while `outstanding()`**: after `invalidate()` nothing would ever wake it.

### `thumb_worker.hpp`
The filmstrip's thumbnails, made off the UI thread: `makeThumb` box-averages a picture to fit 96 × 72, and `ThumbWorker` decodes a queue of files, thumbnails them, and reads each one's sidecar for whether it holds real edits and for its rating, flag and color label. A first-in-first-out queue with **no generations**, so **not** a `LatestWorker`. (Needs LiteUI's decoder.)

**Depends on:** `liteui.hpp` (`CanvasImage`), `image_source.hpp` (`decodeForThumb`), `geometry.hpp`, `raster_ops.hpp` (`orientImage`), `file_utils.hpp` (`fsPath`), `sidecar.hpp` (`sidecarFileOf`, `sidecarScan`), `photo_meta.hpp`, `virtual_copy.hpp` (`vcImageOf`, `vcCopyOf`). **Test:** `cases/thumb_worker.inc`.

**Constants:** `kFilmThumbW` = 96, `kFilmThumbH` = 72 (`int`). `kThumbMaxSidecarBytes` (`std::streamoff`) = 64 MiB: the largest sidecar the worker reads, the same limit the editor puts on one.

**Functions**

**`inline bool readSidecarForThumb(const std::string &imagePath, std::string *text, bool *exists)`**: the sidecar text of `imagePath` (through `sidecarFileOf`, so a virtual copy reads its own file). `*exists` says whether the file was there at all. Returns false if there is no file, it is empty, it is over `kThumbMaxSidecarBytes` or it can't be read (`*text` is then not filled); the worker counts an existing but unreadable sidecar as edited.

**`inline std::shared_ptr<const CanvasImage> makeThumb(const CanvasImage &src)`**
- Returns: a box-averaged copy that fits 96 × 72 (aspect kept, **never enlarged**; each side at least 1). Null for an empty picture or one whose pixel buffer is smaller than `width × height × 4`. All four channels, alpha included, are averaged with rounding.

**`class ThumbWorker`** (non-copyable)

- `struct Result { std::string path; std::shared_ptr<const CanvasImage> thumb; bool thumbSkipped = false; bool edited = false; PhotoMeta meta; }`: `thumb` is **null if the file couldn't be decoded** (the result is still delivered, with its `meta`); `thumbSkipped` is true for a **virtual copy** (its path has the `|vc<n>` marker), which is read for its sidecar only: no decode, `thumb` null on purpose, and the caller must not count it as a failure; `edited` is true if the picture's sidecar holds real edits (a sidecar that exists only because the picture was rated gives false; one that is unreadable, too big (over 64 MiB) or has no `edits` still gives true); `meta` is the rating, flag and color label from that sidecar (the default if there is none or it can't be read).
- `using OrientFn = std::function<Orient(const std::string &)>`: says how a file's camera rotation turns it upright. The app passes its own EXIF reader (`exifOrientationOf`), so thumbnails are turned exactly like the editor turns the picture.
- **`explicit ThumbWorker(OrientFn orientOf)`**: no thread yet.
- **`~ThumbWorker()`**: calls `shutdown()`.
- **`void setNotify(std::function<void()> fn)`**: called **on the worker thread** after each result is stored. Set it before the first `setQueue()`.
- **`void setQueue(std::vector<std::string> paths)`**: replaces what is still to do (starting the thread on first use). The file being processed finishes; results already made stay until collected. Call it again whenever the open picture changes so the work starts next to it.
- **`std::vector<Result> collect()`**: returns everything made since the last call and empties the list. Thread-safe, non-blocking.
- **`void shutdown()`**: clears the queue, requests stop and joins the thread.
- Per file, the worker: decodes it (`image_source::decodeForThumb` on `vcImageOf(path)`; skipped for a virtual copy), applies the orientation from `orientOf` (also given the real file's path), makes the thumbnail, then reads the sidecar (`readSidecarForThumb` through `sidecarFileOf`, so a copy reads its own file; at most 64 MiB) and runs `sidecarScan` on it.

---

### `export_render.hpp`
The pixel half of exporting one picture: from the decoded **original** to the rendered picture the encoders write. A pure function of its inputs (no files, no editor state, no sidecar), so it is tested without any of those; `exportPicture` (`export_worker.hpp`) does the file work around it. (Needs LiteUI for `CanvasImage`.)

The chain is the geometry worker's: orient -> straighten -> crop, from the untouched original, then the render. The original is either 8-bit (a JPEG, an 8-bit PNG: `CanvasImage`) or 16-bit (a 16-bit PNG: `Image16`), and the picture stays in that type until it is rendered. The result is 8-bit (JPEG / 8-bit PNG) or, when `want16` is set, 16-bit (16-bit PNG). An 8-bit original can be exported as 16-bit too: the float pipeline then keeps its extra precision, so a heavy edit does not show steps in smooth gradients.

**Depends on:** `geometry.hpp`, `image16.hpp`, `raster_ops.hpp` (the `CanvasImage` and `Image16` forms of `orientImage`, `rotateArbitrary`, `cropImage`, `resizeToLongEdge`), `render_params.hpp`, `render_core.hpp`, `liteui.hpp`. **Test:** `cases/export_render.inc`.

**Types**

- `struct ExportPixels { std::shared_ptr<CanvasImage> img8; std::shared_ptr<Image16> img16; }`: what the encoders get; **exactly one** is set.
  - `bool is16() const`: `img16 != nullptr`. `int width() const` / `int height() const`: of whichever is set, 0 if neither.
  - `void resizeToLongEdge(int longEdge)`: scales whichever is set so the long side is `longEdge` (`fitLongEdge` rules: never enlarges, `longEdge < 1` changes nothing).
- `struct ExportGeometry { Orient orient; float angle = 0; bool expand = false; CropRect crop; }`: the geometry edits of one picture (`AdjustmentSet::orient` / `angle` / `expand` / `crop`). `crop` is on the oriented, straightened picture, as everywhere.

**Functions**

**`inline bool renderForExport(std::shared_ptr<CanvasImage> original, const ExportGeometry &g, const RenderParams &rp, bool want16, const std::atomic<uint64_t> *latest, uint64_t gen, ExportPixels *out)`**: renders an 8-bit original. `want16` false gives `out->img8` via `renderWith`; true gives `out->img16` via `renderToImage16(source, rp, ...)` (the source is its own 8-bit view).

**`inline bool renderForExport(std::shared_ptr<Image16> original, const ExportGeometry &g, const RenderParams &rp, bool want16, const std::atomic<uint64_t> *latest, uint64_t gen, ExportPixels *out)`**: renders a 16-bit original. `want16` false gives `out->img8` via `renderToDisplay`; true gives `out->img16` via `renderToImage16`. Both pass a **null** `view8`, so if a neighbor effect is in use a temporary 8-bit view is made once for this render (fine for a single export; it is only slow for repeated renders).

- Both overloads: `latest` / `gen` are the usual cancellation pair (`latest` may be null). Returns **false when superseded**, or when the render produced nothing; `*out` then holds no picture (it is reset to empty once the geometry is built, and left untouched if the geometry itself was superseded). True means exactly one of `out->img8` / `out->img16` is set. `out` must not be null.
- The caller's `original` is taken by value and **moved** in, never modified; when the geometry does nothing (identity orientation, angle 0, empty crop) the render reads the original directly, with no copy.

**`export_render_detail::buildSource`** (template, internal): orient -> straighten (`rotateArbitrary`, only when `angle != 0`) -> crop (only when `crop` is not empty). Each intermediate is released as soon as the next step has what it needs, to keep the peak memory low (the biggest cost of exporting a large picture). Returns null when `rotateArbitrary` saw `latest != gen`. It is the same chain as `wide_chain.hpp` and the geometry worker.

### `export_worker.hpp`
Batch export: `exportPicture` renders one picture from its own saved edits and writes it, and `ExportWorker` runs a queue of them off the UI thread. A first-in-first-out queue with **no generations** (like `ThumbWorker`) that can be cancelled. (Needs LiteUI's decoder and encoders.)

**Depends on:** `liteui.hpp`, `geometry.hpp`, `raster_ops.hpp`, `adjustment_set.hpp`, `render.hpp`, `file_utils.hpp`, `image_codec.hpp`, `image_codec16.hpp`, `image_meta.hpp`, `image_source.hpp`, `color_tag.hpp`, `minijson.hpp`, `sidecar.hpp`, `virtual_copy.hpp`, `export_render.hpp`, and through `adjustment_set.hpp` the LUT library. **Test:** `cases/export_worker.inc` (timing-sensitive: loop it after changing the header).

**Constants:** `kExportMaxSidecarBytes` (`std::streamoff`) = 64 MiB (the limit the editor puts on a sidecar).

**Types**

- `struct ExportSettings { bool jpeg = true; bool png16 = false; int quality = 92; bool keepMetadata = true; bool removeGps = false; bool resize = false; int longEdge = 2048; std::string folder; bool overwrite = false; }`: a copy of the Export pane's settings plus the destination folder. `png16` applies to PNG only (ignored when `jpeg`): write 16 bits per channel instead of 8. `overwrite` false means a taken name gets `-2`, `-3`, ...
- `struct ExportOutcome { std::string path; bool ok = false; bool cancelled = false; std::string outFile; std::string error; int width = 0, height = 0; }`: one per picture. `path` is the picture's path (a copy's `|vc<n>` included). `cancelled` is **not** a failure: the picture was dropped or abandoned by `cancel()` and nothing was written. `error` says why a picture was skipped. `outFile`, `width` and `height` are set only when `ok`.

**Functions**

**`inline bool saveJpegFile(const std::string &path, const CanvasImage &img, int quality, bool keepMetadata, bool removeGps, const std::string &sourcePath, std::string *errorOut)`**
- Encodes `img` as a JPEG (quality clamped to 1..100) with `encodeJpegMemory`, then calls `embedSourceMetadata(&jpeg, sourcePath, w, h, removeGps && keepMetadata, /*iccOnly=*/!keepMetadata)` and writes it with `writeFileAtomic`. So the source's **ICC profile always rides along** (the editor converts no pixels, so without it a Display P3 or Adobe RGB picture would be read as sRGB; a profile is not personal data), while the **EXIF block comes only with `keepMetadata`** (orientation reset, size updated), with the GPS cut out when `removeGps` is also on (`removeGps` is ignored without `keepMetadata`). The result of `embedSourceMetadata` is ignored: a source that is not a JPEG just gives a JPEG with no metadata. Returns false with `*errorOut` set if the encode or the write fails (including a picture `jpegSizeOk` refuses). Shared by the editor's single Export and the batch (it used to live in `main.cpp`).

**`inline std::string exportStemOf(const std::string &path)`**
- Returns: the picture's file name without its extension; `"export"` if that is empty; `-copy<n>` appended for a virtual copy's path (`photo.jpg|vc2` gives `photo-copy2`). A leading dot is kept (`.hidden`).

**`inline std::string exportTargetPath(const std::string &folder, const std::string &stem, const std::string &ext, bool overwrite, std::set<std::string> &used)`**
- Returns: `<folder>/<stem>-edit.<ext>`, or with `-2`, `-3`, ... up to 9999 when that name is in `used` (compared **without case**, so `a.jpg` and `A.png` can't collide on a case-insensitive disk) or, unless `overwrite`, already exists. The chosen name is added to `used`. `used` wins over `overwrite`: a name already taken in this batch is never reused. Empty string if every name is taken.

**`inline bool exportLoadEdits(const std::string &path, int w, int h, AdjustmentSet *adj, std::string *err)`**
- `adj` must already have `setOriginalSize(w, h)` (the upright size). No sidecar file: returns true and leaves `adj` neutral. A sidecar that exists but is empty or over 64 MiB, unreadable, not valid JSON, from a newer version, written for a different size, or whose `edits` can't be loaded: returns false with a reason in `*err`. **This is stricter than the editor**, which opens such a picture clean: a batch must not quietly export an unedited picture. The same goes for a LUT: if the picture's LUT is in use (`LUT Amount` above 0) and the library reports it `Missing` (*"its LUT \"<name>\" is not in the LUT library"*) or `Unreadable` (*"... can't be read"*), the picture is skipped with that reason rather than exported without its look. A LUT whose file has changed (`HashChanged`) is used as it is, and a LUT at amount 0 never blocks a picture. The table is resolved here, on the worker thread, through `acquireLut`. The reasons come back in `*err`: *"couldn't read its saved edits"* (can't open or read the file), *"its saved edits have an unexpected size"* (empty or over `kExportMaxSidecarBytes`), *"its saved edits are unreadable"* (not valid JSON, or no usable `edits` object), *"its saved edits are from a newer Literoom"* (a missing or non-numeric `version` counts as newer too) and *"the picture's size has changed since it was edited"* (the sidecar's `image` width and height must equal `w` and `h`). If the existence check itself fails, the file is treated as absent.

**`inline ExportOutcome exportPicture(const std::string &path, const ExportSettings &s, std::set<std::string> &used, const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)`**
- The whole pipeline for one picture, a pure function of its inputs (see [threading.md](threading.md#the-export-worker-is-a-cancellable-fifo-of-whole-pipelines)):
  1. **Decode** `vcImageOf(path)` with `image_source::openUpright` (`want8 = false`). A PNG whose header says 16 bits is read at full precision and stays 16-bit; everything else (JPEG, 8-bit PNG) stays 8-bit. If the full-precision read fails the 8-bit decoder is tried, exactly as the editor does. The EXIF rotation is applied, which gives the upright "original" the saved edits are stored against.
  2. **Edits:** load them into a **local** `AdjustmentSet` with `exportLoadEdits`, then freeze `buildRenderParams()`.
  3. **Render:** `renderForExport` (see [`export_render.hpp`](#export_renderhpp)) does orient -> straighten -> crop -> render from the untouched original, with `want16 = !s.jpeg && s.png16`. So a 16-bit source written as JPEG or 8-bit PNG is rendered down to 8 bits, and an 8-bit source with `png16` on is rendered to 16 bits.
  4. **Size:** `fitLongEdge` gives the final size (never enlarging). For a JPEG, `jpegSizeOk` is checked on that **final** size, **before** the resize; then `ExportPixels::resizeToLongEdge`.
  5. **Write:** a free name from `exportTargetPath` (`.jpg` or `.png`), then the encode: `encodePng16File` when the result is 16-bit, `saveJpegFile` for a JPEG, otherwise `encodePngFile`. The JPEG gets the **real** file's (the one behind a copy) ICC profile always and its EXIF only with `keepMetadata` (`removeGps` only counts when `keepMetadata` is also on; see `saveJpegFile`). After a PNG (8- or 16-bit) is written, `color_tag::carryColorToPng(target, real)` copies the source's color tags into it (best effort; its result is ignored here).
- Cancellation: `latest` / `gen` are the usual pair (both may be null / 0 for a synchronous call). Once superseded the outcome is `cancelled` and **nothing is written**. It is polled before the decode, after the decode, inside `renderForExport`, and after the resize. It is **not** polled during the encode, so a picture that has reached the encoder is finished.
- Failures come back as an outcome with `error` set; the function never throws for them. The messages: *"couldn't open it: <decoder's reason>"*, whatever `exportLoadEdits` said, *"too large for JPEG (turn on Resize, or export as PNG)"*, *"no free file name in the folder"*, and the encoder's own message when writing fails (or *"couldn't write <file name>"* if it gave none).

**`class ExportWorker`** (non-copyable). **Thread model:** `start()`, `cancel()`, `busy()`, `collect()` and `shutdown()` from the UI thread only; `setNotify()` before the first `start()`. The worker thread (a `std::jthread`) starts at the first `start()` and handles one picture at a time, in the order given.

- **`~ExportWorker()`**: calls `shutdown()`.
- **`void setNotify(std::function<void()> fn)`**: called **on the worker thread** after each result is stored. Set it before the first `start()`.
- **`bool start(std::vector<std::string> paths, ExportSettings settings)`**: queues the batch (starting the thread on first use). False, with nothing changed, if `paths` is empty or a batch is still running. The set of used output names (`used`) is cleared here, so names are only de-duplicated within one batch (against each other, and against files already in the folder unless `overwrite`).
- **`void cancel()`**: drops what is still queued (each picture gets a `cancelled` outcome), bumps the epoch that `renderWith` and `rotateArbitrary` poll so the picture in flight is abandoned, and returns at once. A picture that had already been written stays written.
- **`bool busy() const`**: true from `start()` until every picture has an outcome. `size_t total() const` and `size_t done() const` give the progress.
- **`std::vector<ExportOutcome> collect()`**: every outcome made since the last call, in the order they finished, and empties the list. **Call `busy()` first**: if it was already false, these are the last outcomes. Asking after `collect()` could miss a result that lands in between.
- **`void shutdown()`**: clears the queue, abandons the picture in flight, requests stop and joins. Safe to call twice. It is for teardown: pictures still queued are simply dropped **without** an outcome (unlike `cancel()`), so `done()` never reaches `total()` and `busy()` stays true afterwards; only the picture in flight still gets its (cancelled) outcome.

### `lut_preview_worker.hpp`
The LUTs pane's previews, made off the UI thread: each LUT of the library applied to the open picture. One job is the picture, its current look and a list of LUT files; the worker posts one result per file. See [threading.md](threading.md#the-lut-preview-worker-streams-results-for-the-newest-job).

**Depends on:** `liteui.hpp` (`CanvasImage`), `color_pass.hpp`, `lut3d.hpp`, `render_params.hpp`, `lut_cube.hpp`, `lut_library.hpp`, `render.hpp`, `thumb_worker.hpp` (`makeThumb`, `kFilmThumbW` / `kFilmThumbH`). **Tests:** `cases/lut_preview_worker.inc` (timing-sensitive: loop it after changing the header); `tests/lut_pane_test.cpp` also runs the real worker through the pane's per-frame poll. **Thread model:** `submit()`, `cancel()`, `busy()`, `collect()` and `shutdown()` from the UI thread only; `setNotify()` before the first `submit()`. The worker thread starts at the first `submit()`.

**Types**

- **`struct LutPreviewJob { uint64_t serial; std::shared_ptr<const CanvasImage> base; RenderParams params; std::vector<LutEntry> items; }`**: `base` is the picture to preview on (null: no thumbnails, the files are still read); `params` its current look (its own LUT, if any, is replaced); `items` the files (name and path; truncated to `kLutMaxFiles`).
- **`struct LutPreviewResult { uint64_t serial; std::string name, path; std::shared_ptr<const CanvasImage> thumb; uint64_t hash; int size; std::string warning, error; }`**: `thumb` is null when there is no picture or the file is unusable; `hash` and `size` are 0 and `error` is set (the loader's message, which names the file) when it could not be read; `warning` is `lutExpectationWarning`'s sentence or `""`.

**`inline RenderParams lutPreviewParams(const RenderParams &in, const LutPass &lut)`**: the look a preview is rendered with: `in` with `color` replaced by `colorPassWithLut(in.color, lut)` and **without the neighbor effects** (`fx` and every mask's `fx` emptied). They are invisible at thumbnail size, and leaving them out means a preview never touches the shared detail-plane cache the main render uses. Everything else is kept: tone, curves, color, Mixer, grading, Filters, masks, Vignette and Grain. A function of its own so the tests build the same reference.

**`class LutPreviewWorker`**
- **`void submit(LutPreviewJob job)`**: replaces whatever is waiting or running. The picture in flight is abandoned (`renderWith` polls the generation, so a thumbnail stops within a few rows) and results of the replaced job that were not collected are dropped: **nothing of an earlier job is delivered once `submit` has returned**.
- **`void cancel()`**: forgets the job and every uncollected result. **`bool busy() const`**: from `submit()` until the last result of the job was made (or it was replaced or cancelled and the worker has let go of it).
- **`std::vector<LutPreviewResult> collect()`**: every result made since the last call, in the order made, which is the order of `items`.
- **`void shutdown()`**: stops and joins; safe to call twice. The destructor calls it.
- Per job the worker box-averages the picture **once** to the filmstrip's thumbnail size (`makeThumb`), then for each file: `loadLutFile` (**never the cache**, so a hundred previews cannot push out the table in use), `lutPreviewParams` with the table at 100 %, `renderWith`. The notify callback runs after each result and when the job ends.

---

## Test support

### `check.hpp`
The tiny test harness used by `lut_pane_test.cpp` (the other stand-alone program, `golden_render_test.cpp`, keeps its own tally and exit codes): `CHECK`, `CHECK_NEAR` and `finish()`. The unit tests in `cases/*.inc` use Catch2 instead (see `literoom_test.cpp`); the macro names `CHECK` and `CHECK_NEAR` would clash with Catch2's own, so this header cannot be included in that single translation unit. The comment at the top of the header still calls it "shared by the unit tests in this folder", which predates the move to Catch2. See [testing.md](testing.md).

**Depends on:** standard library only (`<cmath>`, `<cstdio>`). No test of its own; it is the harness `lut_pane_test.cpp` uses.

**State:** `tcheck::failures` (`inline int`, starts at 0). A plain `int`, so not thread-safe.

**Macros**
- **`CHECK(...)`**: records a failure when the expression is false and carries on. Prints `FAIL <file>:<line>  <expression>` and increments `tcheck::failures`. Variadic on purpose, so braced initializers with commas (`CropRect{1, 2, 3, 4}`) work inside.
- **`CHECK_NEAR(a, b, tol)`**: fails unless `|a − b| <= tol`. All three are converted to `double`. A NaN on either side fails. Prints both values.

**`inline int finish()`**: prints `all ok` or `<N> FAILED`. Returns 0 if there were no failures, otherwise 1. Use it as the last line: `return finish();`.