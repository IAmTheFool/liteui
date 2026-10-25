# Threading

Slow work (rendering a big photo, rotating it, decoding a file, running the AI model, making thumbnails, syncing settings onto other pictures) happens on background threads so the window never freezes. This page explains the one pattern almost all of it uses, the six workers, and the rules that keep it safe.

## The problem "latest wins" solves

When you drag a slider, the UI asks for a new render many times a second. If each request were queued, the editor would fall further and further behind and then spend seconds drawing results nobody wants. What you actually want is: **only the newest request matters.** Older work should be abandoned as soon as possible.

`latest_worker.hpp` implements exactly that, once, as a reusable class: `LatestWorker<Job, Result>`.

## How `LatestWorker` works

Every request gets a **generation number**. Think of it as a ticket number that only ever goes up.

1. **`submit(job)`** bumps the generation, stores the job (replacing one that was still waiting), and wakes the thread. The thread itself is created lazily, the first time you submit.
2. **The running job can check whether it was superseded.** It receives a `WorkerCancel` with the newest generation number, and calls `cancelled()` (or passes `latest` and `gen` to the routine it calls). If the number has moved on, it returns "abandoned" and its result is thrown away.
3. **A finished result is re-checked under a lock** before it is parked. If `invalidate()` ran while the job was finishing, the result is dropped instead of delivered. A stale result is never handed to the UI.
4. **`notify()` is called outside the lock** once a result is parked. In the app it calls `ui.wake()`, which is safe from any thread and wakes the otherwise sleeping event loop.
5. **The UI collects the result.** `collect()` is non-blocking and returns the result of the newest request if it is ready. `waitForLatest()` blocks, for the few places that need the real pixels *now*.

Other calls you will see:

| Call | Meaning |
|---|---|
| `invalidate()` | Forget whatever is waiting or running; its result will never be taken. |
| `outstanding()` | True from `submit()` until its result is taken or invalidated. |
| `shutdown()` | Stop and join the thread. Blocks until a job that ignores cancellation finishes. |
| `setNotify(fn)` | Must be called **before** the first `submit()`. |

The class has no UI or editor dependency, so it is tested with plain integers.

## The rule that makes it safe

> Everything public is called from **one** thread, the UI thread. The worker thread only runs the job function and touches the parked result.

A job must therefore be a **pure function of its inputs**. It gets an immutable picture and a *copy* of the parameters. It never reads `state`, never calls the LiteUI window and widget API, and never touches the `AdjustmentSet`, which the UI thread keeps changing. (The free decoder functions such as `liteui_image::decodeFile` are fine on a worker, and `LiteUI::wake()` is the one LiteUI member that is safe from any thread.)

