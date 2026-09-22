# Platform notes & known limitations

This is a single reference for everything in `liteui.hpp` that behaves
differently between Windows and Linux, or that's a deliberate v1
simplification rather than a bug. Pulled together from the comments
already sitting next to the relevant code — if you're debugging
something that looks wrong, check here before assuming it's a bug.

## Canvas 2D (`CanvasContext`)

The Windows backend (Direct2D, v1 `ID2D1RenderTarget`) and the Linux
backend (Cairo) expose the identical public API, but a few operations
resolve differently or aren't fully implemented on one side:

| Feature | Linux (Cairo) | Windows (Direct2D) |
|---|---|---|
| `getImageData` | Fully supported — reads back the canvas's own backing surface | **Unsupported** — always returns `std::nullopt`. A plain `ID2D1RenderTarget` isn't CPU-readable without WIC/DXGI plumbing this header doesn't pull in |
| `putImageData` / `drawImage` | Supported | Supported |
| `clearRect` | True sub-rectangle clear (`CAIRO_OPERATOR_CLEAR`) | Clears the **entire** canvas — Direct2D v1's `Clear()` ignores the requested rect, clip, and transform entirely |
| `globalCompositeOperation` | Full range, maps 1:1 onto Cairo operators | Only ever draws normal source-over alpha blending — a plain `ID2D1RenderTarget` has no Porter-Duff blend control. The value is stored but has no visible effect |
| Radial gradients | Exact — Cairo's native two-circle model | Approximated. Direct2D only models a single circle plus a focal point, so the inner-radius stop is approximated by rescaling stop offsets rather than drawn exactly |

Independent of platform:

- **No image decoding inside `CanvasContext` itself.** `CanvasImage`
  just wraps raw, already-decoded RGBA8 pixels — `drawImage` never
  reads a PNG/JPEG file for you. (The higher-level `Image` node *does*
  decode PNG/JPEG automatically via libpng/libjpeg on Linux and WIC on
  Windows — this limitation is specifically about calling
  `CanvasContext::drawImage` directly inside a `Canvas` node's
  `onPaint`.)
- **Shadows are a flat, unblurred offset copy** — `setShadow`'s blur
  parameter is accepted but ignored on both backends. There's no
  Gaussian blur pass.
- **`strokeText` is an approximation**, not a true stroked glyph
  outline. Both backends draw the fill text in the stroke color,
  nudged across a small ring of directions, to fake a stroke effect.
  It will look wrong for large stroke widths or unusual fonts.
- **`fillText`/`strokeText`'s `maxWidth` parameter is accepted for API
  familiarity with HTML canvas but currently does nothing** — there's
  no horizontal squeeze-to-fit.

## SVG (`Svg` node / `liteui_svg` parser)

- Supports: `path`, `rect`, `circle`, `ellipse`, `line`, `polyline`,
  `polygon`, `<g>`/`<symbol>`/nested `<svg>` as transform/style
  containers, `<use>` (both `href` and `xlink:href`, with cycle and
  nesting-depth guards), presentation attributes, inline `style=""`,
  and a **single-class** subset of `<style>` block CSS (`.foo {
  fill: ...; }` rules referenced via `class="foo"`). Compound
  selectors, `@media`, tag/id/descendant selectors, and CSS
  specificity rules are not implemented — only flat `.classname`
  rules are recognized, and are matched in file order.
- **Not supported at all:** gradients, patterns, `clipPath`/`mask`,
  and `<text>` elements inside SVG documents.
- **`<use>` referencing a `<symbol>` does not honor that symbol's own
  `width`/`height`/`viewBox`/`preserveAspectRatio`** — it's rendered
  at its natural coordinates, exactly like referencing a plain `<g>`
  would be.
- **Group opacity is approximated**, not true isolated-layer
  compositing — an `opacity` on a `<g>` is folded into each
  descendant shape's own fill/stroke alpha individually. This looks
  correct for non-overlapping shapes but will look wrong wherever a
  group's own shapes overlap each other.
