# Saved edits (the sidecar file)

Literoom never modifies your photo. Your edits live in a small JSON file next to it, called a **sidecar**. For `photo.jpg` the sidecar is `photo.jpg.literoom.json`. A picture can have more than one: each [virtual copy](#virtual-copies) is another set of edits for the same file, with its own sidecar, `photo.jpg.vc2.literoom.json`.

This page describes what is in that file, when it is written and read, and how loading protects the editor from damaged or hand-edited files.

## Where the code lives

| Piece | File |
|---|---|
| Turning the edits into JSON and back (pure functions on plain values), and the constant `kSidecarVersion` | `sidecar.hpp` |
| The JSON value, writer and parser | `minijson.hpp` |
| File handling and the top-level wrapper (`app`, `version`, `image`): when to save, the temp-file-and-rename write, the version, size and picture-size checks on load | `main.cpp`, next to `openImage` (`saveSidecar`, `loadSidecar`, `autosaveSidecar`, `flushSidecar`) |
| `AdjustmentSet::toJson()` / `loadJson()` | `adjustment_set.hpp` (thin wrappers around `sidecar.hpp`) |
| Rating, flag and color label (the `meta` object) and the filmstrip filter | `photo_meta.hpp` (values, JSON, `MetaFilter`); `sidecarFileText`, `sidecarMetaFromRoot` and `sidecarScan` in `sidecar.hpp`; the commands (`ratePhoto`, `flagPhoto`, `labelPhoto`) and the filter bar in `main.cpp` |
| **Named snapshots** (the `snapshots` array, see [below](#named-snapshots)) | `NamedSnapshot`, `SnapshotList`, `sidecarSnapshotsToJson` / `FromJson` and `sanitizeSnapshotName` in `sidecar.hpp`; `AdjustmentSet::currentEdits()` / `restoreEdits()` in `adjustment_set.hpp`; the pane and its commands (`addSnapshot`, `applySnapshotEntry`, `deleteSelectedSnapshot`) in `main.cpp` |
| **Virtual copies** (more sidecars for one picture, see [below](#virtual-copies)) | `virtual_copy.hpp` (the path marker, the file-name rules, the folder scan); `sidecarFileOf` in `sidecar.hpp`; `createVirtualCopy`, `deleteVirtualCopy` and the filmstrip code in `main.cpp` |
| The separate **preset** file (a saved look, see [Presets](#presets-a-related-file) below) | `preset.hpp`, which reuses the readers and writers in `sidecar.hpp` |
| Putting a look into **another picture's** sidecar without opening it (Sync Settings, see [below](#syncing-a-look-onto-other-pictures)) | `sidecarSyncLook` in `sidecar.hpp`; the file reading, atomic write and worker are in `main.cpp` (`readSidecarText`, `writeTextAtomic`, `runSyncJob`) |
| The **3D LUT** (the `lut` object, and `LUT Amount` in `adjust`; see [below](#the-3d-lut)) | `LutRef` and the name / hash helpers in `lut3d.hpp`; `sidecarLutToJson` / `sidecarLutFromJson` in `sidecar.hpp`; the table files, the folder and the cache in `lut_library.hpp` |

## What the file contains

The top level wraps the edits with a little information about the picture:

```json
{
  "app": "literoom",
  "version": 1,
  "image": { "name": "photo.jpg", "width": 6000, "height": 4000 },
  "meta": { "rating": 4, "flag": "pick", "label": "red" },
  "edits": { ... },
  "snapshots": [ { "name": "Golden hour", "edits": { ... } } ]
}
```

`image.width` and `image.height` are the size of the **original** picture. They are how a sidecar written for a different-sized picture is detected and ignored. `image.name` is informational only and is never compared. `version` is `kSidecarVersion`: a file with a higher (or a missing) version is ignored.

`meta` is the picture's **rating, flag and color label**. It is optional and written only when something differs from the default, so a picture that was never rated has no `meta` key. These are **not edits**: they have no undo step and no History label, they sit beside `edits` rather than inside it, and Sync Settings leaves them alone. Fields (each left out at its default): `rating` 1 to 5, `flag` `"pick"` or `"reject"`, `label` one of `"red"`, `"yellow"`, `"green"`, `"blue"`, `"purple"`. Loading clamps a rating into 0 to 5 and turns anything it doesn't recognize (a non-number rating, an unknown flag or color name) into "none"; a `meta` that isn't an object is ignored. Adding it needed no `kSidecarVersion` bump: an older build ignores the key and, when it next saves that picture, writes the file without it.

`snapshots` is the picture's **named snapshots**, each a name and a complete copy of the edits (see [Named snapshots](#named-snapshots)). It is optional and written only when there is at least one, after `edits`, so a picture without snapshots has exactly the layout it had before they existed. Like `meta` it needed no `kSidecarVersion` bump, and an older build drops it the next time it saves that picture.

Inside `edits`:

| Key | Content |
|---|---|
| `adjust` | The slider values, keyed by each slider's **label** (for example `"Exposure"`, `"Noise Reduction"`, `"LUT Amount"`). Sliders at 0 are left out. |
| `curves` | Four tone curves under the keys `rgb`, `r`, `g`, `b`, each a list of control points. |
| `colorMix` | The 24 Color Mixer values, as three lists of 8 numbers (one per color band) under the keys `hue`, `sat` and `lum`. |
| `colorGrading` | The four wheels, plus blending and balance. |
| `lut` | Optional: the picture's 3D LUT as `{ "name", "hash" }` (see [The 3D LUT](#the-3d-lut)). **Absent when the picture has none**, like `meta`, so a picture that never had a LUT keeps exactly the file it had before LUTs existed. |
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

## The 3D LUT

A picture's LUT is stored as a **reference**, never as a table:

```json
"lut": { "name": "Warm Film", "hash": "0123456789abcdef" }
```

- `name` is the LUT's name in the LUT library (the file `<name>.cube` in the library folder, see `lut_library.hpp`). It is cleaned on load like every name: printable ASCII only, the characters Windows does not allow in file names (`< > : " / \ | ? *`) become `_`, at most 48 characters, no trailing dot or space. A name can therefore never point outside the library folder, whatever a hand-edited file says.
- `hash` is the hash (FNV-1a, 64 bit) of the file's bytes **when the LUT was chosen**, as exactly 16 hex digits. It is optional; a missing or malformed one is read as "unknown", which accepts any file by that name. It exists so that replacing a `.cube` under the same name is noticed (the picture still uses the new file, and says so when it is opened).
- **How strongly** the LUT is used is the ordinary slider `LUT Amount` (0 to 100) in `adjust`, like every other slider. A picture can hold a LUT at amount 0 (chosen, but off); removing a LUT sets the amount back to 0.
- The table is **never** stored: a 65³ table is about 7 MB of numbers, and a sidecar's snapshots, undo steps (in memory) and presets would each repeat it. A name and a hash are about 60 bytes. The price is that the file must still be in the library when the picture is opened: if it is missing or unreadable, the picture opens **without** the look (the reference and the amount are kept, so putting the file back restores it), the status bar says so, and a batch export skips the picture with a reason. See [3D LUTs in architecture.md](architecture.md#3d-luts).
- The `lut` object is part of the *look*: presets, Copy / Paste Settings and Sync Settings carry it, and a look without a LUT **clears** the target's LUT (the look is replaced, not merged).

## When it is written and read

- **Written** after any change (once no mouse drag is in progress; applying a preset counts as a change, and so does changing the rating, flag or color label, or adding or deleting a snapshot), when another picture replaces this one, and when the app closes.
- **Written safely:** to a temporary file first, then renamed over the real one, so a crash can never leave half a file.
- **If saving fails** (a read-only folder, for example) the status bar says "Couldn't save edits...". The change still counts as seen, so a failing disk is not retried every frame, only on the next edit. After a successful save of **edits** the picture's thumbnail in the filmstrip gets its "has edits" dot; a save that only records a rating, flag or color label does not set it.
- **Written for other pictures** by Sync Settings, and by a batch rating when pictures are selected: the sidecars of the pictures selected in the filmstrip are rewritten in the background (see [below](#syncing-a-look-onto-other-pictures)). The open picture is never one of them.
- **Not written** if you opened a picture and changed nothing. The open picture's sidecar is written only by the editor (autosave); other pictures' sidecars are written only by the Sync worker (Sync Settings, or a batch rating), never behind the editor's back. A picture that is rated and has no edits gets a sidecar whose `edits` are the neutral ones.
- **Read** when the picture is opened. A missing file is normal.
- **The undo history is not saved.** It starts fresh each time the picture is opened.

## What happens when a file is bad

A sidecar can be damaged, hand-edited, written by a newer version, or written for a different picture. Loading is built so that **no file can put the editor into a state its own UI could not reach**:

- A file is **ignored** (the editor starts clean and says so in the status bar) when it is empty or larger than 64 MiB, can't be read, isn't valid JSON, has a missing or higher `version` ("made by a newer Literoom"), was written for a different-sized picture, or has an `edits` part that can't be loaded.
- Inside a valid file, nothing is trusted. Slider values are clamped to their ranges, and non-numbers become the neutral value.
- A tone curve must keep the editor's own rules (2 to 16 points, end points at x = 0 and 255, the rest at least a minimum gap apart). Anything else becomes a straight line.
- The crop is checked against the size of the turned and straightened picture, and dropped if it does not fit.
- At most 16 masks are read (`kMaxMasks`) and at most 6 parts per mask (`kMaxMaskParts`). Further ones are ignored. A mask with no valid part is dropped.
- Brush masks keep at most 512 strokes and 20,000 points per stroke; a stroke with no points is dropped.
- **A bad `lut` never makes a file bad.** If it is not an object, has no string `name`, or has a name with nothing left after cleaning, the picture simply has no LUT (the rest of the file loads normally). A `hash` that is not exactly 16 hex digits is dropped. Whether the named file exists is not checked at load time: that is the library's business, reported when the LUT is first used.
- Object masks are limited: at most 256 prompts per selection, and a stored bitmap must be 1 to 16,384 pixels on each side, no more than about 67 million pixels in total (`kSidecarMaxMapPixels`), and decode to exactly `w × h` bytes.
- **Snapshots never make a file bad.** `snapshots` that isn't an array gives none. Each entry is skipped if it isn't an object or its `edits` can't be loaded, and each one that is kept goes through `sidecarEditsFromJson` like the live edits (sliders clamped, the crop checked against the picture's size, at most 16 masks, and so on). At most `kMaxSnapshots` (20) are kept, and the name is cleaned (printable ASCII, trimmed, at most 32 characters; a blank one becomes `Snapshot`).

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
| Contains | Sliders, curves, Color Mixer, Color Grading, the 3D LUT, **geometry and masks** | Sliders, curves, Color Mixer, Color Grading and the 3D LUT only |
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
  "colorGrading": { ... },
  "lut": { "name": "Warm Film", "hash": "0123456789abcdef" }
}
```

The `adjust`, `curves`, `colorMix` and `colorGrading` groups are **exactly** the sidecar's forms: sliders keyed by label with zero ones left out (so `LUT Amount` is in `adjust`), curves as `[x, y]` lists with identity curves left out, and so on. `lut` is the sidecar's `lut` object and is **only written when the look has a LUT**, so a neutral look has four empty groups and no `lut`. The LUT is a name and a hash, never a table, so a preset stays a few hundred bytes.

**Loading is as strict as for a sidecar.** The preset is read with the same functions, so nothing in the file is trusted: slider values are clamped, non-numbers become 0, a tone curve that breaks the editor's rules becomes a straight line, Mixer and grading values are clamped, and a bad `lut` object becomes no LUT. A file is **refused** (the picture is not changed, and the pane simply leaves it out of the list) when it is empty, larger than 1 MiB, not valid JSON, not an object, has a `kind` other than `"preset"`, or has a missing, zero or higher-than-supported `version`. The preset's `version` is its own number (`kPresetVersion`) and is not tied to `kSidecarVersion`.

Things that differ from a sidecar:
- There is no picture size to check, because a preset has no geometry and no masks.
- The `name` is cleaned on load (printable ASCII only, trimmed, at most 32 characters). If it is missing, the file name is used.
- Applying a preset copies its values into the picture's edits. The picture's sidecar then stores those **values**, not the preset: reopening the picture shows the same look, but nothing records that it came from a preset, and later changes to the preset file never affect pictures it was applied to.

## Syncing a look onto other pictures

**Image > Sync Settings to Selected** (Ctrl+Shift+S) copies the open picture's look (sliders, curves, Color Mixer, Color Grading and the 3D LUT: what a preset holds) onto the pictures selected in the filmstrip. Those pictures are not open, so nothing goes through `AdjustmentSet`: their sidecar files are rewritten directly by `sidecarSyncLook`, a pure function in `sidecar.hpp`. It takes the picture's current sidecar text and returns the new text. How it decides:

- **No sidecar yet:** a new file is made. It must record the picture's size, so the background job decodes the picture once and uses its upright size (a camera's quarter turn swaps width and height).
- **A sidecar exists:** its recorded `image.width` and `image.height` are used, so no decode is needed. The file is read through `sidecarEditsFromJson`, **only the look is replaced** (the four groups, and the `lut`: a look without a LUT clears the picture's LUT, like any other part of the look), and the whole thing is written back through `sidecarEditsToJson`. The picture's own crop, rotation, straighten angle and masks are kept, so a crop that suits one photo is never pushed onto another.
- **A file that can't be trusted is never overwritten.** The picture is *skipped*, and the status bar says why, when the file is not valid JSON or not an object, is from a newer version (or has none), has no picture size, was written for a different size than the one given, or has an `edits` part that can't be loaded. The edits in such a file might still be recoverable by hand, so syncing leaves it alone.
- The picture's own `meta` (rating, flag, color label) is read from the old file and written back unchanged: syncing a look never changes how a picture is rated. A picture with no `meta` stays without one.
- The picture's own **snapshots** are read and written back as well, each keeping the look it captured: syncing a look changes the picture's current edits, never its saved snapshots. A picture with none stays without any.
- The rewritten file is validated and clamped by the same loader as an opened one. That is what keeps a hand-edited sidecar from carrying bad values across a sync.

The result is the same file the app would write if you had opened the picture, applied the look and closed it, except that there is no undo step: the picture's history is not saved, and it starts fresh when the picture is next opened. Syncing the same look twice produces an identical file. Thumbnails of synced pictures get the "has edits" dot at once, but, as for any picture that is not open, they show the look only after the picture has been opened and left.

The batch runs on a worker that is never cancelled, and opening pictures waits until it is done; see [threading.md](threading.md#the-sync-worker-is-a-batch-that-is-never-superseded).

**Copy and Paste Settings** (Ctrl+Shift+C / V) work on the open picture only, hold the look in memory (not a file, and not kept after the app closes), and apply it as a preset step. See [undo-and-history.md](undo-and-history.md#copy-and-paste-settings).

## Rating, flag, color label and the filmstrip filter

What the user sees (all in `main.cpp`, on top of `photo_meta.hpp`):

- **Keys** (the open picture, not while typing in a text field): `0` clears the rating, `1` to `5` set it, `P` pick, `X` reject, `U` remove the flag, `6` red, `7` yellow, `8` green, `9` blue (purple is in the Photo menu). Choosing the value a picture already has clears it. The **Photo** menu has the same commands.
- **The selection is rated too.** If other pictures are selected in the filmstrip (Ctrl+click, Shift+click), the command changes the open picture at once and the selected ones in the background, on the Sync Settings worker. Whether to set or clear is decided from the open picture (pressing the value it already has clears it), and that result is then applied to every selected picture alike. Each picture's sidecar is patched by `sidecarPatchMeta`, which changes only the fields the command names and keeps the edits, geometry and masks. It has the same rules as Sync Settings: a picture with no sidecar is decoded once to learn its size (unless the result would be "no rating, no flag, no label", when nothing is written and nothing is decoded), a file that can't be trusted is skipped with the reason in the status bar, and writes are temp-file-then-rename. The open picture is never in the batch, so it can't race with autosave; a second batch (or a Sync Settings) is refused while one runs, and the strip updates when the batch finishes. A selection that is hidden by the filter is not touched.
- **The value is kept in `state.meta`**, mirrored into the picture's filmstrip entry, and saved with the sidecar. `state.metaDirty` is what makes autosave and `flushSidecar` write when only a rating changed. `adjust.version()` is untouched, so ratings never appear in History or the undo steps.
- **The strip** shows gold stars on a thumbnail's bottom-left corner, a color dot top-left, and a green `P` or red `X` badge top-right; a rejected picture is also dimmed.
- **The filter bar** above the thumbnails: the stars mean "at least N" (click the lit one again to switch off), `Pick` and `Reject` toggle (either matches), the five color dots toggle (any of them matches), and `Clear` appears while a filter is on. Different kinds of criteria are AND-ed (`MetaFilter::matches`). The right edge shows "N of M".
- **The open picture always stays in the strip**, even when it doesn't match, so rating it under a filter can't make it vanish. Left and Right step through the pictures that are shown.
- **Filtering and Sync Settings:** Ctrl+A selects only the pictures shown, changing the filter deselects any that it hides, and Sync Settings skips a hidden picture, so a look is never written into a picture you can't see.
- **Opening another folder clears the filter.**

How the strip learns each picture's rating: the thumbnail worker reads each picture's sidecar after its thumbnail (`sidecarScan`), and returns `edited` and `meta` in the same result. So a folder's ratings arrive as its thumbnails do, nearest to the open picture first; while a filter is on and some pictures are still unread the bar says "reading ratings...". Only the first reading of a picture counts (`FilmEntry::scanned`); after that the editor's own value is the truth. `edited` is now false for a file that holds only a rating (its `edits` are exactly the neutral ones); a file that can't be read, or has no `edits`, still counts as edited as before.

## Named snapshots

A **snapshot** is a named copy of every edit on one picture: the look, curves, Color Mixer, Color Grading, crop, rotation, straighten angle, corner mode **and all masks** (brush strokes and object bitmaps included). Restoring one puts the picture back exactly as it was. That is what separates it from a preset, which holds the look only and never touches geometry or masks.

```json
"snapshots": [
  { "name": "Golden hour", "edits": { "adjust": { ... }, "geometry": { ... }, "masks": [ ... ] } },
  { "name": "Black and white", "edits": { ... } }
]
```

Each `edits` object is **exactly** the form of the top-level `edits`: the snapshot is a `SidecarEdits` plus a name, written by `sidecarEditsToJson` and read by `sidecarEditsFromJson`. So a new slider, mask type or other field of the edits is saved in snapshots with no extra code. Things to know:

- **Not edits.** Like `meta`, snapshots sit beside `edits`. Adding or deleting one has no undo step and no History label. *Restoring* one is an ordinary edit: one undo step, labelled `Snapshot: <name>` (see [undo-and-history.md](undo-and-history.md#named-snapshots)).
- **Limits.** At most 20 per picture (`kMaxSnapshots`). A snapshot repeats every mask, and an object mask carries its bitmap, so the list is also capped at 24 MiB of JSON (`kSnapshotsMaxBytes`); the app refuses to add one past that. The whole sidecar is still read only up to 64 MiB.
- **Written with everything else.** `state.snapshotsDirty` is what makes autosave and `flushSidecar` write when only the list changed. Every writer of a whole sidecar passes the snapshots to `sidecarFileText`: the app's save, `sidecarSyncLook` and `sidecarPatchMeta` (which read them back through `sidecarReadForRewrite`). Names need not be unique.
- **The "has edits" dot ignores them.** A picture whose current edits are neutral is not marked edited just because it has snapshots (`sidecarScan` looks at `edits` only).
- **Per picture, and per virtual copy.** A copy has its own sidecar, so its own snapshots. A new copy starts with none.

The Snapshots pane (activity 11, `S`) has a name field, a **Save snapshot** button (Enter also saves), a list (click a row to restore it) and **Delete selected snapshot**. A blank name becomes `Snapshot 1`, `Snapshot 2`, and so on.

## Virtual copies

A **virtual copy** is another set of edits for the same picture file, so one photo can have, say, a color and a black-and-white version without a second file. A copy is only a sidecar, a few kilobytes:

```
photo.jpg                      the picture
photo.jpg.literoom.json        the original's edits
photo.jpg.vc1.literoom.json    virtual copy 1
photo.jpg.vc2.literoom.json    virtual copy 2   (copies 1 to 9; kMaxVirtualCopies)
```

A copy's sidecar is an ordinary sidecar (same wrapper, same `edits`, its own `meta` and `snapshots`), so everything on this page applies to it. `image.name` holds the real picture's name. There is no list of copies anywhere: they are found by scanning the folder for names of the form `<picture>.vc<n>.literoom.json` (`listVirtualCopies`) when the filmstrip is built. A copy whose picture isn't in the strip is not shown, and its file is left alone.

**One string identifies a picture in the program**, its path (the filmstrip's name index, thumbnail and sync results, the open picture). A copy gets its own path by appending a marker that no file name can contain: `/photos/photo.jpg|vc2`. Everything that keys on the path then tells copies apart for free. The few places that need the real file call `vcImageOf()` first: decoding, EXIF, the Sync worker's size lookup, the export's metadata copy. `sidecarFileOf()` understands the marker and returns the copy's own file name. See [headers.md](headers.md#virtual_copyhpp).

How the UI treats them (`main.cpp`):

- **Image > Create Virtual Copy** (Ctrl+Shift+N) writes a new sidecar holding the open picture's *current* edits (not its snapshots or rating), puts a cell after the picture's (badge `C2`), and opens it. The lowest free number is used. A tenth copy is refused.
- **Switching is instant.** Opening another copy of the picture already on screen reuses its pixels (`filmOpenIndex` hands `adoptDecoded` a copy of `state.original`) instead of decoding the file again. Everything else is the normal open: the old copy's edits are saved first, workers are invalidated, the new copy's sidecar is loaded.
- **Image > Delete Virtual Copy** (choose it twice) deletes the open copy's sidecar and returns to the original. The picture and its other copies are untouched. The original cannot be deleted this way.
- **Each copy has its own rating, flag, color label and "has edits" dot**, read from its own sidecar by the thumbnail worker. The worker does not decode a copy; a copy's thumbnail is its original's until the copy has been opened and left.
- **Sync Settings and batch ratings** treat a selected copy as any other picture: its own sidecar is rewritten. The open copy is never in the batch, but the original and the other copies are.
- **Export** names the file `<picture>-copy2-edit.jpg` by default and copies the real picture's metadata.
- **Limits of the design.** Renaming or deleting the picture in a file manager leaves its `.vc` files behind (they simply stop being shown). A copy's sidecar is checked against the picture's size like any other.

## Changing the format

- `kSidecarVersion` (currently 1) is written to every file by `saveSidecar` and checked by `loadSidecar`. A build whose own `kSidecarVersion` is lower than the file's ignores the **whole** file ("made by a newer Literoom"), so older builds cannot read newer sidecars. Bump it only for a change an older reader could not handle safely; additive changes (see below) usually don't need one.
- The `adjust` keys are slider **labels**. Renaming a label in `kAdjustmentSpecs` would orphan existing values in sidecars **and in saved presets** (see [extending.md](extending.md)).
- Adding a field is safe if the loader gives it a default and clamps it.
- **The 3D LUT was added without a version bump**: `lut` is a new optional key and `LUT Amount` a new `adjust` key. An older build ignores both (it opens the picture without the LUT) and its next save drops them, which is the usual cost of an additive change; a newer build reads every older file unchanged.
- **Every code path that rewrites a whole sidecar must go through `sidecarFileText`** (the app's own save, `sidecarSyncLook` and `sidecarPatchMeta` do). It writes the wrapper, `meta` and `snapshots` included, in one place, so a new top-level key added there can't be dropped by one writer and kept by another. A rewriter of *another* picture's file must also read the new key back in `sidecarReadForRewrite` and pass it to `sidecarFileText`, as it does for `meta` and `snapshots`.
- **Every field must be written by `sidecarEditsToJson` and read by `sidecarEditsFromJson`.** Sync Settings reloads a picture's sidecar and writes it back, so a field that only the app's own code kept (and the loader dropped) would be lost from every picture it syncs onto.
- A new mask type must be added to both `sidecarShapeToJson` and `sidecarShapeFromJson`. Presets do not store masks, so they need no change. Snapshots store whole edits, masks included, so they pick the new type up from those two functions.
- A new field of `SidecarEdits` is saved in snapshots automatically, but `AdjustmentSet::currentEdits()` and `restoreEdits()` must handle it too (see [extending.md](extending.md#two-things-to-watch)).
- The copy marker (`|vc`) and the `.vc<n>` part of the file name are part of the format now: changing either orphans existing virtual copies.
- A change to the slider, curve, Mixer or grading forms reaches presets too, because they share the same functions. Presets have their own `kPresetVersion`; bump it only for a change an older reader could not handle safely.