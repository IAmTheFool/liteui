# Architecture

This page explains how Literoom is put together and, more importantly, **why**. If you are about to change the code, read this first. If you only want the one-line purpose of every file, see [headers.md](headers.md).

## The big idea: engine and UI are separate

Literoom is split into three layers:

| Layer | What it is | Where |
|---|---|---|
| **Engine** | Header-only files that describe edits and turn them into pixels. No window, no widgets, no editor state (the only globals are internal caches, guarded by mutexes). | `*.hpp` next to `main.cpp` |
| **UI shell** | One big file that builds the window, the panes and the canvas, and connects them to the engine. | `main.cpp` |
| **LiteUI** | The window, widget and canvas library, plus the image decoders. | `liteui.hpp` |

Why split it this way? Because everything in the engine is a *plain function of plain values*. A slider value goes in, a number or a table comes out. That makes the engine easy to test without opening a window (nearly every engine header has its own unit test; the exceptions are `sam_segmenter.hpp`, which needs ONNX Runtime, and `check.hpp`, the test harness itself), and it makes it safe to run on a background thread.

`main.cpp` is long (about 8,800 lines) because it holds all the UI. It reads roughly top to bottom as: layout constants and colors, the render worker, crop and filmstrip state, `EditorState`, handing worker results back to the UI thread, crop, the geometry worker, rotate/flip and straighten, opening a file (the decode worker), object segmentation, saving and loading edits, Sync Settings (its worker), the filmstrip, small reusable widgets, one section per pane (plus the History panel, the Presets list, which also holds the Copy / Paste Settings clipboard, and Before / After), and finally `main()` which wires up shortcuts, menus, the workers' wake-ups and the event loop.

## The picture pipeline

`EditorState` (in `main.cpp`) does not keep one image. It keeps a **chain**, where each image is built from the one before:

```
original  →  oriented  →  rotated  →  source  →  image
(decoded)    (turned/     (straightened) (cropped)  (adjusted:
              flipped)                              drawn + exported)
```