- This stays fully vector — an `Svg` node re-renders at whatever pixel
  size it's laid out to (including across a window resize), unlike
  `Image`, which is a fixed-resolution bitmap.

## XML parsing (`liteui_xml`, used internally by SVG)

A general-purpose but intentionally partial XML parser:

- No external entity resolution, no DTD validation.
- No namespace resolution — a prefix like `xlink:href` is kept as
  plain attribute-name text, not resolved against a namespace URI.
- **Mixed content order is not preserved.** All text at a given
  nesting level is concatenated into that node's `text` field in
  document order, regardless of where child elements fall between the
  runs. This is fine for SVG (which never relies on interleaved
  text/element content) but would misrepresent arbitrary XML/HTML
  where text and elements alternate meaningfully.

## Layout engine (flexbox subset)

- **Percentages resolve to `0` when an ancestor's size is itself
  `Fit`** (i.e. indefinite) — there's no "auto" fallback the way CSS
  sometimes provides.
- **`FlexWrap::Wrap` only affects the placement pass, not sizing.** A
  container with `Size::fit()` on its main axis still measures its
  natural size assuming a single line, since a `Fit` main axis has no
  definite width to wrap against in the first place. If you want
  wrapping content inside a `Fit`-sized container to actually wrap,
  give the container a definite or `Size::full()` main-axis size
  instead.

## Absolute positioning, scrolling, and hit-testing

- `Position::Absolute` descendants are **not** threaded through the
  clip-aware walks that back scrolling, hit-testing, or tooltip
  targeting (`resolveScrollTarget`, `hitTestFlow`,
  `hitTestTooltipFlow`). This means an absolutely positioned view that
  sits visually inside a scrolled-out region of some ancestor can
  still register clicks/hover/tooltips as if it were visible. This is
  a deliberate v1 trade-off, not an oversight — absolutes are always
  collected and tested globally, unclipped, matching how they're also
  always painted in a separate global-stacking pass on top of the flow
  tree.
- Absolute nodes stack by `zIndex`, ties broken by document discovery
  order (`collectAbsolutes`/`sortAbsolutes`) — this ordering is shared
  identically between painting and hit-testing, so what's on top
  visually is also what receives the click.

## `TextInput`

- **The built-in single-line text field only accepts ASCII input**
  (codepoints `0x20`–`0x7F`) — both typed text (`onTextInput`) and
  pasted text (`Ctrl+V`, which strips anything ≥ `0x80` along with line
  breaks) are filtered to ASCII. This matches the widget's own
  byte-offset cursor tracking, which does not currently handle
  multi-byte UTF-8 correctly. Plain `Text` rendering (via DirectWrite
  / Pango) has no such restriction — this limitation is specific to
  the editable `TextInput` widget.

## Clipboard

- On Linux, copy/paste shells out to whichever of `wl-copy`/`wl-paste`
  (Wayland), `xclip`, or `xsel` is actually installed and usable —
  checked in that order, and only counted as "available" if the
  corresponding display server environment variable is also set.
- **If none of those tools are present, clipboard falls back to an
  in-process string** that only round-trips within the same running
  app — it will not see what another application copied, and other
  applications will not see what LiteUI "copied." This fallback is
  silent: there's no error surfaced to the app when it engages.

## Native file/folder pickers

- On Linux, `openFilePicker`/`saveFilePicker`/`openFolderPicker`/
  `saveFolderPicker` shell out to `zenity` or `kdialog`, in that
  order. **If neither is installed, every picker call returns
  `std::nullopt`** as if the user cancelled — there is no built-in
  fallback dialog.

## Windowing / backend coverage

- Windows: native decorations via Win32, no custom titlebar code path.
- Linux: Wayland only (via `xdg-shell`), with a custom client-side
  titlebar drawn in-process (`kTitlebarHeight`), since Wayland has no
  server-side decoration guarantee. There is no X11 backend.
- There is no macOS backend at all.
- No touch/gesture input handling on either platform — only mouse and
  keyboard events are wired up.