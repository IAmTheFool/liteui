# Layout

LiteUI lays out a `View` tree with a **flexbox subset**. You describe each node with a `Style`; the engine computes an absolute window-space box for every node and the renderers only ever read that result.

This guide explains how sizes are resolved, how space is distributed, how absolute positioning and scrolling work, and where LiteUI deliberately differs from CSS. It also collects the recipes and gotchas that come up most often.

- [How layout runs](#how-layout-runs)
- [Box model](#box-model)
- [Sizing](#sizing)
- [Distributing space: grow, shrink, min and max](#distributing-space-grow-shrink-min-and-max)
- [Alignment](#alignment)
- [Wrapping](#wrapping)
- [Text](#text)
- [Absolute positioning](#absolute-positioning)
- [display and visibility](#display-and-visibility)
- [Overflow and scrolling](#overflow-and-scrolling)
- [Dynamic layout values and `onLayout`](#dynamic-layout-values-and-onlayout)
- [Paint and hit-test order](#paint-and-hit-test-order)
- [Recipes](#recipes)
- [Not supported](#not-supported)
- [Gotchas at a glance](#gotchas-at-a-glance)

## How layout runs

Layout is two passes over each subtree:

1. **Measure (bottom-up).** Each node resolves its own border-box size. `Fixed`, `Percentage` and `Full` resolve against the space its parent offers; `Fit` sums or maxes its children's sizes.
2. **Place (top-down).** Given a node's final box, the engine works out each child's basis size, distributes leftover space with `flexGrow`/`flexShrink`, positions children with `justifyContent` and `alignItems`, and recurses.

Layout runs:

- when you call `LiteUI::setRoot` (twice, with a poll of dynamic values in between, so the first paint is already correct),
- after any event or timer tick where a `Dynamic<>` value changed,
- when the window is resized, and
- when a scroll offset changes.

The root is sized by the same rules as any other node, against the **window size**, and placed at the window origin. (On Linux the origin is shifted down by the 32 px title bar unless the window was created with `hideTitlebar`.) A root left at the default `Size::fit()` will shrink to its content and **will not track the window**. Give the root `Size::full()` on both axes if you want it to fill the window.

```cpp
View root;
root.style.width  = Size::full();
root.style.height = Size::full();
```

### Coordinates

`View::computed` holds the result. `computed.x/y/w/h` is the node's **border box in absolute window coordinates**. Pointer handlers such as `onPressAt` and `onDragTo` report coordinates *local* to the view (0,0 at its own top-left). `onLayout` reports the window-space box (see [below](#dynamic-layout-values-and-onlayout)).

## Box model

LiteUI uses **border-box** sizing. A node's `width` and `height` include its padding.

| Property | Behavior |
|---|---|
| `padding` | Insets the content box that children are laid out in. Included in the node's own size. |
| `margin` | Space outside the box. Counted when distributing space along the main axis and when aligning on the cross axis. **Margins do not collapse.** |
| `gap` | Space between siblings along the main axis. For wrapped content it is also the space between lines. A single value, used for both. |
| `borderWidth`, `borderColor`, `borderRadius` | **Paint only.** The border takes no layout space. If you want content kept clear of it, add padding yourself. |

The border is drawn slightly differently per platform. Direct2D strokes it centered on the box edge, so half of it extends outside the box. The Linux renderer draws it inside the box and snaps rectangles to whole pixels, and clamps `borderRadius` to half of the smaller side.

Scroll containers also clip to their box with a plain rectangle. A `borderRadius` does not round the clip.

## Sizing

Each axis takes a `Size`:

| Constructor | Resolves to |
|---|---|
| `Size::pixel(n)` | Exactly `n` pixels (border box). |
| `Size::percentage(n)` | `n` percent (0 to 100) of the **space offered by the parent**. |
| `Size::full()` | All of the space offered by the parent on that axis. |
| `Size::fit()` (default) | The size of the content: the sum (main axis) or maximum (cross axis) of the children, plus padding. |

`minWidth`, `maxWidth`, `minHeight`, `maxHeight` clamp the result in pixels. If `max < min`, `min` wins.

Three details that differ from what you might expect from CSS:

- **"Space offered by the parent" is the parent's content box** (inside its padding). It does *not* subtract the child's own margin. In the main axis `flexShrink` absorbs the margin; on the cross axis a `Full` child with a margin extends past the container by that margin. Use `alignItems = Stretch` with a `Fit` size on the cross axis if you want margins respected there.
- **Percentages resolve against the offered space, not the parent's final size.** Inside a `Fit` container, a percentage child is a percentage of whatever space that container was offered, and the container then fits to the child.
- **Percentages and `Full` need a definite size.** Along an axis that is scrolling (see [Overflow](#overflow-and-scrolling)), children are measured against unbounded space where there is nothing to take a percentage of. Prefer `Fixed` or `Fit` sizes along the scrolling axis.

## Distributing space: grow, shrink, min and max

For each flow child, the engine computes a **basis**: its resolved size along the main axis (`Fixed`, `Percentage`, `Full`, or its content size for `Fit`). There is no separate `flexBasis` property. Then, per line:

- If children plus gaps and margins **fit**, leftover space goes to children with `flexGrow > 0`, in proportion to their `flexGrow`. Everything else keeps its basis.
- If they **overflow**, children shrink in proportion to `flexShrink` (default `1`).
- A child that would violate its own `min`/`max` is **frozen** at that bound, and the remaining space is redistributed among the others. This repeats until nothing new freezes.

So `flexGrow: 1` on every child gives each one *its basis plus an equal share of what is left*. Equal sizes only result if the bases are equal.

### Shrink is not weighted by size

In CSS, shrinking is weighted by each item's basis. **In LiteUI it is weighted by `flexShrink` alone.** Every item with the default `flexShrink = 1` gives up the same number of pixels, however big or small it is. The practical effect:

> A fixed-width sibling beside a `Full` (or otherwise large) sibling gets squeezed when the row is too narrow.

For example, in a 400 px row, a `Full` item (basis 400) next to a `Size::pixel(100)` item (basis 100) overflows by 100 px, so each loses 50 px: the `Full` item becomes 350 and the fixed item becomes **50**. Fix it by marking things that must keep their size:

```cpp
sidebar.style.width = Size::pixel(220);
sidebar.style.flexShrink = 0;      // never squeezed
```

A `minWidth` equal to the width has the same effect.

Along a scrolling main axis, shrinking is switched off entirely: children keep their natural size and the container scrolls instead.

## Alignment

### `justifyContent` (main axis, per line)

| Value | Effect |
|---|---|
| `Start` | Pack at the start. |
| `End` | Pack at the end. |
| `Center` | Center the group. |
| `SpaceBetween` | Equal space between items, none at the ends. |
| `SpaceAround` | Equal space around each item (half-size at the ends). |
| `SpaceEvenly` | Equal space between items and at both ends. |

The free space is never negative: if the content overflows, nothing is shifted off-screen by centering.

### `alignItems` (cross axis, within a line)

| Value | Effect |
|---|---|
| `Stretch` (default) | Grow to fill the line's cross size, **unless the child has an explicit cross size** (anything but `Fit`). |
| `Start` | Align to the cross-start edge. |
| `End` | Align to the cross-end edge. |
| `Center` | Center within the line. |

A child's cross size is also clamped by its own `min`/`max`.

### `alignContent` (cross axis, between lines)

Default is `Stretch`. With a **single line** (no wrapping, or wrapping that produced one line) the options behave like this:

- `Stretch` makes the line as tall (or wide) as the container's cross size. This is why `alignItems = Center` centers children in the whole container by default.
- Anything else packs the line at the start, the end or the center, and `alignItems` then works within **that line's own height**, not the container's.

If you set `alignContent` to something other than `Stretch` and then wonder why `alignItems = Center` stopped centering vertically, this is why.

## Wrapping

With `flexWrap = FlexWrap::Wrap`, children are packed greedily onto lines. A new line starts when the next child's basis would overflow the main-axis space. A line always takes at least one child, even an oversized one. `alignContent` then distributes the lines on the cross axis, and `gap` separates both items and lines.

Two limits to know about:

- **A `Fit` main axis never wraps.** There is no definite width to wrap against, so give the container a definite main size (`Full`, `Fixed`, `Percentage`, or a `flexGrow` in a bounded parent).
- **A `Fit` cross size assumes one line.** Measuring a `Fit`-height wrapping container reports the height of a *single* line to its parent, so wrapped lines overflow it. Give wrapping containers a definite height, or place them where overflow is acceptable.

```cpp
View tags;
tags.style.direction     = FlexDirection::Row;
tags.style.flexWrap      = FlexWrap::Wrap;
tags.style.alignContent  = AlignContent::Start;
tags.style.gap           = 8;
tags.style.width         = Size::full();       // definite width, so it can wrap
tags.style.height        = Size::pixel(200);   // definite height, so lines fit
```

## Text

`Text` nodes are measured with the real text backend, using the node's font settings, so layout and paint always agree.

- **Fit width** measures the text on **one line** (no wrapping) and uses that as the basis. If the container is too narrow the box shrinks and the glyphs wrap, **but the box's height was measured for the unwrapped text**, so wrapped lines can spill below the box.
- **A definite width** (`Full`, `Fixed`, `Percentage`) measures the text *at that width*, so the height accounts for wrapped lines. **Use a definite width for paragraphs.**

```cpp
Text body;
body.label = std::string("A long paragraph that should wrap inside its container...");
body.style.width = Size::full();     // wraps, and the height reflects the wrapped lines
```

- `Text::style` is **layout only**. A text leaf never paints its own background or border. To put text on a colored or bordered box, wrap it in a `View` and style that.
- `maxLines`, `overflow = TextOverflow::Ellipsis`, `lineHeight` and `letterSpacing` all take part in measurement.

## Absolute positioning

`position = Position::Absolute` pulls a node out of flex flow. It takes no space, does not affect siblings, and is placed against its **parent's content box** (inside the parent's padding, and shifted with the parent's scroll offset if it scrolls).

Position it with `left`, `top`, `right`, `bottom`. Each is `NaN` when unset (the default), and each is a `Dynamic<float>`.

| Edges set | Placement |
|---|---|
| `left` | `left + margin.left` from the content box's left edge |
| `right` only | `right + margin.right` in from the right edge |
| neither | the content box's origin, plus margin |
| `top` / `bottom` | same rules vertically |

Size rules:

- `Fixed`, `Percentage` and `Full` resolve against the parent's content box (always definite here).
- A `Fit` width with **both** `left` and `right` set derives its width from them (`contentW - left - right`). Likewise for height with `top` and `bottom`. Otherwise `Fit` uses the node's natural content size.

```cpp
View badge;
badge.style.position = Position::Absolute;
badge.style.right = 8.0f;
badge.style.top   = 8.0f;
```

### Stacking

Absolute nodes are **not** painted inside their parent's normal order. All absolute nodes in the whole tree are collected and painted **after** the entire in-flow tree, sorted by `zIndex` ascending, with document order (pre-order) breaking ties. An absolute node nested inside another absolute node still gets its own slot in that global list. `zIndex` has no effect on in-flow nodes: those paint in tree order, later siblings on top.

Hit-testing mirrors this: absolutes are tested first, from the highest `zIndex` / latest in document order down, then the flow tree.

### Clipping

An absolute node is clipped by every scrollable ancestor's viewport, like any other content, so it disappears when scrolled out of view. Set `escapeClip = true` to ignore ancestors' clips. This is how a dropdown or tooltip-like popup can extend outside a scroll container.

One known limitation: scrolling and wheel targeting do not apply the ancestors' clip to absolute descendants, so an absolute view inside a scrolled-out region can still be found by the wheel.

## display and visibility

| Property | Effect |
|---|---|
| `display = Display::None` | The node and its whole subtree are skipped by layout, paint and hit-testing. **No space is reserved.** |
| `visibility = Visibility::Hidden` | The node and its subtree are **not painted or hit-tested**, but **layout still reserves its space** and siblings flow around it. |

Both are `Dynamic<>`, so a callback can toggle them:

```cpp
bool open = false;
menu.style.display = [&] { return open ? Display::Flex : Display::None; };
```

## Overflow and scrolling

`overflowX` and `overflowY` are set independently per axis.

| Value | Clips children | Scrollbar | Notes |
|---|---|---|---|
| `Visible` (default) | no | no | Children can draw outside the box. No scrolling. |
| `Hidden` | yes | no | See the note below. |
| `Scroll` | yes | always | Even if the content fits. |
| `Auto` | yes | only when content exceeds the viewport on that axis | |

Any value other than `Visible` makes the node a **scroll container**. Its children are measured against *unbounded* space along each scrolling axis, so they report their true desired size instead of being squeezed into the viewport, and that natural total becomes the **scrollable extent**. The extent includes padding and is deliberately not capped by `maxWidth`/`maxHeight` (those limit the box, not the content). In-flow children only count: absolute children do not contribute.

> **Note on `Hidden`.** It clips and draws no scrollbar, but in the current implementation the content can still be moved with the mouse wheel or by dragging, because every non-`Visible` axis is treated as scrollable. If you want a truly fixed clipped box, also set `wheelScrollEnabled = false` and `contentPanEnabled = false`.

### Giving a container something to scroll

A scroll container only scrolls if its **own box is smaller than its content**, so its size along the scrolling axis must not be `Fit` (a `Fit` box simply grows to hold everything). Typical ways to bound it:

- a `Fixed` or `Full` size, or
- leave it `Fit` as a child of a bounded column and let `flexShrink` (default `1`) squeeze it to the available space, which is exactly what you want for a "fill the remaining height" pane.

```cpp
View list;
list.style.overflowY = Overflow::Auto;
list.style.flexGrow  = 1;          // take the remaining height of the column; scroll if it is not enough
```

### Scrollbars

- Width/height of a bar is `kScrollbarThickness` (**12 px**), a public constant shared by layout and painting so they can never disagree.
- A visible bar **reserves a gutter**: a vertical bar narrows the content box by 12 px, and a horizontal bar reduces its height by 12 px. Content never sits under a bar. `Auto` shows a bar only once content exceeds the viewport (with a half-pixel tolerance so a perfect fit does not flicker).
- The thumb's length is proportional to viewport divided by content, with a minimum of 20 px. Where both bars show, the corner square is filled with the track color.
- Colors: `Style::scrollTrackColor` and `scrollThumbColor` override the window-wide defaults from `LiteUI::setScrollbarColors`, independently for track and thumb.
- `scrollbarAutoHide = true` paints the bar only while the view is hovered or its thumb is being dragged.

### How users scroll

| Input | Behavior |
|---|---|
| Mouse wheel | Scrolls the **deepest scrollable view under the pointer** (scrollbar or content, it does not matter). Disable per view with `wheelScrollEnabled = false`. Wheel notches still fire `onScrollUp`/`onScrollDown`, so a view can turn the wheel into something else (a zoom, say). |
| Drag a thumb | Scrolls proportionally. |
| Click empty track | Jumps the thumb to the click position. |
| Drag in the content | **Pans** the content. The pointer must move more than **4 px** before it counts as a drag. Until then the press is still an ordinary click, and once it becomes a pan the pending `onClick` is **cancelled**. Disable per view with `contentPanEnabled = false`. |

Scroll offsets belong to the node. They are **re-clamped on every relayout** (so shrinking content or growing the window cannot leave you scrolled past the end), they are preserved on rows reused by a [keyed list](../README.md#keyed-dynamic-lists), and they reset when `setRoot` replaces the tree.

## Dynamic layout values and `onLayout`

Layout-affecting fields such as `width`, `height`, `margin`, `padding`, `left`/`top`/`right`/`bottom`, `display` and `zIndex` are `Dynamic<>`, so they can be callbacks. LiteUI polls them after each dispatched event, and if one changed it relays out and repaints. `gap`, `direction`, `flexGrow`, `flexShrink`, `minWidth`... are plain fields, so changing them after the tree is built does not reach the screen. Make something dynamic (for example with `display` or a sized wrapper) if it must change at runtime.

`onLayout(x, y, w, h)` fires once per layout, right after the node's final box is known. It is the way to read a view's real size, for instance to anchor a popup:

```cpp
float rx = 0, ry = 0;                      // parent's origin
float bx = 0, by = 0, bh = 0;              // button's box
bool  open = false;

root.onLayout   = [&](float x, float y, float, float)        { rx = x; ry = y; };
button.onLayout = [&](float x, float y, float, float h)      { bx = x; by = y; bh = h; };

menu.style.position = Position::Absolute;
menu.style.zIndex   = 10;
menu.style.left     = [&] { return bx - rx; };       // left/top are relative to the parent's
menu.style.top      = [&] { return by + bh - ry; };  // content box; onLayout reports window coords
menu.style.display  = [&] { return open ? Display::Flex : Display::None; };
```

Callbacks are polled *before* the relayout that updates the values they read, so a popup follows a moving anchor one update behind. The first `setRoot` runs an extra layout pass specifically so values captured this way are correct before the first paint.

## Paint and hit-test order

1. A node paints its background, then its border, then (for a canvas) its drawing, then its in-flow children in order, then its scrollbars.
2. Children of a scroll container are clipped to its box, and nested scroll containers narrow the clip further.
3. After the whole in-flow tree, absolute nodes paint in `zIndex`, then document order.
4. Hover state, `hoverColor` and click bubbling follow the same tree: clicks go to the topmost view under the pointer that has a handler for that button, then bubble to the nearest ancestor that does.

## Recipes

**Centered content**

```cpp
View root;
root.style.width          = Size::full();
root.style.height         = Size::full();
root.style.direction      = FlexDirection::Column;
root.style.alignItems     = Align::Center;
root.style.justifyContent = Justify::Center;
root.style.gap            = 16;
```

**Header, scrolling content, footer**

```cpp
View app;
app.style.direction = FlexDirection::Column;
app.style.width = Size::full();
app.style.height = Size::full();

header.style.height = Size::pixel(48);  header.style.flexShrink = 0;
footer.style.height = Size::pixel(28);  footer.style.flexShrink = 0;

content.style.flexGrow  = 1;               // fill what the header and footer leave
content.style.overflowY = Overflow::Auto;  // scroll if it is still too tall
```

**Sidebar plus main area**

```cpp
View body;                                  // direction defaults to Row
body.style.width = Size::full();
body.style.height = Size::full();

sidebar.style.width = Size::pixel(220);
sidebar.style.flexShrink = 0;               // keep its width when the window narrows
main.style.flexGrow = 1;                    // take the rest
```

**Equal-width columns**

```cpp
for (View *col : {&a, &b, &c}) {
    col->style.width = Size::pixel(0);      // zero basis...
    col->style.flexGrow = 1;                // ...so every column gets an equal share
}
```

**A spacer that pushes siblings apart**

```cpp
View spacer;
spacer.style.flexGrow = 1;                  // between two buttons in a toolbar row
```

**A badge in the corner of a card**

```cpp
badge.style.position = Position::Absolute;
badge.style.top = 8.0f;
badge.style.right = 8.0f;
card.addChild(badge);
```

**A wrapping paragraph**

```cpp
Text p;
p.label = std::string("Long text...");
p.style.width = Size::full();               // definite width, so it wraps with a correct height
```

## Not supported

These CSS features have no equivalent in `Style` today:

- `flexBasis`, `alignSelf`, `order`, reverse directions, and separate row/column gaps
- auto margins, percentage margins or padding, negative-space tricks such as margin collapsing
- `position: relative`, `fixed` or `sticky`
- `aspect-ratio`, `calc()`, grid layout
- `box-sizing` other than border-box, borders that take layout space
- per-side borders, per-corner radii, and clipping to a rounded rectangle

## Gotchas at a glance

| Symptom | Cause and fix |
|---|---|
| The UI does not fill the window | The root is `Fit` by default. Set `Size::full()` on the root's width and height. |
| A fixed-width box shrinks next to a `Full` or large sibling | Shrink is weighted by `flexShrink` only. Set `flexShrink = 0` (or a `minWidth`) on the box. |
| Wrapped text overflows its box vertically | Text with `Fit` width is measured unwrapped. Give it a `Full`, `Fixed` or `Percentage` width. |
| A wrapping container's lines overflow it | A `Fit` height assumes a single line. Give the container a definite height. |
| A container with `overflowY` never scrolls | Its height is `Fit`, so it grows to fit its content. Bound it (`Fixed`, `Full`, or `flexGrow`/shrink inside a bounded parent). |
| `alignItems = Center` stopped centering | `alignContent` is not `Stretch`, so the single line is only as tall as its content. Leave `alignContent` at `Stretch`. |
| A `Full` child with a margin pokes out of its parent on the cross axis | `Full` ignores the child's margin there. Use `Fit` plus `alignItems = Stretch`, or reduce the size to account for it. |
| Text on a colored background shows no background | `Text::style` is layout only. Put the text in a styled `View`. |
| Content is hidden behind a border | Borders are paint-only and take no space. Add padding. |
| A popup is positioned wrongly on Linux | `onLayout` reports window coordinates, but `left`/`top` are relative to the parent's content box. Subtract the parent's origin. |
| Changing `gap`, `direction` or `flexGrow` at runtime does nothing | Those are plain fields, not `Dynamic<>`. Rebuild the node (or use a keyed list) instead. |
| A `Hidden`-overflow box still moves under the wheel | Expected today. Set `wheelScrollEnabled = false` and `contentPanEnabled = false`. |
