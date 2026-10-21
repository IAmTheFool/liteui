# Saved edits (the sidecar file)

Literoom never modifies your photo. Your edits live in a small JSON file next to it, called a **sidecar**. For `photo.jpg` the sidecar is `photo.jpg.literoom.json`.

This page describes what is in that file, when it is written and read, and how loading protects the editor from damaged or hand-edited files.

## Where the code lives

| Piece | File |
|---|---|
| Turning the edits into JSON and back (pure functions on plain values), and the constant `kSidecarVersion` | `sidecar.hpp` |
| The JSON value, writer and parser | `minijson.hpp` |
| File handling and the top-level wrapper (`app`, `version`, `image`): when to save, the temp-file-and-rename write, the version, size and picture-size checks on load | `main.cpp`, next to `openImage` (`saveSidecar`, `loadSidecar`, `autosaveSidecar`, `flushSidecar`) |
| `AdjustmentSet::toJson()` / `loadJson()` | `adjustment_set.hpp` (thin wrappers around `sidecar.hpp`) |

## What the file contains

The top level wraps the edits with a little information about the picture:

```json
{
  "app": "literoom",
  "version": 1,
  "image": { "name": "photo.jpg", "width": 6000, "height": 4000 },
  "edits": { ... }
}
```

`image.width` and `image.height` are the size of the **original** picture. They are how a sidecar written for a different-sized picture is detected and ignored. `image.name` is informational only and is never compared. `version` is `kSidecarVersion`: a file with a higher (or a missing) version is ignored.

Inside `edits`:

| Key | Content |
|---|---|
| `adjust` | The slider values, keyed by each slider's **label** (for example `"Exposure"`, `"Noise Reduction"`). Sliders at 0 are left out. |
| `curves` | Four tone curves under the keys `rgb`, `r`, `g`, `b`, each a list of control points. |
| `colorMix` | The 24 Color Mixer values, as three lists of 8 numbers (one per color band) under the keys `hue`, `sat` and `lum`. |
| `colorGrading` | The four wheels, plus blending and balance. |
| `geometry` | `flip`, `rotate` (clockwise quarter turns, 0 to 3), `angle` (degrees), `keepFullCanvas`, and, only if there is one, `crop` as `[x, y, w, h]` measured on the turned and straightened picture. |
| `masks` | A list of masks (see below). |

### Masks

Each mask is an object with:

- `enabled`, `invert`, and an optional `name`
- `adjust`: the mask's own sliders (same label-keyed form as above)
- optional `amount` (omitted when 100), `colorMix`, `colorGrading` and `curves` (each omitted when untouched)
- `parts`: a list of shapes. Each part is an object with a `type`, the shape's own fields, and an `op` (`"add"`, `"subtract"` or `"intersect"`) saying how it joins the parts before it.

The six shape types and their fields:

| `type` | Fields |
|---|---|
| `radial` | `cx`, `cy`, `rx`, `ry`, `angle`, `feather` |
| `linear` | `cx`, `cy`, `len`, `angle` |
| `brush` | `strokes`: each has `erase`, `size`, `feather`, `density`, `flow` (left out when it is 1) and `pts`, a flat list `x0, y0, x1, y1, ...` |
| `luminosity` | `lo`, `hi`, `feather` |
| `colorRange` | `set`, `r`, `g`, `b`, `range`, `feather` |
| `object` | `feather`, `grow`, `prompts` (each `[x, y, fg]`) and `map` = `{ "w", "h", "rle" }`, where `rle` is base64 text of the run-length-coded bytes |

Shape positions are **fractions of the original picture** (centers as a fraction of its width and height, radii as a fraction of its shorter side). This is why a mask survives rotating, straightening and cropping without being rewritten. See [architecture.md](architecture.md).

Brush strokes and object-selection bitmaps are saved too. A photo with many strokes or object masks therefore has a larger sidecar.

## When it is written and read

- **Written** after any change (once no mouse drag is in progress), when another picture replaces this one, and when the app closes.
- **Written safely:** to a temporary file first, then renamed over the real one, so a crash can never leave half a file.
- **If saving fails** (a read-only folder, for example) the status bar says "Couldn't save edits...". The change still counts as seen, so a failing disk is not retried every frame, only on the next edit. After a successful save the picture's thumbnail in the filmstrip gets its "has edits" dot.
- **Not written** if you opened a picture and changed nothing.
- **Read** when the picture is opened. A missing file is normal.
- **The undo history is not saved.** It starts fresh each time the picture is opened.

## What happens when a file is bad

A sidecar can be damaged, hand-edited, written by a newer version, or written for a different picture. Loading is built so that **no file can put the editor into a state its own UI could not reach**:

- A file is **ignored** (the editor starts clean and says so in the status bar) when it is empty or larger than 64 MiB, can't be read, isn't valid JSON, has a missing or higher `version` ("made by a newer Literoom"), was written for a different-sized picture, or has an `edits` part that can't be loaded.
- Inside a valid file, nothing is trusted. Slider values are clamped to their ranges, and non-numbers become the neutral value.
- A tone curve must keep the editor's own rules (2 to 16 points, end points at x = 0 and 255, the rest at least a minimum gap apart). Anything else becomes a straight line.
- The crop is checked against the size of the turned and straightened picture, and dropped if it does not fit.
- At most 8 masks are read (`kMaxMasks`) and at most 6 parts per mask (`kMaxMaskParts`). Further ones are ignored. A mask with no valid part is dropped.
- Brush masks keep at most 512 strokes and 20,000 points per stroke; a stroke with no points is dropped.
- Object masks are limited: at most 256 prompts per selection, and a stored bitmap must be 1 to 16,384 pixels on each side, no more than about 67 million pixels in total (`kSidecarMaxMapPixels`), and decode to exactly `w × h` bytes.

## Details of the JSON code

- **Numbers are locale-independent.** The parser and writer swap the C library's decimal separator for `.` and back, so a comma-decimal locale can neither corrupt a file nor make a valid one unreadable.
- **Save in binary mode.** The writer uses `\n` for line breaks, so open files in binary mode to avoid `\r\n` rewriting.
- `minijson.hpp` also provides base64 and byte run-length helpers for storing binary data in a text file.

## Changing the format

- `kSidecarVersion` (currently 1) is written to every file by `saveSidecar` and checked by `loadSidecar`. A build whose own `kSidecarVersion` is lower than the file's ignores the **whole** file ("made by a newer Literoom"), so older builds cannot read newer sidecars. Bump it only for a change an older reader could not handle safely; additive changes (see below) usually don't need one.
- The `adjust` keys are slider **labels**. Renaming a label in `kAdjustmentSpecs` would orphan existing values (see [extending.md](extending.md)).
- Adding a field is safe if the loader gives it a default and clamps it.
- A new mask type must be added to both `sidecarShapeToJson` and `sidecarShapeFromJson`.