One qualification: the segmentation job keeps per-image state (the loaded model, the encoded image embedding, the stand-in's prepared image) in function-local statics. That is safe only because a single thread ever runs it, and each cache is keyed by the image (`imageKey`), so a new picture never reuses a stale one. The one value the UI reads back from that worker, the engine label shown in the Object panel, goes through a mutex-guarded string.

This is also why the editor never edits images in place: a render job holds a pointer to `source`, and that pointer must stay valid and unchanged for as long as the job runs.

## The six workers

| Worker | Job | Can it be cancelled? | Notes |
|---|---|---|---|
| **Render** | Full-resolution `renderWith()` of `source` using a `RenderParams` copy. | Yes, checked between 128-row blocks and while building blur planes. | Started after an adjustment-only change on a large photo whose size hasn't changed: a slider release, undo, redo and so on. |
| **Geometry** | The whole pipeline: orient → straighten → crop → render, from `original`. | Yes, between stages, every 64 rows of the straighten, and between render blocks. | Used for rotate/straighten/crop on large photos. See below. |
| **Decode** | Decode a PNG/JPEG file and apply its EXIF rotation. | **No.** The image libraries have no cancel hook. | A second "open" queues behind a running decode; the first result is simply dropped. |
| **Segment** | Object selection: MobileSAM if available, otherwise the color-grow stand-in. | Partly. The stand-in can be cancelled at any time; MobileSAM's encode step can't be interrupted, so a superseded result is discarded after encoding. | Also does a "prefetch" job that only encodes the image, to warm up the model. Keeps per-image state in worker-only statics (see the rule above). |
| **Thumbnail** | Decode, orient and shrink each picture of the folder. | Not applicable (it is a queue). | **Different design**, see below. |
| **Sync** | Write one look into the sidecars of the pictures picked in the filmstrip (Sync Settings), or apply one rating / flag / color label change to them (a batch rating). | **No.** It always finishes its batch. | A `LatestWorker` used as a one-shot batch, see below. |

The first four and Sync are `LatestWorker`s. Jobs that cannot be cancelled simply always return a value (a failure value, if need be).

### Virtual copies and the workers

A virtual copy is another set of edits for the same file, identified by a path with a `|vc<n>` marker (see [sidecar-format.md](sidecar-format.md#virtual-copies)). Workers see only that string, so the rule is: **a worker that needs the real file calls `vcImageOf(path)` first.** The decode worker does (`runDecodeJob`), as does the Sync batch when it must learn a picture's size. The thumbnail worker skips decoding for copies altogether (see below), and the other workers never touch files.

Switching between copies of the picture that is already open needs **no decode**: `filmOpenIndex` invalidates the decode worker (a decode of some other file is no longer wanted) and calls `adoptDecoded` directly with a copy of `state.original`, so the usual consequences follow synchronously: the old copy's edits are saved, the render, geometry and segmentation workers are invalidated, and `imageSerial` is bumped. This is safe because the editor never edits images in place and `state.original` is the untouched decoded picture.

### The geometry worker rebuilds everything

A render job can start from `source`, because the source has not changed. A geometry job might be *changing* the source, so it always rebuilds the entire chain from `original`. That wastes some work (an unchanged straighten is redone when only the crop moved), but it keeps a job a pure function of its inputs with nothing that can drift out of sync, and the cost is only latency on a background thread, never a frozen window.

Its result is a `GeometryBundle` containing every geometry-derived field. The UI thread adopts it as **one atomic swap**, so the picture, the document size and the crop selection always change together.

While a geometry job is running, `state.rotated`, `state.source` and the crop selection still describe the *previous* geometry although the edit state already holds the new one. So `geometryBusy()` is true until the bundle lands, and any edit that measures against those (a second rotate that remaps the crop, Apply Crop) is ignored; their buttons are dimmed to match. The status bar shows "Applying changes..." meanwhile.

### The thumbnail worker is not a `LatestWorker`

Thumbnails are a different kind of work: you want *all* of them, not just the newest. `ThumbWorker` is a plain first-in-first-out queue with no generation numbers:

- `setQueue()` **replaces** what is still to do (the app calls it again whenever the open picture changes, so work always starts next to it).
- The item already in flight finishes.
- Results accumulate until `collect()` drains them.
- Like `LatestWorker`, `setNotify()` must be called before the first `setQueue()`.
- For each file it also reads the saved-edits sidecar (up to 64 MiB, like the editor): whether it holds real edits is what puts the dot on a thumbnail, and the rating, flag and color label in it are what the filmstrip filter uses. A picture that can't be decoded still gets those.
- **A virtual copy is read for its sidecar only.** Its path carries the `|vc<n>` marker (see [sidecar-format.md](sidecar-format.md#virtual-copies)), so `sidecarFileOf` finds the copy's own file. The worker never decodes it (`Result::thumbSkipped`), because it shows the same picture as its original: `collectThumbJobs` hands the original's raw thumbnail to the copies that follow it, and a failed original is the only failure that counts. A copy that is opened and left gets its own thumbnail like any picture.

### The sync worker is a batch that is never superseded

Sync Settings copies the open picture's look onto the other pictures selected in the filmstrip. Those pictures are not open, so there is no `AdjustmentSet` to edit. The job rewrites each one's sidecar file directly (see [sidecar-format.md](sidecar-format.md#syncing-a-look-onto-other-pictures)). A picture with no sidecar yet is decoded once to learn its size, which is why this is background work.

It uses `LatestWorker` for the thread, the parking slot and the wake-up, but not for "newest wins", because dropping a half-finished batch would leave some pictures synced and others not:

- **The job ignores cancellation** and always returns a result: one `SyncOutcome` (path, ok, reason) per picture. A failure on one picture (an unreadable sidecar, a different size, a read-only folder) is recorded and the batch carries on.
- **A second sync is refused while one is outstanding**, and `syncBusy()` also makes opening a picture (File > Open, a filmstrip click, the arrow keys) or a folder wait ("Wait for the sync to finish"). Nothing reads a sidecar that is about to be replaced.
- **The open picture is never in the batch.** Its edits live in memory and autosave writes its sidecar; syncing onto it would race with that. It is the *source* of the look, copied on the UI thread by `currentPreset()` when the job is built, so the job holds a plain value and never touches `state`.
- **Writes are temp-file-then-rename**, as in `saveSidecar`, so a crash or a full disk cannot leave half a sidecar.
- **Snapshots and copies.** The job rewrites a sidecar through `sidecarReadForRewrite` and `sidecarFileText`, so the picture's named snapshots are carried through unchanged. A selected virtual copy is just another path: its own sidecar is rewritten. The open picture is never in the batch, but its original and its other copies can be, since they are different files.
- **The same worker does batch ratings.** When pictures are selected, a rating / flag / color label command changes the open picture on the UI thread and submits a `metaOnly` job for the other selected pictures: `sidecarPatchMeta` instead of `sidecarSyncLook`, with the same refusals and the same one-batch-at-a-time rule. Its outcomes carry each picture's new meta, so `collectSyncJob()` updates the strip's entries (and sets no "has edits" dot).
- `collectSyncJob()` runs once per frame from `flushPendingAdjust`. It sets the "has edits" dot on each synced picture and shows "Synced N" or "Synced N, skipped M (file: reason)" in the status bar.

## How the UI picks results up

The event loop sleeps until the OS gives it an event, and a worker finishing is not an OS event. So each worker's `notify` is wired in `main()` to `ui.wake()`. When the loop wakes, it polls and adopts any finished result. Once per frame, `flushPendingAdjust` calls `collectFullRender()`, `collectGeometryJob()`, `collectDecodeJob()`, `collectSegmentJob()` and `collectSyncJob()`. Thumbnails are collected separately: `collectThumbJobs()` runs from the filmstrip canvas's `canvasDirtySource`.

Nothing runs while nothing is outstanding, so an idle editor costs nothing.

## Practical consequences

- **Export blocks on purpose.** `exportImage()` calls `finishGeometryJob()` and `finishFullRender()` first, so you never export a stale frame.
- **A drag cancels the full render.** `enterDrag()` invalidates the render worker, because the render in flight would be for values the drag is about to change.
- **A result for something that no longer exists is dropped.** For example, a finished object segmentation for a mask you deleted in the meantime, or a thumbnail for a folder you have left.
- **A new picture invalidates everything in flight.** When a decode lands, `adoptDecoded` invalidates the render, geometry and segmentation workers and bumps `imageSerial`, so nothing made for the old picture can be adopted over the new one.
- **Shutdown order matters.** `LiteUI::wake()` may only be called while the `LiteUI` object exists, so `main()` calls every worker's `shutdown()` right after `ui.run()` returns, before `ui` goes out of scope. The decode worker's `shutdown()` waits out a decode that is still running, because decodes can't be cancelled, and the sync worker's finishes the batch it is writing.
- **Small photos skip all of this.** For small images the pass is faster than the hand-off, so the render is synchronous.

## When you add a worker

1. If "newest wins" is the right behavior, use `LatestWorker`. Define a `Job` struct, a `Result` struct and a job function `std::optional<Result>(Job&&, WorkerCancel)`.
2. Give the job **only immutable inputs**. Do not capture `state`.
3. Pass `c.latest` and `c.gen` to any long loop, and return `std::nullopt` if it was superseded.
4. Call `setNotify(...)` with `ui.wake()` in `main()`, before the first `submit()`.
5. Poll with `collect()` each frame, and drop results that no longer apply.
6. Call its `shutdown()` in `main()` after `ui.run()` returns, before `ui` is destroyed.
7. Test timing with the repeat loop in [testing.md](testing.md).