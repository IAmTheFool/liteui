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
| The separate **preset** file (a saved look, see [Presets](#presets-a-related-file) below) | `preset.hpp`, which reuses the readers and writers in `sidecar.hpp` |
| Putting a look into **another picture's** sidecar without opening it (Sync Settings, see [below](#syncing-a-look-onto-other-pictures)) | `sidecarSyncLook` in `sidecar.hpp`; the file reading, atomic write and worker are in `main.cpp` (`readSidecarText`, `writeTextAtomic`, `runSyncJob`) |

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

- **Written** after any change (once no mouse drag is in progress; applying a preset counts as a change), when another picture replaces this one, and when the app closes.
- **Written safely:** to a temporary file first, then renamed over the real one, so a crash can never leave half a file.
- **If saving fails** (a read-only folder, for example) the status bar says "Couldn't save edits...". The change still counts as seen, so a failing disk is not retried every frame, only on the next edit. After a successful save the picture's thumbnail in the filmstrip gets its "has edits" dot.
- **Written for other pictures** by Sync Settings: the sidecars of the pictures selected in the filmstrip are rewritten in the background (see [below](#syncing-a-look-onto-other-pictures)). The open picture is never one of them.
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

## Presets (a related file)

A **preset** is a saved look that can be applied to any picture. It is a different file from the sidecar, but it is built from the same pieces, so it is described here. The code is `preset.hpp`; see [headers.md](headers.md#presethpp).

|  | Sidecar | Preset |
|---|---|---|
| Belongs to | One picture | No picture: any picture |
| Where | Next to the picture: `photo.jpg.literoom.json` | In the presets folder: `<name>.literoom-preset.json` |
| Wrapper | `app`, `version`, `image`, `edits` | Flat: `app`, `kind`, `version`, `name`, then the groups |
| Contains | Sliders, curves, Color Mixer, Color Grading, **geometry and masks** | Sliders, curves, Color Mixer, Color Grading only |
| Written when | After every change, on switching pictures, on close | Only when you press "Save current look" |
| Size limit on load | 64 MiB | 1 MiB |

The presets folder is `%APPDATA%\Literoom\presets` on Windows, and `$XDG_CONFIG_HOME/literoom/presets` or `~/.config/literoom/presets` elsewhere. The file name is made from the preset's name: characters Windows does not allow become `_`, and a few reserved names get a leading `_`. Saving a preset under a name that maps to an existing file replaces it.

```json
{
  "app": "literoom",
  "kind": "preset",
  "version": 1,
  "name": "Warm Film",
  "adjust": { "Exposure": 12.5, "Temperature": 30 },
  "curves": { ... },
  "colorMix": { ... },
  "colorGrading": { ... }
}
```

The `adjust`, `curves`, `colorMix` and `colorGrading` groups are **exactly** the sidecar's forms: sliders keyed by label with zero ones left out, curves as `[x, y]` lists with identity curves left out, and so on. A neutral look has four empty groups.

**Loading is as strict as for a sidecar.** The preset is read with the same functions, so nothing in the file is trusted: slider values are clamped, non-numbers become 0, a tone curve that breaks the editor's rules becomes a straight line, and Mixer and grading values are clamped. A file is **refused** (the picture is not changed, and the pane simply leaves it out of the list) when it is empty, larger than 1 MiB, not valid JSON, not an object, has a `kind` other than `"preset"`, or has a missing, zero or higher-than-supported `version`. The preset's `version` is its own number (`kPresetVersion`) and is not tied to `kSidecarVersion`.

Things that differ from a sidecar:
- There is no picture size to check, because a preset has no geometry and no masks.
- The `name` is cleaned on load (printable ASCII only, trimmed, at most 32 characters). If it is missing, the file name is used.
- Applying a preset copies its values into the picture's edits. The picture's sidecar then stores those **values**, not the preset: reopening the picture shows the same look, but nothing records that it came from a preset, and later changes to the preset file never affect pictures it was applied to.

## Syncing a look onto other pictures

**Image > Sync Settings to Selected** (Ctrl+Shift+S) copies the open picture's look (sliders, curves, Color Mixer, Color Grading: the same four groups a preset holds) onto the pictures selected in the filmstrip. Those pictures are not open, so nothing goes through `AdjustmentSet`: their sidecar files are rewritten directly by `sidecarSyncLook`, a pure function in `sidecar.hpp`. It takes the picture's current sidecar text and returns the new text. How it decides:

- **No sidecar yet:** a new file is made. It must record the picture's size, so the background job decodes the picture once and uses its upright size (a camera's quarter turn swaps width and height).
- **A sidecar exists:** its recorded `image.width` and `image.height` are used, so no decode is needed. The file is read through `sidecarEditsFromJson`, **only the four look groups are replaced**, and the whole thing is written back through `sidecarEditsToJson`. The picture's own crop, rotation, straighten angle and masks are kept, so a crop that suits one photo is never pushed onto another.
- **A file that can't be trusted is never overwritten.** The picture is *skipped*, and the status bar says why, when the file is not valid JSON or not an object, is from a newer version (or has none), has no picture size, was written for a different size than the one given, or has an `edits` part that can't be loaded. The edits in such a file might still be recoverable by hand, so syncing leaves it alone.
- The rewritten file is validated and clamped by the same loader as an opened one. That is what keeps a hand-edited sidecar from carrying bad values across a sync.

The result is the same file the app would write if you had opened the picture, applied the look and closed it, except that there is no undo step: the picture's history is not saved, and it starts fresh when the picture is next opened. Syncing the same look twice produces an identical file. Thumbnails of synced pictures get the "has edits" dot at once, but, as for any picture that is not open, they show the look only after the picture has been opened and left.

The batch runs on a worker that is never cancelled, and opening pictures waits until it is done; see [threading.md](threading.md#the-sync-worker-is-a-batch-that-is-never-superseded).

**Copy and Paste Settings** (Ctrl+Shift+C / V) work on the open picture only, hold the look in memory (not a file, and not kept after the app closes), and apply it as a preset step. See [undo-and-history.md](undo-and-history.md#copy-and-paste-settings).

## Changing the format

- `kSidecarVersion` (currently 1) is written to every file by `saveSidecar` and checked by `loadSidecar`. A build whose own `kSidecarVersion` is lower than the file's ignores the **whole** file ("made by a newer Literoom"), so older builds cannot read newer sidecars. Bump it only for a change an older reader could not handle safely; additive changes (see below) usually don't need one.
- The `adjust` keys are slider **labels**. Renaming a label in `kAdjustmentSpecs` would orphan existing values in sidecars **and in saved presets** (see [extending.md](extending.md)).
- Adding a field is safe if the loader gives it a default and clamps it.
- **Every field must be written by `sidecarEditsToJson` and read by `sidecarEditsFromJson`.** Sync Settings reloads a picture's sidecar and writes it back, so a field that only the app's own code kept (and the loader dropped) would be lost from every picture it syncs onto.
- A new mask type must be added to both `sidecarShapeToJson` and `sidecarShapeFromJson`. Presets do not store masks, so they need no change.
- A change to the slider, curve, Mixer or grading forms reaches presets too, because they share the same functions. Presets have their own `kPresetVersion`; bump it only for a change an older reader could not handle safely.