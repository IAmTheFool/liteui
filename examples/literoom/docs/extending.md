# Extending Literoom

Two common extensions are covered here: adding a **slider** and adding a **mask type**. Both were designed so that most of the program picks the new thing up automatically.

## Adding a slider

Every slider in the Adjust pane is one entry in the `kAdjustmentSpecs` table in `adjustments.hpp`. The same table also drives each mask's own sliders, the History labels, and the saved-edits file.

### Step 1: add an `AdjId` and a table entry

1. Add a name to the `AdjId` enum, **before `Count`**. The order of the enum must match the order of the table.
2. Add one entry to `kAdjustmentSpecs` in the same position. It has an id, a label (the text shown in the UI), a minimum and a maximum, an `apply` function, and a stage.

A compile-time check (`static_assert`) fails the build if the table and the enum do not line up, if a range is empty, or if a Tone entry has no `apply`. You cannot forget a half silently.

**Keep the table grouped by stage: all Tone entries, then all Color entries, then all Detail entries.** The Adjust pane (and each mask's Basic list) adds its "Color" and "Detail" headers when it reaches the first entry of that stage, so a Color slider placed after a Detail entry would show up under the Detail header.

### Step 2: pick the stage

The stage decides where the slider acts and what else you need to write.

| Stage | Use it when | What you must provide |
|---|---|---|
| **Tone** | The result for a pixel depends only on that one channel's value (like Exposure or Contrast). | Just an `apply(float channel, float value)` function. It is composed into the shared lookup table automatically. |
| **Color** | You need per-channel gains or cross-channel math (like Saturation or Hue). | Set `apply` to `nullptr` and teach the color pass about it (below). |
| **Detail** | You need neighboring pixels (like Texture or Blur). | Set `apply` to `nullptr` and teach the neighbor effects about it (below). |

**Tone sliders** need nothing else. Keep these rules in mind:

- `apply` returns an *unrounded, unclamped* value. The table builder rounds and clamps after each slider.
- It must be pointwise per channel. If it needs neighbors, it is not a Tone slider.
- The order of Tone entries in the table is the order they are applied. Each one runs on the previous one's output.
- If your curve should stay monotonic (never make darker input brighter than lighter input), check it at both extremes of the range. The existing Highlights/Shadows/Whites/Blacks comments show how the margin was worked out.

**Color sliders** also need:

1. A field in `ColorPass` and the math in `makeColorPass` / `applyColorPass` (`color_pass.hpp`). `makeColorPass` takes the slider values as parameters, so add a parameter.
2. The new value passed in at **both** call sites: `AdjustmentSet::buildColorPass()` for the global edit, and `buildMaskPasses()` in `render_params.hpp` for masks.
3. The new slider included in `cp.active`, so the fast path is skipped when it is non-zero.

**Detail sliders** also need:

1. A new gain in `neighbor_fx::Gains` and the math in `neighbor_fx.hpp` (or `detail_layer.hpp`). Include it in `Gains::any()`: when `any()` is false the effects are skipped, and a mask with only that slider set is dropped by `buildMaskPasses`. If your effect needs a new blurred plane, add it to the set that `acquire` builds, and to `need()`.
2. The new value passed to `neighbor_fx::gainsFromValues` at **both** call sites: `AdjustmentSet::buildRenderParams()` and `buildMaskPasses()`.

### What you get for free

Once the table entry exists, these need no changes:

- The Adjust pane (it loops over the table, adding the Tone, Color and Detail headers itself).
- Each mask's Basic sliders (same table).
- Undo, redo, and coalescing of slider drags into one step.
- The History label ("Name +20").
- The saved-edits file.
- The drag-preview path.

### Two things to watch

- **The label is the saved-edits key.** Sliders are written to the sidecar under their `label`, and zero-valued sliders are left out. Renaming a label silently orphans that slider's value in existing files.
- **A new slider at 0 must change nothing.** Default values are 0, and the golden render test (see [testing.md](testing.md)) fingerprints the output. If adding a slider changes an *existing* fingerprint while the new slider is untouched, something is wrong.

## Adding a mask type

A mask kind is one alternative in a `std::variant`. The render pipeline, undo, the preview path, the worker threads, export, and the per-mask sliders all work on the variants, so they need no changes. (Color Range was added exactly this way.)

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
3. **One more enumerator in `MaskType`** (before `Count`) and **one more alternative in `MaskShape` and `MaskEval`**, in the same order, plus one more line in `maskTypeName`. If the weights read the picture's pixels, update `maskTypeUsesPixels` too. `maskShapeToSource`, `prepareMask`, `maskRowSpan` and `maskRowWeights` are generic `std::visit` functions and need no per-kind line.
4. **The UI** (all in `main.cpp`):
   - `newMaskShape(MaskType)`: the starting shape of a new mask of this kind.
   - A button in the Masks pane that calls `addMask(MaskType)`, and a "+ Part: ..." button that calls `addPart(MaskType)`.
   - Its Shape controls panel, shown while `selectedPartType()` is your kind.
   - Its overlay and handles (`paintMaskOverlay`, `maskPress` and friends).

Also teach the **sidecar** about it: `sidecarShapeToJson` and `sidecarShapeFromJson` in `sidecar.hpp`. The loader validates and clamps everything, so keep that property for your new fields.

### Rules for the Eval

- Keep the per-row loops plain (raw pointers, no `std::` helpers). The project does this deliberately so Debug builds stay usable.
- If building the Eval is expensive (a bitmap, a blur), cache it, like the Brush and Object masks do, so a slider drag does not start over. Caches are small, memoized lists protected by a mutex, because renders run on worker threads.
- Never modify a cached map after it is shared. Maps are immutable.
- If you add a cache, empty it in `clearMaskCaches()` (`mask.hpp`) so one call still clears every mask cache.