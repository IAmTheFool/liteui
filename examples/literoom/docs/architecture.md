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

`main.cpp` is long (about 8,800 lines) because it holds all the UI. It reads roughly top to bottom as: layout constants and colors, the render worker, crop and filmstrip state, `EditorState`, handing worker results back to the UI thread, crop, the geometry worker, rotate/flip and straighten, opening a file (the decode worker), object segmentation, saving and loading edits, Sync Settings (its worker), batch export (its worker), the filmstrip, small reusable widgets, one section per pane (plus the History panel, the Presets list, which also holds the Copy / Paste Settings clipboard, and Before / After), and finally `main()` which wires up shortcuts, menus, the workers' wake-ups and the event loop.

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

When you drag a slider, the edit state (`AdjustmentSet`) is **frozen** into a small value called `RenderParams`. It holds three 256-entry lookup tables, a color-pass description, the detail-effect gains, a list of frozen masks, and the Vignette and Grain settings. Building it takes O(256) work, no matter how big the picture is.

That value is then handed to `renderWith()` in `render.hpp`, which processes the picture in blocks of 128 rows. Per pixel, the order is always:

1. **Tone tables.** Exposure, Brightness, Contrast, Highlights, Shadows, Whites, Blacks and the three Levels sliders (Black Point, White Point, Gamma) are composed into one lookup table, in that order, followed by the master curve, then each channel's own curve. A pixel costs one table read per channel.
2. **Color pass.** One float calculation per pixel, in this order: white-balance gains → vibrance → saturation → hue → Color Mixer → colorize → Color Grading → Filters. It rounds to 8 bits only **once, at the end**, so stacking several color sliders does not pile up rounding errors.
3. **Neighbor effects.** Dehaze, then Texture / Clarity / Sharpness, then noise reduction, then Blur (last, so it wins).
4. **Masks**, in list order. Each mask repeats steps 1 to 3 on its own copy of the pixels and blends the result back.
5. **Vignette and Grain** (`film_fx.hpp`), on the finished pixel, after everything else. They are global only: masks do not get them.

Some details that explain choices you will see in the code:

