# Architecture

This page explains how Literoom is put together and, more importantly, **why**. If you are about to change the code, read this first. If you only want the one-line purpose of every file, see [headers.md](headers.md).

## The big idea: engine and UI are separate

Literoom is split into three layers:

| Layer | What it is | Where |
|---|---|---|
| **Engine** | Header-only files that describe edits and turn them into pixels. No window, no widgets, no editor state (the only globals are internal caches: mutex-guarded, apart from the object-selection wand's one-entry cache, which only its own worker thread touches, and the test-only LUT folder override). | `*.hpp` in the folders under `src/` |
| **UI shell** | One big file that builds the window, the panes and the canvas, and connects them to the engine. | `src/main.cpp` |
| **LiteUI** | The window, widget and canvas library, plus the image decoders. | `liteui.hpp` |

Why split it this way? Because everything in the engine is a *plain function of plain values*. A slider value goes in, a number or a table comes out. That makes the engine easy to test without opening a window (every engine header has its own test file in `tests/cases/`; the one for `sam_segmenter.hpp` covers only `sam::preprocess` and the no-ONNX stand-in), and it makes it safe to run on a background thread. See [Tests and the safety net](#tests-and-the-safety-net).

### Where the engine files live

All sources are under `src/`, which is the include root, so every file is included by its path from there (`#include "mask/mask.hpp"`). The folders are layers, lowest first:

```
core  →  color / detail / mask  →  io  →  edit  →  render
```

| Folder | What is in it |
|---|---|
| `core/` | Pixel math, geometry, the slider table (`adjustments`), Auto Tone, raster operations, the 16-bit picture (`image16`, `image16_convert`, `wide_chain`), the float tone table (`tone_table`) |
| `color/` | The color pass and what feeds it: Color Mixer, Color Grading, curves, the 3D LUT |
| `detail/` | Neighbor effects (Texture, Clarity, Sharpness, Dehaze, noise reduction, Blur) and Vignette / Grain |
| `mask/` | Mask shapes, their weights, brush / polygon / object masks, the segmenters |
| `io/` | Everything that reads or writes bytes: sidecar, presets, JSON, codecs, EXIF, color-space tags (`color_tag`), the LUT library, how a file is opened (`image_source`) |
| `edit/` | `AdjustmentSet` (the edit state with undo) and `RenderParams` (the frozen form of it) |
| `render/` | The float pixel pass (`render_core`), the synchronous entry (`render`), the byte reference (`render_bytes`), histogram and scopes |
| `workers/` | Background workers: the cancellable "latest wins" worker, thumbnails, batch export, LUT previews |

A header may include headers from folders below its own, never above. The few exceptions, and the full per-file list, are in [headers.md](headers.md#where-things-live).

`main.cpp` is long (about 11,900 lines) because it holds all the UI. A few of its big sections are fenced with `// BEGIN <name>` / `// END <name>` comments (`renderworker`, `geometryworker`, `filmstrip`, `maskui`, `compare`), which mark the parts that move together if the file is ever split. It reads roughly top to bottom as: layout constants and colors, the render worker, crop and filmstrip state, `EditorState`, handing worker results back to the UI thread, crop, the geometry worker, rotate/flip and straighten, opening a file (the decode worker), object segmentation, saving and loading edits, Sync Settings (its worker), batch export (its worker), the filmstrip (with the rating, flag and label filter bar and commands), small reusable widgets, one section per pane (plus the History panel, the Presets list, which also holds the Copy / Paste Settings clipboard, the LUTs list with its preview polling, and Before / After), and finally `main()` which wires up shortcuts, menus, the workers' wake-ups and the event loop.

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

A picture that really has more than 8 bits (a 16-bit PNG) also has a **16-bit twin** of every stage; see [16-bit pictures](#16-bit-pictures).

`EditorState` also keeps `orientedPreview`, a downsampled copy of `oriented` that makes live straighten-angle drags cheap, and `previewBase`, the downsampled `source` used for slider drags (see "The fast preview path").

A rule that holds everywhere: **images are replaced, never edited in place.** A worker thread can therefore hold a pointer to `source` safely while the UI thread builds a new one.

### Geometry: one transform for everything

The order of geometry is always **orient → straighten → crop**. Instead of tracking each step separately, `geometry.hpp` folds them into a single `GeoFrame`: one rotation/mirror plus an offset that maps any point of the *original* picture to where it sits in the *current* one.

This matters for masks. Masks are stored against the original picture (see below), and the `GeoFrame` maps them into the picture on screen whenever one is drawn, edited or rendered. So rotating, flipping, straightening or cropping **never has to touch a mask**. Undo and redo stay consistent for free, and removing a crop brings back a mask that was outside it.

## From sliders to pixels

When you drag a slider, the edit state (`AdjustmentSet`) is **frozen** into a small value called `RenderParams`. It holds the tone tables (three 256-entry byte tables for the old byte pipeline, and the float tables the render actually reads), a color-pass description (which points at the picture's 3D LUT table, shared and never copied), the detail-effect gains, a list of frozen masks, and the Vignette and Grain settings. Building it takes O(256) work, no matter how big the picture is.

That value is then handed to `renderWith()` (the float pipeline in `render/render_core.hpp`; `renderAdjusted()` in `render.hpp` is the synchronous shortcut that builds the `RenderParams` first), which processes the picture in blocks of 128 rows. A block is loaded into **float "levels"** (0 to 255, the unit every constant is written in), goes through every stage as floats, and is **rounded once**, when it is stored into the result. Per pixel, the order is always:

1. **Tone tables.** Exposure, Brightness, Contrast, Highlights, Shadows, Whites, Blacks and the three Levels sliders (Black Point, White Point, Gamma) are composed into one table of floats (`core/tone_table.hpp`), in that order, followed by the master curve, then each channel's own curve. A pixel costs one table read per channel, interpolated.
2. **Color pass.** One float calculation per pixel, in this order: white-balance gains → vibrance → saturation → hue → Color Mixer → colorize → Color Grading → Filters → 3D LUT. It clamps to 0..255 at the end but does not round, so stacking several color sliders does not pile up rounding errors.
3. **Neighbor effects.** Dehaze, then Texture / Clarity / Sharpness, then noise reduction, then Blur (last, so it wins).
4. **Masks**, in list order. Each mask repeats steps 1 to 3 on its own copy of the pixels and blends the result back.
5. **Vignette and Grain** (`film_fx.hpp`), on the finished pixel, after everything else. They are global only: masks do not get them.

Some details that explain choices you will see in the code:

- **Why lookup tables?** The tone sliders are pointwise (a pixel's result depends only on that pixel's own channel value), so all of them can be baked into one table. The float table has a grid point every 1/16 level (4081 entries), so an 8-bit input lands exactly on a grid point and gets the exact value; 16-bit input is read through a finer "head" over the first two levels, where a steep curve (Gamma +100) would otherwise be off. Each slider's result is clamped to 0..255 before the next one (that clamp is what makes Highlights, Whites and Contrast behave at the ends), but it is **not rounded** any more. The older byte table (`toneLutFromValues`, 256 entries rounded after every slider) still exists: Auto Tone reasons with it and the tests compare against it.
- **Why the Filters come last.** Greyscale, Sepia, Vintage, Fade, Negative, Posterize, Solarize, Bleach Bypass, Threshold and Cross Process are a "look" laid over your edits, so they run at the end of the color pass and see the finished color. Each is a blend, `lerp(pixel, filtered, strength)`, so a strength of 0 is exactly the identity. They stay in float, so they add no extra rounding. They are plain Color-stage sliders, so masks get them for free (a local black-and-white, for example).
- **Why the 3D LUT is the last step of the color pass.** A LUT is a "look" like the Filters, laid over finished color: the picture's own tone, white balance, Mixer, grading and Filters happen first, and the LUT maps that result, the way a film emulation or a grade is applied to a print. The pass stays in float: the input is clamped to 0..255 once, and the result is `lerp(pixel, lut(pixel), amount)`, with `amount` from the LUT Amount slider, so an amount of 0, no LUT, or a LUT the library no longer has leaves every byte untouched. It comes *before* the neighbor effects, the masks and Vignette / Grain, so masks, sharpening and the film effects all see the LUT'd picture. It is global only: a mask has no LUT and no LUT Amount slider. See [3D LUTs](#3d-luts).
- **Why Vignette and Grain come last.** They are the finishing touch on the picture as it is shown, so they see the result of every slider and every mask, like a lens vignette or film grain on the print. They are also the only effects that depend on *where* a pixel is, which none of the other stages know: the tone tables see one value, the color pass one pixel, the detail layers a neighborhood of the source. That is why they are their own stage ("Effects") with their own pass, and why a mask has no Vignette or Grain sliders (the three renders in `main.cpp` that sample "the picture just before a mask" clear them, so a Color Range or Luminosity mask never sees a vignette).
- **Why the vignette needs no `GeoFrame`.** `renderWith` is always handed the picture that is already oriented, straightened and cropped, and the vignette is measured against *that* frame: centered on it, following its aspect ratio, 0 at the center and 1 at the corners. A crop, a rotation or a flip therefore just gives the vignette of the new frame (Lightroom calls this post-crop vignetting). A vignette anchored to the original picture would have to go through the `GeoFrame` like a mask, and a crop could then leave it off-center.
- **Why grain is a function of position.** Grain is an integer hash of the pixel's (x, y) in the picture being rendered: no random number generator, no state, nothing that depends on render order, block size, thread or compiler. The 128-row blocks, the background render and the batch export therefore give the same bytes for the same picture and sliders. Two things it does *not* promise: the fast preview renders a smaller picture, so its grain is the same kind (cell size is a fraction of the long edge) but not the same pixels; and a crop shifts the origin, so it changes the pattern. Every picture gets the same pattern for the same size and sliders, because there is no per-picture seed.
- **Why a float color pass, and a float pipeline?** Cross-channel math (saturation, hue, the mixer) cannot be a per-channel table. Doing it in float is both cheaper and more accurate than chaining 8-bit stages, and the same argument now covers the whole pass: tone, color, neighbor effects and masks all stay in float and the block is rounded once at the end. The old byte pipeline (`render_bytes.hpp`, `renderWithBytes`) rounded after every stage. It is kept only as the reference `cases/render_core.inc` compares against: for an 8-bit picture the two agree exactly when one stage did anything, and within a few levels when stages are stacked. Where the byte code clamped to 0..255 the float code does too, so highlights cannot run past white between stages.
- **Why the color pass shares its luma weights.** Several pieces of the color pass (Color Grading's tint, the white-balance normalization, vibrance, saturation) use the same Rec. 601 luma weights (`kGradeLumaR/G/B`). If the weights ever disagreed, a tint would shift brightness or read the wrong table row. Other parts of the program use their own luma, each consistent within itself: `detail_layer.hpp`, `neighbor_fx.hpp` and the Luminosity mask use integer 54/183/19 (about Rec. 709), Color Range uses float Rec. 709, and the histogram uses integer Rec. 601 (77/150/29). The vectorscope's chroma (`scopes::rgbToCbCr`) is the matching integer Rec. 601 Cb / Cr, so a gray is exactly the center of the plot.
- **Why 128-row blocks?** So a background render can check "have I been superseded?" between blocks and stop within a few milliseconds. Every pixel is independent, so the block size never changes the result.
- **Everything is sanitized.** Slider values are clamped and NaN/infinite values become 0 before they reach any formula, so a damaged sidecar or a UI bug can never produce an out-of-range table index.

### Detail effects look at neighbors, without halo rows

Texture, Clarity, Sharpness, Dehaze, noise reduction and Blur all need neighboring pixels, which sounds incompatible with processing 128-row blocks. The trick (explained at the top of `detail_layer.hpp` and `neighbor_fx.hpp`):

- They read **whole-image blurred copies of the *source* picture**, built once and cached. The cache key is the source's pixel pointer, its size, a cheap fingerprint (about 1024 samples, bytes or 16-bit values, taken across the picture) and the kind of plane, so a new picture that happens to reuse a freed buffer's address is still told apart, and a source that lives for a whole slider drag keeps its planes. The caches are least-recently-used with byte caps (about 384 MB for the luma planes plus 256 MB for the chroma ones, roughly 700 MB worst case on a very large picture), and they are shared by the UI thread's drag preview and the render workers. **One rule follows:** the cache assumes a picture's pixels are never edited in place once it is being rendered (the app builds a new picture for every orient, straighten and crop). A future retouch tool that edits pixels in place must call `neighbor_fx::clearCaches()` afterwards. The planes are built from the source at its own precision (RGBA8 or 16-bit), never from a rounded copy.
- Each effect is then a plain per-pixel formula such as `pixel + gain × (pixel's luma − blurred luma)`.

Two consequences follow. First, no pixel ever lacks neighbors, so no halo rows are needed, and masks can use these effects with no special handling. Second, dragging *any* slider (even Texture itself, which only changes a gain) never rebuilds the blur.

The trade-off is that detail is measured on the source before tone mapping, so a large Exposure boost does not amplify it the way sharpening an already-brightened picture would. For Texture, Clarity and Sharpness only luma is blurred and the same amount is added to R, G and B, which avoids color fringes along edges. Noise reduction and Blur work a little differently: each is applied as a "blurred minus source" delta, measured on the source picture and added to the current pixel, so it sits on top of the tone and color edits instead of undoing them. Color noise reduction and Blur use color (chroma) planes rather than luma only. Inside one `neighbor_fx::apply` the order is fixed: Dehaze first (it rescales the pixel), then the detail layers (Texture, Clarity, Sharpness), then noise reduction, then Blur last, so it wins.

## The fast preview path

Rendering a 24-megapixel photo on every mouse-move would be far too slow, so large pictures have two paths:

- **While dragging:** adjustments are applied to `previewBase`, a box-downsampled copy whose long side is at most 1024 px (`kPreviewMaxEdge`). This is quick enough to feel live. Pictures whose long side is within 1.5 times that (about 1536 px; `kPreviewMinRatio`) skip the preview entirely, because it would not be meaningfully cheaper. A 16-bit picture has a matching `previewBase16`, downsampled by the same factor, so the drag starts from the same precision as the full render that follows it.
- **When you let go:** the full-resolution render is handed to a background worker (see [threading.md](threading.md)). The preview stays on screen until the finished image arrives, then replaces it.

Masks are stored in normalized units (fractions of the picture), so the same mask renders identically on the small preview and on the full-size picture. The vignette is normalized too (distance from the center in corner units), so it matches as well; only the grain differs in its exact pixels, as explained above.

## 16-bit pictures

Most photos are 8 bits per channel, but a 16-bit PNG (and, later, a TIFF or RAW) holds more than a byte can. Literoom keeps that extra precision from the file to the export instead of throwing it away on open. It adds no new kind of edit: it is a second picture type that runs through the same float pipeline.

- **The type.** `Image16` (`core/image16.hpp`) is an RGBA picture with `uint16_t` samples, straight alpha, the same layout as `CanvasImage`. It is stored as 16 bits, not float, because a 24-megapixel picture is 192 MB at 16 bits and 384 MB as float, and the editor keeps several stages of it. The passes still compute in float, a block of rows at a time. An 8-bit sample widens to 16 bits as `v * 257`, which is exact (255 becomes 65535) and reversible, so a widened 8-bit picture gives the 8-bit result.
- **How a file is opened.** `io/image_source.hpp` is the one place that knows. `depthOf` reads the file's header, not its extension; today only a PNG whose header says 16 bits is *Wide*. `openUpright` first checks that a PNG is whole (`pngIsTruncated`: a file cut off before its last chunk is refused with a clear message on both platforms, because libpng would reject it but WIC would hand back blank rows), then reads a Wide file at full precision (`io/image_codec16.hpp`, because LiteUI's decoders reduce every PNG to 8 bits) and also makes its 8-bit form. Any other file goes through LiteUI's decoder; `decodeImage16` widens such a picture (`v * 257`), so the 16-bit pipeline starts from exactly the pixels the editor shows. The decode job, the batch export, the Sync job's size read and the thumbnail worker all go through it, so they agree on what a file is. If the full-precision read fails, the 8-bit decoder is tried instead, for the editor and the export alike. The camera's EXIF rotation is applied here, once (the caller passes the orientation in, so `image_source` does not depend on `image_meta`). `uprightSize` still decodes the whole file to learn the size; a header-only read for JPEG, TIFF or RAW is the planned improvement, and it belongs in that one function.
- **The twin chain.** For a Wide file, `EditorState` keeps `original16`, `oriented16`, `rotated16` and `source16` beside the 8-bit chain, built by the **same operations with the same arguments** (`core/wide_chain.hpp`, which mirrors `syncSource` and the geometry job line for line), so each twin has exactly the size of its 8-bit namesake. For an 8-bit file every twin is null. The 8-bit chain stays the picture everything is measured on (the histogram, scopes, the white-balance picker, Auto Tone, the mask tools); the twin only feeds the render.
- **Rendering.** `renderToDisplay(Image16, ...)` renders a 16-bit source to the 8-bit picture on screen; `renderToImage16` renders to 16 bits, from a 16-bit or an 8-bit source. The render worker and the geometry job take a `src16` when there is one, and a slider drag renders `previewBase16`. All of them are one template, `render_core::renderPipeline<Src, Out>`, so a 16-bit render is the same code as an 8-bit one: only the load (`samplesToLevels`) and the store (`levelsToSamples`) differ by sample type. A 16-bit picture keeps its extra precision through every stage, because rounding happens once, at the end (see [From sliders to pixels](#from-sliders-to-pixels)). The `view8` argument that the 16-bit entry points still take is no longer used: the neighbor effects read the source itself, and an 8-bit view of a 16-bit picture is never measured. A `RenderParams` built by hand with no float tone table (`toneF` null) falls back to the byte tables, rounded per value like the old pipeline.
- **What stays 8-bit.** The picture shown, thumbnails, the histogram and scopes, the segmenters, JPEG export. A 16-bit export is PNG only (the Export pane's 16-bit option); an 8-bit photo can be exported as 16-bit too, so a heavy edit comes out as smooth gradients instead of re-quantized steps.
- **Not pinned by the golden test.** The golden render test renders 8-bit pictures only, so `renderToDisplay` and `renderToImage16` are covered by `cases/render_core.inc` and `cases/export_render.inc` instead. Both halves of the 16-bit codec (WIC on Windows, libpng on Linux) are built and run; see [headers.md](headers.md#image_codec16hpp).

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
| Rectangle | Closed-form: a rotated rectangle (`max(|u|, |v|)` in its own axes), eased with a smoothstep; its row span is exact |
| Brush | Strokes are rasterized into a coverage map (cached), then read back |
| Polygon | The outline is filled (even-odd, anti-aliased) and feathered into a coverage map (cached), then read back like the Brush |
| Object | A coverage bitmap from a segmentation, warped, grown/shrunk and feathered (cached) |
| Luminosity | A table indexed by the pixel's brightness (256 entries, plus a finer 1/16-level one for the float pipeline) |
| Color Range | Distance from a sampled color in a cheap luma/chroma space |

Luminosity and Color Range are **not spatial**: they select by the picture's own values, so they need no `GeoFrame` mapping. They read the row's pixels as they are when the mask is reached (after the global edits and earlier masks) but *before* the mask's own edit, so a mask can never change what it selects.

The Masks list holds at most 16 masks (`kMaxMasks`) and each mask at most 6 parts (`kMaxMaskParts`). The brush, polygon and object maps are cached, and `clearMaskCaches()` empties all of them (brush maps, polygon maps, and the warped and grown object maps).

**Object selection.** An Object mask's coverage bitmap comes from a segmenter that runs on the object-selection worker (`SegmentRequest` / `SegmentResult` in `main.cpp`, a `LatestWorker`), and the choice between engines is made **at run time**, in `segmentObject()` in `main.cpp`:

- The first time it runs it tries to load MobileSAM (`mask/sam_segmenter.hpp`, ONNX Runtime) from `$LITEROOM_MODELS`, else a `models` folder next to the executable (CMake copies `models/` there for ONNX builds). If that works, the pane shows "Engine: MobileSAM".
- If it does not (the project was configured without `-DLITEROOM_ONNX=ON`, or the model files are missing or unusable), the whole session falls back to a basic color-grow selection (`segmentObjectWand`, built on `mask/seg_basic.hpp`) and the pane says why and which folder it looked in. The choice is made once and kept.
- `sam_segmenter.hpp` is always included. Without `LITEROOM_ONNX` its `sam::Segmenter` class has the same interface but never loads (it reports "built without LITEROOM_ONNX"), so the rest of the app has no `#if`.
- MobileSAM works on the 8-bit picture. Encoding is the slow step (roughly 0.5 to 2 seconds on a CPU), so it happens once per opened picture (keyed by the picture), as a prefetch with no click points, and each click then only runs the cheap decode (tens of milliseconds). Two encoder layouts are supported, detected from the model: a normalized 1x3x1024x1024 input, or raw 8-bit HWC at the resized size where the model normalizes itself. The segmenter is not thread-safe, which is why it lives on that one worker.
- The tests cover `sam::preprocess` (a plain function) and the no-ONNX class only; the real model path has no automatic test.

To add a new mask type, see [extending.md](extending.md).

## Presets

A **preset** is a saved *look*: the slider values, the four tone curves, the Color Mixer, Color Grading and the 3D LUT (by name, see [3D LUTs](#3d-luts)). It deliberately leaves out crop, rotation, straighten and masks. Those belong to one picture: a crop that suits one photo is wrong for another, and masks are stored against one original picture's size. Slider values, curves, the mixer, grading and a LUT are the same for every picture, so those are what a preset holds.

Presets follow the same rules as the rest of the engine:

- **A plain value, in a plain header.** `io/preset.hpp` holds the `Preset` struct and its file helpers. It has no window and no editor state, and a unit test needs no UI.
- **Saved the same way as edits.** A preset file reuses the sidecar's writers and readers, so it is validated and clamped just like a saved-edits file: a damaged or hand-edited preset can never put the editor in a state its own UI could not reach. See [sidecar-format.md](sidecar-format.md#presets-a-related-file).
- **Applied through `AdjustmentSet`.** `applyPreset` replaces the look and is one undo step, labelled `Preset: <name>` in History. A step stores a full copy of the look, so deleting the preset file later never changes a picture or its history. See [undo-and-history.md](undo-and-history.md#presets).
- **Nothing geometric is touched.** Because a preset holds no crop, rotation or masks, applying one never goes through the `GeoFrame`, the geometry worker or the mask caches. It only changes values the normal render path already reads, so the picture is redrawn like any other adjustment-only change: synchronously for small photos, on the background render worker for large ones (it is not a slider drag, so the fast preview path is not used).

The Presets pane in `main.cpp` is a thin layer over this: a list of the files in the presets folder, a name field and a Save button. The list is a cache, re-read when the pane is opened and after a save or delete, never per frame. For the per-function details see `preset.hpp` in [headers.md](headers.md#presethpp).

## 3D LUTs

A **3D LUT** (a `.cube` file) is a color lookup table: it maps every RGB value of the picture to another one, which is how film emulations and "looks" are usually shared. A picture can have one, with a strength slider (**LUT Amount**, 0 to 100). It is part of the look, so presets, Copy / Paste Settings, Sync Settings, snapshots, virtual copies and batch export all carry it. It adds no special case anywhere: it is built from the same pieces as everything else.

- **The table is a plain value.** `color/lut3d.hpp` holds `Lut3D` (a float grid of 2 to 65 points per axis, in the picture's own 0..255 scale, together with the file's input domain) and the lookup. The lookup is **tetrahedral**: each grid cell is cut into six tetrahedra along its main diagonal and the output is linear in each. Unlike trilinear interpolation this keeps the gray axis exactly gray (a LUT that maps grays to grays does not pick up a color cast between grid points) and reproduces any table that is affine inside a cell exactly.
- **A file is parsed strictly.** `io/lut_cube.hpp` reads 3D `.cube` files and nothing else: a wrong number of entries, a non-finite or absurd value, an unknown keyword, a keyword after the data, a size outside 2..65, or a 1D LUT is an error with a line number, never a guess. Numbers are read with `std::from_chars`, so a decimal-comma locale cannot change what a file means.
- **An edit stores a name, not a table.** The picture's LUT is a `LutRef`: the library name and a 64-bit hash of the file's bytes taken when it was chosen. A 65³ table is about 7 MB of text; stored in the sidecar it would be repeated in every one of the 200 undo steps, the 20 named snapshots, and every preset. A name and a hash are about 60 bytes. The price is that the file must still be in the library when the picture is opened, and the rules for that are below.
- **The library** (`io/lut_library.hpp`) is a folder of `.cube` files (`%APPDATA%\Literoom\luts` on Windows, `$XDG_CONFIG_HOME/literoom/luts` or `~/.config/literoom/luts` elsewhere), at most 100 files, each at most 16 MiB. *Importing* validates a file by parsing it, then copies its bytes unchanged under a cleaned name (a taken name gets `-2`, `-3`; the same bytes again reuse the file). `acquireLut(ref)` turns a `LutRef` into a shared, immutable table through a small mutex-guarded cache (four tables, failures remembered so a missing file costs one check, not one per slider frame), and says how it went: `Ok`, `HashChanged`, `Missing` or `Unreadable`. The table is parsed on the calling thread the first time it is needed (roughly 150 ms for the largest, 65³, size in an unoptimized build), then served from the cache.
- **What the render sees.** `AdjustmentSet::buildColorPass()` resolves the picture's LUT and freezes it with `makeLutPass` into the color pass as a `shared_ptr` to the immutable table plus the amount. So `RenderParams` stays cheap to copy however big the table is, and the worker threads never touch the library. `ColorPass::active` includes the LUT, so the fast path is not taken when it is in play.
- **When the library and the edit disagree.** `Ok` is the normal case. `HashChanged` (the file has the same name but other bytes) still applies the file; the status bar says so when the picture is opened. `Missing` and `Unreadable` apply nothing and keep the reference (the picture opens without the look, the LUTs pane says why, and putting the file back restores it). **Batch export is stricter**, like it is for an unusable sidecar: a picture whose LUT is in use (amount above 0) but missing or unreadable is *skipped with a reason* rather than exported without its look.
- **Which color space.** Only display-referred LUTs, made for ordinary sRGB / Rec.709 pictures, because the app edits JPEG and PNG pictures and does no color management. A LUT made for camera-log footage will look wrong on them. Nothing in a `.cube` file says so for certain, so import only *suggests*: a one-time, non-blocking status message when the name or title contains a log-curve word (`log`, `slog3`, `vlog`, `logc`, ...), when the input domain is not 0..1, or when mid-gray comes out far from mid-gray. The table keeps the file's domain, so log and RAW workflows can be supported later without changing the format.
- **Previews.** The LUTs pane shows each LUT applied to the open picture. That needs every LUT file read and a small render for each, so it runs on its own worker (`workers/lut_preview_worker.hpp`, see [threading.md](threading.md#the-lut-preview-worker-streams-results-for-the-newest-job)). A preview is the picture's own look with its LUT *replaced* by the one being previewed (never stacked), at the filmstrip's thumbnail size and without the neighbor effects. Previews bypass the cache (`loadLutFile`), so previewing a hundred LUTs cannot push out the one in use. The cache is emptied with `clearLutCache()` whenever the folder may have changed: on import and when a picture is opened (listing the folder afresh does not empty it).
- **The LUTs pane** (`main.cpp`, activity 12) is a thin layer: Import (a file picker, `importLutPaths`), a list of rows (each a preview, a name and a line about the file; click applies it as one undo step), the picture's current LUT with its LUT Amount slider, and Remove LUT. Previews are made again when the folder's list or the picture's look changes, not when the pane's own controls do, and never in the middle of a drag.
- **What is not supported (yet).** 1D LUTs, `.3dl` and Hald PNG files; camera-log input; a LUT per mask; deleting a LUT from inside the app (remove the file from the folder; pictures that used it then show the "missing" message).

## Copy, paste and sync settings

These reuse the preset machinery instead of adding a new kind of data. The thing being moved around is a `Preset` (the look: sliders, curves, Color Mixer, Color Grading and the LUT), never crop, rotation or masks, for the same reason presets leave them out.

- **Copy / Paste Settings** (Ctrl+Shift+C / V) work on the open picture. Copy keeps `currentPreset()` in a small in-memory clipboard in `main.cpp`; Paste calls `applyPreset`, so it is one undo step labelled `Preset: Pasted`. See [undo-and-history.md](undo-and-history.md#copy-and-paste-settings).
- **Sync Settings** (Ctrl+Shift+S) copies the open picture's look onto the pictures selected in the filmstrip (Ctrl+click toggles one, Shift+click selects a range, Ctrl+A selects all, a plain click clears the selection and opens a picture). The other pictures are not open, so there is no `AdjustmentSet` to change. Instead `sidecarSyncLook` (a pure function in `io/sidecar.hpp`) takes a picture's sidecar text, replaces only the look (the four groups a preset holds, and the LUT: a look without a LUT clears the picture's LUT), and returns the new text, so each picture keeps its own crop, rotation and masks. A background worker does the file work, because a picture with no sidecar must be decoded once to learn its size. See [sidecar-format.md](sidecar-format.md#syncing-a-look-onto-other-pictures) and [threading.md](threading.md#the-sync-worker-is-a-batch-that-is-never-superseded).
- **Batch export** (File > Export Selected... Ctrl+Shift+E, Export All Shown..., Stop Export; the same buttons are in the Export pane) renders every picked filmstrip picture from its own saved edits into one folder, using the Export pane's format, quality, metadata and resize settings. It is the missing last step after ratings, Sync Settings and virtual copies: each picture, and each virtual copy, is exported with its own look. See [Exporting a picture](#exporting-a-picture) and [threading.md](threading.md#the-export-worker-is-a-cancellable-fifo-of-whole-pipelines).

The engine stays UI-free: `sidecarSyncLook` has no file, window or editor dependency and is tested with plain strings. The selection flags and the clipboard are UI-side state in `main.cpp`.

## Snapshots and virtual copies

Both are ways to keep more than one version of a picture's edits, and both are built from the sidecar rather than from new kinds of data.

**Named snapshots** are saved states of *one* set of edits that you can return to. A snapshot is a name plus a whole `SidecarEdits` (look, curves, Mixer, grading, crop, rotation, straighten **and masks**), stored in a `snapshots` array in the picture's sidecar and written by the same functions as the edits. Unlike a preset it replaces everything, geometry and masks included. Restoring one goes through `AdjustmentSet::restoreEdits`, so it is one undo step labelled `Snapshot: <name>`. The list is kept in `state.snapshots` beside the rating, not in `AdjustmentSet`, because it is not an edit. See [undo-and-history.md](undo-and-history.md#named-snapshots) and [sidecar-format.md](sidecar-format.md#named-snapshots).

**Virtual copies** are *separate* sets of edits for the same picture file: a second sidecar (`photo.jpg.vc2.literoom.json`) and a second filmstrip cell. The trick that keeps this small is that the program already identifies a picture by one string, its path, so a copy simply gets its own path (`photo.jpg|vc2`: the `|` is a character Windows forbids in file names and that is very rare elsewhere). Everything keyed by path (the filmstrip index, thumbnail and sync results, the open picture) then tells copies apart with no other change; only the few places that need the real file translate it back with `vcImageOf()`, and `sidecarFileOf()` yields the copy's own sidecar. Switching between copies of the open picture reuses its pixels instead of decoding. The pure helpers are in `io/virtual_copy.hpp`. See [sidecar-format.md](sidecar-format.md#virtual-copies) and [threading.md](threading.md#virtual-copies-and-the-workers).

Each copy is a complete picture as far as the rest of the program is concerned: its own rating, flag, label, undo history (fresh on every open, as always) and snapshots.

## Exporting a picture

There are two export paths, and they are built differently on purpose.

- **Single export** (File > Export..., Ctrl+E) works on the open picture. A JPEG or an 8-bit PNG writes the picture already on screen (`state.image`), scaled to the long edge if Resize is on. A 16-bit PNG cannot reuse it, because the screen picture is 8-bit, so it renders `state.source16` (or `state.source`) again with the same edits through `renderToImage16`, then scales and writes it.
- **Batch export** (`exportPicture` in `workers/export_worker.hpp`) is handed only a picture's *path* and the Export settings, and rebuilds everything from the file and its sidecar. The open picture is exported like any other, so the app writes its sidecar to disk before the batch is queued. Only one batch runs at a time (`start` refuses while one is busy), pictures go in the order given, and each one ends with an outcome (written, skipped with a reason, or cancelled). Its pixel half is a pure function in `workers/export_render.hpp`: orient → straighten → crop from the untouched original, then the render (`renderForExport`), with each intermediate released as soon as the next step has what it needs, because the peak memory of a large picture is the biggest cost. An 8-bit or 16-bit original can come out as 8 or 16 bits.

Rules both paths share:

- **JPEG** goes through one shared function, `saveJpegFile`, that both paths call. It encodes to memory, so the source photo's metadata can be spliced in before the file is written (`embedSourceMetadata`) and then written atomically. The **ICC profile always** comes along (the editor converts no pixels, so a Display P3 picture exported without its profile would look dull everywhere); the **EXIF block** only when Keep Metadata is on. The EXIF splice resets the orientation (the pixels are already upright), erases the embedded thumbnail and fixes the size fields. GPS is carried over unless Remove GPS is on, which only counts when Keep Metadata is on. XMP and IPTC (captions, copyright, develop settings kept in XMP) are **not** carried over, and neither is any EXIF into a PNG (a PNG gets the source's color tags instead, see [Color-space tags](#color-space-tags)). A JPEG side is limited to 65,500 px (`kJpegMaxSide`; the limit is checked on the *final* size, so turn on Resize or export PNG for larger).
- **Names.** The output is `<stem>-edit.jpg` or `.png`, written to the chosen folder. A virtual copy is exported from the real file behind it (`vcImageOf`) and its stem gets `-copy<n>`.

Rules that make the batch stricter than the editor:

- An unusable sidecar (unreadable, not JSON, newer version, written for a different size) skips the picture with a reason. The editor would open it clean, but a batch must not quietly export an unedited picture.
- A picture whose LUT is in use but missing or unreadable is skipped as well, rather than exported without its look.
- Output names never collide: a taken name gets `-2`, `-3`, within the batch and against the folder (unless Overwrite is on).
- Cancelling drops what is queued and abandons the picture in flight. A picture that reached the encoder is finished; one already written stays written.

The per-function details are in [headers.md](headers.md#export_renderhpp) and [headers.md](headers.md#export_workerhpp); the worker rules are in [threading.md](threading.md#the-export-worker-is-a-cancellable-fifo-of-whole-pipelines).

## Color-space tags

The editor converts no pixels: it edits a picture's numbers as they are and treats them as sRGB. That is fine as long as an export says what its source said, and wrong when the tag is lost (a Display P3 picture exported as an untagged PNG looks dull in every other program). So the **tag travels with the picture**; nothing is ever invented, and an untagged source stays untagged. `io/color_tag.hpp` does the work on bytes, with no libpng and no zlib, so it behaves the same on both platforms.

| Source | Export | What happens |
|---|---|---|
| PNG | PNG | its `iCCP` / `sRGB` / `gAMA` / `cHRM` chunks are copied (`carryColorToPng`) |
| JPEG | PNG | its ICC profile becomes an `iCCP` chunk (stored, not compressed, so no compressor is needed) |
| JPEG | JPEG | the ICC profile is always spliced in by `embedSourceMetadata`, even with Keep Metadata off |
| PNG | JPEG | **not carried**: the PNG's profile is deflate-compressed and reading it would need an inflater; the status bar says so (single export) |

- **Fresh PNGs come out untagged.** libpng writes no color chunks, but WIC always writes its own `gAMA` and `cHRM`, so both PNG encoders strip them on Windows (`insertColorIntoFile` with an empty `PngColor`); the source's tags, if any, are then the only ones the file carries.
- **Both export paths do it.** The single export and the batch `exportPicture` call `carryColorToPng` after writing a PNG (8- or 16-bit), and ignore its result: the export is valid either way.
- **The Info pane and the status bar** show what the open file says (`state.colorTag`, from `colorTagOf`, read once per opened picture): for example "sRGB (embedded profile)" or "Display P3 (embedded profile, not sRGB)". A file tagged as something other than sRGB is still shown as sRGB; the status says it was not converted.
- **Everything is read defensively.** A chunk with a wrong size or a bad CRC is skipped, an ICC profile over 16 MiB is refused, and a PNG whose first image data lies beyond the first 1 MiB is left untagged rather than rewritten.

The per-function details are in [headers.md](headers.md#color_taghpp).

## Ratings, flags, color labels and camera info

A picture's **rating** (0 to 5 stars), **flag** (pick / reject) and **color label** are `PhotoMeta` (`io/photo_meta.hpp`), a plain value with its JSON form and the filmstrip filter. They are **not edits**: they never go through `AdjustmentSet`, so there is no undo step and no History label, and changing one leaves `adjust.version()` alone. They are saved in the sidecar's top-level `meta` object.

- **The open picture** keeps its value in `state.meta`, mirrored into its filmstrip entry, and saved with the rest of the sidecar when `state.metaDirty` is set.
- **A command** (`applyMetaPatch`, a `MetaPatch` of only the fields it changes) changes the open picture at once. If other pictures are selected in the filmstrip it changes them too, on the **Sync Settings worker** (a `SyncJob` with `metaOnly` set, patching each sidecar with `sidecarPatchMeta`), and the strip updates when the batch finishes. The open picture is never in that batch, so it cannot race with autosave. "Choose the value it already has to clear it" is decided from the open picture and then applied to all of them alike.
- **Saved only when it differs from the default**, so a picture that was never rated has no `meta` key at all. Loading is as strict as for edits: an out-of-range rating is clamped and an unknown flag or label name becomes "none". Any code that rewrites a sidecar (Sync Settings, the meta patch) must write `meta` and the snapshots back, or it would drop them; `sidecarReadForRewrite` hands them to it for that reason.
- **The filmstrip learns other pictures' values from the thumbnail worker.** `ThumbWorker` reads each picture's sidecar with `sidecarScan`, which returns whether it holds real edits (the dot) and its `PhotoMeta`, without loading the edits. A sidecar that exists but is unreadable or over 64 MB counts as edited, with no meta. A virtual copy's thumbnail is not decoded at all (it shows its original's); only its sidecar is read.
- **The filter bar** at the top of the filmstrip holds a `MetaFilter` (minimum rating, accepted flags, accepted labels): every switched-on criterion must hold, and within flags or labels any chosen value matches. Choosing the rating already set switches that criterion off.

The **Info pane** shows the open picture's camera data. `io/exif.hpp` does the byte-level work on a buffer, bounds-checking every read (a damaged file just yields "no metadata"), and `io/image_meta.hpp` is the glue to files: `photoInfoOf` fills an `exif::PhotoInfo` (make, model, lens, date, ISO, shutter, aperture, focal length) from the first 256 KiB of a JPEG, held in `state.photoInfo` and read through `currentPhotoInfo()`; `exifOrientationOf` gives the rotation that `image_source` applies; `embedSourceMetadata` is the splice used by JPEG export. All three are JPEG only (another format gives an empty result), GPS is never read for the Info pane, and a virtual copy's path is resolved to the real file first. They are UI-free and read the file, never the editor state. `io/minijson.hpp` is the small JSON reader and writer under the sidecar, presets and photo meta, so none of them depends on an outside library.

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
- **What it leaves alone.** Brightness, Highlights, Shadows, the Levels sliders (Black Point, White Point, Gamma), curves and masks. Auto Tone is a global edit: masks keep their own sliders.
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

All editor state is one global `state` (an `EditorState`) in `main.cpp`, touched only by the UI thread. The edit values live in `state.adjust` (an `AdjustmentSet`), which also owns the undo history. See [undo-and-history.md](undo-and-history.md). Edits are written to disk by [the sidecar](sidecar-format.md). The 16-bit twins (`original16`, `oriented16`, `rotated16`, `source16`, `previewBase16`) are null for an 8-bit file. The open picture's rating, flag and label are `state.meta`, its camera data is `state.photoInfo`, what the file says about its color space is `state.colorTag`, and the filter bar's state is `state.film.filter`; none of them is an edit. Other UI-side state is the Presets pane's list of preset files (a cache of what is in the presets folder), the Copy Settings clipboard (one `Preset` held in memory) and the filmstrip's multi-selection (a flag on each `FilmEntry`, plus the anchor of a Shift-click range). The white-balance picker's armed flag (`state.wbPicking`) is plain UI state and is never saved, and so are the pinned canvas's view (`state.scopeKind`) and its scope picture (`state.scopeImage`). The LUTs pane keeps two pieces of UI-side state: the list of the library's files with their previews (`gLuts`), and a record of what the previews were made for (`gLutBuilt`); a picture's own LUT lives in `state.adjust` (`AdjustmentSet::lut()`), and the library's table cache is a mutex-guarded global in `io/lut_library.hpp`. Presets themselves live in files, not in `state`; applying one, or pasting the clipboard, copies its values into `state.adjust`. Sync Settings never touches `state.adjust` for the pictures it writes to.

## The rule about threads

Workers never read `state` and never call LiteUI. They receive an immutable picture and a copy of the parameters, and they return a value. This is what makes the threading simple enough to reason about. Details in [threading.md](threading.md). There are two families of worker. The **"latest wins"** ones (`LatestWorker` in `workers/latest_worker.hpp`: the render, geometry, decode, object-selection and sync workers) bump a generation number on every request, so only the newest request can win: a newer one replaces a job that is still waiting, a running job polls the number and abandons itself, and a result that was invalidated while it finished is never delivered. The **queue** ones (`ThumbWorker`, `ExportWorker`) are FIFOs, because you want every item: `setQueue` / `start` replace what is still to do, the item in flight finishes (export can also abandon it with `cancel`), and results accumulate until the UI thread collects them. `LutPreviewWorker` is a mix: it streams one result per LUT but is replaced as a whole by a newer job (an epoch counter that the preview render polls). Every one is driven from the UI thread only, and the UI is woken by a `notify` callback after a result. Presets need no worker: saving, loading and listing read or write a few hundred bytes, so they run on the UI thread like the sidecar's small writes. Sync Settings does need one (it can decode pictures and write many files) and gets its look as a copied value when the job is built. Opening a file has a worker as well (`DecodeWorker`): it returns the 8-bit picture and, for a 16-bit file, the `Image16` at full precision, and nothing about the editor changes until the UI thread adopts the result. Batch export needs one too, and goes a step further: its job is handed only a picture's *path* and the Export settings, and rebuilds everything else (pixels, edits, geometry, render parameters) from the file and its sidecar, with a local `AdjustmentSet`. That is possible because the edits are fully described by the sidecar and the render chain is a set of plain functions.

## Tests and the safety net

The engine has no UI, so every header has its own test file: 54 `.inc` files in `tests/cases/` (one per engine header, plus `lut_render`, which tests the LUT through the renderer), all included into one translation unit, `tests/literoom_test.cpp`, that is built as the single Catch2 executable `literoom_test`. Two more programs stand apart (the golden render test and the LUT-pane test, below). They are built and run from the **liteui root**, not from `examples/literoom`, because literoom's `CMakeLists.txt` refuses to configure on its own. How to run them, what each one covers and where a new one belongs are in [testing.md](testing.md).

Four things shape how the code is organized:

- **Layers are enforced by the build.** `src/` is the only include path (the test targets add `tests/` for the `.inc` files and `check.hpp`), so a test includes a header exactly as the app does. Headers are picked up with a `CONFIGURE_DEPENDS` glob, and a new test is a new `.inc` plus one `#include` in `literoom_test.cpp`: nothing to edit in `CMakeLists.txt`.
- **The golden render test.** It compiles `src/main.cpp` itself (with `main()` renamed) and fingerprints every render scenario: sliders, masks, geometry, undo, the sidecar, the histogram. It is the net for refactoring `main.cpp`, which the unit tests cannot reach. The fingerprints are float-exact, so there is one golden file per compiler and configuration (`tests/golden/render_<compiler>_<config>.txt`), the test never runs under sanitizers, and a missing file makes it report "skipped" (exit code 77) rather than failed (`ctest -LE golden` leaves it out). It renders 8-bit pictures only. Two cases inside `literoom_test` (`neighbor_fx` and `raster_ops`) are float-exact fingerprints of their own; they are tagged hidden (`[.]`) so CTest leaves them out, and are run by hand.
- **The LUT-pane test.** `tests/lut_pane_test.cpp` also compiles `main.cpp` itself and drives the LUTs pane's logic against a real temp library, the real preview worker and the real renderer, on the `check.hpp` harness. Its CTest label is `threaded`, like the Catch2 cases that start threads (`ctest -L threaded --repeat until-fail:50` shakes out timing flakiness).
- **`render_bytes.hpp` is a test reference.** `cases/render_core.inc` compares the float pipeline against it, and it holds the only tests of the 16-bit paths, so deleting it removes that reference.