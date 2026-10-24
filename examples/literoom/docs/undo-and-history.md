# Undo and History

`AdjustmentSet` (`adjustment_set.hpp`) owns **every edit** the user has made, plus the undo/redo history and the labels the History panel shows. It has no UI and no pixels. This page explains how it works and why it is built the way it is.

## What an edit is

The edit state is everything needed to reproduce the picture from the original:

- the slider values (one per `AdjId`)
- the four tone curves (RGB, R, G, B)
- the Color Mixer values (24 sliders)
- the Color Grading wheels, blending and balance
- the committed crop, the 90° orientation (turns and flip), the straighten angle and the corner mode
- the list of masks (shapes, brush strokes, object bitmaps, and each mask's own sliders)

A picture's **rating, flag and color label** are not edits either. They are kept beside the edit state (see [sidecar-format.md](sidecar-format.md#rating-flag-color-label-and-the-filmstrip-filter)), have no undo step and no History label, and never bump `version()`.

A **preset** is not another kind of edit. It is a way of setting the first four groups (sliders, curves, Color Mixer, Color Grading) in one go; see [Presets](#presets) below.

## Snapshots

The history is a list of **snapshots**, each a complete copy of the edit state above. Undo and redo move an index along that list and restore the snapshot there.

Why whole copies rather than "deltas"? It is simple and robust: restoring a step is a plain assignment, and nothing can drift out of sync. Object bitmaps, the largest items, are immutable and shared by pointer (and compared by an id), so copying a mask list does not copy them.

- A snapshot also carries an optional text `label`. It is empty for every step except a preset step (see [Presets](#presets)) a white-balance pick (see [White balance picker](#white-balance-picker)) and an Auto Tone (see [Auto Tone](#auto-tone)). It is not part of the edit state: it is never compared with another snapshot and never restored.
- Up to **200** steps can be undone (`kMaxUndoSteps`). Older ones are dropped as new edits arrive.
- A new edit after an undo **discards the redo tail**, like most editors.
- Opening a picture (`reset()`) or loading saved edits (`loadJson()`) starts a fresh history containing only the starting state. **The history itself is not saved to disk.**

## The coalescing problem

A slider drag calls `set()` many times a second, and LiteUI has **no "drag ended" event**. If every call made a history step, dragging Exposure from -20 to +40 would create hundreds of steps and the History panel would be useless.

So `AdjustmentSet` **coalesces**: repeated calls for the *same control* overwrite the newest snapshot instead of appending a new one. Touching a different control, or doing an undo/redo/crop, starts a fresh step. One drag is one undo step.

Overwriting only happens while the newest step is also the current one (no redo tail pending). After an undo, touching the same slider appends a new step instead of overwriting the one you navigated to.

It remembers the control being dragged in `lastEdited_`. For things that are not sliders, a few pseudo-ids past `AdjId::Count` mark what is being dragged:

| Marker | What it marks |
|---|---|
| a real `AdjId` | a slider drag |
| `kEditAngle` | a straighten-angle drag |
| `kEditCurve` | dragging a tone-curve point (with `lastCurveCh_` for which channel) |
| `kEditMix` | a Color Mixer slider (with `lastMixIdx_` for which one) |
| `kEditGrade` | a Color Grading control (with `lastGradeKey_`) |
| `kEditMask` | a mask control (with `lastMaskKey_`) |
| `AdjId::Count` | no edit in progress |

### Keys for grading and masks

`setGrade` and `setMasks` take an integer `key` that identifies the control being dragged. Repeated calls with the **same key** fold into one step. A **negative key** means a discrete action (an eye toggle, add, delete, reset) which is always its own step. Callers pack the mask index and the selected part into the key (together with the control being dragged), so two different masks, or two parts of one mask, never share a step.

### Ending a gesture explicitly

Where the UI *does* know a gesture ended, it says so: `endCurveEdit()`, `endGradeEdit()` and `endMaskEdit()` clear the marker so the next edit starts a new step. Undo, redo, jumping in History and applying a preset also clear it. There is no such call for the Color Mixer or for plain sliders: their drag simply ends when something else is touched.

## How each kind of edit becomes a step

| Call | Behavior |
|---|---|
| `set(id, value)` | Coalesces per slider. |
| `setCrop(rect)` | Always its own step. Never coalesced into a slider drag. |
| `setGeometry(orient, angle, crop)` | One step. The three change together because the crop is stored relative to the oriented, straightened picture. |
| `setAngle(deg, coalesce)` | Drops any crop (the picture just changed size). Coalesces during a drag; discrete for a nudge button. |
| `setExpand(bool)` | Always its own step. Drops the crop if the angle is non-zero. |
| `setCurve(channel, points, coalesce)` | Coalesces while a point is dragged. `resetAllCurves()` is one step. |
| `setMix(chan, band, value)` | Coalesces per slider. `resetMix()` is one step. |
| `setGrade(grade, key)` | See keys above. |
| `setMasks(masks, key)` | See keys above. |
| `setWhiteBalance(temp, tint)` | Always its own step, and it ends any drag in progress. Sets Temperature and Tint together; see [White balance picker](#white-balance-picker). |
| `setAutoTone(exp, contrast, whites, blacks)` | Always its own step, and it ends any drag in progress. Sets Exposure, Contrast, Whites and Blacks together; see [Auto Tone](#auto-tone). |
| `applyPreset(preset)` | Always its own step, and it ends any drag in progress. Replaces the sliders, curves, Color Mixer and Color Grading together; see [Presets](#presets). |

All of them except `set` skip the step if nothing actually changed (the new value compares equal to the old), so no-op edits never clutter the history. `set(id, value)` has no such check: a call with an unchanged value still overwrites the current step (same slider) or appends a new one (a different control), and it still bumps `version()`.

## The History panel's labels

Labels are not stored with the steps. Instead, each step is **described by comparing it with the step before it** (`describeStep`). This has two benefits: the setters need to know nothing about labels, and a coalesced drag that overwrites a step can never leave a stale label behind.

`describeStep` first looks at the step's own `label`: if it is not empty (only a preset step, a white-balance pick and an Auto Tone have one), that text is the label and nothing is diffed. Otherwise it checks, in order: global sliders ("Exposure +20"), orientation ("Rotate / Flip"), straighten angle, corner mode, crop, tone curves, Color Mixer ("Color Mix: Blue Hue +10", or "Color Mix: Reset" if several changed), Color Grading, then masks (add, delete, reorder, and per-mask changes). The first difference found names the step. Step 0 is always "Original".

The per-mask labels are `Mask <n>: ` followed by `Rename`, a slider and its value ("Mask 2: Exposure +30"), `Amount <n>`, `Color Mix`, `Color Grading`, `Curve`, `On` / `Off`, `Invert`, `Parts` (a part was added or removed), or `Edit`. A change to a shape itself (feather, radius, a brush stroke, an object selection) has no label of its own and shows as `Mask <n>: Edit`.

Numbers are printed with a sign: values within 0.05 of a whole number as `+20`, others as `+12.5`. Straighten prints as `Straighten +1.5°`.

Labels are cached and rebuilt only when the version changes, so polling them every frame is cheap.

## Presets

A preset is a saved look: the slider values, the four tone curves, the Color Mixer values and Color Grading. Crop, orientation, straighten angle and masks are **not** part of it. The file format and helpers are in `preset.hpp` (see [headers.md](headers.md#presethpp)).

`AdjustmentSet` has two calls for them:

- `currentPreset(name)` copies the current look into a `Preset`. It does not change the history or `version()`.
- `applyPreset(preset)` **replaces** the whole look with the preset's. A slider the preset leaves at 0 is reset to 0; it is not left as it was. Crop, orientation, angle and masks stay as they are. Slider values are clamped again on the way in.

How it fits the history:

- **One step.** Applying a preset is one undo step, however many controls it changes, and undo puts every one of them back.
- **Always its own step.** It goes through the same "start a new step" path as a crop, so it ends any drag in progress. The next slider touch starts a new step instead of overwriting the preset step.
- **No-op applies add nothing.** If the preset gives exactly the current look, `applyPreset` returns false, adds no step and does not bump `version()`.
- **Its own label.** Diffing would name only the first thing that changed ("Exposure +20"), which would be misleading for a preset that changes dozens of controls. So the step stores its label, `Preset: <name>` (or just `Preset` if the name is blank), in its snapshot, and the History panel shows that.
- **History keeps a copy, not a link.** The step holds a full copy of the look, like every other step. If the preset *file* is later deleted, the step stays in History and can still be jumped to, undone and redone. Deleting a preset only removes the file; it never changes a picture or its history.

Applying a preset is a normal edit, so it also bumps `version()` and the edited picture is saved to its sidecar like any other change. The sidecar stores the resulting sliders and curves, not the preset's name: reopening the picture shows the look but not that it came from a preset, and the history (with its preset label) is not saved.

### Copy and Paste Settings

Copy Settings (Ctrl+Shift+C) stores `currentPreset("")` in a small in-memory clipboard in `main.cpp` (a `Preset`, plus the name of the picture it came from). It is not a file and it is gone when the app closes. Paste Settings (Ctrl+Shift+V) calls `applyPreset` on the open picture with the name `Pasted`, so everything above holds: one undo step labelled `Preset: Pasted`, crop, orientation and masks untouched, a paste that changes nothing adds no step ("Already has these settings"), and undo restores every control.

**Sync Settings** is different: it writes the look into the *sidecars* of other pictures, which are not open and have no history, so it creates no step anywhere. See [sidecar-format.md](sidecar-format.md#syncing-a-look-onto-other-pictures).

## White balance picker

The Adjust pane's white-balance eyedropper sets two sliders from one click, so it needs a call that changes both as a single step. `set()` cannot do that: it coalesces per slider, so setting Temperature and then Tint would leave two History entries and two Ctrl+Z presses to undo one click.

`setWhiteBalance(temperature, tint)` (the numbers come from `solveWhiteBalance` in `color_pass.hpp`, see [headers.md](headers.md#color_passhpp)):

- **One step.** Undo puts Temperature and Tint back together; redo restores both.
- **Always its own step.** It goes through the same "start a new step" path as a preset or a crop, so it ends any drag in progress. The next slider touch, even on Temperature, starts a new step instead of overwriting the pick.
- **Its own label.** `White Balance -67 / +14` (Temperature, then Tint, signed). Like a preset step, the label is stored in the snapshot, so it survives undo, redo and History jumps. It is not saved: the sidecar stores only the two slider values.
- **No-op picks add nothing.** If both values already equal the current ones it returns false, adds no step and does not bump `version()`.
- **Sanitized.** Values are clamped to the slider range and NaN becomes 0, as in `applyPreset`.
- **Nothing else moves.** Other sliders, curves, the Mixer, grading, crop, orientation and masks are untouched.

It is a normal edit afterwards: it bumps `version()`, is autosaved to the sidecar, and travels with Copy / Paste Settings, presets and Sync Settings, because those carry slider values.

## Auto Tone

The Adjust pane's **Auto Tone** button sets four sliders from one press, so like the white-balance pick it needs a call that changes them as a single step. `set()` cannot do that: it coalesces per slider, so four calls would leave four History entries and four Ctrl+Z presses to undo one click.

`setAutoTone(exposure, contrast, whites, blacks)` (the numbers come from `solveAutoTone` in `auto_tone.hpp`, see [headers.md](headers.md#auto_tonehpp)):

- **One step.** Undo puts all four sliders back together; redo restores them.
- **Always its own step.** It goes through the same "start a new step" path as a preset, a white-balance pick or a crop, so it ends any drag in progress. The next slider touch, even on Exposure, starts a new step instead of overwriting it.
- **Its own label.** `Auto Tone`. Like a preset step, the label is stored in the snapshot, so it survives undo, redo and History jumps. It is not saved: the sidecar stores only the four slider values.
- **No-op presses add nothing.** If all four values already equal the current ones it returns false, adds no step and does not bump `version()`. This is also what makes a second press harmless: the solver reads the picture before the tone tables, so it gives the same four numbers again.
- **Sanitized.** Values are clamped to the slider range and NaN becomes 0, as in `applyPreset`.
- **Nothing else moves.** Brightness, Highlights, Shadows, the curves, the other sliders, the Mixer, grading, crop, orientation and masks are untouched.

It is a normal edit afterwards: it bumps `version()`, is autosaved to the sidecar, and travels with Copy / Paste Settings, presets and Sync Settings, because those carry slider values.

## The version counter

`version()` increases on every change to the edit state (an edit, undo, redo, reset). The app compares it with its value at the last save or load to decide whether the saved-edits file is stale, and uses it to invalidate the label cache. It is not part of the undo history.

## Why `lastEdited_` is reset on undo

After an undo, redo or a jump in the History panel, `lastEdited_` is cleared. Otherwise the next touch of the same slider would overwrite the step you just navigated to, instead of starting a new one.

## Related

- Saving and loading: [sidecar-format.md](sidecar-format.md).
- Preset files and helpers: [headers.md](headers.md#presethpp).
- Putting a look into other pictures' sidecars: [sidecar-format.md](sidecar-format.md#syncing-a-look-onto-other-pictures).
- How an edit becomes pixels: [architecture.md](architecture.md).