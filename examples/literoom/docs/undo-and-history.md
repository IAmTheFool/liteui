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

## Snapshots

The history is a list of **snapshots**, each a complete copy of the edit state above. Undo and redo move an index along that list and restore the snapshot there.

Why whole copies rather than "deltas"? It is simple and robust: restoring a step is a plain assignment, and nothing can drift out of sync. Object bitmaps, the largest items, are immutable and shared by pointer (and compared by an id), so copying a mask list does not copy them.

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

Where the UI *does* know a gesture ended, it says so: `endCurveEdit()`, `endGradeEdit()` and `endMaskEdit()` clear the marker so the next edit starts a new step. Undo, redo and jumping in History also clear it. There is no such call for the Color Mixer or for plain sliders: their drag simply ends when something else is touched.

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

All of them except `set` skip the step if nothing actually changed (the new value compares equal to the old), so no-op edits never clutter the history. `set(id, value)` has no such check: a call with an unchanged value still overwrites the current step (same slider) or appends a new one (a different control), and it still bumps `version()`.

## The History panel's labels

Labels are not stored with the steps. Instead, each step is **described by comparing it with the step before it** (`describeStep`). This has two benefits: the setters need to know nothing about labels, and a coalesced drag that overwrites a step can never leave a stale label behind.

`describeStep` checks, in order: global sliders ("Exposure +20"), orientation ("Rotate / Flip"), straighten angle, corner mode, crop, tone curves, Color Mixer ("Color Mix: Blue Hue +10", or "Color Mix: Reset" if several changed), Color Grading, then masks (add, delete, reorder, and per-mask changes). The first difference found names the step. Step 0 is always "Original".

The per-mask labels are `Mask <n>: ` followed by `Rename`, a slider and its value ("Mask 2: Exposure +30"), `Amount <n>`, `Color Mix`, `Color Grading`, `Curve`, `On` / `Off`, `Invert`, `Parts` (a part was added or removed), or `Edit`. A change to a shape itself (feather, radius, a brush stroke, an object selection) has no label of its own and shows as `Mask <n>: Edit`.

Numbers are printed with a sign: values within 0.05 of a whole number as `+20`, others as `+12.5`. Straighten prints as `Straighten +1.5°`.

Labels are cached and rebuilt only when the version changes, so polling them every frame is cheap.

## The version counter

`version()` increases on every change to the edit state (an edit, undo, redo, reset). The app compares it with its value at the last save or load to decide whether the saved-edits file is stale, and uses it to invalidate the label cache. It is not part of the undo history.

## Why `lastEdited_` is reset on undo

After an undo, redo or a jump in the History panel, `lastEdited_` is cleared. Otherwise the next touch of the same slider would overwrite the step you just navigated to, instead of starting a new one.

## Related

- Saving and loading: [sidecar-format.md](sidecar-format.md).
- How an edit becomes pixels: [architecture.md](architecture.md).