- `original` is what came out of the file (with the camera's EXIF rotation already applied once, so it is upright). It is **never modified**.
- `oriented`, `rotated` and `source` are the geometry steps: 90° turns and flips, then the straighten angle, then the committed crop.
- `image` is `source` with every adjustment applied. This is what you see and what gets exported.

Each step is kept so that a small change only redoes the work that depends on it. Moving a slider only recomputes `image`. Changing the crop reuses `rotated`. (That is the synchronous path used for small photos. On large photos, geometry changes go to a background geometry job that rebuilds the whole chain from `original`; see [threading.md](threading.md).)

`EditorState` also keeps `orientedPreview`, a downsampled copy of `oriented` that makes live straighten-angle drags cheap, and `previewBase`, the downsampled `source` used for slider drags (see "The fast preview path").

A rule that holds everywhere: **images are replaced, never edited in place.** A worker thread can therefore hold a pointer to `source` safely while the UI thread builds a new one.

### Geometry: one transform for everything

The order of geometry is always **orient → straighten → crop**. Instead of tracking each step separately, `geometry.hpp` folds them into a single `GeoFrame`: one rotation/mirror plus an offset that maps any point of the *original* picture to where it sits in the *current* one.

This matters for masks. Masks are stored against the original picture (see below), and the `GeoFrame` maps them into the picture on screen whenever one is drawn, edited or rendered. So rotating, flipping, straightening or cropping **never has to touch a mask**. Undo and redo stay consistent for free, and removing a crop brings back a mask that was outside it.

## From sliders to pixels

When you drag a slider, the edit state (`AdjustmentSet`) is **frozen** into a small value called `RenderParams`. It holds three 256-entry lookup tables, a color-pass description, the detail-effect gains, and a list of frozen masks. Building it takes O(256) work, no matter how big the picture is.

That value is then handed to `renderWith()` in `render.hpp`, which processes the picture in blocks of 128 rows. Per pixel, the order is always:

1. **Tone tables.** Exposure, Brightness, Contrast, Highlights, Shadows, Whites and Blacks are composed into one lookup table, followed by the master curve, then each channel's own curve. A pixel costs one table read per channel.
2. **Color pass.** One float calculation per pixel, in this order: white-balance gains → vibrance → saturation → hue → Color Mixer → colorize → Color Grading → Filters. It rounds to 8 bits only **once, at the end**, so stacking several color sliders does not pile up rounding errors.
3. **Neighbor effects.** Dehaze, then Texture / Clarity / Sharpness, then noise reduction, then Blur (last, so it wins).
4. **Masks**, in list order. Each mask repeats steps 1 to 3 on its own copy of the pixels and blends the result back.

Some details that explain choices you will see in the code:

- **Why lookup tables?** The tone sliders are pointwise (a pixel's result depends only on that pixel's own channel value), so all of them can be baked into 256 numbers. The table is rounded and clamped after *each* slider, so the result is bit-identical to running the sliders one after another.
- **Why the Filters come last.** Greyscale, Sepia, Vintage, Fade, Negative, Posterize, Solarize, Bleach Bypass, Threshold and Cross Process are a "look" laid over your edits, so they run at the end of the color pass and see the finished color. Each is a blend, `lerp(pixel, filtered, strength)`, so a strength of 0 is exactly the identity. They stay in float, so they add no extra rounding. They are plain Color-stage sliders, so masks get them for free (a local black-and-white, for example).
- **Why a float color pass?** Cross-channel math (saturation, hue, the mixer) cannot be a per-channel table. Doing it in float and rounding once is both cheaper and more accurate than chaining 8-bit stages.
- **Why the color pass shares its luma weights.** Several pieces of the color pass (Color Grading's tint, the white-balance normalization, vibrance, saturation) use the same Rec. 601 luma weights (`kGradeLumaR/G/B`). If the weights ever disagreed, a tint would shift brightness or read the wrong table row. Other parts of the program use their own luma, each consistent within itself: `detail_layer.hpp`, `neighbor_fx.hpp` and the Luminosity mask use integer 54/183/19 (about Rec. 709), Color Range uses float Rec. 709, and the histogram uses integer Rec. 601 (77/150/29).
- **Why 128-row blocks?** So a background render can check "have I been superseded?" between blocks and stop within a few milliseconds.
- **Everything is sanitized.** Slider values are clamped and NaN/infinite values become 0 before they reach any formula, so a damaged sidecar or a UI bug can never produce an out-of-range table index.

### Detail effects look at neighbors, without halo rows

Texture, Clarity, Sharpness, Dehaze, noise reduction and Blur all need neighboring pixels, which sounds incompatible with processing 128-row blocks. The trick (explained at the top of `detail_layer.hpp` and `neighbor_fx.hpp`):

- They read **whole-image blurred copies of the *source* picture**, built once and cached.
- Each effect is then a plain per-pixel formula such as `pixel + gain × (pixel's luma − blurred luma)`.

Two consequences follow. First, no pixel ever lacks neighbors, so no halo rows are needed, and masks can use these effects with no special handling. Second, dragging *any* slider (even Texture itself, which only changes a gain) never rebuilds the blur.

The trade-off is that detail is measured on the source before tone mapping, so a large Exposure boost does not amplify it the way sharpening an already-brightened picture would. For Texture, Clarity and Sharpness only luma is blurred and the same amount is added to R, G and B, which avoids color fringes along edges. Noise reduction and Blur work a little differently: each is applied as a "blurred minus source" delta, measured on the source picture and added to the current pixel, so it sits on top of the tone and color edits instead of undoing them. Color noise reduction and Blur use color (chroma) planes rather than luma only.

## The fast preview path

Rendering a 24-megapixel photo on every mouse-move would be far too slow, so large pictures have two paths:

- **While dragging:** adjustments are applied to `previewBase`, a box-downsampled copy whose long side is at most 1024 px. This is quick enough to feel live. Pictures whose long side is under about 1536 px skip the preview entirely, because it would not be meaningfully cheaper.
- **When you let go:** the full-resolution render is handed to a background worker (see [threading.md](threading.md)). The preview stays on screen until the finished image arrives, then replaces it.

Masks are stored in normalized units (fractions of the picture), so the same mask renders identically on the small preview and on the full-size picture.

## Masks

A mask is a list of *parts* (shapes) plus its own copy of the adjustments. Rendering a mask works like this:

`out = base + (adjusted − base) × weight`

`weight` is 0 to 1 per pixel: 1 is the full local edit, 0 leaves the pixel alone. Parts are folded left to right with **Add** (`m + c − m·c`), **Subtract** (`m·(1 − c)`) or **Intersect** (`m·c`). The mask's **Invert** is applied last, and its **Amount** scales the final weight.

Each shape has two forms: the *stored* form (against the original picture) and the *source* form (already mapped into the current picture through the `GeoFrame`). Rendering and on-canvas editing use the source form.

Each shape also has an `Eval`: the shape resolved for one concrete image size. It answers two questions per row: *which pixels can be non-zero?* (`rowSpan`, so the renderer skips empty areas) and *what are their weights?* (`rowWeights`).

| Kind | How the weight is found |
|---|---|
| Radial | Closed-form: distance from a rotated ellipse, eased with a smoothstep |
| Linear | Closed-form: position along a direction, eased with a smoothstep |
| Brush | Strokes are rasterized into a coverage map (cached), then read back |
| Object | A coverage bitmap from a segmentation, warped, grown/shrunk and feathered (cached) |
| Luminosity | A 256-entry table indexed by the pixel's brightness |
| Color Range | Distance from a sampled color in a cheap luma/chroma space |

Luminosity and Color Range are **not spatial**: they select by the picture's own values, so they need no `GeoFrame` mapping. They read the row's pixels as they are when the mask is reached (after the global edits and earlier masks) but *before* the mask's own edit, so a mask can never change what it selects.

To add a new mask type, see [extending.md](extending.md).

## Presets

A **preset** is a saved *look*: the slider values, the four tone curves, the Color Mixer and Color Grading. It deliberately leaves out crop, rotation, straighten and masks. Those belong to one picture: a crop that suits one photo is wrong for another, and masks are stored against one original picture's size. Slider values, curves, the mixer and grading are the same for every picture, so those are what a preset holds.

Presets follow the same rules as the rest of the engine:

- **A plain value, in a plain header.** `io/preset.hpp` holds the `Preset` struct and its file helpers. It has no window and no editor state, and a unit test needs no UI.
- **Saved the same way as edits.** A preset file reuses the sidecar's writers and readers, so it is validated and clamped just like a saved-edits file: a damaged or hand-edited preset can never put the editor in a state its own UI could not reach. See [sidecar-format.md](sidecar-format.md#presets-a-related-file).
- **Applied through `AdjustmentSet`.** `applyPreset` replaces the look and is one undo step, labelled `Preset: <name>` in History. A step stores a full copy of the look, so deleting the preset file later never changes a picture or its history. See [undo-and-history.md](undo-and-history.md#presets).
- **Nothing geometric is touched.** Because a preset holds no crop, rotation or masks, applying one never goes through the `GeoFrame`, the geometry worker or the mask caches. It only changes values the normal render path already reads, so the picture is redrawn like any other adjustment-only change: synchronously for small photos, on the background render worker for large ones (it is not a slider drag, so the fast preview path is not used).

The Presets pane in `main.cpp` is a thin layer over this: a list of the files in the presets folder, a name field and a Save button. The list is a cache, re-read when the pane is opened and after a save or delete, never per frame. For the per-function details see `preset.hpp` in [headers.md](headers.md#presethpp).

## Copy, paste and sync settings

These reuse the preset machinery instead of adding a new kind of data. The thing being moved around is a `Preset` (the look: sliders, curves, Color Mixer, Color Grading), never crop, rotation or masks, for the same reason presets leave them out.

- **Copy / Paste Settings** (Ctrl+Shift+C / V) work on the open picture. Copy keeps `currentPreset()` in a small in-memory clipboard in `main.cpp`; Paste calls `applyPreset`, so it is one undo step labelled `Preset: Pasted`. See [undo-and-history.md](undo-and-history.md#copy-and-paste-settings).
- **Sync Settings** (Ctrl+Shift+S) copies the open picture's look onto the pictures selected in the filmstrip (Ctrl+click toggles one, Shift+click selects a range, Ctrl+A selects all, a plain click clears the selection and opens a picture). The other pictures are not open, so there is no `AdjustmentSet` to change. Instead `sidecarSyncLook` (a pure function in `io/sidecar.hpp`) takes a picture's sidecar text, replaces only the four look groups, and returns the new text, so each picture keeps its own crop, rotation and masks. A background worker does the file work, because a picture with no sidecar must be decoded once to learn its size. See [sidecar-format.md](sidecar-format.md#syncing-a-look-onto-other-pictures) and [threading.md](threading.md#the-sync-worker-is-a-batch-that-is-never-superseded).

The engine stays UI-free: `sidecarSyncLook` has no file, window or editor dependency and is tested with plain strings. The selection flags and the clipboard are UI-side state in `main.cpp`.

## White-balance picker

The Adjust pane has a **Pick White Balance (W)** button (above Temperature). Arm it, click something that should be gray, and Temperature and Tint are set so that spot becomes neutral. It adds no new kind of data: it sets two ordinary sliders.

The engine does the work and stays UI-free:

- **The math** is `solveWhiteBalance` in `color/color_pass.hpp`: the inverse of the white-balance gains `makeColorPass` builds, solved in closed form from a spot's R, G and B. It reports `Ok`, `Limited` (needs more than ±100), `TooDark` or `Clipped`. See [headers.md](headers.md#color_passhpp).
- **The edit** is `AdjustmentSet::setWhiteBalance`: both sliders in one undo step, labelled `White Balance <t> / <n>`. See [undo-and-history.md](undo-and-history.md#white-balance-picker).

The UI side lives in `main.cpp` and is small:

- **Which pixels it reads.** Not the shown picture. White balance runs *after* the tone tables, so the spot's color has to be taken where the color pass would receive it: `state.source` (what the adjustments run on) through the current `buildChannelLuts()`, before any white balance. That makes the answer independent of the Temperature and Tint already set, and of Saturation, the Mixer, grading and the Filters, which all come later. The RGB readout under the cursor samples the finished picture, which is why it cannot be reused.
- **A 5 × 5 patch** (`kWbPatchRadius = 2`) is averaged around the click, so one noisy pixel cannot decide the result. Fully transparent pixels (the empty corners of a straightened picture) are skipped.
- **Armed state.** `state.wbPicking` is set by the button or the W key and read through `wbPickerActive()`, which also clears it the moment it stops making sense: another pane is open, no picture is shown, or Before / After is on. Esc cancels it, and a newly opened picture disarms it. A click while armed goes to `wbPress` before the crop and mask tools, which only act in their own panes anyway.
- **One pick per arming.** A successful pick (or "already set") disarms the picker. A click that cannot be used (outside the picture, too dark, clipped) keeps it armed with a message, so you can try another spot.
- **Not while the geometry worker is busy.** `state.source` still describes the previous geometry then, so `wbPress` waits (see [threading.md](threading.md#the-geometry-worker-rebuilds-everything)).
- **Global edit only.** Masks keep their own Temperature and Tint; the picker never touches them.

## Auto tone

The Adjust pane has an **Auto Tone (A)** button at the top of the Tone block (also the A key and Image > Auto Tone). It adds no new kind of data: it sets four ordinary sliders, Exposure, Contrast, Whites and Blacks, in one undo step.

The engine does the work and stays UI-free:

- **The math** is `solveAutoTone` in `core/auto_tone.hpp`: a function of the luma histogram that finds the picture's black and white points (the 0.5 % and 99.5 % levels) and picks the sliders that send them to fixed targets. It stretches the measured range instead of aiming at mid-gray, so a dark scene stays dark and a well-exposed picture is barely touched. See [headers.md](headers.md#auto_tonehpp).
- **The edit** is `AdjustmentSet::setAutoTone`: the four sliders in one undo step labelled `Auto Tone`. See [undo-and-history.md](undo-and-history.md#auto-tone).

The UI side lives in `main.cpp` (`autoTone()`) and is small:

- **Which pixels it reads.** `state.source`, the picture the adjustments run on, counted with `histogram::compute`. Not the shown picture: that one already has the tone sliders applied, so its histogram would change every time Auto Tone ran. Reading the source makes the answer independent of the sliders already set, so pressing it twice is a no-op ("already set").
- **What it leaves alone.** Brightness, Highlights, Shadows, curves and masks. Auto Tone is a global edit: masks keep their own sliders.
- **Messages.** A flat picture or one with nothing to measure says so and changes nothing. If the slider ranges are not enough to reach the targets (a very dark or very washed-out picture), the edit is still applied and the status bar says the ranges are not enough.
- **Not while the geometry worker is busy** (`source` still describes the previous geometry then; see [threading.md](threading.md#the-geometry-worker-rebuilds-everything)), and not mid-drag.

## Where state lives

All editor state is one global `state` (an `EditorState`) in `main.cpp`, touched only by the UI thread. The edit values live in `state.adjust` (an `AdjustmentSet`), which also owns the undo history. See [undo-and-history.md](undo-and-history.md). Edits are written to disk by [the sidecar](sidecar-format.md). Other UI-side state is the Presets pane's list of preset files (a cache of what is in the presets folder), the Copy Settings clipboard (one `Preset` held in memory) and the filmstrip's multi-selection (a flag on each `FilmEntry`, plus the anchor of a Shift-click range). The white-balance picker's armed flag (`state.wbPicking`) is plain UI state and is never saved. Presets themselves live in files, not in `state`; applying one, or pasting the clipboard, copies its values into `state.adjust`. Sync Settings never touches `state.adjust` for the pictures it writes to.

## The rule about threads

Workers never read `state` and never call LiteUI. They receive an immutable picture and a copy of the parameters, and they return a value. This is what makes the threading simple enough to reason about. Details in [threading.md](threading.md). Presets need no worker: saving, loading and listing read or write a few hundred bytes, so they run on the UI thread like the sidecar's small writes. Sync Settings does need one (it can decode pictures and write many files) and gets its look as a copied value when the job is built.