- **Why lookup tables?** The tone sliders are pointwise (a pixel's result depends only on that pixel's own channel value), so all of them can be baked into 256 numbers. The table is rounded and clamped after *each* slider, so the result is bit-identical to running the sliders one after another.
- **Why the Filters come last.** Greyscale, Sepia, Vintage, Fade, Negative, Posterize, Solarize, Bleach Bypass, Threshold and Cross Process are a "look" laid over your edits, so they run at the end of the color pass and see the finished color. Each is a blend, `lerp(pixel, filtered, strength)`, so a strength of 0 is exactly the identity. They stay in float, so they add no extra rounding. They are plain Color-stage sliders, so masks get them for free (a local black-and-white, for example).
- **Why Vignette and Grain come last.** They are the finishing touch on the picture as it is shown, so they see the result of every slider and every mask, like a lens vignette or film grain on the print. They are also the only effects that depend on *where* a pixel is, which none of the other stages know: the tone tables see one value, the color pass one pixel, the detail layers a neighborhood of the source. That is why they are their own stage ("Effects") with their own pass, and why a mask has no Vignette or Grain sliders (the three renders in `main.cpp` that sample "the picture just before a mask" clear them, so a Color Range or Luminosity mask never sees a vignette).
- **Why the vignette needs no `GeoFrame`.** `renderWith` is always handed the picture that is already oriented, straightened and cropped, and the vignette is measured against *that* frame: centered on it, following its aspect ratio, 0 at the center and 1 at the corners. A crop, a rotation or a flip therefore just gives the vignette of the new frame (Lightroom calls this post-crop vignetting). A vignette anchored to the original picture would have to go through the `GeoFrame` like a mask, and a crop could then leave it off-center.
- **Why grain is a function of position.** Grain is an integer hash of the pixel's (x, y) in the picture being rendered: no random number generator, no state, nothing that depends on render order, block size, thread or compiler. The 128-row blocks, the background render and the batch export therefore give the same bytes for the same picture and sliders. Two things it does *not* promise: the fast preview renders a smaller picture, so its grain is the same kind (cell size is a fraction of the long edge) but not the same pixels; and a crop shifts the origin, so it changes the pattern. Every picture gets the same pattern for the same size and sliders, because there is no per-picture seed.
- **Why a float color pass?** Cross-channel math (saturation, hue, the mixer) cannot be a per-channel table. Doing it in float and rounding once is both cheaper and more accurate than chaining 8-bit stages.
- **Why the color pass shares its luma weights.** Several pieces of the color pass (Color Grading's tint, the white-balance normalization, vibrance, saturation) use the same Rec. 601 luma weights (`kGradeLumaR/G/B`). If the weights ever disagreed, a tint would shift brightness or read the wrong table row. Other parts of the program use their own luma, each consistent within itself: `detail_layer.hpp`, `neighbor_fx.hpp` and the Luminosity mask use integer 54/183/19 (about Rec. 709), Color Range uses float Rec. 709, and the histogram uses integer Rec. 601 (77/150/29). The vectorscope's chroma (`scopes::rgbToCbCr`) is the matching integer Rec. 601 Cb / Cr, so a gray is exactly the center of the plot.
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

Masks are stored in normalized units (fractions of the picture), so the same mask renders identically on the small preview and on the full-size picture. The vignette is normalized too (distance from the center in corner units), so it matches as well; only the grain differs in its exact pixels, as explained above.

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
- **Batch export** (File > Export Selected... Ctrl+Shift+E, Export All Shown..., Stop Export; the same buttons are in the Export pane) renders every picked filmstrip picture from its own saved edits into one folder, using the Export pane's format, quality, metadata and resize settings. It is the missing last step after ratings, Sync Settings and virtual copies: each picture, and each virtual copy, is exported with its own look. See [threading.md](threading.md#the-export-worker-is-a-cancellable-fifo-of-whole-pipelines).

The engine stays UI-free: `sidecarSyncLook` has no file, window or editor dependency and is tested with plain strings. The selection flags and the clipboard are UI-side state in `main.cpp`.

## Snapshots and virtual copies

Both are ways to keep more than one version of a picture's edits, and both are built from the sidecar rather than from new kinds of data.

**Named snapshots** are saved states of *one* set of edits that you can return to. A snapshot is a name plus a whole `SidecarEdits` (look, curves, Mixer, grading, crop, rotation, straighten **and masks**), stored in a `snapshots` array in the picture's sidecar and written by the same functions as the edits. Unlike a preset it replaces everything, geometry and masks included. Restoring one goes through `AdjustmentSet::restoreEdits`, so it is one undo step labelled `Snapshot: <name>`. The list is kept in `state.snapshots` beside the rating, not in `AdjustmentSet`, because it is not an edit. See [undo-and-history.md](undo-and-history.md#named-snapshots) and [sidecar-format.md](sidecar-format.md#named-snapshots).

**Virtual copies** are *separate* sets of edits for the same picture file: a second sidecar (`photo.jpg.vc2.literoom.json`) and a second filmstrip cell. The trick that keeps this small is that the program already identifies a picture by one string, its path, so a copy simply gets its own path (`photo.jpg|vc2`, a marker no file name can contain). Everything keyed by path (the filmstrip index, thumbnail and sync results, the open picture) then tells copies apart with no other change; only the few places that need the real file translate it back with `vcImageOf()`, and `sidecarFileOf()` yields the copy's own sidecar. Switching between copies of the open picture reuses its pixels instead of decoding. The pure helpers are in `io/virtual_copy.hpp`. See [sidecar-format.md](sidecar-format.md#virtual-copies) and [threading.md](threading.md#virtual-copies-and-the-workers).

Each copy is a complete picture as far as the rest of the program is concerned: its own rating, flag, label, undo history (fresh on every open, as always) and snapshots.

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

## Waveform and vectorscope

The pinned canvas above the panes (the one that holds the histogram) cycles through three views when you click it: **Histogram**, **Waveform** and **Vectorscope**. A click in either top corner still toggles the clipping overlays, in every view, and the clipping triangles are drawn in every view. The scopes add no new kind of data and are never saved: they are a different way of drawing the picture on screen.

The engine does the work and stays UI-free:

- **The math** is in `render/scopes.hpp`. The waveform keeps, for every column of the picture (folded into at most 256), how many pixels have each R, G and B level. The vectorscope keeps how many pixels have each Cb / Cr pair on a 128 x 128 grid. Both are then drawn into a small `CanvasImage`. See [headers.md](headers.md#scopeshpp).
- **Same pixels as the histogram.** They use the same sample grid (about 300,000 pixels) and skip fully transparent pixels, so a scope costs no more than the histogram, and `computeHistogram()` builds them from the picture on screen: the mid-drag preview while a slider is held, the full-resolution result otherwise.
- **Shares, not counts.** Brightness is a function of the share of a column (waveform) or of the picture (vectorscope) that sits on a point, so the small preview and the full picture look alike.
- **A fixed size, stretched.** The scope picture is always the same size (the waveform is at most 256 x 256, the vectorscope 128 x 128) and the canvas stretches it, so nothing depends on how wide the sidebar is.

The UI side lives in `main.cpp` and is small:

- **State.** `state.scopeKind` (which view) and `state.scopeImage` (the waveform or vectorscope picture). `computeHistogram()` fills `scopeImage` for the current view with `scopes::render`; it is null while the histogram is showing, so the histogram view does no extra work and the golden histogram scenarios (which call `computeHistogram()` with the default view) are unchanged.
- **The click.** `cycleScope()` moves to the next view, calls `computeHistogram()` (which rebuilds the picture and bumps `histVersion`, so the canvas repaints) and marks the canvas dirty. It does nothing without a picture.
- **The height.** The canvas is 80 px tall for the histogram and 120 px for the scopes (`kHistHeightPx`, `kScopeHeightPx`); the height is a function of `scopeKind`, which LiteUI re-reads on every update. The vectorscope is drawn as a centered square.
- **Painting.** `paintHistogram` fills the background, then calls `paintHistogramBars` or `paintScope`, then `paintClipTriangles`. `paintScope` draws the picture and a faint graticule: quarter lines for the waveform; two rings, the two axes and the skin-tone line for the vectorscope.
- **The curve editor** reads `histR/G/B/L` behind the curve and is unaffected by the view.

## Where state lives

All editor state is one global `state` (an `EditorState`) in `main.cpp`, touched only by the UI thread. The edit values live in `state.adjust` (an `AdjustmentSet`), which also owns the undo history. See [undo-and-history.md](undo-and-history.md). Edits are written to disk by [the sidecar](sidecar-format.md). Other UI-side state is the Presets pane's list of preset files (a cache of what is in the presets folder), the Copy Settings clipboard (one `Preset` held in memory) and the filmstrip's multi-selection (a flag on each `FilmEntry`, plus the anchor of a Shift-click range). The white-balance picker's armed flag (`state.wbPicking`) is plain UI state and is never saved, and so are the pinned canvas's view (`state.scopeKind`) and its scope picture (`state.scopeImage`). Presets themselves live in files, not in `state`; applying one, or pasting the clipboard, copies its values into `state.adjust`. Sync Settings never touches `state.adjust` for the pictures it writes to.

## The rule about threads

Workers never read `state` and never call LiteUI. They receive an immutable picture and a copy of the parameters, and they return a value. This is what makes the threading simple enough to reason about. Details in [threading.md](threading.md). Presets need no worker: saving, loading and listing read or write a few hundred bytes, so they run on the UI thread like the sidecar's small writes. Sync Settings does need one (it can decode pictures and write many files) and gets its look as a copied value when the job is built. Batch export needs one too, and goes a step further: its job is handed only a picture's *path* and the Export settings, and rebuilds everything else (pixels, edits, geometry, render parameters) from the file and its sidecar, with a local `AdjustmentSet`. That is possible because the edits are fully described by the sidecar and the render chain is a set of plain functions.