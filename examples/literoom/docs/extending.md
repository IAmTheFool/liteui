# Extending Literoom

Three common extensions are covered here: adding a **slider**, adding a **mask type**, and adding **look data that is not a slider** (the 3D LUT is the worked example). The first two were designed so that most of the program picks the new thing up automatically; the third is a checklist, because a new kind of data touches every place that stores or copies a look.

## Adding a slider

Every slider in the Adjust pane is one entry in the `kAdjustmentSpecs` table in `core/adjustments.hpp`. The same table also drives each mask's own sliders, the History labels, and the saved-edits file.

### Step 1: add an `AdjId` and a table entry

1. Add a name to the `AdjId` enum, **before `Count`**. The order of the enum must match the order of the table.
2. Add one entry to `kAdjustmentSpecs` in the same position. It has an id, a label (the text shown in the UI), a minimum and a maximum, an `apply` function, and a stage.

A compile-time check (`static_assert`) fails the build if the table and the enum do not line up, if a range is empty, or if a Tone entry has no `apply`. You cannot forget a half silently.

**Keep the table grouped by stage: all Tone entries, then all Color entries, then all Detail entries, then all Effects entries.** The Adjust pane (and each mask's Basic list) adds its "Color" and "Detail" headers when it reaches the first entry of that stage, so a Color slider placed after a Detail entry would show up under the Detail header. The Adjust pane adds an "Effects" header the same way; the mask lists skip the whole Effects stage, because Vignette and Grain are global only. The one exception is **Filters**: they are Color-stage entries, and `main.cpp` adds a "Filters" header (in both the Adjust pane and each mask's Basic list) when its loop reaches `AdjId::Greyscale`. Keep new filters in that block, after `Cross Process` and before `LUT Amount`. **LUT Amount** is the other exception: a Color-stage entry (`apply = nullptr`) that sits right after the Filters and closes the Color stage. It is the strength of the 3D LUT, and the LUTs pane carries its slider, so `main.cpp` skips it *by id* in both slider loops (the Adjust pane and each mask's Basic list; a mask has no LUT). A new global-only Color slider follows the same recipe: a table entry, a skip-by-id in those two loops, and no value read for it in `buildMaskPasses`.

### Step 2: pick the stage

The stage decides where the slider acts and what else you need to write.

| Stage | Use it when | What you must provide |
|---|---|---|
| **Tone** | The result for a pixel depends only on that one channel's value (like Exposure or Contrast). | Just an `apply(float channel, float value)` function. It is composed into the shared lookup table automatically. |
| **Color** | You need per-channel gains or cross-channel math (like Saturation or Hue). | Set `apply` to `nullptr` and teach the color pass about it (below). |
| **Detail** | You need neighboring pixels (like Texture or Blur). | Set `apply` to `nullptr` and teach the neighbor effects about it (below). |
| **Effects** | You need the pixel's *position* in the picture, and the effect belongs on the finished picture (like Vignette or Grain). Global only. | Set `apply` to `nullptr` and teach `film_fx` about it (below). |

**Tone sliders** need nothing else. Keep these rules in mind:

- `apply` returns an *unrounded, unclamped* value. The table builder rounds and clamps after each slider.
- It must be pointwise per channel. If it needs neighbors, it is not a Tone slider.
- The order of Tone entries in the table is the order they are applied. Each one runs on the previous one's output.
- If your curve should stay monotonic (never make darker input brighter than lighter input), check it at both extremes of the range. The existing Highlights/Shadows/Whites/Blacks comments show how the margin was worked out. A linear curve is monotonic when its slope stays positive over the whole range (Black Point and White Point cap their range so `255 - v` never nears 0); a power curve is monotonic for any positive exponent (Gamma).
- Return `x` unchanged when `value == 0`. The identity is then exact, with no float noise from `pow(x, 1)` or `x / 255 * 255`.
- Floor any input at 0 before `pow` or `sqrt`, so an out-of-range input cannot give NaN.
- Black Point, White Point and Gamma (the Levels sliders) are a worked example. They sit last in the Tone block, after Blacks. New Tone sliders may go anywhere in the Tone block, but remember that it changes the processing order for the sliders after it.

For a Tone slider, also add:

- A test of its anchor points, the identity at 0, and monotonic tables at both ends of the range and with the neighbors that run before and after it (see `testLevels` in `adjustments_test.cpp`).
- To the golden test: nothing, unless the slider's half-range value would clip or flatten the picture in the kitchen-sink scenarios. Then add it to `isFixedSink` / `fixedSinkValue` (as Levels are). If you insert it in the middle of the table, extend `sinkPos` so older sliders keep their signs. Re-record as in [testing.md](testing.md#checking-a-re-record).

**Color sliders** also need:

1. A field in `ColorPass` and the math in `makeColorPass` / `applyColorPass` (`color/color_pass.hpp`). `makeColorPass` takes the slider values as parameters, so add a parameter.
2. The new value passed in at **both** call sites: `AdjustmentSet::buildColorPass()` for the global edit, and `buildMaskPasses()` in `edit/render_params.hpp` for masks. Forgetting the global one is easy to miss: the slider shows up and saves to the sidecar, and masks work, but the global edit does nothing. (A slider that is global only, like LUT Amount, is passed at the global call site only; `buildMaskPasses` never passes it, and a mask that carries a value for it is dropped as one with no adjustments.)
3. The new slider included in `cp.active`, so the fast path is skipped when it is non-zero.

**Filters** (Greyscale, Sepia, ...) are Color sliders with a shortcut. To add another one:

1. Add an `AdjId` and a table entry in the Filters block of `core/adjustments.hpp`, range 0..100, stage `Color`, `apply = nullptr`.
2. Add a field to `FilterValues` and `FilterPass`, set it in `makeFilterPass` (and in `FilterPass::any`), and add its step to `applyFilterPixel` in `color/color_pass.hpp`. Make it `lerp(pixel, filtered, strength)` so 0 changes nothing, and keep it before the final clamp.
3. Read it in `filterValuesFromAdj` (`edit/render_params.hpp`). Both call sites already use that helper, so they need no change.
4. Nothing else in the app: `cp.active`, masks, undo, History, the sidecar and the Adjust pane follow from the table.
5. Not automatic, the tests: add the filter to `kFilterFields` and give it a case in `color_pass_test`, to the label list in `testFilterBlock` (`adjustments_test`), to `filterValuesFromAdj`'s check in `render_params_test`, and to the filter lists in `adjustment_set_test`. If it goes after `Cross Process` (so before `LUT Amount`), move the upper bound of `isFilter` and the end of the range in `sinkPos` in `golden_render_test.cpp`, and the `LUT Amount`-follows-`Cross Process` check in `testFilterBlock`.
6. Re-record the golden file and check it as described in [testing.md](testing.md#checking-a-re-record). Give the new filter a strength in the kitchen-sink scenarios that keeps the picture's information (see `kSinkFilterStrength`).

**Detail sliders** also need:

1. A new gain in `neighbor_fx::Gains` and the math in `detail/neighbor_fx.hpp` (or `detail/detail_layer.hpp`). Include it in `Gains::any()`: when `any()` is false the effects are skipped, and a mask with only that slider set is dropped by `buildMaskPasses`. If your effect needs a new blurred plane, add it to the set that `acquire` builds, and to `need()`.
2. The new value passed to `neighbor_fx::gainsFromValues` at **both** call sites: `AdjustmentSet::buildRenderParams()` and `buildMaskPasses()`.

**Effects sliders** (Vignette and Grain are the examples) also need:

1. A field in `film_fx::Params` and the math in `film_fx::apply` (`detail/film_fx.hpp`). `paramsFromValues` takes the slider values as parameters, so add a parameter and sanitize it there (clamp, NaN becomes 0). Include it in `Params::any()` (through `hasVignette()` / `hasGrain()` or a new one), because when `any()` is false `renderWith` skips the pass.
2. The new value passed in `filmFromAdj` (`edit/render_params.hpp`). There is only one call site, `AdjustmentSet::buildRenderParams()`, because masks never get an Effects slider.
3. Nothing in the mask code: `main.cpp` skips `Stage::Effects` in both slider loops, and `buildMaskPasses` never reads these values (a mask that carries one, from a hand-edited sidecar, is dropped as one with no adjustments).
4. Keep it a **pure function of position and the sliders**. Anything random must be a hash of (x, y), never a generator or a counter, or the preview, the background render and the export would stop agreeing. Take positions from the whole picture, not from the block: `apply` gets the index of its first pixel for that reason.
5. Anything that renders "the picture before a mask" (the three `rp.film = {}` lines in `main.cpp`) already clears the whole `film` value, so a new Effects slider needs nothing there. A new effect that is *not* part of `film_fx` but also runs after the masks would have to be cleared at those three sites too.
6. Tests: add a case to `film_fx_test`, to `filmFromAdj`'s check in `render_params_test`, and to the label and range lists in `testEffectsBlock` (`adjustments_test`). If its half-range value would wipe out the picture in the kitchen-sink scenarios, give it a fixed value in `fixedSinkValue` (the block is found with `isEffects`; move its upper bound if you add after `Grain Roughness`). Then re-record the golden file and check it as described in [testing.md](testing.md#checking-a-re-record).

### Look data that is not a slider (the 3D LUT)

The 3D LUT is a name that points at a file (`LutRef`) plus a strength slider (LUT Amount). The slider needs nothing special beyond the table entry above; the *reference* is look data that the slider table knows nothing about, so every place that stores, copies or applies a look has to be taught about it. This is the full list, and a missed entry is silent: the data is simply dropped there.

1. **The type and its operations**, in the layer where it belongs: the table, the lookup and the reference are plain values in `color/lut3d.hpp`; anything that reads files is in `io/` (`lut_cube.hpp`, `lut_library.hpp`). A name that ends up in a file name must be made safe on every path it can arrive by (`sanitizeLutName`).
2. **The sidecar.** A field of `SidecarEdits`, written by `sidecarEditsToJson` **only when it is not the default** (so an untouched picture's file, and the filmstrip's "has edits" dot, do not change) and read, validated and clamped by `sidecarEditsFromJson` (`sidecarLutToJson` / `sidecarLutFromJson` here). A hostile or damaged value must give the default, not fail the whole file. Snapshots and virtual copies then carry it with no more code.
3. **The edit state.** A member of `AdjustmentSet`, in `Snapshot`, `snapshot()`, `restore()`, `reset()`, `loadJson()`, `currentEdits()` and `restoreEdits()` (including its equality check), its own setter that pushes one step and **sets the label** (`setLut`: `LUT: <name>` / `Remove LUT`), and a branch in `describeStep` as a safety net for a step that has no stored label.
4. **The look.** A field of `Preset` (and its `==`), `presetToJson` / `presetFromJson`, `currentPreset` / `applyPreset`, and a **required** parameter of `sidecarSyncLook`. It is required on purpose: a defaulted parameter would let a caller silently clear the target pictures' LUT.
5. **The render.** Freeze it into `RenderParams` in a form that is cheap to copy (a `shared_ptr` to an immutable table, never the table by value: `buildRenderParams()` runs on every slider frame), make `ColorPass::active` true when it is in play, and keep an inactive or unavailable value byte-exact to "not there". Resolve any file once, on the calling thread, through a cache (`acquireLut`); a render must never read a file.
6. **Batch export.** `exportLoadEdits` decides what an unusable reference means for a batch (here: a picture whose LUT is in use but missing or unreadable is skipped with a reason; a changed file is used as it is). `export_worker_test` checks both.
7. **The UI.** Skip its slider by id where it should not appear (see Filters above), list its sources, and report problems where the user will see them (the LUTs pane's status line, and a status-bar note when a picture is opened).
8. **Tests.** The value and its parser, the library, `sidecar_test` (round trip, hostile input, Sync), `preset_test`, `adjustment_set_test` (setter, undo, presets, snapshots, save and load), `render_params_test` (masks ignore it), `color_pass_test` (order and blending), a test through the real `renderWith` that proves where it sits among the stages (`lut_render_test`), and golden scenarios (`lut/` in `golden_render_test`).
9. **Names that Windows headers define.** `windows.h` and the headers it pulls in `#define` short common words (`small`, `near`, `far`, `interface`, `IN`, `OUT`, `ERROR`), and they apply inside your header too. A local variable named `small` compiled everywhere except under MSVC, where it became `char`. If a new file builds on GCC and fails on MSVC with an error about `char`, this is why. To catch it before MSVC does, compile the new tests with `-Dsmall=char -Dnear= -Dfar=`.

### What you get for free

Once the table entry exists, these need no changes:

- The Adjust pane (it loops over the table, adding the Tone, Color and Detail headers itself).
- Each mask's Basic sliders (same table).
- Undo, redo, and coalescing of slider drags into one step.
- The History label ("Name +20").
- The saved-edits file.
- **LUT Amount is a slider like any other** in the table (saved under `LUT Amount`, `LUT Amount +60` in History, kept in presets and snapshots), with the two exceptions above: the Adjust pane and the mask lists skip it by id. The *LUT itself* is not a slider (see the section above).
- **Vignette and Grain are sliders like any other** (an "Effects" header in the Adjust pane, `Vignette -40` in History, saved under their labels), with one exception: a mask has no Effects sliders.
- **Presets.** A preset stores sliders by label through the same table, so a new slider is saved in presets and applied by them with no preset code. A preset saved before the slider existed simply has no entry for it, which reads as 0.
- The drag-preview path.
- **Snapshots and virtual copies.** A snapshot stores the whole `SidecarEdits` and a virtual copy has a whole sidecar, so a new slider is saved, restored and kept per copy with no extra code. A snapshot saved before the slider existed has no entry for it, which reads as 0.
- **Batch export.** The export worker loads each picture's sidecar through `AdjustmentSet::loadJson` and renders it with `buildRenderParams()`, so a new slider is exported with no extra code. A picture whose sidecar predates the slider has no entry for it, which reads as 0. (The golden test does not cover this path; `export_worker_test` checks it against a direct `renderWith`.)
- **Copy, Paste and Sync Settings.** They move a `Preset`, which stores sliders by label through the same table, so a new slider is copied, pasted and written into other pictures' sidecars with no extra code. A picture's sidecar that predates the slider has no entry for it, which reads as 0.

### Two things to watch

- **The white-balance picker repeats the Temperature and Tint gain formulas.** `solveWhiteBalance` in `color/color_pass.hpp` is the inverse of the gains in `makeColorPass` (the constants 0.3, 0.1 and 0.2). If you change how Temperature or Tint act, change the solver too. The `color_pass` test (`testWhiteBalancePicker`) runs the picked values back through `makeColorPass`, so it fails if the two drift apart. A new Color slider does not need anything here unless it acts *before* the white-balance gains: the picker samples the picture after the tone tables and before the color pass.
- **The label is the saved-edits key.** Sliders are written to the sidecar, and to every saved preset, under their `label`, and zero-valued sliders are left out. Renaming a label silently orphans that slider's value in existing sidecars and presets.
- **Applying a preset replaces the whole look.** An older preset has no entry for a slider you add later, so applying it sets that slider back to 0. That is the intended "replace" behavior (see [undo-and-history.md](undo-and-history.md#presets)), not a bug in the new slider.
- **Sync Settings replaces the whole look too.** Like applying a preset, it sets every look slider of the target picture to the source's value, including back to 0. That is the intended "replace" behavior.
- **A new kind of look data needs four updates.** If you add a new group to the look (not a slider: a new list, table or reference like the curves or the LUT), add it to `Preset`, to `presetToJson` / `presetFromJson`, to `AdjustmentSet::currentPreset` / `applyPreset`, and to the parameters of `sidecarSyncLook`, or presets, Copy, Paste and Sync would silently skip it. See [Look data that is not a slider](#look-data-that-is-not-a-slider-the-3d-lut) for the full list.
- **A new kind of edit must be loaded by `exportPicture`.** Batch export builds its pipeline from the saved edits alone (`exportLoadEdits`, then orient, straighten, crop, `renderWith`), mirroring `runGeometryJob`. A new geometry step or a new input to the render has to be added there as well as to the geometry worker, or batch export would differ from the editor. `export_worker_test` compares against the same chain, so extend it when you change the pipeline.
- **A new field of `SidecarEdits` (a new kind of edit) must be handled by `AdjustmentSet::currentEdits()` and `restoreEdits()`.** Snapshots save it automatically, because they are written by `sidecarEditsToJson`. But `currentEdits()` builds the struct field by field and `restoreEdits()` assigns and compares field by field, so a field missing there is silently dropped from snapshots or left unchanged by a restore. Add it to the equality check in `restoreEdits` too, or restoring the same state would add a step.
- **Effects sliders are offsets from a default look.** Vignette Midpoint, Vignette Feather and Grain Roughness are 0 at their default (midpoint and feather halfway, roughness an even blend), so the neutral value of every slider is still 0 and a file that only sets "Vignette" gives a normal vignette. Keep that rule for a new Effects slider, or zero values stop being left out of the sidecar and an old file would load with a different look.
- **A new slider at 0 must change nothing.** Default values are 0, and the golden render test (see [testing.md](testing.md)) fingerprints the output. If adding a slider changes an *existing* fingerprint while the new slider is untouched, something is wrong.

## Adding a mask type

A mask kind is one alternative in a `std::variant`. The render pipeline, undo, the preview path, the worker threads, export, and the per-mask sliders all work on the variants, so they need no changes. (Color Range, Rectangle and Polygon were added exactly this way.)

You need four things:

1. **A shape struct** with its parameters (like `RadialShape`). What it must have:
   - `operator==` (`MaskPart`'s defaulted comparison needs it).
   - `toSource(const GeoFrame &) const`: stored form to the form in the current picture. If your shape is not spatial (it selects by the picture's own values, like Luminosity), it simply returns itself.
   - `prepare(int W, int H) const`: resolves it into the Eval below.
   - `applySourceEdit(before, after, frame)` if the UI edits it through `editShapeInView` (sliders or handles). It should touch only the fields that changed.
   - `static fromSource(...)` only if the shape is placed by position on the canvas, as Radial and Linear are (Brush has `pointFromSource` and `sizeFromSource` instead; Object, Luminosity and Color Range have none).
2. **An `Eval` struct** (like `RadialEval`): the shape resolved for one concrete image size. It needs:
   - `rowSpan(y, W, x0, x1)`: which pixels `[x0, x1)` of row `y` can be non-zero? It may return a superset. Returning "all of them" is always correct, just slower.
   - `rowWeights(y, x0, x1, out, pix)`: fill in the weights (0 to 1) for those pixels. `pix` is the row's current RGBA pixels, for shapes that depend on them.
3. **One more enumerator in `MaskType`** (before `Count`; the enum and the variants live in `mask/mask.hpp`) and **one more alternative in `MaskShape` and `MaskEval`**, in the same order, plus one more line in `maskTypeName`. If the weights read the picture's pixels, update `maskTypeUsesPixels` too. `maskShapeToSource`, `prepareMask`, `maskRowSpan` and `maskRowWeights` are generic `std::visit` functions and need no per-kind line.
4. **The UI** (all in `src/main.cpp`):
   - `newMaskShape(MaskType)`: the starting shape of a new mask of this kind.
   - A button in the Masks pane that calls `addMask(MaskType)`, and a "+ Part: ..." button that calls `addPart(MaskType)`.
   - Its Shape controls panel, shown while `selectedPartType()` is your kind.
   - Its overlay and handles (`paintMaskOverlay`, `maskPress` and friends).

Put the shape and its Eval in their own header in `mask/`, the way `mask_range.hpp` does, and include it from `mask/mask.hpp`, where the variants are declared. Add the new header to `LITEROOM_HEADERS` in `CMakeLists.txt`. It may include headers from `core/`, `color/` and other `mask/` headers, but never from `edit/` or `render/`, which sit above `mask/`. Its unit test goes in the first `foreach` list of `CMakeLists.txt` (see [testing.md](testing.md)).

Also teach the **sidecar** about it: `sidecarShapeToJson` and `sidecarShapeFromJson` in `io/sidecar.hpp`. The loader validates and clamps everything, so keep that property for your new fields. Presets do not store masks, so they need no change.

### Rules for the Eval

- Keep the per-row loops plain (raw pointers, no `std::` helpers). The project does this deliberately so Debug builds stay usable.
- If building the Eval is expensive (a bitmap, a blur), cache it, like the Brush and Object masks do, so a slider drag does not start over. Caches are small, memoized lists protected by a mutex, because renders run on worker threads.
- Never modify a cached map after it is shared. Maps are immutable.
- If you add a cache, empty it in `clearMaskCaches()` (`mask/mask.hpp`) so one call still clears every mask cache.