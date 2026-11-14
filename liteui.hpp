// liteui.hpp
//
// Single-header, cross-platform window / UI library.
//
// USAGE
//   Include this file wherever you need the API (it only pulls in a few
//   standard headers, so it is cheap). In EXACTLY ONE .cpp file of your
//   program, define LITEUI_IMPLEMENTATION before including it; that is the
//   one translation unit that compiles the platform backend (Win32 /
//   Wayland+Cairo+Pango+EGL) and sees the platform headers:
//
//       // ui.cpp
//       #define LITEUI_IMPLEMENTATION
//       #include "liteui.hpp"
//
//   A program with a single .cpp file just does both in that file.
//   With CMake, linking liteui::liteui supplies the include path, C++23 and
//   the platform libraries, but you still define LITEUI_IMPLEMENTATION in
//   one .cpp of each executable.
//
// Platform members/methods are selected at compile time via #ifdef.

#ifndef LITEUI_HPP_INCLUDED
#define LITEUI_HPP_INCLUDED

#define LITEUI_VERSION_MAJOR 0
#define LITEUI_VERSION_MINOR 6

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Opaque platform types referenced (by pointer only) from View::Computed, so
// the public part of this header never needs the platform SDK headers.
#if defined(_WIN32)
struct IDWriteTextLayout;
struct ID2D1Bitmap;
struct ID2D1Bitmap1;
#else
typedef struct _cairo_surface cairo_surface_t;
#endif

// Plain RGBA color, one byte per channel (alpha defaults to 255).
struct Color {
  uint8_t r = 0, g = 0, b = 0, a = 255;
  bool operator==(const Color &) const = default;
};

// Which button an event refers to (a pen tip or finger counts as Left). Keys
// the per-button press tracking in the pointer router and selects which of a
// View's onClick / onAuxClick / onContextMenu gets fired.
//
// `None` is what PointerEvent::button holds on events where no button
// changed state (moves, wheel, enter/leave). It is deliberately LAST so the
// numeric values of Left/Middle/Right (used as array indices) are unchanged.
enum class MouseButton { Left, Middle, Right, None };

// Bit for a button in PointerEvent::buttons (Left = 1, Middle = 2, Right = 4;
// None = 0). Note this is NOT the DOM's `buttons` bit order (which puts
// right before middle) — it simply follows the enum order.
constexpr uint8_t buttonBit(MouseButton b) {
  return b == MouseButton::None
             ? uint8_t{0}
             : static_cast<uint8_t>(1u << static_cast<int>(b));
}

// Logical key identity, independent of platform scancode/VK. Extend as
// needed — this covers what a typical text field / shortcut system wants.
enum class Key {
  Unknown = 0,
  A,
  B,
  C,
  D,
  E,
  F,
  G,
  H,
  I,
  J,
  K,
  L,
  M,
  N,
  O,
  P,
  Q,
  R,
  S,
  T,
  U,
  V,
  W,
  X,
  Y,
  Z,
  N0,
  N1,
  N2,
  N3,
  N4,
  N5,
  N6,
  N7,
  N8,
  N9,
  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
  Enter,
  Escape,
  Backspace,
  Tab,
  Space,
  Delete,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  PageUp,
  PageDown,
  Shift,
  Control,
  Alt,
  Super,
  Minus,
  Equal,
  LeftBracket,
  RightBracket,
  Backslash,
  Semicolon,
  Quote,
  Comma,
  Period,
  Slash,
  Grave,
};

struct KeyModifiers {
  bool shift = false, ctrl = false, alt = false, super = false;
  bool operator==(const KeyModifiers &) const = default;
};

struct KeyEvent {
  Key key = Key::Unknown;
  KeyModifiers mods;
};

// ---------------------------------------------------------------------------
// Pointer events (modelled on the DOM pointer-events API).
//
// Dispatch model, written down once:
//
//  * Down / move / up / click / wheel go to the DEEPEST view under the pointer
//    that has a handler for that event, then BUBBLE to its ancestors (also
//    only those with a handler) until a handler calls e.stopPropagation().
//    A Position::Absolute view bubbles to its parent in the tree, like the
//    DOM, not to whatever happens to be painted beneath it.
//  * Views without a handler are transparent to the pointer; so are disabled
//    views.
//  * Capture: a press captures the pointer. The capture target is the view
//    that called stopPropagation() in its onPointerDown, or, if none did, the
//    deepest view whose onPointerDown ran. While any button is held, moves
//    and the final up go to the capture target (then bubble from it) even
//    when the pointer is outside it. It ends when the last button is
//    released, or via onPointerCancel (focus loss, OS taking over the mouse).
//    onPointerCancel is NOT fired for a view that is being destroyed (a
//    keyed-list rebuild, setRoot()): the view may already be gone.
//  * onPointerEnter / onPointerLeave do not bubble. They follow the same
//    hover flag that drives Style::hoverColor, so they are purely geometric
//    and are not suppressed while another view holds capture.
//  * onClick fires for the LEFT button only, and only if the release lands on
//    the same view the press did. Middle = onAuxClick, right = onContextMenu
//    (fired on release). onDoubleClick follows a second left click within
//    500 ms and 4 px; PointerEvent::clickCount holds the running count.
//  * onWheel: if no handler calls stopPropagation(), the wheel falls through
//    to the existing overflow scrolling.
//  * Pen and touch use the same events. A pen tip touching down, or a finger,
//    is MouseButton::Left (so clicks and double-clicks work for them with no
//    extra code); the pen's barrel button is Right and its second barrel
//    button Middle; the eraser end sets PointerEvent::eraser. Each pen tool
//    and finger has its own pointerId, capture, and click pairing. A pen
//    reports hover (moves with buttons == 0, pressure 0) before and after
//    contact and drives onPointerEnter/Leave and hover colours like a mouse;
//    a finger has no hover, so it only produces events while it is down.
//    Touch panning of scrollable content (Style::contentPanEnabled) and
//    scrollbar grabs work for fingers and pens as for the mouse, one pointer
//    at a time. onPointerCancel is also what a touch gets when the system
//    takes it over (a compositor gesture, a lost capture).
//  * Coordinates in an event are rewritten for each view the event reaches:
//    x/y are local to the view whose handler is running; windowX/windowY
//    are not.
// ---------------------------------------------------------------------------
enum class PointerType : uint8_t { Mouse, Pen, Touch };

// The mouse cursor shown over a view; see Style::cursor. The names follow
// CSS's `cursor` keywords. `Inherit` (the default) means "use my parent's";
// the root's fallback is `Default`.
enum class Cursor : uint8_t {
  Inherit,    // CSS `inherit`/`auto`: take the nearest ancestor's cursor
  Default,    // the ordinary arrow
  None,       // hide the cursor
  Pointer,    // hand, for things that are clickable
  Text,       // I-beam
  Crosshair,
  Move,       // four-way arrows
  Grab,       // open hand
  Grabbing,   // closed hand
  NotAllowed,
  Wait,       // busy, blocks the UI
  Progress,   // busy, but still interactive
  Help,
  ResizeEW,   // <->
  ResizeNS,
  ResizeNESW, // diagonal
  ResizeNWSE,
  ZoomIn,
  ZoomOut,
};

// One position sample of a pointer, in WINDOW coordinates (see
// PointerEvent::coalesced and PointerEvent::toLocal).
struct PointerSample {
  float x = 0, y = 0;
  float pressure = 0;
  float tiltX = 0, tiltY = 0, twist = 0;
  uint64_t timeMs = 0;
};

struct PointerEvent {
  float x = 0, y = 0;         // local to the view whose handler is running
  float windowX = 0, windowY = 0;
  // Which pointer this is. The mouse is always 1; every pen tool and every
  // finger contact gets its own id for as long as it exists, so several
  // touches can be in flight at once, each with its own capture and clicks.
  uint32_t pointerId = 1;
  // The primary pointer of its type: the mouse; the first finger that went
  // down (until it lifts); a pen.
  bool isPrimary = true;
  MouseButton button = MouseButton::None; // the button that changed (down/up)
  uint8_t buttons = 0;        // bitmask (buttonBit) of buttons currently held,
                              // AFTER this event's change
  KeyModifiers mods;
  PointerType type = PointerType::Mouse;
  float pressure = 0.5f;      // 0..1 (mouse: 0.5 while a button is held, else 0)
  float tiltX = 0, tiltY = 0, twist = 0;
  bool eraser = false;
  uint8_t clickCount = 0;     // onClick / onDoubleClick only; 0 elsewhere
  uint64_t timeMs = 0;        // monotonic milliseconds (arbitrary epoch)

  // onPointerMove only: EVERY position the pointer reported since the
  // previous move event, oldest first, ending with this event's own position
  // — so a fast pen stroke loses no samples even though handlers run once per
  // frame. Always at least one entry on a move; empty on other events. Valid
  // only while the handler runs: copy what you need. Window coordinates; use
  // toLocal() for the view the handler belongs to.
  std::span<const PointerSample> coalesced;
  PointerSample toLocal(PointerSample sample) const {
    sample.x += x - windowX;
    sample.y += y - windowY;
    return sample;
  }

  // Stops this event from reaching any further (ancestor) view.
  void stopPropagation() { stopped_ = true; }
  bool propagationStopped() const { return stopped_; }
  // True if `b` is currently held.
  bool held(MouseButton b) const { return (buttons & buttonBit(b)) != 0; }
  // A copy that hasn't been stopped yet (for firing a second event off the
  // same data, e.g. onDoubleClick after onClick).
  PointerEvent fresh() const {
    PointerEvent c = *this;
    c.stopped_ = false;
    return c;
  }

private:
  bool stopped_ = false;
};

struct WheelEvent {
  float x = 0, y = 0;         // local to the view whose handler is running
  float windowX = 0, windowY = 0;
  float dx = 0, dy = 0;       // pixels; +dy = content scrolls down, +dx = right
  bool precise = false;       // trackpad-style pixel deltas vs. a notched
                              // wheel. Backends that cannot tell leave false.
  KeyModifiers mods;
  uint64_t timeMs = 0;

  // Stops bubbling AND suppresses the built-in overflow scrolling.
  void stopPropagation() { stopped_ = true; }
  bool propagationStopped() const { return stopped_; }

private:
  bool stopped_ = false;
};

using PointerHandler = std::function<void(PointerEvent &)>;
using WheelHandler = std::function<void(WheelEvent &)>;

// The type of View::onClick (and Canvas::onClick). Accepts either
//   [&](PointerEvent &e) { ... }   // bubbles to ancestors unless the handler
//                                  // calls e.stopPropagation()
//   [&] { ... }                    // short form: runs, then ENDS bubbling
//                                  // (a no-argument handler cannot call
//                                  // stopPropagation(), so it is taken to have
//                                  // handled the click)
class PointerCallback {
public:
  PointerCallback() = default;
  PointerCallback(std::nullptr_t) {}
  template <class F>
    requires(!std::is_same_v<std::decay_t<F>, PointerCallback> &&
             !std::is_same_v<std::decay_t<F>, std::nullptr_t> &&
             (std::is_invocable_v<std::decay_t<F> &, PointerEvent &> ||
              std::is_invocable_v<std::decay_t<F> &>))
  PointerCallback(F &&f) {
    if constexpr (std::is_invocable_v<std::decay_t<F> &, PointerEvent &>) {
      fn_ = std::forward<F>(f);
    } else {
      endsBubbling_ = true;
      fn_ = [g = std::forward<F>(f)](PointerEvent &) mutable { g(); };
    }
  }
  explicit operator bool() const { return static_cast<bool>(fn_); }
  void operator()(PointerEvent &e) const { fn_(e); }
  // Direct call without an event.
  void operator()() const {
    PointerEvent e;
    fn_(e);
  }
  // True if constructed from a no-argument callable (see above).
  bool endsBubbling() const { return endsBubbling_; }

private:
  std::function<void(PointerEvent &)> fn_;
  bool endsBubbling_ = false;
};

// Width (vertical bar) / height (horizontal bar) of a scrollbar's track, in
// pixels. Shared between the layout engine (which reserves this much space
// out of a scroll container's content box on whichever edges show a bar)
// and both renderers (which draw the track/thumb at exactly this size) —
// keeping it in one place means layout and painting can never disagree
// about how much room a bar takes up.
constexpr float kScrollbarThickness = 12.0f;

// A field settable as either a fixed value or a callback polled after each
// dispatched event (see checkForUpdates) — lets author code write
// `x = "hello";` or `x = []{ return someVar; };` through plain assignment.
template <class T> using Dynamic = std::variant<T, std::function<T()>>;

// What every read site outside checkForUpdates should call: the stored
// value directly if it's static (always fresh, no indirection), or `cache`
// — the last value checkForUpdates polled from the callback — if dynamic.
template <class T>
inline const T &resolveDynamic(const Dynamic<T> &field, const T &cache) {
  if (auto *v = std::get_if<T>(&field))
    return *v;
  return cache;
}

// For a Dynamic<> field built from a source that's inherently never a
// callback (Canvas::setFont's plain params, a TextInput's/TooltipStyle's
// plain fields, ...) — no polled cache exists or is needed for these, so
// there's nothing to fall back to. Throws (via std::get) if that
// assumption is ever violated, rather than silently resolving to a wrong
// default.
template <class T> inline const T &resolveStatic(const Dynamic<T> &field) {
  return std::get<T>(field);
}

// ---------------- Layout engine: Size / Style / View ----------------

// A size along one axis. Fixed/Percentage are self-explanatory; Fit sizes
// to the sum/max of children (like CSS's "auto" on a flex item); Full fills
// whatever space the parent gives along that axis (like width: 100%, but
// resolved from available space rather than the parent's own size).
struct Size {
  enum class Kind { Fixed, Percentage, Fit, Full };
  Kind kind = Kind::Fit;
  float value = 0; // pixels for Fixed, 0-100 for Percentage; unused otherwise

  static Size pixel(float v) { return {Kind::Fixed, v}; }
  static Size percentage(float v) { return {Kind::Percentage, v}; }
  static Size fit() { return {Kind::Fit, 0}; }
  static Size full() { return {Kind::Full, 0}; }
};

enum class FlexDirection { Row, Column };
enum class Justify {
  Start,
  End,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly
};

enum class Align { Start, End, Center, Stretch };

// Whether children that overflow the main axis wrap onto additional lines.
enum class FlexWrap { NoWrap, Wrap };

// How multiple flex lines are distributed along the cross axis. Only
// meaningful when FlexWrap::Wrap actually produces more than one line;
// with a single line this reduces to Stretch filling crossAvail (matching
// the old un-wrapped behavior) or the others packing that one line at the
// start.
enum class AlignContent {
  Start,
  End,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly,
  Stretch
};

struct EdgeInsets {
  float top = 0, right = 0, bottom = 0, left = 0;
  static EdgeInsets all(float v) { return {v, v, v, v}; }
  bool operator==(const EdgeInsets &) const = default;
};

enum class Position { Static, Absolute };

// display:none analog — a false-y node is skipped entirely by layout,
// paint, and hit-testing, everywhere Position::Absolute is already
// skipped for being "out of flow". Space is NOT reserved.
enum class Display { Flex, None };

// visibility:hidden analog — layout still reserves the node's space and
// siblings flow around it normally; only paint/hit-test/hover skip it.
enum class Visibility { Visible, Hidden };

// CSS-style overflow behavior for one axis of a container. Visible (the
// default) behavior: children are never clipped and never
// scroll, regardless of how big they get. Hidden clips children to the
// container's box but offers no scrollbar/interaction. Scroll always
// clips *and* always shows that axis's scrollbar, even if content
// currently fits. Auto clips and shows the scrollbar only when content
// actually exceeds the viewport on that axis.
enum class Overflow { Visible, Hidden, Scroll, Auto };

struct Style {
  Dynamic<Size> width = Size::fit();
  Dynamic<Size> height = Size::fit();

  // Clamp bounds applied (in pixels) after width/height above are resolved.
  // Defaults impose no constraint. maxWidth < minWidth (or the height
  // equivalent) is treated as maxWidth == minWidth rather than producing a
  // negative range.
  float minWidth = 0;
  float maxWidth = std::numeric_limits<float>::infinity();
  float minHeight = 0;
  float maxHeight = std::numeric_limits<float>::infinity();

  Dynamic<EdgeInsets> margin;
  Dynamic<EdgeInsets> padding;

  FlexDirection direction = FlexDirection::Row;
  Justify justifyContent = Justify::Start;
  Align alignItems = Align::Stretch;
  float gap = 0;
  FlexWrap flexWrap = FlexWrap::NoWrap;
  AlignContent alignContent = AlignContent::Stretch;

  float flexGrow = 0;
  float flexShrink = 1;

  Dynamic<Color> backgroundColor = Color{0, 0, 0, 0};
  Dynamic<float> borderWidth = 0.0f;
  Dynamic<Color> borderColor = Color{0, 0, 0};
  Dynamic<float> borderRadius = 0.0f;

  // Applied instead of backgroundColor while the pointer is over this
  // view. Set directly by the app; isHovered itself is derived
  // automatically by LiteUI::updateHover from pointer position, never
  // app-sourced.
  std::optional<Color> hoverColor;

  // The mouse cursor while the pointer is over this view. Inherited: a view
  // with Cursor::Inherit (the default) shows its parent's cursor, so setting
  // it once on a container covers everything inside. While a view holds
  // pointer capture (a press with an onPointerDown) the
  // capture target's cursor is shown wherever the pointer goes, so a drag
  // keeps its "grabbing"/resize cursor outside the view. A window-edge resize
  // handle still wins over this. Accepts a plain value or a callback, like
  // the other Dynamic<> style fields:
  //   v.style.cursor = Cursor::Pointer;
  //   v.style.cursor = [&] { return tool == Tool::Brush ? Cursor::Crosshair
  //                                                      : Cursor::Default; };
  Dynamic<Cursor> cursor = Cursor::Inherit;

  // Absolute children are pulled out of flex distribution entirely and
  // placed against the parent's content box using left/top/right/bottom.
  // NaN means "unset" for each edge. If width/height is Fit and both
  // opposing edges are set, size is derived from them (contentW - left -
  // right); otherwise size resolves normally (Fixed/Percentage/Fit/Full)
  // against the containing block, same as an in-flow child would.
  Position position = Position::Static;
  Dynamic<float> left = std::numeric_limits<float>::quiet_NaN();
  Dynamic<float> top = std::numeric_limits<float>::quiet_NaN();
  Dynamic<float> right = std::numeric_limits<float>::quiet_NaN();
  Dynamic<float> bottom = std::numeric_limits<float>::quiet_NaN();
  Dynamic<int> zIndex = 0;

  // When either axis is non-Visible, this view becomes a scroll container:
  // its children are measured at their natural size (not squeezed to fit)
  // and clipped+offset by the view's live scroll position. See
  // View::Computed::scrollX/scrollY and the "Scrolling" section of the
  // layout engine below for how the two axes are handled independently.
  Overflow overflowX = Overflow::Visible;
  Overflow overflowY = Overflow::Visible;
  bool contentPanEnabled = true;
  bool wheelScrollEnabled = true;

  Dynamic<std::optional<Color>> scrollTrackColor;
  Dynamic<std::optional<Color>> scrollThumbColor;
  bool scrollbarAutoHide = false;
  Dynamic<Display> display = Display::Flex;
  Dynamic<Visibility> visibility = Visibility::Visible;
  bool escapeClip = false; // absolute: ignore scrollable ancestors' clip
};

// ---------------- Text: author-facing, leaf-only ----------------
enum class FontWeight : int {
  Thin = 100,
  ExtraLight = 200,
  Light = 300,
  Regular = 400,
  Medium = 500,
  SemiBold = 600,
  Bold = 700,
  ExtraBold = 800,
  Black = 900
};
enum class FontStyle { Normal, Italic };
enum class TextAlign { Start, Center, End, Justify };
enum class TextOverflow { Clip, Ellipsis };
enum class TextWrap { Wrap, NoWrap };

// Internal mirror of Text's styling fields, stored on the View a Text
// flattens into (see View::toView below). Kept as its own struct so View
// doesn't have to duplicate every Text field under a different name.
struct TextStyle {
  Dynamic<float> fontSize = 16.0f;
  Dynamic<FontWeight> fontWeight = FontWeight::Regular;
  Dynamic<FontStyle> fontStyle = FontStyle::Normal;
  std::string fontFamily; // empty = platform default UI font
  Dynamic<Color> color = Color{0, 0, 0};
  TextAlign align = TextAlign::Start;
  TextOverflow overflow = TextOverflow::Clip;
  TextWrap wrap = TextWrap::Wrap;
  float lineHeight = 0; // 0 = auto from font metrics
  float letterSpacing = 0;
  int maxLines = 0; // 0 = unlimited
  bool underline = false;
  bool strikethrough = false;
};

// Author-facing leaf node. addChild(Text) flattens this into a View (see
// View::toView) — Text itself never appears in the retained tree.
struct Text {
  Dynamic<std::string> label = std::string{};
  Style
      style; // layout only — width/height/margin/position/etc, reused from View
  float fontSize = 16.0f;
  FontWeight fontWeight = FontWeight::Regular;
  FontStyle fontStyle = FontStyle::Normal;
  std::string fontFamily;
  Dynamic<Color> color = Color{0, 0, 0};
  TextAlign align = TextAlign::Start;
  TextOverflow overflow = TextOverflow::Clip;
  TextWrap wrap = TextWrap::Wrap;
  float lineHeight = 0;
  float letterSpacing = 0;
  int maxLines = 0;
  bool underline = false;
  bool strikethrough = false;
};

// ---------------- Canvas: retained, immediate-mode-style 2D drawing
// ----------------
//
// Canvas is deliberately modeled after the HTML5 <canvas> 2D context: the
// app gets a CanvasContext and calls familiar methods (moveTo/lineTo/arc/
// fill/stroke/fillText/drawImage/...) on it. Unlike HTML canvas, the app
// never touches raw pixels directly except via getImageData/putImageData —
// everything else is vector drawing, matching what Direct2D (Windows) and
// Cairo (Linux) both do natively.
//
// CanvasContext's public methods are declared below. The platform backends
// (CanvasContextImpl, in the LITEUI_IMPLEMENTATION section, one per
// #if defined(_WIN32) branch) wrap either an ID2D1RenderTarget or a cairo_t
// directly rather than going through a virtual interface — consistent with
// this file's "no vtable" design. Both backends implement the exact same
// public method surface, so app code that only calls CanvasContext's public
// API compiles unchanged on either platform.
//
// Known limitations:
//  - Image decoding is PNG and JPEG only (liteui_image: libpng / libjpeg on
//    Linux, WIC on Windows); CanvasImage just wraps decoded RGBA8 pixels.
//    Other formats are up to the app.
//  - getImageData and clearRect are fully supported on BOTH platforms.
//    On Windows every canvas node renders on the GPU: its backing store is
//    an ID2D1Bitmap1 render-target texture on the window's Direct3D 11
//    device (Direct2D 1.1 drawing through that device). clearRect is a
//    clipped, antialiased D2D1_PRIMITIVE_BLEND_COPY fill of transparent
//    black, so it honours the current transform exactly like Cairo's
//    CAIRO_OPERATOR_CLEAR. getImageData copies the requested region into a
//    CPU-readable staging texture and maps it: that is a GPU->CPU readback
//    (it flushes pending work), so avoid calling it many times per frame.
//    putImageData and drawImage work on both platforms as before.
//  - Linux Canvas rendering runs on the GPU: every canvas node is an
//    OpenGL ES 3.0 texture (EGL context, MSAA when available) drawn with
//    batched shaders, stencil-based fills/clips, and a shared glyph atlas
//    for text. It is the default; configure with -DLITEUI_CANVAS_GL=OFF to
//    use the previous Cairo-based backend instead. On the GL backend:
//      * globalCompositeOperation supports all twelve operators (Multiply
//        reads back the canvas drawn so far, so it costs one copy of the
//        canvas per shape; the others are plain GPU blending).
//      * strokeText strokes the true glyph outlines (the Cairo and Windows
//        backends approximate it with offset copies).
//      * colour (emoji) glyphs keep their own colours in fillText when the
//        fill is a solid colour; with a gradient fill they draw as
//        silhouettes tinted by the gradient.
//      * a lost GL context (EGL_CONTEXT_LOST) is recovered automatically:
//        the context, programs and every canvas/text texture are rebuilt.
//    With the Cairo backend, globalCompositeOperation maps onto the full
//    range of Cairo operators.
//  - On Windows, Direct2D's primitive blend modes only cover SourceOver,
//    Copy and Lighter (additive), so those three work and every other
//    globalCompositeOperation falls back to SourceOver.
//  - Radial gradients use the full two-circle model exactly on Linux (Cairo
//    natively, or the GL backend's shader); Direct2D only models a single circle plus a focal point, so
//    the inner-radius stop is approximated there by rescaling stop
//    offsets rather than drawn exactly.
//  - Shadows are a simple flat-color, unblurred offset copy (no Gaussian
//    blur).
//  - On Windows and with the Cairo backend, strokeText approximates a
//    stroked glyph outline by drawing the fill text in the stroke color,
//    nudged in a ring of directions — it is not a true outline of the
//    glyph contours. (The GL backend does trace the real outlines.)
//  - fillText/strokeText's maxWidth parameter is accepted for API
//    familiarity but currently unused (no horizontal squeeze-to-fit).
class CanvasContext;

// A simple RGBA8, straight- (non-premultiplied) alpha bitmap. The app is
// responsible for populating `pixels` (e.g. from a decoded image file or
// generated procedurally, or decoded with liteui_image::decodeFile /
// decodeMemory).
struct CanvasImage {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels; // width*height*4 bytes, row-major, RGBA8

  CanvasImage() = default;
  CanvasImage(int w, int h)
      : width(w), height(h),
        pixels(static_cast<size_t>(std::max(0, w)) * std::max(0, h) * 4, 0) {}
};

// ---- Pixel formats (storage description only) ---------------------------
// An image's storage format, its colour space and the format a display
// backend wants are three separate properties; this enum covers only the
// first. CanvasImage above is still always RGBA8_UNORM. The helpers are tiny
// constexpr inline functions, so they add nothing to build time.
enum class PixelFormat : uint8_t {
  RGBA8_UNORM,  // 4 x uint8, sRGB-encoded colour, display-referred
  RGBA16_UNORM, // 4 x uint16, sRGB-encoded colour, display-referred
  RGBA16_FLOAT, // 4 x IEEE half, linear light (half the memory of FP32;
                // ~3 decimal digits, max 65504)
  RGBA32_FLOAT  // 4 x IEEE single, linear light (not clamped to 0..1)
};

// Bytes in one pixel; 0 for a value that isn't a valid PixelFormat.
constexpr size_t bytesPerPixel(PixelFormat f) noexcept {
  switch (f) {
  case PixelFormat::RGBA8_UNORM:
    return 4;
  case PixelFormat::RGBA16_UNORM:
  case PixelFormat::RGBA16_FLOAT:
    return 8;
  case PixelFormat::RGBA32_FLOAT:
    return 16;
  }
  return 0;
}

// Smallest legal row size in bytes (width * bytesPerPixel), or nullopt if
// the format is invalid or the product overflows size_t. A row may be
// longer than this (padded stride) but never shorter.
constexpr std::optional<size_t> minimumRowBytes(uint32_t width,
                                                PixelFormat f) noexcept {
  const size_t bpp = bytesPerPixel(f);
  if (bpp == 0 || width > std::numeric_limits<size_t>::max() / bpp)
    return std::nullopt;
  return static_cast<size_t>(width) * bpp;
}

// Total buffer size for `height` rows. rowStride == 0 means tightly packed;
// otherwise it must be >= minimumRowBytes. Returns nullopt for an invalid
// format, a too-small stride, or any overflow. Zero width or height is
// valid and gives 0 bytes (an empty image).
constexpr std::optional<size_t> imageByteSize(uint32_t width, uint32_t height,
                                              PixelFormat f,
                                              size_t rowStride = 0) noexcept {
  const auto minRow = minimumRowBytes(width, f);
  if (!minRow)
    return std::nullopt;
  const size_t stride = rowStride ? rowStride : *minRow;
  if (stride < *minRow)
    return std::nullopt;
  if (height != 0 && stride > std::numeric_limits<size_t>::max() / height)
    return std::nullopt;
  return stride * static_cast<size_t>(height);
}

// IEEE 754 binary16 <-> float. floatToHalf rounds to nearest-even, turns
// values beyond 65504 into +-infinity, flushes nothing (subnormals are kept)
// and keeps NaN as NaN. halfToFloat is exact.
inline float halfToFloat(uint16_t h) noexcept {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  uint32_t e = (h >> 10) & 0x1Fu;
  uint32_t m = h & 0x3FFu;
  uint32_t bits;
  if (e == 0) {
    if (m == 0)
      bits = sign;
    else // subnormal: normalise
    {
      int shift = 0;
      while (!(m & 0x400u)) {
        m <<= 1;
        ++shift;
      }
      m &= 0x3FFu;
      bits = sign | (static_cast<uint32_t>(113 - shift) << 23) | (m << 13);
    }
  } else if (e == 31)
    bits = sign | 0x7F800000u | (m << 13);
  else
    bits = sign | ((e + 112u) << 23) | (m << 13);
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

inline uint16_t floatToHalf(float f) noexcept {
  uint32_t x;
  std::memcpy(&x, &f, sizeof x);
  const uint32_t sign = (x >> 16) & 0x8000u;
  const uint32_t e = (x >> 23) & 0xFFu;
  uint32_t m = x & 0x7FFFFFu;
  if (e == 0xFFu) // inf / NaN
    return static_cast<uint16_t>(sign | 0x7C00u |
                                 (m ? (0x200u | (m >> 13)) : 0u));
  const int exp = static_cast<int>(e) - 127 + 15;
  if (exp >= 31) // overflow
    return static_cast<uint16_t>(sign | 0x7C00u);
  if (exp <= 0) // subnormal half, or zero
  {
    if (exp < -10)
      return static_cast<uint16_t>(sign);
    m |= 0x800000u;
    const int shift = 14 - exp;
    uint32_t half = m >> shift;
    const uint32_t rem = m & ((1u << shift) - 1u);
    const uint32_t mid = 1u << (shift - 1);
    if (rem > mid || (rem == mid && (half & 1u)))
      ++half;
    return static_cast<uint16_t>(sign | half);
  }
  uint32_t half = (static_cast<uint32_t>(exp) << 10) | (m >> 13);
  const uint32_t rem = m & 0x1FFFu;
  if (rem > 0x1000u || (rem == 0x1000u && (half & 1u)))
    ++half; // a carry into the exponent is correct (and may reach infinity)
  return static_cast<uint16_t>(sign | half);
}

// Formats ImageBuffer can allocate and read/write: all of PixelFormat.
constexpr bool isStorageSupported(PixelFormat f) noexcept {
  return bytesPerPixel(f) != 0;
}

// Generic pixel storage for any supported PixelFormat, independent of the UI.
// CanvasImage stays the RGBA8 type the canvas API uses; this is the
// higher-precision sibling. Rows are `rowStride` bytes apart (>= one packed
// row; padding is allowed). Bytes are std::byte because the contents are
// uint8 or float depending on `format`; accessors go through memcpy, so no
// alignment is assumed. Values are stored as given: no colour-space
// conversion, no clamping of float data.
struct ImageBuffer {
  uint32_t width = 0, height = 0;
  size_t rowStride = 0; // bytes between row starts
  PixelFormat format = PixelFormat::RGBA8_UNORM;
  std::vector<std::byte> pixels;

  ImageBuffer() = default;
  ImageBuffer(const ImageBuffer &) = default;
  ImageBuffer &operator=(const ImageBuffer &) = default;
  // A moved-from buffer is left empty (and valid), not half-described.
  ImageBuffer(ImageBuffer &&o) noexcept
      : width(o.width), height(o.height), rowStride(o.rowStride),
        format(o.format), pixels(std::move(o.pixels)) {
    o.reset();
  }
  ImageBuffer &operator=(ImageBuffer &&o) noexcept {
    if (this != &o) {
      width = o.width;
      height = o.height;
      rowStride = o.rowStride;
      format = o.format;
      pixels = std::move(o.pixels);
      o.reset();
    }
    return *this;
  }

  // Allocates a zero-filled buffer. rowStride 0 = tightly packed. Returns
  // nullopt for an unsupported format, a stride below one row, a size
  // overflow, or allocation failure. Zero width/height gives an empty image.
  static std::optional<ImageBuffer> create(uint32_t w, uint32_t h,
                                           PixelFormat f, size_t stride = 0) {
    if (!isStorageSupported(f))
      return std::nullopt;
    const auto total = imageByteSize(w, h, f, stride);
    if (!total)
      return std::nullopt;
    ImageBuffer b;
    try {
      b.pixels.assign(*total, std::byte{0});
    } catch (const std::exception &) {
      return std::nullopt; // bad_alloc / length_error
    }
    b.width = w;
    b.height = h;
    b.format = f;
    b.rowStride = stride ? stride : *minimumRowBytes(w, f);
    return b;
  }

  bool empty() const { return width == 0 || height == 0; }
  size_t byteSize() const { return pixels.size(); }

  // True when the fields and the buffer agree: supported format, stride >=
  // one row, pixels.size() == stride * height. An empty buffer is valid.
  bool valid() const {
    if (!isStorageSupported(format))
      return false;
    if (empty())
      return pixels.empty();
    const auto total = imageByteSize(width, height, format, rowStride);
    return rowStride != 0 && total && *total == pixels.size();
  }

  // Start of row y, or nullptr if the buffer is invalid or y is out of range.
  std::byte *row(uint32_t y) {
    return (y < height && valid() && !empty())
               ? pixels.data() + static_cast<size_t>(y) * rowStride
               : nullptr;
  }
  const std::byte *row(uint32_t y) const {
    return (y < height && valid() && !empty())
               ? pixels.data() + static_cast<size_t>(y) * rowStride
               : nullptr;
  }

  // Reads pixel (x, y) as four floats r,g,b,a exactly as stored: UNORM
  // formats are divided by 255 / 65535 (a plain normalisation, NOT an sRGB
  // decode), float formats are widened unchanged. False if out of range or
  // the buffer is invalid.
  bool getPixel(uint32_t x, uint32_t y, float out[4]) const {
    const std::byte *r = (x < width) ? row(y) : nullptr;
    if (!r)
      return false;
    const size_t bpp = bytesPerPixel(format);
    const std::byte *p = r + static_cast<size_t>(x) * bpp;
    switch (format) {
    case PixelFormat::RGBA8_UNORM:
      for (int i = 0; i < 4; ++i)
        out[i] = static_cast<float>(static_cast<uint8_t>(p[i])) / 255.0f;
      break;
    case PixelFormat::RGBA16_UNORM: {
      uint16_t v[4];
      std::memcpy(v, p, sizeof v);
      for (int i = 0; i < 4; ++i)
        out[i] = static_cast<float>(v[i]) / 65535.0f;
      break;
    }
    case PixelFormat::RGBA16_FLOAT: {
      uint16_t v[4];
      std::memcpy(v, p, sizeof v);
      for (int i = 0; i < 4; ++i)
        out[i] = halfToFloat(v[i]);
      break;
    }
    case PixelFormat::RGBA32_FLOAT:
      std::memcpy(out, p, 4 * sizeof(float));
      break;
    }
    return true;
  }

  // Writes pixel (x, y) from four floats. RGBA32F stores the values
  // untouched (HDR values > 1, negatives, NaN and inf are preserved).
  // RGBA16F rounds to half precision (beyond +-65504 becomes +-inf; NaN is
  // kept). The UNORM formats clamp to 0..1, round to nearest, and map NaN
  // to 0. False if out of range or the buffer is invalid.
  bool setPixel(uint32_t x, uint32_t y, const float in[4]) {
    std::byte *r = (x < width) ? row(y) : nullptr;
    if (!r)
      return false;
    std::byte *p = r + static_cast<size_t>(x) * bytesPerPixel(format);
    switch (format) {
    case PixelFormat::RGBA8_UNORM:
      for (int i = 0; i < 4; ++i) {
        float v = in[i];
        v = (v != v) ? 0.0f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
        p[i] = static_cast<std::byte>(static_cast<uint8_t>(v * 255.0f + 0.5f));
      }
      break;
    case PixelFormat::RGBA16_UNORM: {
      uint16_t v16[4];
      for (int i = 0; i < 4; ++i) {
        float v = in[i];
        v = (v != v) ? 0.0f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
        v16[i] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
      }
      std::memcpy(p, v16, sizeof v16);
      break;
    }
    case PixelFormat::RGBA16_FLOAT: {
      uint16_t v16[4];
      for (int i = 0; i < 4; ++i)
        v16[i] = floatToHalf(in[i]);
      std::memcpy(p, v16, sizeof v16);
      break;
    }
    case PixelFormat::RGBA32_FLOAT:
      std::memcpy(p, in, 4 * sizeof(float));
      break;
    }
    return true;
  }

private:
  void reset() noexcept {
    width = height = 0;
    rowStride = 0;
    pixels.clear();
  }
};

// How a scene-linear image (RGBA32_FLOAT) is turned into 8-bit display
// pixels. Only the output transform lives here; storage and colour space
// are separate properties (see PixelFormat / ImageBuffer).
enum class ClampMode : uint8_t {
  PerChannel, // clip each of r,g,b to 0..1 (simple; very bright colours go
              // toward a primary or white)
  PreserveHue // if the brightest channel exceeds 1, scale r,g,b together so
              // it lands on 1 (keeps the r:g:b ratio); negatives still clip
};

// Display transform used by liteui_image::makeDisplayImage and by
// Image::displaySettings. Today: exposure in stops, a clamp mode, and a
// fixed sRGB output encoding. Applies to RGBA32_FLOAT sources only; RGBA8
// images are already display-referred and pass through unchanged, so
// ordinary UI assets keep their expected appearance.
//
// Room for later (deliberately not implemented yet, so this struct can grow
// without changing makeDisplayImage's signature):
//  - tone-mapping curves (filmic / Reinhard / ACES-style) ahead of the clamp
//  - other output colour spaces (Display P3, Rec.2020, gamma 2.2/2.4) and a
//    working-space -> display-space 3x3 matrix; ICC profiles
//  - highlight roll-off / soft clip and a configurable white point
//  - an RGBA16_FLOAT / RGBA32_FLOAT output option to upload float textures
//    directly once a backend supports them (no 8-bit quantisation)
//  - dithering before 8-bit quantisation to hide banding in gradients
//  - black point, contrast and saturation (develop-style adjustments belong
//    in the separate RAW/photo module, not here)
struct DisplaySettings {
  float exposureEv = 0.0f; // stops: linear rgb *= 2^exposureEv (non-finite ->
                           // 0, limited to +-32)
  ClampMode clamp = ClampMode::PerChannel;

  bool operator==(const DisplaySettings &) const = default;
};

// How a decoded image's aspect ratio is reconciled with the box it's
// drawn into — same semantics as CSS object-fit.
enum class ObjectFit { Fill, Contain, Cover, None, ScaleDown };

// A single filter entry — Windows-style pattern syntax on both platforms
// ("*.png;*.jpg"), so one filter list works unmodified either way.
struct FileFilter {
  std::string name;
  std::string pattern; // ';'-separated globs, e.g. "*.png;*.jpg"
};

// Blocks until the user picks a file or cancels. Returns the chosen path,
// or std::nullopt on cancel/failure. Safe to call from an onClick handler —
// it's a modal, synchronous call on both platforms, matching how
// IFileOpenDialog::Show() already blocks.
std::optional<std::string>
openFilePicker(const std::string &title = "Open File",
               const std::vector<FileFilter> &filters = {});

// Like openFilePicker, but the user may select several files (Ctrl / Shift).
// Returns the chosen paths, or an empty vector on cancel/failure.
std::vector<std::string>
openFilesPicker(const std::string &title = "Open Files",
                const std::vector<FileFilter> &filters = {});

// Blocks until the user picks a save location or cancels. `defaultName` is
// the pre-filled filename (e.g. "export.png"); `defaultExt` (no leading
// dot, e.g. "png") gets appended if the user's typed name doesn't already
// end in a known extension from `filters`. Returns the chosen path, or
// std::nullopt on cancel/failure.
std::optional<std::string>
saveFilePicker(const std::string &title = "Save File",
               const std::string &defaultName = "",
               const std::vector<FileFilter> &filters = {},
               const std::string &defaultExt = "");

// Blocks until the user picks a folder or cancels. Unlike files, folders
// have no natural "open" vs "save" distinction — saveFolderPicker exists
// purely to offer a different default title/button framing (e.g. "Select
// Folder" for a save-destination use case). Returns the chosen path, or
// std::nullopt on cancel/failure.
std::optional<std::string>
openFolderPicker(const std::string &title = "Open Folder");

std::optional<std::string>
saveFolderPicker(const std::string &title = "Select Folder");

// ---------------- Clipboard / image decoding (implemented in the
// LITEUI_IMPLEMENTATION section) ----------------
namespace liteui_clipboard {
std::string toLF(const std::string &in);
std::string toCRLF(const std::string &in);
bool setText(const std::string &utf8);
std::optional<std::string> getText();
void setTextWithFallback(const std::string &s);
std::string getTextWithFallback();
} // namespace liteui_clipboard

// Text helper documented for authors of hand-built Canvas widgets: returns the
// byte index in `text` whose caret position is closest to `clickX` (measured
// from the text's own left edge).
namespace liteui_text {
size_t caretIndexForX(const std::string &text, const TextStyle &style,
                      float fontSize, FontWeight fontWeight,
                      FontStyle fontStyleVal, float clickX);
} // namespace liteui_text

namespace liteui_image {
std::optional<CanvasImage> decodeMemory(const uint8_t *data, size_t size,
                                        std::string *errorOut = nullptr);
std::optional<CanvasImage> decodeFile(const std::string &path,
                                      std::string *errorOut = nullptr);

// ---- Pixel conversion between ImageBuffer formats ----------------------
// Colour conventions: the UNORM formats (RGBA8, RGBA16) hold sRGB-encoded,
// display-referred colour; the float formats (RGBA16F, RGBA32F) hold
// LINEAR-light colour (the working formats, not clamped to 0..1). Alpha is
// straight (not premultiplied) everywhere and is never colour-transformed:
// it is only scaled or quantised.
//
// IEC 61966-2-1 sRGB transfer functions. Not clamped; negative inputs are
// mirrored about 0 (f(-x) = -f(x)); NaN in gives NaN out.
float srgbToLinear(float encoded);
float linearToSrgb(float linear);

// Converts `src` into the format named by dst.format, replacing dst's
// contents with a tightly packed image of src's size. Returns false and
// leaves dst untouched if src is invalid, either format is unsupported, or
// allocation fails.
//   same format        : plain copy (padding dropped)
//   UNORM <-> UNORM    : the encoded values are rescaled (8->16 is x257,
//                        16->8 rounds); no transfer function is involved
//   anything else      : through linear float. UNORM sources are sRGB-
//                        decoded; UNORM destinations get NaN -> 0, a clamp
//                        to 0..1 (+-inf clamp), sRGB encoding and rounding.
//                        Float destinations keep the values (RGBA16F rounds
//                        to half precision and overflows to inf past 65504).
// Converting an image into itself is a no-op (it already has that format).
bool convertImage(const ImageBuffer &source, ImageBuffer &destination);

// Display-ready RGBA8 copy of `source` as a CanvasImage: RGBA8 buffers are
// copied as-is (padding dropped); RGBA32F buffers are treated as linear
// light and sRGB-encoded with clamping (no tone mapping). nullopt if the
// buffer is invalid/unsupported or too large for CanvasImage's int sizes.
std::optional<CanvasImage> toCanvasImage(const ImageBuffer &source);
std::optional<CanvasImage> toCanvasImage(const ImageBuffer &source,
                                         const DisplaySettings &settings);

// Display adapter: an RGBA8 image ready for the existing paint path.
//  - RGBA32_FLOAT (linear light): rgb *= 2^exposureEv, then the clamp mode
//    (NaN -> 0, negatives -> 0, +inf is a very bright value), sRGB-encode,
//    round to 8 bits. Alpha is only clamped/quantised, never exposed.
//  - RGBA8_UNORM: already display-referred, copied unchanged (padding
//    dropped) whatever the settings; convert to RGBA32_FLOAT first with
//    convertImage if you want to apply exposure to an 8-bit image.
// nullopt if `source` is invalid or its format unsupported.
std::optional<ImageBuffer>
makeDisplayImage(const ImageBuffer &source,
                 const DisplaySettings &settings = {});
// Rows are processed in small chunks (no full-size float copy), so an
// RGBA16_FLOAT photo is displayed without ever doubling its memory.
// RGBA16_UNORM is display-referred like RGBA8 and is just rescaled to 8 bits.

// A rectangle of pixels in an image.
struct ImageRect {
  uint32_t x = 0, y = 0, width = 0, height = 0;
};

// Same as makeDisplayImage but only for `region` of the source, producing
// an RGBA8 image of the region's size. This is the building block for
// tiled / viewport-only rendering of large photos: the result is bit-for-
// bit what the matching crop of the full display image would be. nullopt
// if the region does not lie inside the source.
std::optional<ImageBuffer> makeDisplayImage(const ImageBuffer &source,
                                            const DisplaySettings &settings,
                                            const ImageRect &region);

// Reduced-resolution copy for fast previews (e.g. while a slider moves):
// each output pixel is the box average of up to factor x factor source
// pixels, computed in linear light and weighted by alpha so transparent
// pixels don't darken edges (a block of fully transparent pixels gets rgb
// 0). Output is ceil(w/factor) x ceil(h/factor) in the SAME format as the
// source. factor 1 copies; factor 0 or an invalid source gives nullopt.
std::optional<ImageBuffer> downscaleImage(const ImageBuffer &source,
                                          uint32_t factor);

// ---- Image files <-> ImageBuffer, 8- and 16-bit ---------------------------
// decodeFile / decodeMemory above always give an 8-bit CanvasImage (a 16-bit
// PNG is reduced to 8 bits). The functions below keep the file's own
// precision and speak ImageBuffer:
//
//   JPEG, and PNG of 8 bits per channel or fewer  ->  RGBA8_UNORM
//   PNG of 16 bits per channel                    ->  RGBA16_UNORM
//
// Both are sRGB-encoded, display-referred, straight (non-premultiplied)
// alpha, tightly packed, exactly as the PixelFormat comments describe.
// Values arrive as stored in the file: gray becomes RGB, a missing alpha
// becomes opaque, a PNG tRNS chunk becomes alpha, palettes and 1/2/4-bit
// samples are expanded. No gamma / ICC / EXIF-orientation handling is
// applied. The RGB of a pixel whose alpha is 0 is not specified: libpng keeps
// the colour the file stored there, WIC returns black for a gray PNG (8 bits
// or fewer) with a tRNS colour key. To work in the float formats, convert
// afterwards with convertImage (a 16-bit PNG then survives losslessly as
// RGBA32_FLOAT).
//
// The format is recognised from the file's content, never its extension.
// Every function returns nullopt / false and fills *errorOut on failure.

enum class ImageFileFormat : uint8_t { Unknown, Png, Jpeg };

// What a file says about itself, read from its header only (no pixel data
// is decoded, so this is cheap even for a huge picture).
struct ImageInfo {
  ImageFileFormat format = ImageFileFormat::Unknown;
  uint32_t width = 0, height = 0;
  int bitDepth =
      8; // bits per channel in the file: PNG 1/2/4/8/16, JPEG 8 (rarely 12)
  bool hasAlpha = false;  // an alpha channel, or a PNG tRNS chunk
  bool truncated = false; // PNG only: the file ends before its IEND chunk
                          // (decoding such a file is refused on every platform)

  // The format decodeFileBuffer / decodeMemoryBuffer return for this file.
  PixelFormat bufferFormat() const {
    return bitDepth == 16 ? PixelFormat::RGBA16_UNORM
                          : PixelFormat::RGBA8_UNORM;
  }
};

// Reads the header of a PNG or JPEG: size, bit depth, alpha, and (PNG) whether
// the file is complete. nullopt if it is neither format or the header is
// damaged.
std::optional<ImageInfo> probeFile(const std::string &path,
                                   std::string *errorOut = nullptr);
std::optional<ImageInfo> probeMemory(const uint8_t *data, size_t size,
                                     std::string *errorOut = nullptr);

// Decodes a PNG or JPEG into an ImageBuffer in the file's own depth (see
// above): bufferFormat() of probeFile(path).
//
// A PNG is checked before it is decoded, identically on every platform: a file
// that is cut off before its IEND chunk ("PNG is truncated"), has a chunk whose
// CRC does not match ("PNG is corrupt (chunk CRC mismatch)", for any chunk,
// ancillary ones included) or has no IDAT chunk ("PNG has no image data") is
// refused. libpng would catch most of this itself, but WIC decodes such files
// "successfully" with blank rows, so the check is done up front for both.
// The CRC pass reads the whole file once (a few hundred MB/s).
//
// Unusual JPEGs depend on the platform codec: libjpeg (Linux) refuses
// CMYK/YCCK and 12-bit files, WIC (Windows) may decode them.
std::optional<ImageBuffer> decodeFileBuffer(const std::string &path,
                                            std::string *errorOut = nullptr);
std::optional<ImageBuffer> decodeMemoryBuffer(const uint8_t *data, size_t size,
                                              std::string *errorOut = nullptr);

struct PngEncodeOptions {
  // zlib level 0..9 (libpng). Default 3: on a 24-megapixel 16-bit photo,
  // level 6 took ~6x longer than level 3 (31 s vs 5 s) for a file only ~1.4%
  // smaller. Raise it for archival output. The Windows (WIC) encoder has no
  // such knob and ignores it.
  int compression = 3;
  // An image whose alpha is everywhere fully opaque is written as RGB
  // (smaller file, and what a JPEG converted to PNG should look like). Turn
  // off to always write RGBA.
  bool dropOpaqueAlpha = true;
};

// Encodes `image` as a PNG. RGBA8_UNORM gives an 8-bit PNG, RGBA16_UNORM a
// 16-bit one; the float formats are refused (convert them to a UNORM format
// first, with convertImage). The Memory form replaces `out`; the file form
// writes to "<path>.tmp" and renames it over `path`, so a failed write never
// leaves a half-written file behind.
bool encodePngMemory(std::vector<uint8_t> &out, const ImageBuffer &image,
                     std::string *errorOut = nullptr,
                     const PngEncodeOptions &options = {});
bool encodePng(const std::string &path, const ImageBuffer &image,
               std::string *errorOut = nullptr,
               const PngEncodeOptions &options = {});

// Largest width or height a JPEG can hold (the format stores each side in 16
// bits; 65535 is the hard limit, this keeps clear of the encoders' edge cases).
// PNG has no such limit.
inline constexpr uint32_t kJpegMaxSide = 65500;
inline bool jpegSizeOk(uint32_t width, uint32_t height) {
  return width >= 1 && height >= 1 && width <= kJpegMaxSide &&
         height <= kJpegMaxSide;
}

// JPEG has no alpha channel. What to do with the image's alpha:
enum class JpegAlpha : uint8_t {
  // Composite over white: c' = (c*a + 255*(255-a) + 127) / 255. Fully opaque
  // pixels are copied exactly. The default, because it gives the same bytes on
  // every platform (the RGB under a transparent pixel is not specified, see
  // above) and is what a flattened picture should look like.
  FlattenOverWhite,
  // Write the stored RGB as it is and ignore alpha.
  Discard
};

// Chroma subsampling of the encoded JPEG.
enum class JpegChroma : uint8_t {
  Auto, // 4:4:4 (no subsampling) at quality >= 90, to keep coloured edges
        // crisp; else 4:2:0
  S420, // 4:2:0: the smallest files
  S444  // 4:4:4
};

struct JpegEncodeOptions {
  int quality = 90; // 1..100; values outside are clamped
  JpegAlpha alpha = JpegAlpha::FlattenOverWhite;
  JpegChroma chroma = JpegChroma::Auto;
};

// Encodes `image` as a baseline JPEG. JPEG is 8-bit only: RGBA8_UNORM is
// accepted, anything else is refused (convert first), as is a side over
// kJpegMaxSide ("JPEG can't hold a picture this size"). Same Memory / file
// split as encodePng; the Memory form is what you want when splicing EXIF /
// ICC into the bytes. The int overloads use the default options at that
// quality. Windows (WIC) asks for the chroma setting as a property of its
// encoder, which it may not honour for every combination.
bool encodeJpegMemory(std::vector<uint8_t> &out, const ImageBuffer &image,
                      const JpegEncodeOptions &options,
                      std::string *errorOut = nullptr);
bool encodeJpegMemory(std::vector<uint8_t> &out, const ImageBuffer &image,
                      int quality = 90, std::string *errorOut = nullptr);
bool encodeJpeg(const std::string &path, const ImageBuffer &image,
                const JpegEncodeOptions &options,
                std::string *errorOut = nullptr);
bool encodeJpeg(const std::string &path, const ImageBuffer &image,
                int quality = 90, std::string *errorOut = nullptr);
} // namespace liteui_image

struct CanvasGradientStop {
  float offset; // 0..1
  Color color;
};

// A linear or radial gradient paint, built via the static factories and
// addColorStop(). Passed to CanvasContext::setFillGradient/
// setStrokeGradient (copied by value, so it's safe to build one on the
// stack and hand it straight to the setter).
struct CanvasGradient {
  enum class Kind { Linear, Radial } kind = Kind::Linear;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Linear: the two endpoints.
                                        // Radial: (x0,y0) inner circle
                                        // center, (x1,y1) outer circle
                                        // center.
  float r0 = 0, r1 = 0; // Radial only: inner/outer circle radii.
  std::vector<CanvasGradientStop> stops;

  static CanvasGradient linear(float x0, float y0, float x1, float y1) {
    CanvasGradient g;
    g.kind = Kind::Linear;
    g.x0 = x0;
    g.y0 = y0;
    g.x1 = x1;
    g.y1 = y1;
    return g;
  }
  static CanvasGradient radial(float x0, float y0, float r0, float x1, float y1,
                               float r1) {
    CanvasGradient g;
    g.kind = Kind::Radial;
    g.x0 = x0;
    g.y0 = y0;
    g.r0 = r0;
    g.x1 = x1;
    g.y1 = y1;
    g.r1 = r1;
    return g;
  }
  void addColorStop(float offset, Color color) {
    stops.push_back({offset, color});
  }
};

enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };
enum class TextBaseline { Top, Middle, Alphabetic, Bottom };
enum class FillRule { NonZero, EvenOdd };

// Subset of HTML canvas's globalCompositeOperation. Fully honored on
// Linux (maps 1:1 onto cairo_operator_t); on Windows only SourceOver and
// Copy are distinguished (see the file-level Canvas limitations note).
enum class CompositeOp {
  SourceOver,
  SourceIn,
  SourceOut,
  SourceAtop,
  DestinationOver,
  DestinationIn,
  DestinationOut,
  DestinationAtop,
  Lighter,
  Copy,
  Xor,
  Multiply,
  Screen
};

// Drawing surface handed to a Canvas's onPaint. Modeled after the HTML5 2D
// context. Only declared here; the implementation lives in the
// LITEUI_IMPLEMENTATION section (one translation unit per program).
class CanvasContext {
public:
  struct TextMetricsResult {
    float width;
  };
  float width() const;
  float height() const;
  void save();
  void restore();
  void translate(float x, float y);
  void rotate(float radians);
  void scale(float sx, float sy);
  void transformBy(float a, float b, float c, float d, float e, float f);
  void setTransform(float a, float b, float c, float d, float e, float f);
  void resetTransform();
  void setFillColor(Color c);
  void setFillGradient(const CanvasGradient &g);
  void setStrokeColor(Color c);
  void setStrokeGradient(const CanvasGradient &g);
  void setLineWidth(float w);
  void setLineCap(LineCap c);
  void setLineJoin(LineJoin j);
  void setMiterLimit(float m);
  void setLineDash(const std::vector<float> &dashes);
  void setLineDashOffset(float offset);
  void setGlobalAlpha(float a);
  void setGlobalCompositeOperation(CompositeOp op);
  void setShadow(Color color, float blurPx, float offsetX, float offsetY);
  void setFont(const std::string &family, float sizePx,
               FontWeight weight = FontWeight::Regular,
               FontStyle style = FontStyle::Normal);
  void setTextAlign(TextAlign a);
  void setTextBaseline(TextBaseline b);
  void beginPath();
  void closePath();
  void moveTo(float x, float y);
  void lineTo(float x, float y);
  void quadraticCurveTo(float cpx, float cpy, float x, float y);
  void bezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x,
                     float y);
  void arc(float cx, float cy, float r, float startAngle, float endAngle,
           bool ccw = false);
  void arcTo(float x1, float y1, float x2, float y2, float radius);
  void ellipse(float cx, float cy, float rx, float ry, float rotation,
               float startAngle, float endAngle, bool ccw = false);
  void rect(float x, float y, float w, float h);
  void roundRect(float x, float y, float w, float h, float radius);
  void fill(FillRule rule = FillRule::NonZero);
  void stroke();
  void clip(FillRule rule = FillRule::NonZero);
  void fillRect(float x, float y, float w, float h);
  void strokeRect(float x, float y, float w, float h);
  void clearRect(float x, float y, float w, float h);
  TextMetricsResult measureText(const std::string &text);
  void fillText(const std::string &text, float x, float y, float maxWidth = -1);
  void strokeText(const std::string &text, float x, float y,
                  float maxWidth = -1);
  void drawImage(const CanvasImage &img, float dx, float dy);
  void drawImage(const CanvasImage &img, float dx, float dy, float dw,
                 float dh);
  void drawImage(const CanvasImage &img, float sx, float sy, float sw, float sh,
                 float dx, float dy, float dw, float dh);
  std::optional<CanvasImage> getImageData(float x, float y, float w, float h);
  void putImageData(const CanvasImage &img, float dx, float dy);

protected:
  // Only the library creates these (as the base of its platform backend).
  CanvasContext() = default;
  ~CanvasContext() = default;
  CanvasContext(const CanvasContext &) = delete;
  CanvasContext &operator=(const CanvasContext &) = delete;
};

// Author-facing leaf node, addChild(Canvas)'d the same way Text is —
// flattened into a View with isCanvas=true (see View::toView(Canvas)).
// `onPaint` is invoked (with a CanvasContext bound to this node's own
// backing surface) whenever the canvas needs to be redrawn: once after
// the first layout, and again any time `canvasDirtySource` reports true
// (see its own comment on View — that's the mechanism an interactive
// canvas, like a paint program responding to pen strokes via `onPointerMove`,
// uses to request the next redraw, since those handlers have no direct
// reference back into the tree) — plain drawing calls made once are
// *not* re-issued on every frame the way a real-time game loop's draw
// callback would be; treat onPaint as "rebuild my picture from whatever
// app state it closes over" rather than "run every frame".
struct Canvas {
  Style style;
  std::function<void(CanvasContext &)> onPaint;
  // Pointer events — same semantics as on View (see the "Pointer events"
  // comment near PointerEvent). A paint program wants onPointerDown/Move/Up.
  PointerCallback onClick;
  PointerHandler onDoubleClick;
  PointerHandler onAuxClick;
  PointerHandler onContextMenu;
  PointerHandler onPointerDown;
  PointerHandler onPointerMove;
  PointerHandler onPointerUp;
  PointerHandler onPointerCancel;
  PointerHandler onPointerEnter;
  PointerHandler onPointerLeave;
  WheelHandler onWheel;
  std::function<bool()> canvasDirtySource;
  bool focusable = false;
  std::function<void()> onFocus;
  std::function<void()> onBlur;
  std::function<void(KeyEvent)> onKeyDown;
  std::function<void(uint32_t)> onTextInput;
};

// Mutable state for an Image with a dynamic (callback) source, held by
// shared_ptr so every copy of the View an Image flattens into (addChild
// copies Views — same reasoning as TextInputState above) still shares
// one live "currently resolved pixels" cache. Untouched for a plain
// static source (a fixed shared_ptr, or path/memoryData decoded once at
// build time) — those just populate `pixels` a single time and `dirty`
// never flips again after the first paint.
struct ImageState {
  std::shared_ptr<CanvasImage> pixels;
  bool dirty = true; // starts true so the very first paint always runs
  // High-precision source (Image::hdrSource): the buffer last seen, and the
  // RGBA8 display copy in `pixels` derived from it. Replaced display copies
  // stay in `retired` until the next paint has drawn the new one, so a new
  // copy can never reuse a freed copy's address and fool the backends'
  // bitmap cache (keyed on pixel-buffer identity) into a stale hit.
  std::shared_ptr<ImageBuffer> hdr;
  DisplaySettings settings; // settings `pixels` was built with
  std::vector<std::shared_ptr<CanvasImage>> retired;
};

// Author-facing leaf node, addChild(Image)'d the same way Canvas/Text
// are — flattens into an isCanvas View whose onPaint blits a decoded
// CanvasImage (see View::toView(Image)). Set exactly one of `path` /
// `source`: `path` is decoded synchronously via liteui_image::decodeFile at
// addChild time; `source` is used as-is (for procedurally generated pixels, or
// pixels the app decoded/cached itself). `hdrSource` takes an ImageBuffer
// (e.g. RGBA32_FLOAT) instead and takes precedence over the others; see
// the field. If nothing yields a usable image, the node simply paints
// nothing (onError still fires).
struct Image {
  Style style;
  std::string path;
  std::vector<uint8_t> memoryData;
  // Works exactly like Text::label: assign a plain
  // std::shared_ptr<CanvasImage> for a fixed image (as before), or a
  // std::function<std::shared_ptr<CanvasImage>()> to have it re-polled
  // every dispatch cycle (click/key/timer) the same way a Text label
  // re-polls its own callback. Return a *different* shared_ptr when the
  // picture actually changes (comparison is by pointer, not pixel
  // content — cheap, and matches how the app naturally already tracks
  // "which image is current" in its own state) and this node repaints
  // itself with the new pixels; no addChild/setRoot needed.
  Dynamic<std::shared_ptr<CanvasImage>> source;
  ObjectFit fit = ObjectFit::Fill;
  // Painted behind the image before it's drawn — visible in the
  // letterboxed margins Contain/ScaleDown/None can leave. Alpha 0
  // (default) means "leave transparent".
  Color backgroundColor = Color{0, 0, 0, 0};
  // Fired once, synchronously, from addChild(Image) — decoding happens
  // eagerly, not on a background thread — with the image's pixel
  // dimensions if decoding succeeded.
  std::function<void(int width, int height)> onLoad;
  std::function<void(const std::string &error)> onError;
  // High-precision source, same fixed-or-callback rules (and pointer-compare
  // change detection) as `source`. The buffer is converted once per new
  // pointer to an RGBA8 display copy (liteui_image::toCanvasImage): RGBA32F
  // is treated as linear light, clamped and sRGB-encoded, with no tone
  // mapping; an RGBA8 buffer is shown unchanged. Mutating a buffer in place
  // is not detected: hand over a new shared_ptr. Declared last so existing
  // positional initialisation of Image keeps working.
  Dynamic<std::shared_ptr<ImageBuffer>> hdrSource;
  // Output transform for hdrSource (exposure, clamp mode). Fixed, or a
  // callback polled every dispatch cycle like `source`: when the returned
  // settings differ from the last ones the display copy is rebuilt and the
  // node repaints, so an exposure slider needs no new buffer. Ignored for
  // RGBA8 buffers and for the other sources.
  Dynamic<DisplaySettings> displaySettings;
};

// Mutable state for a TextInput, held by shared_ptr so every copy of the
// View a TextInput flattens into (addChild copies Views) still shares one
// live cursor/focus/blink state.
struct TextInputState {
  std::string text;
  size_t cursor = 0; // byte offset; ASCII-only, same limitation as before
  bool focused = false;
  bool blinkOn = true;
  bool dirty = true;
  int blinkTimerHandle = -1;
  float scrollOffset = 0.0f;
  bool requestFocus = false;
};

// Author-facing single-line text field. addChild(TextInput) flattens this
// into a Canvas-backed View (see View::toView(TextInput)) with a blinking
// caret, click-to-position, arrow-key navigation, and text editing already
// wired up. For anything this doesn't cover, build a Canvas node by hand
// using the same liteui_text::caretIndexForX helper this uses internally.
struct TextInput {
  Style style;
  std::string text;
  std::string placeholder;
  float fontSize = 16.0f;
  FontWeight fontWeight = FontWeight::Regular;
  std::string fontFamily;
  Color textColor = Color{0, 0, 0};
  Color placeholderColor = Color{160, 160, 160};
  Color caretColor = Color{20, 20, 20};
  Color borderColor = Color{180, 180, 180};
  Color focusedBorderColor = Color{50, 120, 220};
  float leftPadding = 6.0f;
  std::function<void(const std::string &)> onChange;
  std::function<void(const std::string &)> onSubmit; // fires on Enter

  std::shared_ptr<TextInputState> state = std::make_shared<TextInputState>();
};

// Author-facing leaf node, addChild(Svg)'d the same way Image is — parses
// and flattens into an isCanvas View whose onPaint replays the parsed
// vector shapes through CanvasContext (see View::toView(Svg)). Unlike
// Image, this stays fully vector: it re-renders at whatever pixel size the
// node's box resolves to, including across a window resize, so it never
// blurs the way a pre-rasterized bitmap would.
//
// Known limitations (fine for v1, revisit if needed): no gradients,
// patterns, clipPath/mask, text, or CSS stylesheets (<style> blocks /
// class selectors) — only presentation attributes and inline style="" on
// individual elements. <use> resolves both href and xlink:href, supports
// x/y offset and its own transform/style context, and is guarded against
// reference cycles and excessive nesting depth — but a referenced
// <symbol>'s own width/height/viewBox/preserveAspectRatio is not honored
// (it's rendered at its natural coordinates, un-rescaled, same as a plain
// <g>). Group opacity is approximated by folding into each descendant
// shape's own fill/stroke alpha rather than true isolated-layer
// compositing, so it won't look exactly right where a group's shapes
// overlap each other.
struct Svg {
  Style style;
  std::string path;   // exactly one of path / source should be set
  std::string source; // inline SVG XML text, used if path is empty
  ObjectFit fit = ObjectFit::Contain; // SVG's default "meet" == Contain
  Color backgroundColor = Color{0, 0, 0, 0};
  std::function<void()> onLoad;
  std::function<void(const std::string &error)> onError;
};

// A node in the retained layout tree. Set `style` and `children`; the engine
// fills in `computed` (absolute window pixel coordinates) during layout.
// Renderers only ever read `computed`, never re-derive it from `style`.
class View {
public:
  Style style;
  std::vector<View> children;

  // Identity within a keyed sibling list. Set automatically by
  // reconcileChildren from the key that built this node; empty means
  // "not keyed" — such a child is never matched across a rebuild.
  std::string key;

  // Keyed children. Instead of filling `children` up front, a view can
  // declare which keys it should hold and how to build one child per key.
  // checkForUpdates() polls keysSource after every dispatched event; when
  // the list differs from what's currently built, reconcileChildren()
  // matches old children to new keys by `key` and moves survivors over
  // untouched — cached text texture/layout, canvas surface, scroll offset
  // and hover state included — so adding or removing one row costs one
  // build, not a re-rasterization of the whole list.
  std::function<std::vector<std::string>()> keysSource;
  std::function<View(const std::string &key)> itemBuilder;

  // True for a View created via addChild(Text) — a text leaf. isText
  // implies children.empty() always; text/textStyle are meaningless
  // otherwise.
  bool isText = false;
  Dynamic<std::string> text;
  TextStyle textStyle;

  // True for a View created via addChild(Canvas) — a canvas leaf. isCanvas
  // implies children.empty() always (a canvas has no flow children of its
  // own); onPaint is meaningless otherwise. See the Canvas struct's own
  // comment for what "invoked" means here.
  bool isCanvas = false;
  std::function<void(CanvasContext &)> onPaint;

  // ---- Pointer events (DOM pointer-events model) -------------------------
  // Full dispatch/capture/bubbling rules: see the "Pointer events" comment
  // above PointerEvent. In short: events go to the deepest view with a
  // handler, bubble up through ancestors with handlers until one calls
  // e.stopPropagation(), and a press captures the pointer so drags keep
  // working outside the view. Buttons are a field, not separate handlers:
  // `if (e.button == MouseButton::Middle) ...`.
  PointerHandler onPointerDown;
  PointerHandler onPointerMove; // hover (buttons == 0) and drag moves
  PointerHandler onPointerUp;
  // The press can no longer complete: the OS took the mouse (alt-tab, a
  // system dialog, a window-move grab) or a scroll pan claimed it.
  PointerHandler onPointerCancel;
  // Do not bubble. Follow the same hover flag as Style::hoverColor.
  PointerHandler onPointerEnter;
  PointerHandler onPointerLeave;
  PointerHandler onDoubleClick; // after a 2nd left click (clickCount == 2)
  PointerHandler onAuxClick;    // middle-button click
  PointerHandler onContextMenu; // right-button click, fired on release
  WheelHandler onWheel;         // falls through to overflow scrolling unless
                                // a handler calls e.stopPropagation()

  // Left-button click (press and release on the same view). Accepts
  // `[&](PointerEvent &e) {...}` (bubbles unless stopped) or the shorter
  // `[&] {...}` (runs, then ends bubbling) — see PointerCallback.
  PointerCallback onClick;

  // Fired once per relayout(), right after placeNode() finalizes this
  // node's absolute on-screen box. Lets the app read a view's real
  // x/y/w/h to drive some other view's position — e.g. anchoring a
  // dropdown/menu to the button that opened it.
  std::function<void(float x, float y, float w, float h)> onLayout;

  // Set true to make this view eligible to receive keyboard focus via
  // click-to-focus (see LiteUI::setFocus). Views default to non-focusable
  // — a plain container/box should never steal focus from whatever the
  // user was actually typing into.
  bool focusable = false;
  // Tooltip text shown after hovering this view for LiteUI's configured
  // delay. Empty (default) = no tooltip. Plain field, not Dynamic<> —
  // looked up once when the tooltip shows, not re-evaluated every frame.
  std::string tooltip;

  // Fired when this view gains/loses keyboard focus. Typical use: a text
  // input shows/hides its caret here.
  std::function<void()> onFocus;
  std::function<void()> onBlur;

  std::function<bool()> wantsFocus;

  // Fired only while this view is the focused view (see LiteUI::focusedView_).
  // No bubbling in v1 — exactly one view receives these, the one that has
  // focus. onKeyDown fires on repeat too (both platform-native repeat on
  // Windows, and LiteUI's own repeat timer on Linux — see the Linux
  // keyboard listener).
  std::function<void(KeyEvent)> onKeyDown;
  std::function<void(KeyEvent)> onKeyUp;

  // Fired only while focused, with an already-composed UTF-32 codepoint —
  // this is what a text field should append, not onKeyDown. Separate from
  // onKeyDown because "what character was typed" depends on shift/layout/
  // dead keys in a way a raw Key never captures.
  std::function<void(uint32_t codepoint)> onTextInput;

  // isCanvas nodes only. Polled the same way as the sources above; return
  // true once to make the next paint/render pass re-invoke onPaint (see
  // Canvas's own comment), then go back to returning false until there's
  // something new to draw. This is the mechanism to use from inside a
  // canvas's own onPointerDown/onPointerMove/onClick handlers — those are plain
  // callbacks with no reference back into the tree, so they can't call
  // View::requestCanvasRedraw() on themselves directly; stashing a dirty
  // flag in app state and reporting+clearing it here is the workaround.
  // A typical pattern: `bool dirty = false;` in the app's own state,
  // set true wherever a stroke/shape is added, and
  // `canvasDirtySource = [&]{ bool d = dirty; dirty = false; return d; };`
  std::function<bool()> canvasDirtySource;

  Dynamic<bool> disabled = false;

  struct Computed {
    float x = 0, y = 0, w = 0, h = 0; // border-box, absolute window coords

    // Set by LiteUI::updateHover from pointer position on every move
    // event; drives style.hoverColor at paint time. Not app-sourced.
    mutable bool isHovered = false;

    // True whenever this node's own paint output may be stale — flipped
    // by checkForUpdates()/updateHover() on any actual change. Nothing reads or
    // clears it yet (both paintView/renderView walk unconditionally today);
    // this is scaffolding for per-node skip logic later.
    mutable bool dirty = true;

    // Only meaningful when style.overflowX/Y != Visible. contentW/contentH
    // is how big this view's children naturally want to be (the scrollable
    // extent); scrollX/scrollY is how far the content is currently
    // scrolled, always clamped to [0, max(0, content - viewport)]. A
    // non-scrollable view leaves these at 0 and they're simply unused.
    float contentW = 0, contentH = 0;
    float scrollX = 0, scrollY = 0;

    // Only meaningful for isCanvas nodes. Starts true so the very first
    // paint/render pass always invokes onPaint at least once; cleared by
    // whichever backend (re)builds the cached backing surface below, and
    // set again only by View::requestCanvasRedraw() or a resize (see
    // ensureCanvasSurface/ensureCanvasTarget) — plain repaints (e.g. from
    // an unrelated sibling's hover change) do NOT re-invoke onPaint.
    mutable bool canvasNeedsRedraw = true;

    // Last polled result when text/disabled/value hold a callback; unused
    // (and untouched) when they hold a plain value — see resolveDynamic.
    mutable std::string resolvedText;
    mutable bool resolvedDisabled = false;
    mutable float resolvedValue = 0.0f;
    mutable Color resolvedBackgroundColor{0, 0, 0, 0};
    mutable Size resolvedWidth = Size::fit();
    mutable Size resolvedHeight = Size::fit();
    mutable float resolvedBorderWidth = 0.0f;
    mutable Color resolvedBorderColor{0, 0, 0};
    mutable float resolvedBorderRadius = 0.0f;
    mutable float resolvedLeft = std::numeric_limits<float>::quiet_NaN();
    mutable float resolvedTop = std::numeric_limits<float>::quiet_NaN();
    mutable float resolvedRight = std::numeric_limits<float>::quiet_NaN();
    mutable float resolvedBottom = std::numeric_limits<float>::quiet_NaN();
    mutable Display resolvedDisplay = Display::Flex;
    mutable Visibility resolvedVisibility = Visibility::Visible;
    mutable Cursor resolvedCursor = Cursor::Inherit;
    mutable int resolvedZIndex = 0;
    mutable Color resolvedTextColor{0, 0, 0};
    mutable float resolvedFontSize = 16.0f;
    mutable FontWeight resolvedFontWeight = FontWeight::Regular;
    mutable FontStyle resolvedFontStyle = FontStyle::Normal;
    mutable EdgeInsets resolvedMargin;
    mutable EdgeInsets resolvedPadding;
    mutable std::optional<Color> resolvedScrollTrackColor;
    mutable std::optional<Color> resolvedScrollThumbColor;
    // Key list the current `children` were built for; compared against
    // keysSource each poll to decide whether to reconcile at all.
    mutable std::vector<std::string> resolvedKeys;

    // Cached platform text backing for isText nodes, rebuilt by the
    // renderer whenever the width it was built for goes stale (a resize
    // or relayout can change how the text wraps). `mutable` so paint
    // code — which only ever sees a `const View&` — can lazily build/
    // rebuild this. Nothing here is ever auto-freed by a destructor (see
    // View::freeTextResources) since Views get copied while a tree is
    // being built (e.g. `root.addChild(heading)` copies `heading`), and
    // an auto-freeing destructor would risk a double-Release/double-
    // glDelete across those copies.
#if defined(_WIN32)
    mutable IDWriteTextLayout *textLayout = nullptr;
    mutable float textLayoutBuiltForWidth = -1.0f;
    mutable std::string textLayoutBuiltForText;
    // Font values the layout above was built for — a Dynamic<> fontSize/
    // fontWeight/fontStyle can change without the text or the box width
    // changing, and that alone must still invalidate the cached layout.
    mutable float textLayoutBuiltForFontSize = -1.0f;
    mutable FontWeight textLayoutBuiltForFontWeight = FontWeight::Regular;
    mutable FontStyle textLayoutBuiltForFontStyle = FontStyle::Normal;
    // The GPU texture this canvas node renders into: a render-target
    // bitmap (ID2D1Bitmap1, B8G8R8A8 premultiplied) on the window's
    // Direct3D 11 device, the size of the node's inner (padding-excluded)
    // content box. onPaint draws into it through a Direct2D device
    // context, and the renderer then draws it into the window every paint
    // pass (see LiteUI::ensureCanvasTarget/paintCanvas) — it never leaves
    // video memory unless the app calls getImageData. Rebuilt whenever the
    // content box resizes, and dropped (then lazily rebuilt) when the
    // device is lost.
    mutable ID2D1Bitmap1 *canvasBitmap = nullptr;
    // CPU-readable twin of canvasBitmap, created on the first getImageData
    // and reused for later calls (a GPU texture can't be mapped directly).
    mutable ID2D1Bitmap1 *canvasStaging = nullptr;
    mutable float canvasBuiltForWidth = -1.0f, canvasBuiltForHeight = -1.0f;
    // Caches the ID2D1Bitmap most recently uploaded by this canvas node's
    // own CanvasContext::drawImage() (see that method's comment), keyed on
    // the source CanvasImage's pixel buffer identity+size+dimensions. A
    // repeat draw of the SAME image content — e.g. re-painting at a new
    // zoom level, or a window resize that leaves the image itself
    // untouched — reuses this instead of reconverting+reuploading every
    // pixel again. It is a plain device resource (any context on the same
    // Direct2D device can draw it), so it survives a canvas resize; it is
    // released on teardown and on device loss (freeTextResources).
    mutable ID2D1Bitmap *cachedImageBitmap = nullptr;
    mutable const void *cachedImagePixels = nullptr;
    mutable size_t cachedImagePixelsSize = 0;
    mutable int cachedImageW = -1, cachedImageH = -1;
#else
    mutable unsigned int textTexture = 0;
    mutable int textTexW = 0, textTexH = 0;
    mutable float textTextureBuiltForWidth = -1.0f;
    mutable std::string textTextureBuiltForText;
    // See textLayoutBuiltForFontSize/Weight/Style above — same reasoning,
    // Linux/Cairo side.
    mutable float textTextureBuiltForFontSize = -1.0f;
    mutable FontWeight textTextureBuiltForFontWeight = FontWeight::Regular;
    mutable FontStyle textTextureBuiltForFontStyle = FontStyle::Normal;
    // A CPU-side Cairo image surface the size of this canvas node's inner
    // content box, drawn into by onPaint, then converted+uploaded as an
    // RGBA GL texture for actual display (see
    // LiteUI::ensureCanvasSurface/drawCanvasTexture). Keeping the Cairo
    // surface around (rather than freeing it right after upload) is what
    // makes getImageData possible on this platform: it's just a read of
    // the very same pixels already sitting in `canvasSurface`.
    mutable cairo_surface_t *canvasSurface = nullptr;
    mutable unsigned int canvasTexture = 0;
    // GPU canvas backend (LITEUI_CANVAS_GL): canvasTexture above is then the
    // node's RGBA8 target; canvasFbo wraps it. With multisampling available,
    // drawing goes to canvasMsaaFbo (colour+depth/stencil renderbuffers) and
    // is resolved into canvasTexture; otherwise canvasDsRb is attached to
    // canvasFbo itself. All zero (and unused) with the Cairo backend.
    mutable unsigned int canvasFbo = 0;
    mutable unsigned int canvasMsaaFbo = 0;
    mutable unsigned int canvasMsaaColorRb = 0;
    mutable unsigned int canvasDsRb = 0;
    mutable float canvasBuiltForWidth = -1.0f, canvasBuiltForHeight = -1.0f;
    // Caches the cairo_surface_t most recently uploaded by this canvas
    // node's own CanvasContext::drawImage() (see that method's comment),
    // keyed on the source CanvasImage's pixel buffer identity+size+
    // dimensions. A repeat draw of the SAME image content — e.g.
    // re-painting at a new zoom level, or a window resize that leaves the
    // image itself untouched — reuses this instead of reconverting+
    // reuploading every pixel again. Unlike the Windows bitmap cache
    // above, a standalone Cairo image surface isn't tied to any
    // particular cairo_t, so it deliberately SURVIVES canvasSurface being
    // rebuilt on a resize (the same image redrawn into a differently
    // sized canvas is exactly the case this cache is for). It's only
    // released in freeTextResources, or replaced when drawImage sees a
    // different pixel buffer.
    mutable cairo_surface_t *cachedImageSurface = nullptr;
    // GPU canvas backend (LITEUI_CANVAS_GL): the same one-slot cache, holding
    // the premultiplied RGBA8 texture of the image drawn last (the fields
    // below key it, exactly as for the Cairo surface). Zero otherwise.
    mutable unsigned int cachedImageTexture = 0;
    mutable const void *cachedImagePixels = nullptr;
    mutable size_t cachedImagePixelsSize = 0;
    mutable int cachedImageW = -1, cachedImageH = -1;
#endif
  } computed;

  void addChild(View child) { children.push_back(std::move(child)); }
  void addChild(Text t) { children.push_back(toView(std::move(t))); }
  void addChild(Canvas c) { children.push_back(toView(std::move(c))); }
  void addChild(TextInput ti) { children.push_back(toView(std::move(ti))); }
  void addChild(Image img) { children.push_back(toView(std::move(img))); }
  void addChild(Svg s) { children.push_back(toView(std::move(s))); }

  // Marks this canvas node's cached backing surface stale, so the next
  // paint/render pass re-invokes onPaint instead of reusing whatever it
  // last drew. A no-op (harmlessly) on a non-canvas node. Since the
  // renderer walks a `const View&`, this is the app's own handle to a
  // View it's still holding a mutable reference to (e.g. right after
  // addChild, or via onLayout capturing a pointer/index) — there's no way
  // to reach into an already-submitted tree from outside.
  void requestCanvasRedraw() { computed.canvasNeedsRedraw = true; }

  // Whether this view clips/scrolls on the given axis.
  bool scrollsX() const { return style.overflowX != Overflow::Visible; }
  bool scrollsY() const { return style.overflowY != Overflow::Visible; }

  // Maximum scrollX/scrollY this view can currently have, given its last
  // computed content size vs its viewport size. 0 when content fits (or
  // the axis doesn't scroll).
  float maxScrollX() const {
    return std::max(0.0f, computed.contentW - computed.w);
  }
  float maxScrollY() const {
    return std::max(0.0f, computed.contentH - computed.h);
  }

  // Frees this node's cached text- and canvas-rendering resources (if
  // any) and recurses into children. Must be called explicitly before a
  // tree is discarded/replaced — see setRoot() and ~LiteUI(), the only
  // two places a live tree is actually retired. (The name predates
  // Canvas support; kept as-is rather than renamed to minimize disruption
  // to any code already calling it.)
  void freeTextResources();

private:
  static View toView(Text t);
  static View toView(Canvas c);
  static View toView(TextInput ti);
  static View toView(Image img);
  static View toView(Svg s);
};

struct TooltipStyle {
  int delayMs = 500;
  Color background{50, 50, 50, 230};
  Color textColor{255, 255, 255, 255};
  float padding = 6.0f;
  float fontSize = 13.0f;
  std::string fontFamily; // empty = platform default
};

// The application window. Declared here as a thin handle; the platform
// implementation (Win32 / Wayland) lives in the LITEUI_IMPLEMENTATION section.
class LiteUIImpl;
class LiteUI {
public:
  explicit LiteUI(const std::string &title = "Window", int width = -1,
                  int height = -1, bool hideTitlebar = false);
  ~LiteUI();
  LiteUI(const LiteUI &) = delete;
  LiteUI &operator=(const LiteUI &) = delete;

  // Blocks the calling thread and pumps the platform's event loop.
  void run();
  // Sets (or replaces) the root of the layout tree, lays it out immediately,
  // and triggers a repaint.
  void setRoot(View view);

  void requestClose();
  void requestMinimize();
  // Toggles: maximizes if the window is normal, restores it if maximized.
  void requestMaximize();
  void requestMove();
  // Handler invoked when the user asks to close the window; it must call
  // forceClose() itself. With no handler, close requests close immediately.
  void setOnCloseRequest(std::function<void()> fn);
  // Closes the window immediately, bypassing onCloseRequest.
  void forceClose();
  KeyModifiers modifiers() const;

  // Runs fn every `ms` milliseconds until removeInterval() is called with the
  // returned handle. Each tick polls Dynamic<> sources and repaints.
  int addInterval(int ms, std::function<void()> fn);
  void removeInterval(int handle);
  void setTooltipStyle(TooltipStyle s);
  void setWindowBackground(Color c);
  // Window-wide default scrollbar colors (a view's own Style colors override).
  void setScrollbarColors(Color track, Color thumb);
  // App-global shortcut, checked before the focused view's onKeyDown.
  void addShortcut(KeyModifiers mods, Key key, std::function<void()> fn);
  void requestRepaint();
  static LiteUI *current();

  // Wakes the event loop from ANY thread (the one call that is safe off the
  // UI thread). Wakes coalesce; the caller must join all threads that can
  // still call wake() before this LiteUI is destroyed.
  void wake();
  // Handler run on the UI thread once per coalesced wake, before the repaint.
  void setWakeHandler(std::function<void()> fn);

private:
  LiteUIImpl *p_;
};

#endif // LITEUI_HPP_INCLUDED

// ======================================================================
// Implementation. Compiled only in the one translation unit that defines
// LITEUI_IMPLEMENTATION before including this header.
// ======================================================================
#if defined(LITEUI_IMPLEMENTATION) && !defined(LITEUI_IMPLEMENTATION_DONE)
#define LITEUI_IMPLEMENTATION_DONE

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <dxgiformat.h> // DXGI_FORMAT_R8G8B8A8_UNORM, used by Canvas::drawImage/putImageData
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM/GET_WHEEL_DELTA_WPARAM, used by the mouse-wheel handlers
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "d2d1")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "ole32")
#else
#include "xdg-decoration-client-protocol.h" // Generated client bindings for the xdg-decoration protocol (server-side vs client-side decorations).
#include "xdg-shell-client-protocol.h" // Generated client bindings for the xdg-shell protocol (toplevel windows, configure events).
#include "tablet-client-protocol.h" // Generated client bindings for tablet-unstable-v2 (pen pressure/tilt/eraser/barrel buttons).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h> // OpenGL ES 3.0 (superset of the ES 2.0 entry points used so far)
#include <cairo/cairo.h>
#include <cstdio> // popen, pclose, fgets
#include <fcntl.h>
#include <jpeglib.h>
#include <pango/pangocairo.h> // Text shaping/layout + measurement; rendering still goes through GL (see ensureTextTexture)
#include <png.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client.h> // Core Wayland client protocol: displays, registries, surfaces, shm.
#include <wayland-cursor.h> // wl_cursor_theme_load / wl_cursor_theme_get_cursor, for showing resize/arrow cursors.
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
#endif

#if defined(_WIN32)
// Converts a UTF-8 std::string to the UTF-16 wide string Win32/DirectWrite
// APIs require. File-scope (rather than a LiteUI member) because both
// LiteUI's WndProc path and liteui_text's DirectWrite layout builder need
// it, and DirectWrite objects aren't tied to any particular window.
inline std::wstring toWide(const std::string &s) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), wlen);
  return w;
}
inline Key vkToKey(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z')
    return static_cast<Key>(static_cast<int>(Key::A) + (vk - 'A'));
  if (vk >= '0' && vk <= '9')
    return static_cast<Key>(static_cast<int>(Key::N0) + (vk - '0'));
  if (vk >= VK_F1 && vk <= VK_F12)
    return static_cast<Key>(static_cast<int>(Key::F1) + (vk - VK_F1));
  switch (vk) {
  case VK_RETURN:
    return Key::Enter;
  case VK_ESCAPE:
    return Key::Escape;
  case VK_BACK:
    return Key::Backspace;
  case VK_TAB:
    return Key::Tab;
  case VK_SPACE:
    return Key::Space;
  case VK_DELETE:
    return Key::Delete;
  case VK_LEFT:
    return Key::Left;
  case VK_RIGHT:
    return Key::Right;
  case VK_UP:
    return Key::Up;
  case VK_DOWN:
    return Key::Down;
  case VK_HOME:
    return Key::Home;
  case VK_END:
    return Key::End;
  case VK_PRIOR:
    return Key::PageUp;
  case VK_NEXT:
    return Key::PageDown;
  case VK_SHIFT:
    return Key::Shift;
  case VK_CONTROL:
    return Key::Control;
  case VK_MENU:
    return Key::Alt;
  case VK_LWIN:
  case VK_RWIN:
    return Key::Super;
  case VK_OEM_MINUS:
    return Key::Minus;
  case VK_OEM_PLUS:
    return Key::Equal;
  case VK_OEM_4:
    return Key::LeftBracket;
  case VK_OEM_6:
    return Key::RightBracket;
  case VK_OEM_5:
    return Key::Backslash;
  case VK_OEM_1:
    return Key::Semicolon;
  case VK_OEM_7:
    return Key::Quote;
  case VK_OEM_COMMA:
    return Key::Comma;
  case VK_OEM_PERIOD:
    return Key::Period;
  case VK_OEM_2:
    return Key::Slash;
  case VK_OEM_3:
    return Key::Grave;
  default:
    return Key::Unknown;
  }
}

inline KeyModifiers currentModifiers() {
  KeyModifiers m;
  m.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  m.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  m.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
  m.super = (GetKeyState(VK_LWIN) & 0x8000) != 0 ||
            (GetKeyState(VK_RWIN) & 0x8000) != 0;
  return m;
}
#endif

// ---- optional frame profiler ------------------------------------------
// Compile with -DLITEUI_PROFILE to print, once per second on stderr, how
// long each stage of interaction/painting takes (avg and worst case, in
// ms, plus how many times it ran that second). Compiled out entirely
// otherwise — LITEUI_PROF_START/ADD expand to nothing. Stages nest: e.g.
// `build` (all of WM_PAINT's drawing) contains `canvas` and `blit`.
#ifdef LITEUI_PROFILE
#include <chrono>
namespace liteui_prof {
struct Stat {
  double sum = 0, worst = 0;
  int n = 0;
  void add(double ms) {
    sum += ms;
    if (ms > worst)
      worst = ms;
    ++n;
  }
  double avg() const { return n ? sum / n : 0.0; }
};
struct Report {
  Stat move, build, endDraw, canvas, blit, recompute, appPaint;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  void flushIfDue() {
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1))
      return;
    auto line = [](const char *name, const Stat &s) {
      if (s.n)
        std::fprintf(stderr, "  %-9s x%-4d avg %6.2f ms  worst %6.2f ms\n",
                     name, s.n, s.avg(), s.worst);
    };
    if (move.n || build.n) {
      std::fprintf(stderr, "[liteui profile]\n");
      line("mouse", move);
      line("recompute", recompute);
      line("build", build);
      line("canvas", canvas);
      line("appPaint", appPaint);
      line("blit", blit);
      line("EndDraw", endDraw);
    }
    move = build = endDraw = canvas = blit = recompute = appPaint = Stat{};
    last = now;
  }
};
inline Report &report() {
  static Report r;
  return r;
}
inline double msSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t)
      .count();
}
} // namespace liteui_prof
#define LITEUI_PROF_START(var) auto var = std::chrono::steady_clock::now()
#define LITEUI_PROF_ADD(stat, var)                                             \
  do {                                                                         \
    liteui_prof::report().stat.add(liteui_prof::msSince(var));                 \
    liteui_prof::report().flushIfDue();                                        \
  } while (0)
#else
#define LITEUI_PROF_START(var) ((void)0)
#define LITEUI_PROF_ADD(stat, var) ((void)0)
#endif

// ======================= File / folder pickers =======================
//
// All five public pickers (openFilePicker, openFilesPicker, saveFilePicker,
// openFolderPicker, saveFolderPicker) funnel through a single
// platform-specific `liteui_filepicker::pick(Request)`. The platform code
// below only knows how to show "a dialog of kind X" and hand back a list of
// paths; the public wrappers at the bottom are platform-independent.
namespace liteui_filepicker {

enum class Mode {
  OpenFile,
  OpenFiles,
  SaveFile,
  Folder,    // pick an existing folder
  SaveFolder // same, but "Save"/"Select" framing where the platform has it
};

struct Request {
  Mode mode;
  std::string title;
  std::vector<FileFilter> filters;
  std::string defaultName; // SaveFile only
  std::string defaultExt;  // SaveFile only
};

inline std::optional<std::string> first(std::vector<std::string> &&v) {
  if (v.empty())
    return std::nullopt;
  return std::move(v.front());
}

#if defined(_WIN32)

// RAII CoInitializeEx/CoUninitialize. Only balances the call when WE were
// the ones to initialize COM (S_OK); S_FALSE / RPC_E_CHANGED_MODE mean the
// host app already did, and must not be uninitialized by us.
struct ComScope {
  bool owns;
  ComScope()
      : owns(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                         COINIT_DISABLE_OLE1DDE) == S_OK) {}
  ~ComScope() {
    if (owns)
      CoUninitialize();
  }
  ComScope(const ComScope &) = delete;
  ComScope &operator=(const ComScope &) = delete;
};

// COMDLG_FILTERSPEC just points at wide strings we own, so the backing
// wstrings must outlive SetFileTypes/Show — hence a struct held by caller.
struct FilterSpecs {
  std::vector<std::wstring> names, patterns;
  std::vector<COMDLG_FILTERSPEC> specs;

  explicit FilterSpecs(const std::vector<FileFilter> &filters) {
    names.reserve(filters.size());
    patterns.reserve(filters.size());
    for (auto &f : filters) {
      names.push_back(toWide(f.name));
      patterns.push_back(toWide(f.pattern));
    }
    for (size_t i = 0; i < filters.size(); ++i)
      specs.push_back({names[i].c_str(), patterns[i].c_str()});
  }
  void applyTo(IFileDialog *dlg) {
    if (!specs.empty())
      dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
  }
};

// Appends the item's filesystem path (UTF-8) to `out`. Releases `item`.
inline void appendItemPath(IShellItem *item, std::vector<std::string> &out) {
  PWSTR path = nullptr;
  if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
    int len =
        WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
    if (len > 0)
      WideCharToMultiByte(CP_UTF8, 0, path, -1, s.data(), len, nullptr,
                          nullptr);
    out.push_back(std::move(s));
    CoTaskMemFree(path);
  }
  item->Release();
}

template <class Dlg> struct DialogClsid;
template <> struct DialogClsid<IFileOpenDialog> {
  static const CLSID &get() { return CLSID_FileOpenDialog; }
};
template <> struct DialogClsid<IFileSaveDialog> {
  static const CLSID &get() { return CLSID_FileSaveDialog; }
};

// The one place that creates, configures, shows and harvests an
// IFileOpenDialog / IFileSaveDialog. `extraOpts` are OR'd into FOS_*.
template <class Dlg>
std::vector<std::string> showDialog(const Request &r, DWORD extraOpts) {
  ComScope com;

  Dlg *dlg = nullptr;
  if (FAILED(CoCreateInstance(DialogClsid<Dlg>::get(), nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    return {};

  dlg->SetTitle(toWide(r.title).c_str());
  if (extraOpts) {
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | extraOpts);
  }
  if (!r.defaultName.empty())
    dlg->SetFileName(toWide(r.defaultName).c_str());
  // Prompt-on-overwrite is IFileSaveDialog's default, so nothing to request.
  if (!r.defaultExt.empty())
    dlg->SetDefaultExtension(toWide(r.defaultExt).c_str());

  FilterSpecs filters(r.filters);
  filters.applyTo(dlg);

  std::vector<std::string> out;
  if (SUCCEEDED(dlg->Show(nullptr))) // blocks until closed
  {
    bool collected = false;
    if constexpr (std::is_same_v<Dlg, IFileOpenDialog>) {
      if (r.mode == Mode::OpenFiles) {
        collected = true;
        IShellItemArray *items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
          DWORD count = 0;
          items->GetCount(&count);
          for (DWORD i = 0; i < count; ++i) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &item)))
              appendItemPath(item, out);
          }
          items->Release();
        }
      }
    }
    if (!collected) {
      IShellItem *item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item)))
        appendItemPath(item, out);
    }
  }
  dlg->Release();
  return out;
}

inline std::vector<std::string> pick(const Request &r) {
  switch (r.mode) {
  case Mode::OpenFile:
    return showDialog<IFileOpenDialog>(r, 0);
  case Mode::OpenFiles:
    return showDialog<IFileOpenDialog>(r, FOS_ALLOWMULTISELECT);
  case Mode::SaveFile:
    return showDialog<IFileSaveDialog>(r, 0);
  case Mode::Folder:
    return showDialog<IFileOpenDialog>(r, FOS_PICKFOLDERS);
  case Mode::SaveFolder:
    return showDialog<IFileSaveDialog>(r, FOS_PICKFOLDERS);
  }
  return {};
}

#else // Linux — shells out to zenity, falling back to kdialog

inline bool commandExists(const char *name) {
  std::string check = std::string("command -v ") + name + " >/dev/null 2>&1";
  return std::system(check.c_str()) == 0;
}

// Very small shell-arg escaper: single-quote the whole thing, escaping any
// embedded single quotes. Titles/patterns here are app-supplied, but this
// keeps popen() safe regardless.
inline std::string shellQuote(const std::string &s) {
  std::string out = "'";
  for (char c : s)
    out += (c == '\'') ? "'\\''" : std::string(1, c);
  out += "'";
  return out;
}

// Our patterns are ';'-separated; zenity/kdialog want space-separated.
inline std::string spaceSeparated(const std::string &pattern) {
  std::string g = pattern;
  std::replace(g.begin(), g.end(), ';', ' ');
  return g;
}

// " --file-filter='Images | *.png *.jpg'" per filter (leading space each).
inline std::string zenityFilterArgs(const std::vector<FileFilter> &filters) {
  std::string args;
  for (auto &f : filters)
    args += " --file-filter=" +
            shellQuote(f.name + " | " + spaceSeparated(f.pattern));
  return args;
}

// kdialog wants one trailing arg: "*.png *.jpg|Images\n*.mp3|Audio".
// Returns " '<spec>'" (leading space), or "" when there are no filters.
inline std::string kdialogFilterArg(const std::vector<FileFilter> &filters) {
  if (filters.empty())
    return {};
  std::string spec;
  for (size_t i = 0; i < filters.size(); ++i) {
    if (i)
      spec += "\n";
    spec += spaceSeparated(filters[i].pattern) + "|" + filters[i].name;
  }
  return " " + shellQuote(spec);
}

// Runs whichever of the two commands matches an installed picker binary
// (zenity preferred). Returns the raw stdout, or nullopt if neither exists,
// the pipe failed, the user cancelled (non-zero exit), or output is empty.
inline std::optional<std::string> runPicker(const std::string &zenityCmd,
                                            const std::string &kdialogCmd) {
  std::string cmd;
  if (commandExists("zenity"))
    cmd = zenityCmd;
  else if (commandExists("kdialog"))
    cmd = kdialogCmd;
  else
    return std::nullopt; // no picker binary available
  cmd += " 2>/dev/null";

  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe)
    return std::nullopt;
  std::string out;
  char buf[1024];
  while (fgets(buf, sizeof(buf), pipe))
    out += buf;
  int rc = pclose(pipe);
  if (rc != 0 || out.empty())
    return std::nullopt;
  return out;
}

inline void trimTrailingNewlines(std::string &s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
}

inline std::vector<std::string> splitLines(const std::string &out) {
  std::vector<std::string> result;
  size_t start = 0;
  while (start <= out.size()) {
    size_t nl = out.find('\n', start);
    std::string line = out.substr(
        start, nl == std::string::npos ? std::string::npos : nl - start);
    while (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty())
      result.push_back(std::move(line));
    if (nl == std::string::npos)
      break;
    start = nl + 1;
  }
  return result;
}

// Appends defaultExt to path if it doesn't already end in one of the
// extensions listed in `filters`' patterns. Keeps zenity/kdialog (which
// don't reliably force an extension the way Windows' dialog does) from
// producing "export" instead of "export.png".
inline std::string ensureExtension(std::string path,
                                   const std::string &defaultExt,
                                   const std::vector<FileFilter> &filters) {
  if (defaultExt.empty() || path.empty())
    return path;
  auto iequal = [](char a, char b) {
    return std::tolower(a) == std::tolower(b);
  };
  auto endsWithAny = [&](const std::string &p) {
    for (auto &f : filters) {
      size_t start = 0;
      while (start < f.pattern.size()) {
        size_t sep = f.pattern.find(';', start);
        std::string glob = f.pattern.substr(start, sep - start);
        // glob is like "*.png" — check the suffix after '*'
        size_t star = glob.find('*');
        if (star != std::string::npos) {
          std::string suffix = glob.substr(star + 1);
          if (suffix.size() <= p.size() &&
              std::equal(suffix.rbegin(), suffix.rend(), p.rbegin(), iequal))
            return true;
        }
        if (sep == std::string::npos)
          break;
        start = sep + 1;
      }
    }
    return false;
  };
  if (!filters.empty() && endsWithAny(path))
    return path;
  if (path.size() >= defaultExt.size() + 1 &&
      path[path.size() - defaultExt.size() - 1] == '.' &&
      std::equal(defaultExt.rbegin(), defaultExt.rend(), path.rbegin(), iequal))
    return path;
  return path + "." + defaultExt;
}

inline std::vector<std::string> pick(const Request &r) {
  const std::string t = shellQuote(r.title);
  const std::string zf = zenityFilterArgs(r.filters);
  const std::string kf = kdialogFilterArg(r.filters);
  std::string zen, kde;

  switch (r.mode) {
  case Mode::OpenFile:
    zen = "zenity --file-selection --title=" + t + zf;
    kde = "kdialog --getopenfilename . --title " + t + kf;
    break;
  case Mode::OpenFiles:
    zen = "zenity --file-selection --multiple --separator=" + shellQuote("\n") +
          " --title=" + t + zf;
    kde = "kdialog --getopenfilename . --multiple --separate-output "
          "--title " +
          t + kf;
    break;
  case Mode::SaveFile:
    zen = "zenity --file-selection --save --confirm-overwrite --title=" + t;
    if (!r.defaultName.empty())
      zen += " --filename=" + shellQuote(r.defaultName);
    zen += zf;
    // kdialog itself prompts on overwrite by default.
    kde = "kdialog --getsavefilename " +
          shellQuote(r.defaultName.empty() ? "." : r.defaultName) + kf +
          " --title " + t;
    break;
  case Mode::Folder:
  case Mode::SaveFolder:
    // zenity/kdialog have no separate "save" mode for directories; the
    // two modes differ only in the title the caller passes.
    zen = "zenity --file-selection --directory --title=" + t;
    kde = "kdialog --getexistingdirectory . --title " + t;
    break;
  }

  auto raw = runPicker(zen, kde);
  if (!raw)
    return {};

  if (r.mode == Mode::OpenFiles)
    return splitLines(*raw);

  trimTrailingNewlines(*raw);
  if (r.mode == Mode::SaveFile)
    *raw = ensureExtension(std::move(*raw), r.defaultExt, r.filters);
  return {std::move(*raw)};
}

#endif // _WIN32

} // namespace liteui_filepicker

// ---- Public API: platform-independent wrappers over pick() ----

std::optional<std::string>
openFilePicker(const std::string &title,
               const std::vector<FileFilter> &filters) {
  return liteui_filepicker::first(liteui_filepicker::pick(
      {liteui_filepicker::Mode::OpenFile, title, filters, {}, {}}));
}

std::vector<std::string>
openFilesPicker(const std::string &title,
                const std::vector<FileFilter> &filters) {
  return liteui_filepicker::pick(
      {liteui_filepicker::Mode::OpenFiles, title, filters, {}, {}});
}

std::optional<std::string>
saveFilePicker(const std::string &title, const std::string &defaultName,
               const std::vector<FileFilter> &filters,
               const std::string &defaultExt) {
  return liteui_filepicker::first(
      liteui_filepicker::pick({liteui_filepicker::Mode::SaveFile, title,
                               filters, defaultName, defaultExt}));
}

std::optional<std::string> openFolderPicker(const std::string &title) {
  return liteui_filepicker::first(liteui_filepicker::pick(
      {liteui_filepicker::Mode::Folder, title, {}, {}, {}}));
}

std::optional<std::string> saveFolderPicker(const std::string &title) {
  return liteui_filepicker::first(liteui_filepicker::pick(
      {liteui_filepicker::Mode::SaveFolder, title, {}, {}, {}}));
}

// ==================== Clipboard ====================
namespace liteui_clipboard {

// Same-process fallback: only used when NO working OS clipboard mechanism
// exists (headless container, no wl-copy/xclip/xsel, Windows clipboard
// permanently locked). It is never consulted when the OS clipboard works,
// so it can't paste stale text over something the user copied elsewhere.
inline std::string &internalFallback() {
  static std::string s;
  return s;
}

// Normalizes "\r\n" and lone "\r" to "\n" — the editor splits lines on
// '\n' only, so any '\r' left in pasted text would end up at the end of
// every line.
std::string toLF(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\r') {
      if (i + 1 < in.size() && in[i + 1] == '\n')
        continue;  // CRLF: drop the CR, the LF is kept on the next iteration
      out += '\n'; // lone CR (classic Mac line ending)
      continue;
    }
    out += in[i];
  }
  return out;
}

// Converts "\n" to "\r\n" without doubling any that are already CRLF.
std::string toCRLF(const std::string &in) {
  std::string out;
  out.reserve(in.size() + in.size() / 16 + 1);
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\n' && (i == 0 || in[i - 1] != '\r'))
      out += '\r';
    out += in[i];
  }
  return out;
}

#if defined(_WIN32)

namespace detail {
// Another process can briefly hold the clipboard open; a few short
// retries avoids spurious failures without ever blocking noticeably.
inline bool openClipboardRetry() {
  for (int i = 0; i < 5; ++i) {
    if (OpenClipboard(nullptr))
      return true;
    Sleep(10);
  }
  return false;
}
} // namespace detail

// Returns false if the clipboard couldn't be written.
bool setText(const std::string &utf8) {
  // Windows apps (Notepad, Word, browsers) expect CRLF line endings.
  std::string crlf = toCRLF(utf8);
  int wlen = MultiByteToWideChar(CP_UTF8, 0, crlf.c_str(), -1, nullptr, 0);
  if (wlen <= 0)
    return false;
  if (!detail::openClipboardRetry())
    return false;
  EmptyClipboard();
  HGLOBAL hMem =
      GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(wchar_t));
  if (!hMem) {
    CloseClipboard();
    return false;
  }
  wchar_t *dst = static_cast<wchar_t *>(GlobalLock(hMem));
  if (!dst) {
    GlobalFree(hMem);
    CloseClipboard();
    return false;
  }
  MultiByteToWideChar(CP_UTF8, 0, crlf.c_str(), -1, dst, wlen);
  GlobalUnlock(hMem);
  if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
    GlobalFree(hMem); // clipboard did NOT take ownership on failure
    CloseClipboard();
    return false;
  }
  CloseClipboard(); // on success the clipboard owns hMem
  return true;
}

// nullopt  = clipboard couldn't be opened (caller may use the fallback)
// ""       = clipboard opened fine but holds no text (e.g. an image)
std::optional<std::string> getText() {
  if (!detail::openClipboardRetry())
    return std::nullopt;
  std::string result;
  if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
      const wchar_t *w = static_cast<const wchar_t *>(GlobalLock(h));
      if (w) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr,
                                      nullptr);
        if (len > 1) {
          result.assign(static_cast<size_t>(len - 1), '\0');
          WideCharToMultiByte(CP_UTF8, 0, w, -1, result.data(), len, nullptr,
                              nullptr);
        }
        GlobalUnlock(h);
      }
    }
  }
  CloseClipboard();
  return result;
}

#else // Linux: shell out to whatever clipboard tool is usable here.

namespace detail {

inline bool commandExists(const char *name) {
  std::string check = std::string("command -v ") + name + " >/dev/null 2>&1";
  return std::system(check.c_str()) == 0;
}

inline bool envSet(const char *name) {
  const char *v = std::getenv(name);
  return v && *v;
}

struct Tools {
  std::string copyCmd, pasteCmd; // both empty = no usable clipboard tool
};

// Detected once and cached. A tool only counts if its display server is
// actually present: wl-copy installed inside a headless container is
// useless, and treating it as "available" would suppress the fallback.
inline const Tools &tools() {
  static const Tools t = [] {
    Tools r;
    if (envSet("WAYLAND_DISPLAY") && commandExists("wl-copy") &&
        commandExists("wl-paste")) {
      r.copyCmd = "wl-copy";
      // "--type text" makes wl-paste refuse non-text contents (an image)
      // instead of dumping binary data into the editor.
      r.pasteCmd = "wl-paste --no-newline --type text";
    } else if (envSet("DISPLAY") && commandExists("xclip")) {
      r.copyCmd = "xclip -selection clipboard";
      r.pasteCmd = "xclip -selection clipboard -o";
    } else if (envSet("DISPLAY") && commandExists("xsel")) {
      r.copyCmd = "xsel --clipboard --input";
      r.pasteCmd = "xsel --clipboard --output";
    }
    return r;
  }();
  return t;
}

} // namespace detail

// Returns false if no clipboard tool is available or it couldn't be run.
bool setText(const std::string &utf8) {
  const auto &t = detail::tools();
  if (t.copyCmd.empty())
    return false;
  FILE *pipe = popen((t.copyCmd + " 2>/dev/null").c_str(), "w");
  if (!pipe)
    return false;
  if (!utf8.empty())
    fwrite(utf8.data(), 1, utf8.size(), pipe);
  pclose(pipe); // wl-copy/xclip/xsel fork and detach to hold ownership,
                // so this returns promptly
  return true;
}

// nullopt  = no usable clipboard tool (caller may use the fallback)
// ""       = tool ran fine but the clipboard has no text (e.g. an image)
std::optional<std::string> getText() {
  const auto &t = detail::tools();
  if (t.pasteCmd.empty())
    return std::nullopt;
  FILE *pipe = popen((t.pasteCmd + " 2>/dev/null").c_str(), "r");
  if (!pipe)
    return std::nullopt;
  std::string result;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0)
    result.append(buf, n);
  pclose(pipe);
  return result;
}

#endif

// What CodeEditor actually calls.
//
// Set:  always mirrors into the in-process fallback, then tries the OS.
// Get:  if the OS clipboard is reachable, its answer is final, even when
//       that answer is "empty". The fallback is used only when there is
//       no OS clipboard to ask. Line endings are normalized to '\n'.
void setTextWithFallback(const std::string &s) {
  internalFallback() = s;
  setText(s);
}

std::string getTextWithFallback() {
  if (std::optional<std::string> os = getText())
    return toLF(*os);
  return internalFallback();
}

} // namespace liteui_clipboard

namespace liteui_image {

// ======================================================================
// Image files <-> ImageBuffer: the part that needs no codec library.
// probe*, file reading/writing, input validation and the public entry
// points live here; the actual decoders/encoders (libpng + libjpeg on
// Linux, WIC on Windows) are the three *Backend functions below, defined
// once per platform.
// ======================================================================
namespace codec_detail {
inline uint32_t be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline uint32_t be16(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]);
}

// UTF-8 path in, FILE* out (the Windows CRT's fopen would read the path in
// the ANSI code page, so go through the wide variant there).
inline FILE *openFile(const std::string &path, bool forWrite) {
#if defined(_WIN32)
  return _wfopen(toWide(path).c_str(), forWrite ? L"wb" : L"rb");
#else
  return std::fopen(path.c_str(), forWrite ? "wb" : "rb");
#endif
}
inline bool seekTo(FILE *f, uint64_t off) {
#if defined(_WIN32)
  return _fseeki64(f, static_cast<__int64>(off), SEEK_SET) == 0;
#else
  return fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
}
inline bool fileLength(FILE *f, uint64_t &len) {
#if defined(_WIN32)
  if (_fseeki64(f, 0, SEEK_END) != 0)
    return false;
  const __int64 n = _ftelli64(f);
#else
  if (fseeko(f, 0, SEEK_END) != 0)
    return false;
  const off_t n = ftello(f);
#endif
  if (n < 0)
    return false;
  len = static_cast<uint64_t>(n);
  return true;
}

// Random-access byte sources for the header probe, so one parser serves
// both a memory buffer and a file (the file is never read whole).
struct MemSource {
  const uint8_t *data;
  uint64_t length;
  uint64_t size() const { return length; }
  bool read(uint64_t off, void *dst, size_t n) const {
    if (off > length || n > length - off)
      return false;
    std::memcpy(dst, data + off, n);
    return true;
  }
};
struct FileSource {
  FILE *f;
  uint64_t length;
  uint64_t size() const { return length; }
  bool read(uint64_t off, void *dst, size_t n) const {
    if (off > length || n > length - off)
      return false;
    return seekTo(f, off) && std::fread(dst, 1, n, f) == n;
  }
};

// PNG: IHDR gives size/depth/colour type; then the chunk list is walked
// (lengths only, nothing is read or checksummed) to see whether a tRNS
// chunk exists and whether the file really reaches its IEND.
template <class Src>
std::optional<ImageInfo> probePng(const Src &src, std::string *err) {
  auto fail = [&](const char *msg) -> std::optional<ImageInfo> {
    if (err)
      *err = msg;
    return std::nullopt;
  };
  const uint64_t total = src.size();
  // Layout after the 8-byte signature: length(4) "IHDR"(4) then 13 bytes:
  // width(4) height(4) depth(1) colorType(1) compression(1) filter(1)
  // interlace(1).
  uint8_t h[21];
  if (!src.read(8, h, sizeof h))
    return fail("not a valid PNG (header too short)");
  if (be32(h) != 13 || std::memcmp(h + 4, "IHDR", 4) != 0)
    return fail("not a valid PNG (no IHDR chunk)");
  ImageInfo info;
  info.format = ImageFileFormat::Png;
  info.width = be32(h + 8);
  info.height = be32(h + 12);
  info.bitDepth = h[16];
  const int colorType = h[17];
  if (info.width == 0 || info.height == 0 || info.width > 0x7FFFFFFFu ||
      info.height > 0x7FFFFFFFu)
    return fail("not a valid PNG (bad image size)");
  const bool depthOk = info.bitDepth == 1 || info.bitDepth == 2 ||
                       info.bitDepth == 4 || info.bitDepth == 8 ||
                       info.bitDepth == 16;
  const bool typeOk = colorType == 0 || colorType == 2 || colorType == 3 ||
                      colorType == 4 || colorType == 6;
  if (!depthOk || !typeOk)
    return fail("not a valid PNG (bad bit depth or color type)");
  info.hasAlpha = colorType == 4 || colorType == 6;

  bool sawEnd = false;
  uint64_t off = 8;
  for (;;) {
    uint8_t c[8];
    if (off + 12 > total || !src.read(off, c, sizeof c))
      break; // ran out of file before an IEND
    const uint64_t len = be32(c);
    if (len > 0x7FFFFFFFull)
      return fail("not a valid PNG (corrupt chunk length)");
    const uint64_t next = off + 12 + len; // length + type + data + CRC
    if (next > total)
      break;
    if (std::memcmp(c + 4, "tRNS", 4) == 0)
      info.hasAlpha = true;
    if (std::memcmp(c + 4, "IEND", 4) == 0) {
      sawEnd = true;
      break;
    }
    off = next;
  }
  info.truncated = !sawEnd;
  return info;
}

// JPEG: walk the marker segments up to the first start-of-frame, which
// holds precision, height and width.
template <class Src>
std::optional<ImageInfo> probeJpeg(const Src &src, std::string *err) {
  auto fail = [&](const char *msg) -> std::optional<ImageInfo> {
    if (err)
      *err = msg;
    return std::nullopt;
  };
  uint64_t pos = 2; // past the FF D8 that sniffing already matched
  for (int guard = 0; guard < 100000; ++guard) {
    uint8_t b = 0;
    if (!src.read(pos, &b, 1))
      return fail("not a valid JPEG (header is truncated)");
    if (b != 0xFF)
      return fail("not a valid JPEG (corrupt marker)");
    uint8_t code = 0xFF;
    while (code == 0xFF) // any number of 0xFF fill bytes may precede a code
    {
      ++pos;
      if (!src.read(pos, &code, 1))
        return fail("not a valid JPEG (header is truncated)");
    }
    ++pos; // now just past the marker code
    if (code == 0xD8 || code == 0x01 || (code >= 0xD0 && code <= 0xD7))
      continue; // standalone marker, no length field
    if (code == 0xD9 || code == 0xDA)
      return fail("not a valid JPEG (no frame header before the image data)");
    uint8_t seg[8];
    if (!src.read(pos, seg, 2))
      return fail("not a valid JPEG (header is truncated)");
    const uint32_t len = be16(seg);
    if (len < 2)
      return fail("not a valid JPEG (corrupt segment length)");
    const bool sof =
        code >= 0xC0 && code <= 0xCF && code != 0xC4 && code != 0xC8 &&
        code != 0xCC; // DHT, JPG, DAC share the range but are not frames
    if (sof) {
      if (len < 8 || !src.read(pos + 2, seg, 6))
        return fail("not a valid JPEG (corrupt frame header)");
      ImageInfo info;
      info.format = ImageFileFormat::Jpeg;
      info.bitDepth = seg[0];
      info.height = be16(seg + 1);
      info.width = be16(seg + 3);
      if (info.width == 0 || info.height == 0)
        return fail(
            "unsupported JPEG (image height is not in the frame header)");
      return info;
    }
    pos += len;
  }
  return fail("not a valid JPEG (too many segments before the frame header)");
}

template <class Src>
std::optional<ImageInfo> probeSource(const Src &src, std::string *err) {
  static const uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47,
                                     0x0D, 0x0A, 0x1A, 0x0A};
  uint8_t head[8] = {};
  const size_t n =
      static_cast<size_t>(std::min<uint64_t>(src.size(), sizeof head));
  if (n == 0) {
    if (err)
      *err = "empty file";
    return std::nullopt;
  }
  if (!src.read(0, head, n)) {
    if (err)
      *err = "failed to read file";
    return std::nullopt;
  }
  if (n == 8 && std::memcmp(head, kPngSig, 8) == 0)
    return probePng(src, err);
  if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
    return probeJpeg(src, err);
  if (err)
    *err = "unrecognized image format (not PNG or JPEG)";
  return std::nullopt;
}

inline bool readFileBytes(const std::string &path, std::vector<uint8_t> &out,
                          std::string *err) {
  FILE *f = openFile(path, false);
  if (!f) {
    if (err)
      *err = "failed to open file";
    return false;
  }
  uint64_t len = 0;
  bool ok = fileLength(f, len) && seekTo(f, 0);
  if (ok) {
    try {
      out.resize(static_cast<size_t>(len));
    } catch (const std::exception &) {
      std::fclose(f);
      if (err)
        *err = "out of memory";
      return false;
    }
    ok = len == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
  }
  std::fclose(f);
  if (!ok && err)
    *err = "failed to read file";
  return ok;
}

// Writes `path` via "<path>.tmp" + rename, so an existing file is replaced
// in one step and a failed write leaves nothing half-written.
inline bool writeFileAtomic(const std::string &path,
                            const std::vector<uint8_t> &bytes,
                            std::string *err) {
  const std::string tmp = path + ".tmp";
  FILE *f = openFile(tmp, true);
  if (!f) {
    if (err)
      *err = "failed to create file";
    return false;
  }
  bool ok = bytes.empty() ||
            std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  ok = std::fflush(f) == 0 && ok;
  ok = std::fclose(f) == 0 && ok;
#if defined(_WIN32)
  const std::wstring wtmp = toWide(tmp);
  if (ok)
    ok = MoveFileExW(wtmp.c_str(), toWide(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!ok)
    _wremove(wtmp.c_str());
#else
  if (ok)
    ok = std::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok)
    std::remove(tmp.c_str());
#endif
  if (!ok && err)
    *err = "failed to write file";
  return ok;
}

// True when every pixel's alpha is fully opaque.
inline bool isOpaque(const ImageBuffer &img) {
  const bool wide = img.format == PixelFormat::RGBA16_UNORM;
  for (uint32_t y = 0; y < img.height; ++y) {
    const std::byte *row = img.row(y);
    if (wide) {
      for (uint32_t x = 0; x < img.width; ++x) {
        uint16_t a;
        std::memcpy(&a, row + (static_cast<size_t>(x) * 4 + 3) * 2, 2);
        if (a != 0xFFFF)
          return false;
      }
    } else {
      for (uint32_t x = 0; x < img.width; ++x)
        if (static_cast<uint8_t>(row[static_cast<size_t>(x) * 4 + 3]) != 0xFF)
          return false;
    }
  }
  return true;
}

// CRC-32 as PNG uses it (polynomial 0xEDB88320), continued across calls: start
// from 0xFFFFFFFF and xor the final value with 0xFFFFFFFF.
inline uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n) {
  struct Table {
    uint32_t v[256];
    Table() {
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
          c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        v[i] = c;
      }
    }
  };
  static const Table table;
  for (size_t i = 0; i < n; ++i)
    crc = table.v[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

// Walks every chunk of a PNG held in memory and checks the file is whole:
// each chunk's data and CRC are present and correct, there is at least one
// IDAT, and the last chunk is IEND. (The signature and IHDR were already
// checked by probePng.) libpng enforces most of this itself; WIC does not, so
// this runs before either decoder, which also makes the two agree.
inline bool validatePngChunks(const uint8_t *d, size_t n, std::string *err) {
  auto fail = [&](const char *msg) {
    if (err)
      *err = msg;
    return false;
  };
  size_t off = 8;
  bool sawIdat = false, sawEnd = false;
  while (off + 12 <= n) {
    const uint64_t len = be32(d + off);
    if (len > 0x7FFFFFFFull)
      return fail("PNG chunk length is invalid");
    if (off + 12 + len > n)
      return fail("PNG is truncated");
    const uint8_t *type =
        d + off + 4; // type and data are contiguous: one CRC run
    const uint32_t crc =
        crc32Update(0xFFFFFFFFu, type, static_cast<size_t>(4 + len)) ^
        0xFFFFFFFFu;
    if (crc != be32(d + off + 8 + len))
      return fail("PNG is corrupt (chunk CRC mismatch)");
    if (std::memcmp(type, "IDAT", 4) == 0)
      sawIdat = true;
    if (std::memcmp(type, "IEND", 4) == 0) {
      sawEnd = true;
      break;
    }
    off += 12 + static_cast<size_t>(len);
  }
  if (!sawEnd)
    return fail("PNG is truncated");
  if (!sawIdat)
    return fail("PNG has no image data");
  return true;
}

// Alpha composited over white, rounded: exact for a == 255.
inline uint8_t overWhite(uint8_t c, uint8_t a) {
  return static_cast<uint8_t>(
      (static_cast<unsigned>(c) * a + 255u * (255u - a) + 127u) / 255u);
}

// One RGBA8 row -> packed 8-bit colour, RGB or BGR (WIC wants BGR), with the
// alpha composited over white or ignored. The one place JPEG rows are made,
// for both backends.
inline void packRowRgb(const uint8_t *src, uint32_t w, bool flatten, bool bgr,
                       uint8_t *dst) {
  for (uint32_t x = 0; x < w; ++x, src += 4, dst += 3) {
    const uint8_t a = src[3];
    const bool blend = flatten && a != 255;
    const uint8_t r = blend ? overWhite(src[0], a) : src[0];
    const uint8_t g = blend ? overWhite(src[1], a) : src[1];
    const uint8_t b = blend ? overWhite(src[2], a) : src[2];
    dst[0] = bgr ? b : r;
    dst[1] = g;
    dst[2] = bgr ? r : b;
  }
}

inline bool wantFullChroma(const JpegEncodeOptions &o) {
  return o.chroma == JpegChroma::S444 ||
         (o.chroma == JpegChroma::Auto && o.quality >= 90);
}

inline bool checkEncodable(const ImageBuffer &img, bool jpeg,
                           std::string *err) {
  auto fail = [&](const char *msg) {
    if (err)
      *err = msg;
    return false;
  };
  if (!img.valid() || img.empty())
    return fail("image is empty or invalid");
  if (jpeg) {
    if (img.format != PixelFormat::RGBA8_UNORM)
      return fail(
          "JPEG needs an RGBA8_UNORM image (convert with convertImage first)");
    if (!jpegSizeOk(img.width, img.height))
      return fail("JPEG can't hold a picture this size");
  } else {
    if (img.format != PixelFormat::RGBA8_UNORM &&
        img.format != PixelFormat::RGBA16_UNORM)
      return fail("PNG needs an RGBA8_UNORM or RGBA16_UNORM image (convert "
                  "with convertImage first)");
    if (img.width > 0x7FFFFFFFu || img.height > 0x7FFFFFFFu)
      return fail("image is too large for PNG");
  }
  return true;
}

// Per-platform codecs (defined below in the Windows / Linux branches). The
// public functions have already validated their arguments.
inline std::optional<ImageBuffer> decodeBackend(const uint8_t *data,
                                                size_t size,
                                                const ImageInfo &info,
                                                std::string *err);
inline bool encodePngBackend(std::vector<uint8_t> &out, const ImageBuffer &img,
                             const PngEncodeOptions &opt, std::string *err);
inline bool encodeJpegBackend(std::vector<uint8_t> &out, const ImageBuffer &img,
                              const JpegEncodeOptions &opt, std::string *err);
} // namespace codec_detail

std::optional<ImageInfo> probeMemory(const uint8_t *data, size_t size,
                                     std::string *errorOut) {
  if (!data || size == 0) {
    if (errorOut)
      *errorOut = "empty buffer";
    return std::nullopt;
  }
  return codec_detail::probeSource(codec_detail::MemSource{data, size},
                                   errorOut);
}

std::optional<ImageInfo> probeFile(const std::string &path,
                                   std::string *errorOut) {
  FILE *f = codec_detail::openFile(path, false);
  if (!f) {
    if (errorOut)
      *errorOut = "failed to open file";
    return std::nullopt;
  }
  uint64_t len = 0;
  std::optional<ImageInfo> info;
  if (codec_detail::fileLength(f, len))
    info =
        codec_detail::probeSource(codec_detail::FileSource{f, len}, errorOut);
  else if (errorOut)
    *errorOut = "failed to read file";
  std::fclose(f);
  return info;
}

std::optional<ImageBuffer> decodeMemoryBuffer(const uint8_t *data, size_t size,
                                              std::string *errorOut) {
  try {
    auto info = probeMemory(data, size, errorOut);
    if (!info)
      return std::nullopt;
    if (info->truncated) {
      if (errorOut)
        *errorOut = "PNG is truncated";
      return std::nullopt;
    }
    if (info->format == ImageFileFormat::Png &&
        !codec_detail::validatePngChunks(data, size, errorOut))
      return std::nullopt;
    return codec_detail::decodeBackend(data, size, *info, errorOut);
  } catch (const std::exception &e) {
    if (errorOut)
      *errorOut = std::string("decode failed: ") + e.what();
    return std::nullopt;
  }
}

std::optional<ImageBuffer> decodeFileBuffer(const std::string &path,
                                            std::string *errorOut) {
  // The whole file is read into memory first: both libpng and WIC then decode
  // from one buffer, and the same code path serves decodeMemoryBuffer.
  std::vector<uint8_t> bytes;
  try {
    if (!codec_detail::readFileBytes(path, bytes, errorOut))
      return std::nullopt;
  } catch (const std::exception &e) {
    if (errorOut)
      *errorOut = std::string("read failed: ") + e.what();
    return std::nullopt;
  }
  if (bytes.empty()) {
    if (errorOut)
      *errorOut = "empty file";
    return std::nullopt;
  }
  return decodeMemoryBuffer(bytes.data(), bytes.size(), errorOut);
}

bool encodePngMemory(std::vector<uint8_t> &out, const ImageBuffer &image,
                     std::string *errorOut, const PngEncodeOptions &options) {
  out.clear();
  if (!codec_detail::checkEncodable(image, false, errorOut))
    return false;
  bool ok = false;
  try {
    ok = codec_detail::encodePngBackend(out, image, options, errorOut);
  } catch (const std::exception &e) {
    if (errorOut)
      *errorOut = std::string("PNG encode failed: ") + e.what();
  }
  if (!ok)
    out.clear();
  return ok;
}

bool encodePng(const std::string &path, const ImageBuffer &image,
               std::string *errorOut, const PngEncodeOptions &options) {
  std::vector<uint8_t> bytes;
  return encodePngMemory(bytes, image, errorOut, options) &&
         codec_detail::writeFileAtomic(path, bytes, errorOut);
}

bool encodeJpegMemory(std::vector<uint8_t> &out, const ImageBuffer &image,
                      const JpegEncodeOptions &options, std::string *errorOut) {
  out.clear();
  if (!codec_detail::checkEncodable(image, true, errorOut))
    return false;
  JpegEncodeOptions opt = options;
  opt.quality = std::clamp(opt.quality, 1, 100);
  bool ok = false;
  try {
    ok = codec_detail::encodeJpegBackend(out, image, opt, errorOut);
  } catch (const std::exception &e) {
    if (errorOut)
      *errorOut = std::string("JPEG encode failed: ") + e.what();
  }
  if (!ok)
    out.clear();
  return ok;
}

bool encodeJpegMemory(std::vector<uint8_t> &out, const ImageBuffer &image,
                      int quality, std::string *errorOut) {
  JpegEncodeOptions opt;
  opt.quality = quality;
  return encodeJpegMemory(out, image, opt, errorOut);
}

bool encodeJpeg(const std::string &path, const ImageBuffer &image,
                const JpegEncodeOptions &options, std::string *errorOut) {
  std::vector<uint8_t> bytes;
  return encodeJpegMemory(bytes, image, options, errorOut) &&
         codec_detail::writeFileAtomic(path, bytes, errorOut);
}

bool encodeJpeg(const std::string &path, const ImageBuffer &image, int quality,
                std::string *errorOut) {
  JpegEncodeOptions opt;
  opt.quality = quality;
  return encodeJpeg(path, image, opt, errorOut);
}

#if defined(_WIN32)

std::optional<CanvasImage> decodeMemory(const uint8_t *data, size_t size,
                                        std::string *errorOut) {
  if (!data || size == 0) {
    if (errorOut)
      *errorOut = "empty buffer";
    return std::nullopt;
  }

  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool weInitialized = coHr == S_OK;

  IWICImagingFactory *factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: CoCreateInstance failed";
    return std::nullopt;
  }

  // IWICStream::InitializeFromMemory wraps the buffer in place (no copy),
  // so `data` must stay valid for the lifetime of this call — fine here
  // since decoding happens synchronously before we return.
  IWICStream *stream = nullptr;
  hr = factory->CreateStream(&stream);
  if (SUCCEEDED(hr))
    hr = stream->InitializeFromMemory(const_cast<BYTE *>(data),
                                      static_cast<DWORD>(size));
  if (FAILED(hr)) {
    if (stream)
      stream->Release();
    factory->Release();
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: InitializeFromMemory failed";
    return std::nullopt;
  }

  IWICBitmapDecoder *decoder = nullptr;
  hr = factory->CreateDecoderFromStream(
      stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
  if (FAILED(hr)) {
    stream->Release();
    factory->Release();
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: failed to decode buffer (unrecognized format?)";
    return std::nullopt;
  }

  IWICBitmapFrameDecode *frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  IWICFormatConverter *converter = nullptr;
  if (SUCCEEDED(hr)) {
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
      hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom);
  }
  if (FAILED(hr)) {
    if (converter)
      converter->Release();
    if (frame)
      frame->Release();
    decoder->Release();
    stream->Release();
    factory->Release();
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: decode/convert failed";
    return std::nullopt;
  }

  UINT w = 0, h = 0;
  converter->GetSize(&w, &h);
  CanvasImage img(static_cast<int>(w), static_cast<int>(h));
  hr = converter->CopyPixels(
      nullptr, w * 4, static_cast<UINT>(img.pixels.size()), img.pixels.data());

  converter->Release();
  frame->Release();
  decoder->Release();
  stream->Release();
  factory->Release();
  if (weInitialized)
    CoUninitialize();

  if (FAILED(hr)) {
    if (errorOut)
      *errorOut = "WIC: CopyPixels failed";
    return std::nullopt;
  }
  return img;
}

std::optional<CanvasImage> decodeFile(const std::string &path,
                                      std::string *errorOut) {
  // WIC needs an apartment; tolerate "already initialized differently"
  // rather than fail, since the host app may have called this itself.
  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool weInitialized = coHr == S_OK;

  IWICImagingFactory *factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: CoCreateInstance failed";
    return std::nullopt;
  }

  std::wstring wpath = toWide(path);
  IWICBitmapDecoder *decoder = nullptr;
  hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                          WICDecodeMetadataCacheOnDemand,
                                          &decoder);
  if (FAILED(hr)) {
    factory->Release();
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: failed to open/decode file";
    return std::nullopt;
  }

  IWICBitmapFrameDecode *frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  IWICFormatConverter *converter = nullptr;
  if (SUCCEEDED(hr)) {
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
      // Straight (non-premultiplied) alpha, matching CanvasImage's contract.
      hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom);
  }
  if (FAILED(hr)) {
    if (converter)
      converter->Release();
    if (frame)
      frame->Release();
    decoder->Release();
    factory->Release();
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: decode/convert failed";
    return std::nullopt;
  }

  UINT w = 0, h = 0;
  converter->GetSize(&w, &h);
  CanvasImage img(static_cast<int>(w), static_cast<int>(h));
  hr = converter->CopyPixels(
      nullptr, w * 4, static_cast<UINT>(img.pixels.size()), img.pixels.data());

  converter->Release();
  frame->Release();
  decoder->Release();
  factory->Release();
  if (weInitialized)
    CoUninitialize();

  if (FAILED(hr)) {
    if (errorOut)
      *errorOut = "WIC: CopyPixels failed";
    return std::nullopt;
  }
  return img;
}

// ---- Image files <-> ImageBuffer: WIC backend ------------------------------
namespace codec_detail {
// Minimal owning COM pointer (no ATL / WRL dependency).
template <class T> struct ComRef {
  T *p = nullptr;
  ComRef() = default;
  ComRef(const ComRef &) = delete;
  ComRef &operator=(const ComRef &) = delete;
  ~ComRef() { reset(); }
  void reset() {
    if (p) {
      p->Release();
      p = nullptr;
    }
  }
  T **put() {
    reset();
    return &p;
  }
  T *get() const { return p; }
  T *operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

// CoInitialize for the scope. S_OK and S_FALSE both need a matching
// CoUninitialize; RPC_E_CHANGED_MODE (the host chose another apartment
// model) is tolerated and needs none.
struct WicCom {
  HRESULT hr;
  WicCom() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~WicCom() {
    if (SUCCEEDED(hr))
      CoUninitialize();
  }
  WicCom(const WicCom &) = delete;
  WicCom &operator=(const WicCom &) = delete;
};

inline bool wicFactory(ComRef<IWICImagingFactory> &f, std::string *err) {
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put()));
  if (FAILED(hr)) {
    if (err)
      *err = "WIC: CoCreateInstance failed";
    return false;
  }
  return true;
}

// Decodes through a format converter to straight-alpha RGBA: 32bppRGBA for
// 8-bit files, 64bppRGBA (16-bit UNORM per channel) for a 16-bit PNG.
inline std::optional<ImageBuffer> decodeBackend(const uint8_t *data,
                                                size_t size,
                                                const ImageInfo &info,
                                                std::string *err) {
  auto fail = [&](const char *msg) -> std::optional<ImageBuffer> {
    if (err)
      *err = msg;
    return std::nullopt;
  };
  if (size > 0xFFFFFFFFull)
    return fail("WIC: buffer too large");
  const PixelFormat fmt = info.bufferFormat();
  const bool wide = fmt == PixelFormat::RGBA16_UNORM;
  auto out = ImageBuffer::create(info.width, info.height, fmt);
  if (!out)
    return fail("out of memory");
  if (out->pixels.size() > 0xFFFFFFFFull)
    return fail("WIC: image too large");

  WicCom com;
  ComRef<IWICImagingFactory> factory;
  if (!wicFactory(factory, err))
    return std::nullopt;
  // InitializeFromMemory wraps the buffer in place: `data` only has to
  // outlive this call, which it does (decoding is synchronous).
  ComRef<IWICStream> stream;
  if (FAILED(factory->CreateStream(stream.put())) ||
      FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(data),
                                          static_cast<DWORD>(size))))
    return fail("WIC: InitializeFromMemory failed");
  ComRef<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                              WICDecodeMetadataCacheOnDemand,
                                              decoder.put())))
    return fail("WIC: failed to decode buffer (unrecognized format?)");
  ComRef<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.put())))
    return fail("WIC: file has no image frame");
  // A 16-bit gray PNG: WIC's converter does not return the exact 16-bit values
  // when widening 16bppGray to 64bppRGBA (every other 16-bit layout converts
  // exactly). Copy the gray plane in its native format and replicate it into
  // RGB by hand instead.
  WICPixelFormatGUID native = {};
  if (wide && SUCCEEDED(frame->GetPixelFormat(&native)) &&
      IsEqualGUID(native, GUID_WICPixelFormat16bppGray)) {
    UINT fw = 0, fh = 0;
    frame->GetSize(&fw, &fh);
    if (fw != info.width || fh != info.height)
      return fail("WIC: decoded size differs from the file header");
    std::vector<uint16_t> gray(static_cast<size_t>(fw) * fh);
    if (FAILED(frame->CopyPixels(nullptr, fw * 2u,
                                 static_cast<UINT>(gray.size() * 2),
                                 reinterpret_cast<BYTE *>(gray.data()))))
      return fail("WIC: CopyPixels failed");
    for (UINT y = 0; y < fh; ++y) {
      std::byte *dst = out->row(y);
      const uint16_t *srcRow = &gray[static_cast<size_t>(y) * fw];
      for (UINT x = 0; x < fw; ++x) {
        const uint16_t px[4] = {srcRow[x], srcRow[x], srcRow[x], 0xFFFF};
        std::memcpy(dst + static_cast<size_t>(x) * 8, px, 8);
      }
    }
    return out;
  }
  ComRef<IWICFormatConverter> conv;
  if (FAILED(factory->CreateFormatConverter(conv.put())) ||
      FAILED(conv->Initialize(
          frame.get(),
          wide ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppRGBA,
          WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
    return fail("WIC: decode/convert failed");
  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  if (w != info.width || h != info.height)
    return fail("WIC: decoded size differs from the file header");
  if (FAILED(conv->CopyPixels(nullptr, static_cast<UINT>(out->rowStride),
                              static_cast<UINT>(out->pixels.size()),
                              reinterpret_cast<BYTE *>(out->pixels.data()))))
    return fail("WIC: CopyPixels failed");
  return out;
}

// Encodes `bmp` (pixel layout srcFmt) with the given container encoder into
// memory, writing it as pixel format dstFmt (a converter is inserted when
// they differ, e.g. RGBA -> BGR, which also drops the alpha). jpegQuality is
// 0..1 for WIC's ImageQuality property, negative for PNG; jpegSubsampling is a
// WICJpegYCrCbSubsampling value, 0 to leave WIC's default. The properties are
// best effort: an encoder that rejects one is not an error.
inline bool wicEncodeBitmap(IWICImagingFactory *factory, const GUID &container,
                            IWICBitmap *bmp, UINT width, UINT height,
                            const GUID &srcFmt, const GUID &dstFmt,
                            float jpegQuality, int jpegSubsampling,
                            std::vector<uint8_t> &out, std::string *err) {
  auto fail = [&](const char *msg) {
    if (err)
      *err = msg;
    return false;
  };
  ComRef<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())))
    return fail("WIC: CreateStreamOnHGlobal failed");
  ComRef<IWICBitmapEncoder> enc;
  if (FAILED(factory->CreateEncoder(container, nullptr, enc.put())) ||
      FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)))
    return fail("WIC: could not create the encoder");
  ComRef<IWICBitmapFrameEncode> frame;
  ComRef<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(frame.put(), props.put())))
    return fail("WIC: CreateNewFrame failed");
  if (props && jpegQuality >= 0.0f) {
    PROPBAG2 opt;
    ZeroMemory(&opt, sizeof opt);
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;
    ZeroMemory(&v, sizeof v);
    v.vt = VT_R4;
    v.fltVal = jpegQuality;
    props->Write(1, &opt, &v);
  }
  if (props && jpegSubsampling != 0) {
    PROPBAG2 opt;
    ZeroMemory(&opt, sizeof opt);
    opt.pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
    VARIANT v;
    ZeroMemory(&v, sizeof v);
    v.vt = VT_UI1;
    v.bVal = static_cast<BYTE>(jpegSubsampling);
    props->Write(1, &opt, &v);
  }
  if (FAILED(frame->Initialize(props.get())) ||
      FAILED(frame->SetSize(width, height)))
    return fail("WIC: frame setup failed");
  WICPixelFormatGUID pf = dstFmt;
  if (FAILED(frame->SetPixelFormat(&pf)))
    return fail("WIC: SetPixelFormat failed");
  IWICBitmapSource *source = bmp;
  ComRef<IWICFormatConverter> conv;
  if (!IsEqualGUID(pf, srcFmt)) {
    if (FAILED(factory->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(bmp, pf, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
      return fail("WIC: pixel format conversion failed");
    source = conv.get();
  }
  if (FAILED(frame->WriteSource(source, nullptr)) || FAILED(frame->Commit()) ||
      FAILED(enc->Commit()))
    return fail("WIC: encoding failed");

  STATSTG st;
  ZeroMemory(&st, sizeof st);
  if (FAILED(stream->Stat(&st, STATFLAG_NONAME)))
    return fail("WIC: could not size the output");
  const ULONGLONG n = st.cbSize.QuadPart;
  LARGE_INTEGER zero;
  zero.QuadPart = 0;
  if (n == 0 || n > 0xFFFFFFFFull ||
      FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
    return fail("WIC: encoder produced no data");
  out.resize(static_cast<size_t>(n));
  ULONG got = 0;
  if (FAILED(stream->Read(out.data(), static_cast<ULONG>(n), &got)) || got != n)
    return fail("WIC: could not read back the output");
  return true;
}

inline bool encodePngBackend(std::vector<uint8_t> &out, const ImageBuffer &img,
                             const PngEncodeOptions &opt, std::string *err) {
  const bool wide = img.format == PixelFormat::RGBA16_UNORM;
  const bool rgb = opt.dropOpaqueAlpha && isOpaque(img);
  const GUID &src =
      wide ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppRGBA;
  const GUID &dst =
      wide ? (rgb ? GUID_WICPixelFormat48bppRGB : GUID_WICPixelFormat64bppRGBA)
           : (rgb ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA);
  if (img.pixels.size() > 0xFFFFFFFFull) {
    if (err)
      *err = "WIC: image too large";
    return false;
  }
  WicCom com;
  ComRef<IWICImagingFactory> factory;
  if (!wicFactory(factory, err))
    return false;
  ComRef<IWICBitmap> bmp; // copies the pixels
  if (FAILED(factory->CreateBitmapFromMemory(
          img.width, img.height, src, static_cast<UINT>(img.rowStride),
          static_cast<UINT>(img.pixels.size()),
          const_cast<BYTE *>(reinterpret_cast<const BYTE *>(img.pixels.data())),
          bmp.put()))) {
    if (err)
      *err = "WIC: CreateBitmapFromMemory failed";
    return false;
  }
  return wicEncodeBitmap(factory.get(), GUID_ContainerFormatPng, bmp.get(),
                         img.width, img.height, src, dst, -1.0f, 0, out, err);
}

// JPEG: the colour is packed straight into a 24bpp BGR WIC bitmap (alpha
// already flattened or dropped by packRowRgb), so there is one copy of the
// picture, not two, and no format converter is needed.
inline bool encodeJpegBackend(std::vector<uint8_t> &out, const ImageBuffer &img,
                              const JpegEncodeOptions &opt, std::string *err) {
  auto fail = [&](const char *msg) {
    if (err)
      *err = msg;
    return false;
  };
  WicCom com;
  ComRef<IWICImagingFactory> factory;
  if (!wicFactory(factory, err))
    return false;
  ComRef<IWICBitmap> bmp;
  if (FAILED(factory->CreateBitmap(img.width, img.height,
                                   GUID_WICPixelFormat24bppBGR,
                                   WICBitmapCacheOnLoad, bmp.put())))
    return fail("WIC: CreateBitmap failed");
  {
    WICRect rc = {0, 0, static_cast<INT>(img.width),
                  static_cast<INT>(img.height)};
    ComRef<IWICBitmapLock>
        lock; // released at the end of this block, before encoding
    if (FAILED(bmp->Lock(&rc, WICBitmapLockWrite, lock.put())))
      return fail("WIC: could not lock the bitmap");
    UINT stride = 0, size = 0;
    BYTE *dst = nullptr;
    if (FAILED(lock->GetStride(&stride)) ||
        FAILED(lock->GetDataPointer(&size, &dst)) || !dst)
      return fail("WIC: could not address the bitmap");
    for (uint32_t y = 0; y < img.height; ++y)
      packRowRgb(reinterpret_cast<const uint8_t *>(img.row(y)), img.width,
                 opt.alpha == JpegAlpha::FlattenOverWhite, /*bgr=*/true,
                 dst + static_cast<size_t>(y) * stride);
  }
  return wicEncodeBitmap(factory.get(), GUID_ContainerFormatJpeg, bmp.get(),
                         img.width, img.height, GUID_WICPixelFormat24bppBGR,
                         GUID_WICPixelFormat24bppBGR,
                         static_cast<float>(opt.quality) / 100.0f,
                         wantFullChroma(opt) ? WICJpegYCrCbSubsampling444
                                             : WICJpegYCrCbSubsampling420,
                         out, err);
}
} // namespace codec_detail

#else // Linux

namespace detail {

inline std::string lowerExt(const std::string &path) {
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return "";
  std::string ext = path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return ext;
}

// Since there's no filename/extension for a memory buffer, sniff the
// format from its magic bytes instead — same signatures libpng/libjpeg
// themselves check internally.
enum class ImageFormat { Unknown, Png, Jpeg };
inline ImageFormat sniffFormat(const uint8_t *data, size_t size) {
  static const uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47,
                                     0x0D, 0x0A, 0x1A, 0x0A};
  if (size >= 8 && std::memcmp(data, kPngSig, 8) == 0)
    return ImageFormat::Png;
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
    return ImageFormat::Jpeg;
  return ImageFormat::Unknown;
}

// Read-callback state for libpng's png_set_read_fn — tracks how far
// we've consumed into the caller's buffer.
struct PngMemReader {
  const uint8_t *data;
  size_t size, offset = 0;
};
inline void pngMemRead(png_structp png, png_bytep out, png_size_t count) {
  auto *r = static_cast<PngMemReader *>(png_get_io_ptr(png));
  if (r->offset + count > r->size) {
    png_error(png, "liteui: PNG read past end of buffer");
    return;
  }
  std::memcpy(out, r->data + r->offset, count);
  r->offset += count;
}

// Custom error manager so a corrupt/truncated JPEG returns nullopt instead
// of libjpeg's default error_exit calling exit() and killing the process.
// Mirrors the setjmp/longjmp escape libpng uses via png_jmpbuf above.
struct JpegErrorMgr {
  jpeg_error_mgr pub; // must be first member — libjpeg treats this struct
                      // as a jpeg_error_mgr* via reinterpret_cast
  jmp_buf escape;
  std::string message;
};

inline void jpegErrorExit(j_common_ptr cinfo) {
  auto *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
  char buf[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, buf);
  err->message = buf;
  longjmp(err->escape, 1);
}

inline std::optional<CanvasImage>
decodePngMemory(const uint8_t *data, size_t size, std::string *errorOut) {
  if (size < 8 || png_sig_cmp(data, 0, 8)) {
    if (errorOut)
      *errorOut = "not a valid PNG buffer";
    return std::nullopt;
  }
  png_structp png =
      png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info) {
    if (png)
      png_destroy_read_struct(&png, info ? &info : nullptr, nullptr);
    if (errorOut)
      *errorOut = "libpng init failed";
    return std::nullopt;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    if (errorOut)
      *errorOut = "libpng decode error";
    return std::nullopt;
  }

  PngMemReader reader{data, size};
  png_set_read_fn(png, &reader, pngMemRead);
  png_read_info(png, info);

  png_uint_32 w = png_get_image_width(png, info);
  png_uint_32 h = png_get_image_height(png, info);
  int bitDepth = png_get_bit_depth(png, info);
  int colorType = png_get_color_type(png, info);

  if (bitDepth == 16)
    png_set_strip_16(png);
  if (colorType == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_GRAY ||
      colorType == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  if (colorType == PNG_COLOR_TYPE_GRAY ||
      colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  png_set_interlace_handling(png);
  png_read_update_info(png, info);

  CanvasImage img(static_cast<int>(w), static_cast<int>(h));
  size_t rowBytes = png_get_rowbytes(png, info);
  std::vector<png_bytep> rows(h);
  for (png_uint_32 y = 0; y < h; ++y)
    rows[y] = img.pixels.data() + y * rowBytes;
  png_read_image(png, rows.data());

  png_destroy_read_struct(&png, &info, nullptr);
  return img;
}

inline std::optional<CanvasImage>
decodeJpegMemory(const uint8_t *data, size_t size, std::string *errorOut) {
  jpeg_decompress_struct cinfo;
  JpegErrorMgr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;

  if (setjmp(jerr.escape)) {
    jpeg_destroy_decompress(&cinfo);
    if (errorOut)
      *errorOut = jerr.message;
    return std::nullopt;
  }

  jpeg_create_decompress(&cinfo);
  // jpeg_mem_src takes a non-const unsigned char* in some libjpeg
  // versions; it never writes through it, so the cast is safe.
  jpeg_mem_src(&cinfo, const_cast<unsigned char *>(data),
               static_cast<unsigned long>(size));
  jpeg_read_header(&cinfo, TRUE);
  jpeg_start_decompress(&cinfo);

  int w = static_cast<int>(cinfo.output_width);
  int h = static_cast<int>(cinfo.output_height);
  int channels = cinfo.output_components;

  CanvasImage img(w, h);
  std::vector<unsigned char> row(static_cast<size_t>(w) * channels);
  unsigned char *rowPtr = row.data();
  while (cinfo.output_scanline < cinfo.output_height) {
    int y = static_cast<int>(cinfo.output_scanline);
    jpeg_read_scanlines(&cinfo, &rowPtr, 1);
    unsigned char *dst = img.pixels.data() + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      if (channels == 1) {
        dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = row[x];
      } else {
        dst[x * 4 + 0] = row[x * 3 + 0];
        dst[x * 4 + 1] = row[x * 3 + 1];
        dst[x * 4 + 2] = row[x * 3 + 2];
      }
      dst[x * 4 + 3] = 255;
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return img;
}

inline std::optional<CanvasImage> decodePngFile(const std::string &path,
                                                std::string *errorOut) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp) {
    if (errorOut)
      *errorOut = "failed to open file";
    return std::nullopt;
  }
  png_byte header[8];
  if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8)) {
    fclose(fp);
    if (errorOut)
      *errorOut = "not a valid PNG file";
    return std::nullopt;
  }
  png_structp png =
      png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info) {
    if (png)
      png_destroy_read_struct(&png, info ? &info : nullptr, nullptr);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng init failed";
    return std::nullopt;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng decode error";
    return std::nullopt;
  }
  png_init_io(png, fp);
  png_set_sig_bytes(png, 8);
  png_read_info(png, info);

  png_uint_32 w = png_get_image_width(png, info);
  png_uint_32 h = png_get_image_height(png, info);
  int bitDepth = png_get_bit_depth(png, info);
  int colorType = png_get_color_type(png, info);

  // Normalize every PNG variant (palette/gray/gray+alpha/RGB/RGBA, 1-16 bit)
  // down to straight-alpha 8-bit RGBA.
  if (bitDepth == 16)
    png_set_strip_16(png);
  if (colorType == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_GRAY ||
      colorType == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  if (colorType == PNG_COLOR_TYPE_GRAY ||
      colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  png_set_interlace_handling(png);
  png_read_update_info(png, info);

  CanvasImage img(static_cast<int>(w), static_cast<int>(h));
  size_t rowBytes = png_get_rowbytes(png, info);
  std::vector<png_bytep> rows(h);
  for (png_uint_32 y = 0; y < h; ++y)
    rows[y] = img.pixels.data() + y * rowBytes;
  png_read_image(png, rows.data());

  png_destroy_read_struct(&png, &info, nullptr);
  fclose(fp);
  return img;
}

inline std::optional<CanvasImage> decodeJpegFile(const std::string &path,
                                                 std::string *errorOut) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp) {
    if (errorOut)
      *errorOut = "failed to open file";
    return std::nullopt;
  }

  jpeg_decompress_struct cinfo;
  JpegErrorMgr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;

  // Any libjpeg call inside this guarded region that would otherwise
  // exit() instead longjmps back here — jpeg_create_decompress itself is
  // included, per libjpeg's own documented pattern, since a version
  // mismatch can in principle fail there too.
  if (setjmp(jerr.escape)) {
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    if (errorOut)
      *errorOut = jerr.message;
    return std::nullopt;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, fp);
  jpeg_read_header(&cinfo, TRUE);
  jpeg_start_decompress(&cinfo);

  int w = static_cast<int>(cinfo.output_width);
  int h = static_cast<int>(cinfo.output_height);
  int channels = cinfo.output_components; // 1 (gray) or 3 (RGB) — no alpha

  CanvasImage img(w, h);
  std::vector<unsigned char> row(static_cast<size_t>(w) * channels);
  unsigned char *rowPtr = row.data();
  while (cinfo.output_scanline < cinfo.output_height) {
    int y = static_cast<int>(cinfo.output_scanline);
    jpeg_read_scanlines(&cinfo, &rowPtr, 1);
    unsigned char *dst = img.pixels.data() + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      if (channels == 1) {
        dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = row[x];
      } else {
        dst[x * 4 + 0] = row[x * 3 + 0];
        dst[x * 4 + 1] = row[x * 3 + 1];
        dst[x * 4 + 2] = row[x * 3 + 2];
      }
      dst[x * 4 + 3] = 255; // JPEG carries no alpha channel
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  fclose(fp);
  return img;
}

} // namespace detail

std::optional<CanvasImage> decodeMemory(const uint8_t *data, size_t size,
                                        std::string *errorOut) {
  switch (detail::sniffFormat(data, size)) {
  case detail::ImageFormat::Png:
    return detail::decodePngMemory(data, size, errorOut);
  case detail::ImageFormat::Jpeg:
    return detail::decodeJpegMemory(data, size, errorOut);
  default:
    if (errorOut)
      *errorOut = "unrecognized image format (not PNG or JPEG)";
    return std::nullopt;
  }
}

std::optional<CanvasImage> decodeFile(const std::string &path,
                                      std::string *errorOut) {
  std::string ext = detail::lowerExt(path);
  if (ext == "png")
    return detail::decodePngFile(path, errorOut);
  if (ext == "jpg" || ext == "jpeg")
    return detail::decodeJpegFile(path, errorOut);
  if (errorOut)
    *errorOut = "unsupported image extension: ." + ext;
  return std::nullopt;
}

// ---- Image files <-> ImageBuffer: libpng / libjpeg backend -----------------
namespace codec_detail {
// libpng's default handlers print to stderr. These keep the message for the
// caller instead (warnings are dropped) and jump back to the setjmp point.
struct PngDiag {
  char message[192];
};
inline void pngErrorFn(png_structp png, png_const_charp msg) {
  if (auto *d = static_cast<PngDiag *>(png_get_error_ptr(png)))
    std::snprintf(d->message, sizeof d->message, "%s", msg ? msg : "");
  png_longjmp(png, 1);
}
inline void pngWarningFn(png_structp, png_const_charp) {}
inline std::string pngMessage(const PngDiag &d, const char *fallback) {
  return d.message[0] ? std::string("PNG: ") + d.message
                      : std::string(fallback);
}

// PNG -> ImageBuffer. 16-bit files stay 16-bit (RGBA16_UNORM); everything
// else is delivered as RGBA8. `info` is the already-probed header.
inline std::optional<ImageBuffer> decodePngBuffer(const uint8_t *data,
                                                  size_t size,
                                                  const ImageInfo &info,
                                                  std::string *err) {
  const PixelFormat fmt = info.bufferFormat();
  const bool wide = fmt == PixelFormat::RGBA16_UNORM;

  // Everything with a destructor is declared BEFORE setjmp: libpng reports
  // errors by longjmp, which skips the destructors of anything created after
  // it (that would leak a whole picture).
  std::optional<ImageBuffer> out;
  std::vector<png_bytep> rows;
  PngDiag diag = {};

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &diag,
                                           pngErrorFn, pngWarningFn);
  png_infop pinfo = png ? png_create_info_struct(png) : nullptr;
  if (!png || !pinfo) {
    if (png)
      png_destroy_read_struct(&png, pinfo ? &pinfo : nullptr, nullptr);
    if (err)
      *err = "libpng init failed";
    return std::nullopt;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &pinfo, nullptr);
    if (err)
      *err = pngMessage(diag, "libpng decode error");
    return std::nullopt;
  }

  detail::PngMemReader reader{data, size};
  png_set_read_fn(png, &reader, detail::pngMemRead);
  png_read_info(png, pinfo);

  const png_uint_32 w = png_get_image_width(png, pinfo);
  const png_uint_32 h = png_get_image_height(png, pinfo);
  const int bitDepth = png_get_bit_depth(png, pinfo);
  const int colorType = png_get_color_type(png, pinfo);

  // Normalize every PNG variant to straight RGBA. Unlike decodeFile this does
  // NOT strip a 16-bit file down to 8 bits.
  if (colorType == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, pinfo, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_GRAY ||
      colorType == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png, wide ? 0xFFFF : 0xFF, PNG_FILLER_AFTER);
  if (colorType == PNG_COLOR_TYPE_GRAY ||
      colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  png_set_interlace_handling(png);
  png_read_update_info(png, pinfo);

  const size_t bpp = wide ? 8 : 4;
  bool good = w == info.width && h == info.height &&
              png_get_rowbytes(png, pinfo) == static_cast<size_t>(w) * bpp;
  if (good) {
    out = ImageBuffer::create(w, h, fmt);
    if (!out) {
      png_destroy_read_struct(&png, &pinfo, nullptr);
      if (err)
        *err = "out of memory";
      return std::nullopt;
    }
    rows.resize(h);
    for (png_uint_32 y = 0; y < h; ++y)
      rows[y] = reinterpret_cast<png_bytep>(out->row(y));
    png_read_image(png, rows.data());
  }
  png_destroy_read_struct(&png, &pinfo, nullptr);
  if (!good) {
    if (err)
      *err = "unexpected PNG layout";
    return std::nullopt;
  }

  if (wide) {
    // libpng hands 16-bit samples over big-endian; make them native. Written
    // as byte assembly + memcpy, so it is right on either endianness.
    const size_t samples = static_cast<size_t>(w) * 4;
    for (png_uint_32 y = 0; y < h; ++y) {
      uint8_t *p = reinterpret_cast<uint8_t *>(out->row(y));
      for (size_t i = 0; i < samples; ++i, p += 2) {
        const uint16_t v = static_cast<uint16_t>((p[0] << 8) | p[1]);
        std::memcpy(p, &v, 2);
      }
    }
  }
  return out;
}

// JPEG -> RGBA8 ImageBuffer (gray is replicated, alpha is opaque).
inline std::optional<ImageBuffer>
decodeJpegBuffer(const uint8_t *data, size_t size, std::string *err) {
  std::optional<ImageBuffer> out; // declared before setjmp, see decodePngBuffer
  std::vector<unsigned char> row;

  jpeg_decompress_struct cinfo;
  detail::JpegErrorMgr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = detail::jpegErrorExit;
  jerr.pub.output_message = [](j_common_ptr) {
  }; // warnings (corrupt data etc.) must not spam stderr
  if (setjmp(jerr.escape)) {
    jpeg_destroy_decompress(&cinfo);
    if (err)
      *err = jerr.message;
    return std::nullopt;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, const_cast<unsigned char *>(data),
               static_cast<unsigned long>(size));
  jpeg_read_header(&cinfo, TRUE);
  // Ask for gray or RGB explicitly. libjpeg cannot convert CMYK/YCCK to RGB;
  // such a file raises its "unsupported color conversion" error here.
  cinfo.out_color_space = cinfo.num_components == 1 ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_start_decompress(&cinfo);

  const int w = static_cast<int>(cinfo.output_width);
  const int h = static_cast<int>(cinfo.output_height);
  const int channels = cinfo.output_components; // 1 or 3
  out = ImageBuffer::create(static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                            PixelFormat::RGBA8_UNORM);
  if (!out) {
    jpeg_destroy_decompress(&cinfo);
    if (err)
      *err = "out of memory";
    return std::nullopt;
  }
  row.resize(static_cast<size_t>(w) * channels);
  unsigned char *rowPtr = row.data();
  while (cinfo.output_scanline < cinfo.output_height) {
    const int y = static_cast<int>(cinfo.output_scanline);
    jpeg_read_scanlines(&cinfo, &rowPtr, 1);
    uint8_t *dst =
        reinterpret_cast<uint8_t *>(out->row(static_cast<uint32_t>(y)));
    for (int x = 0; x < w; ++x) {
      if (channels == 1)
        dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = row[x];
      else {
        dst[x * 4 + 0] = row[x * 3 + 0];
        dst[x * 4 + 1] = row[x * 3 + 1];
        dst[x * 4 + 2] = row[x * 3 + 2];
      }
      dst[x * 4 + 3] = 255; // JPEG carries no alpha channel
    }
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return out;
}

inline std::optional<ImageBuffer> decodeBackend(const uint8_t *data,
                                                size_t size,
                                                const ImageInfo &info,
                                                std::string *err) {
  if (info.format == ImageFileFormat::Png)
    return decodePngBuffer(data, size, info, err);
  return decodeJpegBuffer(data, size, err);
}

// libpng write callback appending to a vector. An allocation failure must
// not throw through libpng's C frames, so it is turned into png_error.
struct PngMemWriter {
  std::vector<uint8_t> *out;
};
inline void pngMemWrite(png_structp png, png_bytep data, png_size_t n) {
  auto *w = static_cast<PngMemWriter *>(png_get_io_ptr(png));
  bool ok = true;
  try {
    w->out->insert(w->out->end(), data, data + n);
  } catch (...) {
    ok = false;
  }
  if (!ok)
    png_error(png, "liteui: out of memory");
}
inline void pngMemFlush(png_structp) {}

// Feeds the image to libpng row by row. Kept out of encodePngBackend so no
// local of the setjmp-ing function is live across a possible longjmp.
struct PngLayout {
  bool wide; // 16 bits per channel
  bool rgb;  // no alpha channel written
};
inline void writePngRows(png_structp png, const ImageBuffer &img,
                         const PngLayout &layout) {
  const bool wide = layout.wide, rgb = layout.rgb;
  const int channels = rgb ? 3 : 4;
  std::vector<uint8_t> rowBuf(static_cast<size_t>(img.width) * channels *
                              (wide ? 2 : 1));
  for (uint32_t y = 0; y < img.height; ++y) {
    const uint8_t *src = reinterpret_cast<const uint8_t *>(img.row(y));
    if (!wide) {
      if (!rgb)
        std::memcpy(rowBuf.data(), src, rowBuf.size());
      else
        for (uint32_t x = 0; x < img.width; ++x)
          std::memcpy(&rowBuf[static_cast<size_t>(x) * 3],
                      src + static_cast<size_t>(x) * 4, 3);
    } else {
      // native 16-bit samples -> big-endian bytes (PNG's byte order)
      uint8_t *d = rowBuf.data();
      for (uint32_t x = 0; x < img.width; ++x)
        for (int c = 0; c < channels; ++c, d += 2) {
          uint16_t v;
          std::memcpy(&v, src + (static_cast<size_t>(x) * 4 + c) * 2, 2);
          d[0] = static_cast<uint8_t>(v >> 8);
          d[1] = static_cast<uint8_t>(v & 0xFF);
        }
    }
    png_write_row(png, rowBuf.data());
  }
}

inline bool encodePngBackend(std::vector<uint8_t> &out, const ImageBuffer &img,
                             const PngEncodeOptions &opt, std::string *err) {
  // Held in a struct that is passed by reference (so it lives in memory, not
  // a register) because libpng's error path longjmps back into this function.
  const PngLayout layout{img.format == PixelFormat::RGBA16_UNORM,
                         opt.dropOpaqueAlpha && isOpaque(img)};

  PngDiag diag = {};
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &diag,
                                            pngErrorFn, pngWarningFn);
  png_infop pinfo = png ? png_create_info_struct(png) : nullptr;
  if (!png || !pinfo) {
    if (png)
      png_destroy_write_struct(&png, pinfo ? &pinfo : nullptr);
    if (err)
      *err = "libpng init failed";
    return false;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &pinfo);
    if (err)
      *err = pngMessage(diag, "libpng encode error");
    return false;
  }

  PngMemWriter writer{&out};
  png_set_write_fn(png, &writer, pngMemWrite, pngMemFlush);
  png_set_IHDR(png, pinfo, img.width, img.height, layout.wide ? 16 : 8,
               layout.rgb ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGB_ALPHA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_set_compression_level(png, std::clamp(opt.compression, 0, 9));
  png_write_info(png, pinfo);

  writePngRows(png, img, layout);
  png_write_end(png, pinfo);
  png_destroy_write_struct(&png, &pinfo);
  return true;
}

inline bool encodeJpegBackend(std::vector<uint8_t> &out, const ImageBuffer &img,
                              const JpegEncodeOptions &opt, std::string *err) {
  std::vector<unsigned char> rowBuf(static_cast<size_t>(img.width) * 3);
  unsigned char *buf =
      nullptr; // allocated by jpeg_mem_dest (malloc); freed below
  unsigned long bufSize = 0;

  jpeg_compress_struct cinfo;
  detail::JpegErrorMgr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = detail::jpegErrorExit;
  jerr.pub.output_message = [](j_common_ptr) {};
  if (setjmp(jerr.escape)) {
    jpeg_destroy_compress(&cinfo);
    std::free(buf);
    if (err)
      *err = jerr.message;
    return false;
  }

  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &buf, &bufSize);
  cinfo.image_width = img.width;
  cinfo.image_height = img.height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, opt.quality, TRUE);
  // After the defaults: luma sampled 2x2 against 1x1 chroma is 4:2:0, luma 1x1
  // is 4:4:4.
  const int lumaSampling = wantFullChroma(opt) ? 1 : 2;
  cinfo.comp_info[0].h_samp_factor = lumaSampling;
  cinfo.comp_info[0].v_samp_factor = lumaSampling;
  jpeg_start_compress(&cinfo, TRUE);

  unsigned char *rowPtr = rowBuf.data();
  for (uint32_t y = 0; y < img.height; ++y) {
    packRowRgb(reinterpret_cast<const uint8_t *>(img.row(y)), img.width,
               opt.alpha == JpegAlpha::FlattenOverWhite, /*bgr=*/false,
               rowBuf.data());
    jpeg_write_scanlines(&cinfo, &rowPtr, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  out.assign(buf, buf + bufSize);
  std::free(buf);
  return true;
}
} // namespace codec_detail

#endif

float srgbToLinear(float v) {
  const float a = std::fabs(v);
  const float r =
      a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
  return std::copysign(r, v);
}

float linearToSrgb(float v) {
  const float a = std::fabs(v);
  const float r =
      a <= 0.0031308f ? a * 12.92f : 1.055f * std::pow(a, 1.0f / 2.4f) - 0.055f;
  return std::copysign(r, v);
}

namespace detail {
// 8-bit sRGB code -> linear float, built once.
inline const float *srgbDecode8Table() {
  static const auto table = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i)
      t[i] = srgbToLinear(static_cast<float>(i) / 255.0f);
    return t;
  }();
  return table.data();
}

// 16-bit sRGB code -> linear float (256 KB), built once.
inline const float *srgbDecode16Table() {
  static const std::vector<float> table = [] {
    std::vector<float> t(65536);
    for (int i = 0; i < 65536; ++i)
      t[i] = srgbToLinear(static_cast<float>(i) / 65535.0f);
    return t;
  }();
  return table.data();
}

// half -> float for every possible 16-bit pattern (256 KB), built once;
// faster than the branchy halfToFloat in the per-pixel loops.
inline const float *halfDecodeTable() {
  static const std::vector<float> table = [] {
    std::vector<float> t(65536);
    for (uint32_t i = 0; i < 65536; ++i)
      t[i] = halfToFloat(static_cast<uint16_t>(i));
    return t;
  }();
  return table.data();
}

// linear 0..1 -> sRGB 0..1 by table lookup with linear interpolation.
// 16384 steps keep the error near 1e-6, far below half an LSB even of a
// 16-bit output, and replace a pow() per channel per pixel.
constexpr uint32_t kEncodeSteps = 16384;
inline const float *srgbEncodeTable() {
  static const std::vector<float> table = [] {
    std::vector<float> t(kEncodeSteps + 1);
    for (uint32_t i = 0; i <= kEncodeSteps; ++i)
      t[i] = linearToSrgb(static_cast<float>(i) /
                          static_cast<float>(kEncodeSteps));
    return t;
  }();
  return table.data();
}
// `v` must already be in 0..1 (see clamp01).
inline float encodeFast(const float *table, float v) {
  const float t = v * static_cast<float>(kEncodeSteps);
  const uint32_t i = static_cast<uint32_t>(t);
  if (i >= kEncodeSteps)
    return table[kEncodeSteps];
  const float f = t - static_cast<float>(i);
  return table[i] + (table[i + 1] - table[i]) * f;
}

// NaN -> 0, clamp to 0..1.
inline float clamp01(float v) {
  return (v != v) ? 0.0f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
}
inline uint8_t quantize8(float v) {
  return static_cast<uint8_t>(clamp01(v) * 255.0f + 0.5f);
}
inline uint16_t quantize16(float v) {
  return static_cast<uint16_t>(clamp01(v) * 65535.0f + 0.5f);
}

// Pixels processed per step by the row loops: float scratch space stays
// a fixed 16 KB whatever the image size.
constexpr uint32_t kChunk = 1024;

inline bool isUnorm(PixelFormat f) {
  return f == PixelFormat::RGBA8_UNORM || f == PixelFormat::RGBA16_UNORM;
}

// Loads n pixels of row y starting at column x0 as linear-light float
// RGBA. UNORM colour is sRGB-decoded; their alpha is just normalised.
// The caller has validated the buffer and the range.
inline void loadLinear(const ImageBuffer &b, uint32_t y, uint32_t x0,
                       uint32_t n, float *out) {
  const std::byte *p = b.pixels.data() + static_cast<size_t>(y) * b.rowStride +
                       static_cast<size_t>(x0) * bytesPerPixel(b.format);
  switch (b.format) {
  case PixelFormat::RGBA8_UNORM: {
    const float *lut = srgbDecode8Table();
    const uint8_t *q = reinterpret_cast<const uint8_t *>(p);
    for (uint32_t i = 0; i < n; ++i, q += 4, out += 4) {
      out[0] = lut[q[0]];
      out[1] = lut[q[1]];
      out[2] = lut[q[2]];
      out[3] = static_cast<float>(q[3]) / 255.0f;
    }
    break;
  }
  case PixelFormat::RGBA16_UNORM: {
    const float *lut = srgbDecode16Table();
    for (uint32_t i = 0; i < n; ++i, p += 8, out += 4) {
      uint16_t v[4];
      std::memcpy(v, p, sizeof v);
      out[0] = lut[v[0]];
      out[1] = lut[v[1]];
      out[2] = lut[v[2]];
      out[3] = static_cast<float>(v[3]) / 65535.0f;
    }
    break;
  }
  case PixelFormat::RGBA16_FLOAT: {
    const float *lut = halfDecodeTable();
    for (uint32_t i = 0; i < n; ++i, p += 8, out += 4) {
      uint16_t v[4];
      std::memcpy(v, p, sizeof v);
      out[0] = lut[v[0]];
      out[1] = lut[v[1]];
      out[2] = lut[v[2]];
      out[3] = lut[v[3]];
    }
    break;
  }
  case PixelFormat::RGBA32_FLOAT:
    std::memcpy(out, p, static_cast<size_t>(n) * 16);
    break;
  }
}

// Inverse of loadLinear: stores n linear float RGBA pixels. UNORM
// destinations sanitise (NaN -> 0), clamp, sRGB-encode and round.
inline void storeLinear(ImageBuffer &b, uint32_t y, uint32_t x0, uint32_t n,
                        const float *in) {
  std::byte *p = b.pixels.data() + static_cast<size_t>(y) * b.rowStride +
                 static_cast<size_t>(x0) * bytesPerPixel(b.format);
  switch (b.format) {
  case PixelFormat::RGBA8_UNORM: {
    const float *tab = srgbEncodeTable();
    uint8_t *q = reinterpret_cast<uint8_t *>(p);
    for (uint32_t i = 0; i < n; ++i, q += 4, in += 4) {
      q[0] = quantize8(encodeFast(tab, clamp01(in[0])));
      q[1] = quantize8(encodeFast(tab, clamp01(in[1])));
      q[2] = quantize8(encodeFast(tab, clamp01(in[2])));
      q[3] = quantize8(in[3]);
    }
    break;
  }
  case PixelFormat::RGBA16_UNORM: {
    const float *tab = srgbEncodeTable();
    for (uint32_t i = 0; i < n; ++i, p += 8, in += 4) {
      const uint16_t v[4] = {quantize16(encodeFast(tab, clamp01(in[0]))),
                             quantize16(encodeFast(tab, clamp01(in[1]))),
                             quantize16(encodeFast(tab, clamp01(in[2]))),
                             quantize16(in[3])};
      std::memcpy(p, v, sizeof v);
    }
    break;
  }
  case PixelFormat::RGBA16_FLOAT:
    for (uint32_t i = 0; i < n; ++i, p += 8, in += 4) {
      uint16_t v[4];
      for (int c = 0; c < 4; ++c)
        v[c] = floatToHalf(in[c]);
      std::memcpy(p, v, sizeof v);
    }
    break;
  case PixelFormat::RGBA32_FLOAT:
    std::memcpy(p, in, static_cast<size_t>(n) * 16);
    break;
  }
}

// RGBA8 display copy of `region` (already bounds-checked by the caller).
inline std::optional<ImageBuffer>
displayRegion(const ImageBuffer &src, const ImageRect &r,
              const DisplaySettings &settings) {
  auto out = ImageBuffer::create(r.width, r.height, PixelFormat::RGBA8_UNORM);
  if (!out || out->empty())
    return out;
  const size_t bpp = bytesPerPixel(src.format);

  if (src.format == PixelFormat::RGBA8_UNORM) {
    for (uint32_t y = 0; y < r.height; ++y)
      std::memcpy(out->pixels.data() + static_cast<size_t>(y) * out->rowStride,
                  src.pixels.data() +
                      static_cast<size_t>(r.y + y) * src.rowStride +
                      static_cast<size_t>(r.x) * bpp,
                  static_cast<size_t>(r.width) * 4);
    return out;
  }
  if (src.format == PixelFormat::RGBA16_UNORM) {
    for (uint32_t y = 0; y < r.height; ++y) {
      const std::byte *s = src.pixels.data() +
                           static_cast<size_t>(r.y + y) * src.rowStride +
                           static_cast<size_t>(r.x) * bpp;
      uint8_t *d = reinterpret_cast<uint8_t *>(out->pixels.data()) +
                   static_cast<size_t>(y) * out->rowStride;
      for (size_t i = 0; i < static_cast<size_t>(r.width) * 4; ++i) {
        uint16_t v;
        std::memcpy(&v, s + i * 2, 2);
        d[i] = static_cast<uint8_t>((v + 128u) / 257u);
      }
    }
    return out;
  }

  // Float sources (linear light): exposure, clamp mode, sRGB, quantise.
  float ev = settings.exposureEv;
  if (!std::isfinite(ev))
    ev = 0.0f;
  const float gain = std::exp2(std::clamp(ev, -32.0f, 32.0f));
  const bool keepHue = settings.clamp == ClampMode::PreserveHue;
  const float *tab = srgbEncodeTable();
  float buf[kChunk * 4];

  for (uint32_t y = 0; y < r.height; ++y) {
    uint8_t *d = reinterpret_cast<uint8_t *>(out->pixels.data()) +
                 static_cast<size_t>(y) * out->rowStride;
    for (uint32_t x0 = 0; x0 < r.width; x0 += kChunk) {
      const uint32_t n = std::min(kChunk, r.width - x0);
      loadLinear(src, r.y + y, r.x + x0, n, buf);
      for (uint32_t i = 0; i < n; ++i) {
        const float *px = buf + static_cast<size_t>(i) * 4;
        float rgb[3];
        for (int c = 0; c < 3; ++c) {
          const float v = px[c] * gain;
          rgb[c] = (v != v) ? 0.0f : v; // NaN -> 0
        }
        if (keepHue) {
          const float m = std::max({rgb[0], rgb[1], rgb[2]});
          if (std::isinf(m)) {
            for (float &c : rgb) // only the infinite channels count
              c = std::isinf(c) && c > 0.0f ? 1.0f : 0.0f;
          } else if (m > 1.0f) {
            const float inv = 1.0f / m;
            for (float &c : rgb)
              c *= inv;
          }
        }
        uint8_t *o = d + static_cast<size_t>(x0 + i) * 4;
        o[0] = quantize8(encodeFast(tab, clamp01(rgb[0])));
        o[1] = quantize8(encodeFast(tab, clamp01(rgb[1])));
        o[2] = quantize8(encodeFast(tab, clamp01(rgb[2])));
        o[3] = quantize8(px[3]);
      }
    }
  }
  return out;
}
} // namespace detail

bool convertImage(const ImageBuffer &src, ImageBuffer &dst) {
  if (!src.valid() || !isStorageSupported(src.format) ||
      !isStorageSupported(dst.format))
    return false;
  const PixelFormat to = dst.format;
  if (&src == &dst)
    return true; // dst.format == src.format here: nothing to do

  auto out = ImageBuffer::create(src.width, src.height, to);
  if (!out)
    return false;
  if (out->empty()) {
    dst = std::move(*out);
    return true;
  }

  const bool rescale =
      src.format != to && detail::isUnorm(src.format) && detail::isUnorm(to);
  const size_t rowBytes = *minimumRowBytes(src.width, to);
  float buf[detail::kChunk * 4];
  for (uint32_t y = 0; y < src.height; ++y) {
    const std::byte *s =
        src.pixels.data() + static_cast<size_t>(y) * src.rowStride;
    std::byte *d = out->pixels.data() + static_cast<size_t>(y) * out->rowStride;
    if (src.format == to) {
      std::memcpy(d, s, rowBytes);
    } else if (rescale) {
      const size_t count = static_cast<size_t>(src.width) * 4;
      if (to == PixelFormat::RGBA16_UNORM) // 8 -> 16: exact x257
      {
        for (size_t i = 0; i < count; ++i) {
          const uint16_t v =
              static_cast<uint16_t>(static_cast<uint8_t>(s[i]) * 257u);
          std::memcpy(d + i * 2, &v, 2);
        }
      } else // 16 -> 8: round to nearest
      {
        for (size_t i = 0; i < count; ++i) {
          uint16_t v;
          std::memcpy(&v, s + i * 2, 2);
          d[i] = static_cast<std::byte>((v + 128u) / 257u);
        }
      }
    } else {
      for (uint32_t x0 = 0; x0 < src.width; x0 += detail::kChunk) {
        const uint32_t n = std::min(detail::kChunk, src.width - x0);
        detail::loadLinear(src, y, x0, n, buf);
        detail::storeLinear(*out, y, x0, n, buf);
      }
    }
  }
  dst = std::move(*out);
  return true;
}

std::optional<ImageBuffer> makeDisplayImage(const ImageBuffer &src,
                                            const DisplaySettings &settings,
                                            const ImageRect &r) {
  if (!src.valid())
    return std::nullopt;
  if (r.x > src.width || r.width > src.width - r.x || r.y > src.height ||
      r.height > src.height - r.y)
    return std::nullopt;
  return detail::displayRegion(src, r, settings);
}

std::optional<ImageBuffer> makeDisplayImage(const ImageBuffer &src,
                                            const DisplaySettings &settings) {
  return makeDisplayImage(src, settings,
                          ImageRect{0, 0, src.width, src.height});
}

std::optional<ImageBuffer> downscaleImage(const ImageBuffer &src,
                                          uint32_t factor) {
  if (!src.valid() || factor == 0)
    return std::nullopt;
  if (src.empty() || factor == 1) {
    ImageBuffer out;
    out.format = src.format;
    if (!convertImage(src, out))
      return std::nullopt;
    return out;
  }
  const uint32_t nw =
      static_cast<uint32_t>((uint64_t{src.width} + factor - 1) / factor);
  const uint32_t nh =
      static_cast<uint32_t>((uint64_t{src.height} + factor - 1) / factor);
  auto out = ImageBuffer::create(nw, nh, src.format);
  if (!out)
    return std::nullopt;

  std::vector<double> acc(static_cast<size_t>(nw) * 4);
  std::vector<float> outRow(static_cast<size_t>(nw) * 4);
  float buf[detail::kChunk * 4];
  for (uint32_t oy = 0; oy < nh; ++oy) {
    std::fill(acc.begin(), acc.end(), 0.0);
    const uint32_t sy0 = oy * factor;
    const uint32_t sy1 = static_cast<uint32_t>(
        std::min<uint64_t>(src.height, uint64_t{sy0} + factor));
    for (uint32_t sy = sy0; sy < sy1; ++sy)
      for (uint32_t x0 = 0; x0 < src.width; x0 += detail::kChunk) {
        const uint32_t n = std::min(detail::kChunk, src.width - x0);
        detail::loadLinear(src, sy, x0, n, buf);
        for (uint32_t i = 0; i < n; ++i) {
          const float *px = buf + static_cast<size_t>(i) * 4;
          double *a = acc.data() + static_cast<size_t>((x0 + i) / factor) * 4;
          const double w = px[3]; // alpha weights colour
          a[0] += px[0] * w;
          a[1] += px[1] * w;
          a[2] += px[2] * w;
          a[3] += w;
        }
      }
    for (uint32_t ox = 0; ox < nw; ++ox) {
      const uint64_t cols =
          std::min<uint64_t>(src.width, uint64_t{ox} * factor + factor) -
          uint64_t{ox} * factor;
      const double count = static_cast<double>(cols * (sy1 - sy0));
      const double *a = acc.data() + static_cast<size_t>(ox) * 4;
      float *o = outRow.data() + static_cast<size_t>(ox) * 4;
      for (int c = 0; c < 3; ++c)
        o[c] = a[3] > 0.0 ? static_cast<float>(a[c] / a[3]) : 0.0f;
      o[3] = static_cast<float>(a[3] / count);
    }
    for (uint32_t x0 = 0; x0 < nw; x0 += detail::kChunk)
      detail::storeLinear(*out, oy, x0, std::min(detail::kChunk, nw - x0),
                          outRow.data() + static_cast<size_t>(x0) * 4);
  }
  return out;
}

std::optional<CanvasImage> toCanvasImage(const ImageBuffer &src) {
  return toCanvasImage(src, DisplaySettings{});
}

std::optional<CanvasImage> toCanvasImage(const ImageBuffer &src,
                                         const DisplaySettings &settings) {
  constexpr uint32_t kMaxDim =
      static_cast<uint32_t>(std::numeric_limits<int>::max());
  if (src.width > kMaxDim || src.height > kMaxDim)
    return std::nullopt;
  auto disp = makeDisplayImage(src, settings);
  if (!disp)
    return std::nullopt;
  ImageBuffer &rgba8 = *disp;
  CanvasImage img;
  img.width = static_cast<int>(rgba8.width);
  img.height = static_cast<int>(rgba8.height);
  img.pixels.resize(rgba8.pixels.size());
  if (!rgba8.pixels.empty())
    std::memcpy(img.pixels.data(), rgba8.pixels.data(), rgba8.pixels.size());
  return img;
}

} // namespace liteui_image

// ---------------- Minimal XML parser (general-purpose subset) -----------
// Supports: elements, attributes, nested content, comments, PIs, DOCTYPE
// (with bracketed internal subset), CDATA sections, named + numeric
// character references, and concatenated text content per element.
// Still not a full XML implementation: no external entities, no DTD
// validation, no namespace resolution (prefixes are kept as plain name
// text, e.g. "xlink:href"), and mixed content order (interleaved text vs.
// child elements) is not preserved — all text at a given level is
// concatenated into that node's `text`, in document order, regardless of
// where child elements fall between the runs. That's sufficient for
// SVG (which never relies on interleaving) and most simple config/markup
// use; a caller needing faithful mixed-content order would need a
// different Node design (e.g. a single ordered vector of variant
// text/element children).
namespace liteui_xml {
struct Node {
  std::string tag;
  std::unordered_map<std::string, std::string> attrs;
  std::vector<Node> children;
  std::string text; // concatenated character data (CDATA + decoded entities)
};

inline void skipWs(const std::string &s, size_t &i) {
  while (i < s.size() && (unsigned char)s[i] <= ' ')
    ++i;
}

inline void appendUtf8(std::string &out, unsigned long cp) {
  if (cp <= 0x7F) {
    out += (char)cp;
  } else if (cp <= 0x7FF) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

inline std::string decodeEntities(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in[i] == '&') {
      size_t semi = in.find(';', i);
      if (semi != std::string::npos && semi - i <= 12) {
        std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") {
          out += '&';
          i = semi + 1;
          continue;
        }
        if (ent == "lt") {
          out += '<';
          i = semi + 1;
          continue;
        }
        if (ent == "gt") {
          out += '>';
          i = semi + 1;
          continue;
        }
        if (ent == "quot") {
          out += '"';
          i = semi + 1;
          continue;
        }
        if (ent == "apos") {
          out += '\'';
          i = semi + 1;
          continue;
        }
        if (ent.size() > 1 && ent[0] == '#') {
          bool hex = ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X');
          const char *numStart = ent.c_str() + (hex ? 2 : 1);
          char *end = nullptr;
          unsigned long cp = std::strtoul(numStart, &end, hex ? 16 : 10);
          if (end && *end == '\0') {
            appendUtf8(out, cp);
            i = semi + 1;
            continue;
          }
        }
      }
    }
    out += in[i++];
  }
  return out;
}

inline std::string parseAttrValue(const std::string &s, size_t &i) {
  char q = s[i++];
  size_t start = i;
  while (i < s.size() && s[i] != q)
    ++i;
  std::string raw = s.substr(start, i - start);
  if (i < s.size())
    ++i;
  return decodeEntities(raw);
}

inline void parseAttrs(const std::string &s, size_t &i, Node &node) {
  while (true) {
    skipWs(s, i);
    if (i >= s.size() || s[i] == '>' || s[i] == '/' || s[i] == '?')
      break;
    size_t nameStart = i;
    while (i < s.size() && s[i] != '=' && (unsigned char)s[i] > ' ' &&
           s[i] != '>' && s[i] != '/')
      ++i;
    std::string name = s.substr(nameStart, i - nameStart);
    skipWs(s, i);
    std::string value;
    if (i < s.size() && s[i] == '=') {
      ++i;
      skipWs(s, i);
      if (i < s.size() && (s[i] == '"' || s[i] == '\''))
        value = parseAttrValue(s, i);
      else {
        // Unquoted attribute value (invalid XML, but tolerate it — common
        // in hand-written or HTML-ish markup).
        size_t vs = i;
        while (i < s.size() && (unsigned char)s[i] > ' ' && s[i] != '>' &&
               s[i] != '/')
          ++i;
        value = decodeEntities(s.substr(vs, i - vs));
      }
    }
    if (!name.empty())
      node.attrs[name] = value;
  }
}

// Skips a comment, processing instruction, or DOCTYPE (including a
// bracketed internal subset, which may itself contain '>' characters
// before the subset's closing ']'). Returns true if it consumed something.
inline bool skipMisc(const std::string &s, size_t &i) {
  if (s.compare(i, 4, "<!--") == 0) {
    size_t end = s.find("-->", i);
    i = (end == std::string::npos) ? s.size() : end + 3;
    return true;
  }
  if (s.compare(i, 2, "<?") == 0) {
    size_t end = s.find("?>", i);
    i = (end == std::string::npos) ? s.size() : end + 2;
    return true;
  }
  if (s.compare(i, 9, "<![CDATA[") != 0 && s.compare(i, 2, "<!") == 0) {
    size_t j = i + 2;
    int depth = 0;
    while (j < s.size()) {
      if (s[j] == '[')
        ++depth;
      else if (s[j] == ']')
        --depth;
      else if (s[j] == '>' && depth <= 0) {
        ++j;
        break;
      }
      ++j;
    }
    i = j;
    return true;
  }
  return false;
}

inline bool parseElement(const std::string &s, size_t &i, Node &out) {
  skipWs(s, i);
  while (i < s.size() && s[i] == '<' && skipMisc(s, i))
    skipWs(s, i);
  if (i >= s.size() || s[i] != '<')
    return false;
  ++i;
  size_t nameStart = i;
  while (i < s.size() && (unsigned char)s[i] > ' ' && s[i] != '>' &&
         s[i] != '/')
    ++i;
  out.tag = s.substr(nameStart, i - nameStart);
  parseAttrs(s, i, out);
  skipWs(s, i);
  if (i < s.size() && s[i] == '/') {
    ++i;
    if (i < s.size() && s[i] == '>')
      ++i;
    return true;
  }
  if (i < s.size() && s[i] == '>')
    ++i;
  while (true) {
    if (i >= s.size())
      break;
    if (s.compare(i, 2, "</") == 0) {
      size_t end = s.find('>', i);
      i = (end == std::string::npos) ? s.size() : end + 1;
      break;
    }
    if (s.compare(i, 9, "<![CDATA[") == 0) {
      size_t start = i + 9;
      size_t end = s.find("]]>", start);
      size_t stop = (end == std::string::npos) ? s.size() : end;
      out.text += s.substr(start, stop - start); // raw, no entity decoding
      i = (end == std::string::npos) ? s.size() : end + 3;
      continue;
    }
    if (s[i] == '<') {
      if (skipMisc(s, i))
        continue;
      Node child;
      if (!parseElement(s, i, child))
        break;
      out.children.push_back(std::move(child));
      continue;
    }
    size_t next = s.find('<', i);
    size_t stop = (next == std::string::npos) ? s.size() : next;
    out.text += decodeEntities(s.substr(i, stop - i));
    i = stop;
  }
  return true;
}

inline std::optional<Node> parse(const std::string &xml) {
  size_t i = 0;
  Node root;
  if (!parseElement(xml, i, root))
    return std::nullopt;
  return root;
}
} // namespace liteui_xml

namespace liteui_svg {

struct Mat2x3 {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  static Mat2x3 identity() { return {}; }
  static Mat2x3 translate(float tx, float ty) { return {1, 0, 0, 1, tx, ty}; }
  static Mat2x3 scaleM(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
  static Mat2x3 rotate(float rad) {
    float c = std::cos(rad), s = std::sin(rad);
    return {c, s, -s, c, 0, 0};
  }
  static Mat2x3 skewX(float rad) { return {1, 0, std::tan(rad), 1, 0, 0}; }
  static Mat2x3 skewY(float rad) { return {1, std::tan(rad), 0, 1, 0, 0}; }
  Mat2x3 multiply(const Mat2x3 &r) const {
    return {a * r.a + c * r.b, b * r.a + d * r.b,     a * r.c + c * r.d,
            b * r.c + d * r.d, a * r.e + c * r.f + e, b * r.e + d * r.f + f};
  }
};

inline void skipSep(const std::string &s, size_t &i) {
  while (i < s.size() && (s[i] == ',' || (unsigned char)s[i] <= ' '))
    ++i;
}
inline bool scanNumber(const std::string &s, size_t &i, float &out) {
  skipSep(s, i);
  size_t start = i;
  if (i < s.size() && (s[i] == '+' || s[i] == '-'))
    ++i;
  bool any = false;
  while (i < s.size() && isdigit((unsigned char)s[i])) {
    ++i;
    any = true;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && isdigit((unsigned char)s[i])) {
      ++i;
      any = true;
    }
  }
  if (!any) {
    i = start;
    return false;
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    size_t save = i;
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
      ++i;
    bool expDigits = false;
    while (i < s.size() && isdigit((unsigned char)s[i])) {
      ++i;
      expDigits = true;
    }
    if (!expDigits)
      i = save;
  }
  out = std::strtof(s.c_str() + start, nullptr);
  return true;
}

inline Mat2x3 parseTransform(const std::string &str) {
  Mat2x3 m = Mat2x3::identity();
  size_t i = 0;
  while (i < str.size()) {
    skipSep(str, i);
    size_t nameStart = i;
    while (i < str.size() && isalpha((unsigned char)str[i]))
      ++i;
    std::string fn = str.substr(nameStart, i - nameStart);
    if (fn.empty())
      break;
    skipSep(str, i);
    if (i >= str.size() || str[i] != '(')
      break;
    ++i;
    std::vector<float> args;
    float v;
    while (scanNumber(str, i, v))
      args.push_back(v);
    skipSep(str, i);
    if (i < str.size() && str[i] == ')')
      ++i;
    constexpr float kDeg2Rad = 3.14159265358979f / 180.0f;
    if (fn == "translate" && args.size() >= 1)
      m = m.multiply(Mat2x3::translate(args[0], args.size() > 1 ? args[1] : 0));
    else if (fn == "scale" && args.size() >= 1)
      m = m.multiply(
          Mat2x3::scaleM(args[0], args.size() > 1 ? args[1] : args[0]));
    else if (fn == "rotate" && args.size() >= 1) {
      float rad = args[0] * kDeg2Rad;
      if (args.size() >= 3) {
        m = m.multiply(Mat2x3::translate(args[1], args[2]));
        m = m.multiply(Mat2x3::rotate(rad));
        m = m.multiply(Mat2x3::translate(-args[1], -args[2]));
      } else
        m = m.multiply(Mat2x3::rotate(rad));
    } else if (fn == "skewX" && args.size() >= 1)
      m = m.multiply(Mat2x3::skewX(args[0] * kDeg2Rad));
    else if (fn == "skewY" && args.size() >= 1)
      m = m.multiply(Mat2x3::skewY(args[0] * kDeg2Rad));
    else if (fn == "matrix" && args.size() >= 6)
      m = m.multiply(
          Mat2x3{args[0], args[1], args[2], args[3], args[4], args[5]});
  }
  return m;
}

inline bool parseHexColor(const std::string &s, Color &out) {
  auto hex1 = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  if (s.size() == 4) {
    int r = hex1(s[1]), g = hex1(s[2]), b = hex1(s[3]);
    if (r < 0 || g < 0 || b < 0)
      return false;
    out = {(uint8_t)(r * 17), (uint8_t)(g * 17), (uint8_t)(b * 17), 255};
    return true;
  }
  if (s.size() == 7 || s.size() == 9) {
    int vals[4] = {255, 255, 255, 255};
    for (size_t k = 0; k * 2 + 1 < s.size() - 1; ++k) {
      int hi = hex1(s[1 + k * 2]), lo = hex1(s[2 + k * 2]);
      if (hi < 0 || lo < 0)
        return false;
      vals[k] = hi * 16 + lo;
    }
    out = {(uint8_t)vals[0], (uint8_t)vals[1], (uint8_t)vals[2],
           (uint8_t)vals[3]};
    return true;
  }
  return false;
}

// Extend as needed — small deliberately.
inline bool namedColor(const std::string &name, Color &out) {
  static const std::unordered_map<std::string, Color> kNames = {
      {"black", {0, 0, 0, 255}},      {"white", {255, 255, 255, 255}},
      {"red", {255, 0, 0, 255}},      {"green", {0, 128, 0, 255}},
      {"blue", {0, 0, 255, 255}},     {"yellow", {255, 255, 0, 255}},
      {"gray", {128, 128, 128, 255}}, {"grey", {128, 128, 128, 255}},
      {"orange", {255, 165, 0, 255}}, {"purple", {128, 0, 128, 255}},
      {"cyan", {0, 255, 255, 255}},   {"magenta", {255, 0, 255, 255}},
      {"lime", {0, 255, 0, 255}},     {"navy", {0, 0, 128, 255}},
      {"transparent", {0, 0, 0, 0}},
  };
  auto it = kNames.find(name);
  if (it == kNames.end())
    return false;
  out = it->second;
  return true;
}

inline bool parsePaint(const std::string &raw, Color &out) {
  std::string s = raw;
  size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  if (b == std::string::npos)
    return false;
  s = s.substr(b, e - b + 1);
  if (s == "none" || s.empty())
    return false;
  if (s[0] == '#')
    return parseHexColor(s, out);
  if (s.compare(0, 4, "rgb(") == 0 || s.compare(0, 5, "rgba(") == 0) {
    size_t p = s.find('(');
    std::string inner = s.substr(p + 1, s.size() - p - 2);
    std::vector<float> vals;
    size_t i = 0;
    float v;
    while (scanNumber(inner, i, v)) {
      skipSep(inner, i);
      if (i < inner.size() && inner[i] == '%') {
        v = v * 255.0f / 100.0f;
        ++i;
      }
      vals.push_back(v);
      skipSep(inner, i);
    }
    if (vals.size() < 3)
      return false;
    out = {(uint8_t)std::clamp(vals[0], 0.0f, 255.0f),
           (uint8_t)std::clamp(vals[1], 0.0f, 255.0f),
           (uint8_t)std::clamp(vals[2], 0.0f, 255.0f),
           (uint8_t)std::clamp(vals.size() > 3 ? vals[3] * 255.0f : 255.0f,
                               0.0f, 255.0f)};
    return true;
  }
  return namedColor(s, out);
}

inline float parseOpacity(const std::string &s) {
  if (s.empty())
    return 1.0f;
  float v = std::strtof(s.c_str(), nullptr);
  if (s.back() == '%')
    v /= 100.0f;
  return std::clamp(v, 0.0f, 1.0f);
}

struct PathOp {
  enum class Kind { Move, Line, Cubic, Quad, Close } kind;
  float x = 0, y = 0;
  float c1x = 0, c1y = 0, c2x = 0, c2y = 0; // Cubic
  float qx = 0, qy = 0;                     // Quad control point
};

// SVG-1.1 Appendix-F endpoint->center arc conversion, then split into
// <=90-degree cubic segments (same kappa approach CanvasContext's own
// appendArc uses for arc()/ellipse()) — kept independent of that internal
// helper since path parsing only talks to CanvasContext's public API.
inline void arcToCubics(std::vector<PathOp> &out, float x0, float y0, float rx,
                        float ry, float xAxisRotDeg, bool largeArc, bool sweep,
                        float x1, float y1) {
  if (rx == 0 || ry == 0) {
    out.push_back({PathOp::Kind::Line, x1, y1});
    return;
  }
  rx = std::abs(rx);
  ry = std::abs(ry);
  float phi = xAxisRotDeg * 3.14159265358979f / 180.0f;
  float cosPhi = std::cos(phi), sinPhi = std::sin(phi);
  float dx2 = (x0 - x1) / 2.0f, dy2 = (y0 - y1) / 2.0f;
  float x1p = cosPhi * dx2 + sinPhi * dy2, y1p = -sinPhi * dx2 + cosPhi * dy2;
  float rxsq = rx * rx, rysq = ry * ry, x1psq = x1p * x1p, y1psq = y1p * y1p;
  float lambda = x1psq / rxsq + y1psq / rysq;
  if (lambda > 1.0f) {
    float s = std::sqrt(lambda);
    rx *= s;
    ry *= s;
    rxsq = rx * rx;
    rysq = ry * ry;
  }
  float sign = (largeArc != sweep) ? 1.0f : -1.0f;
  float num = rxsq * rysq - rxsq * y1psq - rysq * x1psq;
  float den = rxsq * y1psq + rysq * x1psq;
  float coef = den > 1e-9f ? sign * std::sqrt(std::max(0.0f, num / den)) : 0.0f;
  float cxp = coef * (rx * y1p / ry), cyp = coef * -(ry * x1p / rx);
  float cx = cosPhi * cxp - sinPhi * cyp + (x0 + x1) / 2.0f;
  float cy = sinPhi * cxp + cosPhi * cyp + (y0 + y1) / 2.0f;
  auto angle = [](float ux, float uy, float vx, float vy) {
    float dot = ux * vx + uy * vy,
          len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    float a = std::acos(std::clamp(dot / len, -1.0f, 1.0f));
    return (ux * vy - uy * vx) < 0 ? -a : a;
  };
  float theta1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
  float dtheta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx,
                       (-y1p - cyp) / ry);
  if (!sweep && dtheta > 0)
    dtheta -= 6.28318530718f;
  if (sweep && dtheta < 0)
    dtheta += 6.28318530718f;
  int segCount =
      std::max(1, (int)std::ceil(std::abs(dtheta) / (3.14159265f / 2)));
  float segAngle = dtheta / segCount, t = theta1;
  for (int i = 0; i < segCount; ++i) {
    float t2 = t + segAngle;
    float alpha = std::sin(segAngle) *
                  (std::sqrt(4.0f + 3.0f * std::tan(segAngle / 4.0f) *
                                        std::tan(segAngle / 4.0f)) -
                   1.0f) /
                  3.0f;
    auto ellipsePt = [&](float ang, float &px, float &py) {
      float ex = rx * std::cos(ang), ey = ry * std::sin(ang);
      px = cx + ex * cosPhi - ey * sinPhi;
      py = cy + ex * sinPhi + ey * cosPhi;
    };
    auto ellipseDeriv = [&](float ang, float &dx, float &dy) {
      float ex = -rx * std::sin(ang), ey = ry * std::cos(ang);
      dx = ex * cosPhi - ey * sinPhi;
      dy = ex * sinPhi + ey * cosPhi;
    };
    float p1x, p1y, p2x, p2y, d1x, d1y, d2x, d2y;
    ellipsePt(t, p1x, p1y);
    ellipsePt(t2, p2x, p2y);
    ellipseDeriv(t, d1x, d1y);
    ellipseDeriv(t2, d2x, d2y);
    out.push_back({PathOp::Kind::Cubic, p2x, p2y, p1x + alpha * d1x,
                   p1y + alpha * d1y, p2x - alpha * d2x, p2y - alpha * d2y});
    t = t2;
  }
}

inline std::vector<PathOp> parsePathData(const std::string &d) {
  std::vector<PathOp> ops;
  size_t i = 0;
  float cx = 0, cy = 0, startX = 0, startY = 0, lastCtrlX = 0, lastCtrlY = 0;
  char lastCmd = 0;
  auto isCmd = [](char c) {
    return std::string("MmLlHhVvCcSsQqTtAaZz").find(c) != std::string::npos;
  };
  while (i < d.size()) {
    skipSep(d, i);
    if (i >= d.size())
      break;
    char cmd = d[i];
    if (isCmd(cmd))
      ++i;
    else {
      cmd = lastCmd;
      if (cmd == 0)
        break;
      if (cmd == 'M')
        cmd = 'L';
      if (cmd == 'm')
        cmd = 'l';
    }
    bool rel = islower((unsigned char)cmd);
    char up = (char)toupper(cmd);
    float n[7];
    auto need = [&](int count) {
      for (int k = 0; k < count; ++k)
        if (!scanNumber(d, i, n[k]))
          return false;
      return true;
    };
    switch (up) {
    case 'M':
      if (!need(2)) {
        i = d.size();
        break;
      }
      cx = rel ? cx + n[0] : n[0];
      cy = rel ? cy + n[1] : n[1];
      startX = cx;
      startY = cy;
      ops.push_back({PathOp::Kind::Move, cx, cy});
      break;
    case 'L':
      if (!need(2)) {
        i = d.size();
        break;
      }
      cx = rel ? cx + n[0] : n[0];
      cy = rel ? cy + n[1] : n[1];
      ops.push_back({PathOp::Kind::Line, cx, cy});
      break;
    case 'H':
      if (!need(1)) {
        i = d.size();
        break;
      }
      cx = rel ? cx + n[0] : n[0];
      ops.push_back({PathOp::Kind::Line, cx, cy});
      break;
    case 'V':
      if (!need(1)) {
        i = d.size();
        break;
      }
      cy = rel ? cy + n[0] : n[0];
      ops.push_back({PathOp::Kind::Line, cx, cy});
      break;
    case 'C': {
      if (!need(6)) {
        i = d.size();
        break;
      }
      float c1x = rel ? cx + n[0] : n[0], c1y = rel ? cy + n[1] : n[1],
            c2x = rel ? cx + n[2] : n[2], c2y = rel ? cy + n[3] : n[3];
      float ex = rel ? cx + n[4] : n[4], ey = rel ? cy + n[5] : n[5];
      ops.push_back({PathOp::Kind::Cubic, ex, ey, c1x, c1y, c2x, c2y});
      lastCtrlX = c2x;
      lastCtrlY = c2y;
      cx = ex;
      cy = ey;
      break;
    }
    case 'S': {
      if (!need(4)) {
        i = d.size();
        break;
      }
      bool prevCubic =
          lastCmd == 'C' || lastCmd == 'c' || lastCmd == 'S' || lastCmd == 's';
      float c1x = prevCubic ? 2 * cx - lastCtrlX : cx,
            c1y = prevCubic ? 2 * cy - lastCtrlY : cy;
      float c2x = rel ? cx + n[0] : n[0], c2y = rel ? cy + n[1] : n[1];
      float ex = rel ? cx + n[2] : n[2], ey = rel ? cy + n[3] : n[3];
      ops.push_back({PathOp::Kind::Cubic, ex, ey, c1x, c1y, c2x, c2y});
      lastCtrlX = c2x;
      lastCtrlY = c2y;
      cx = ex;
      cy = ey;
      break;
    }
    case 'Q': {
      if (!need(4)) {
        i = d.size();
        break;
      }
      float qx = rel ? cx + n[0] : n[0], qy = rel ? cy + n[1] : n[1];
      float ex = rel ? cx + n[2] : n[2], ey = rel ? cy + n[3] : n[3];
      ops.push_back({PathOp::Kind::Quad, ex, ey, 0, 0, 0, 0, qx, qy});
      lastCtrlX = qx;
      lastCtrlY = qy;
      cx = ex;
      cy = ey;
      break;
    }
    case 'T': {
      if (!need(2)) {
        i = d.size();
        break;
      }
      bool prevQuad =
          lastCmd == 'Q' || lastCmd == 'q' || lastCmd == 'T' || lastCmd == 't';
      float qx = prevQuad ? 2 * cx - lastCtrlX : cx,
            qy = prevQuad ? 2 * cy - lastCtrlY : cy;
      float ex = rel ? cx + n[0] : n[0], ey = rel ? cy + n[1] : n[1];
      ops.push_back({PathOp::Kind::Quad, ex, ey, 0, 0, 0, 0, qx, qy});
      lastCtrlX = qx;
      lastCtrlY = qy;
      cx = ex;
      cy = ey;
      break;
    }
    case 'A': {
      if (!need(7)) {
        i = d.size();
        break;
      }
      float ex = rel ? cx + n[5] : n[5], ey = rel ? cy + n[6] : n[6];
      arcToCubics(ops, cx, cy, n[0], n[1], n[2], n[3] != 0, n[4] != 0, ex, ey);
      cx = ex;
      cy = ey;
      break;
    }
    case 'Z':
      ops.push_back({PathOp::Kind::Close});
      cx = startX;
      cy = startY;
      break;
    default:
      i = d.size();
      break;
    }
    lastCmd = cmd;
  }
  return ops;
}

inline std::vector<PathOp> rectToOps(float x, float y, float w, float h,
                                     float rx, float ry) {
  std::vector<PathOp> ops;
  if (w <= 0 || h <= 0)
    return ops;
  rx = std::clamp(rx, 0.0f, w / 2.0f);
  ry = std::clamp(ry, 0.0f, h / 2.0f);
  if (rx <= 0 || ry <= 0) {
    ops = {{PathOp::Kind::Move, x, y},
           {PathOp::Kind::Line, x + w, y},
           {PathOp::Kind::Line, x + w, y + h},
           {PathOp::Kind::Line, x, y + h},
           {PathOp::Kind::Close}};
    return ops;
  }
  constexpr float k = 0.5522847498f;
  ops.push_back({PathOp::Kind::Move, x + rx, y});
  ops.push_back({PathOp::Kind::Line, x + w - rx, y});
  ops.push_back({PathOp::Kind::Cubic, x + w, y + ry, x + w - rx + k * rx, y,
                 x + w, y + ry - k * ry});
  ops.push_back({PathOp::Kind::Line, x + w, y + h - ry});
  ops.push_back({PathOp::Kind::Cubic, x + w - rx, y + h, x + w,
                 y + h - ry + k * ry, x + w - rx + k * rx, y + h});
  ops.push_back({PathOp::Kind::Line, x + rx, y + h});
  ops.push_back({PathOp::Kind::Cubic, x, y + h - ry, x + rx - k * rx, y + h, x,
                 y + h - ry + k * ry});
  ops.push_back({PathOp::Kind::Line, x, y + ry});
  ops.push_back(
      {PathOp::Kind::Cubic, x + rx, y, x, y + ry - k * ry, x + rx - k * rx, y});
  ops.push_back({PathOp::Kind::Close});
  return ops;
}

inline std::vector<PathOp> ellipseToOps(float cx, float cy, float rx,
                                        float ry) {
  std::vector<PathOp> ops;
  if (rx <= 0 || ry <= 0)
    return ops;
  constexpr float k = 0.5522847498f;
  ops.push_back({PathOp::Kind::Move, cx + rx, cy});
  ops.push_back({PathOp::Kind::Cubic, cx, cy + ry, cx + rx, cy + k * ry,
                 cx + k * rx, cy + ry});
  ops.push_back({PathOp::Kind::Cubic, cx - rx, cy, cx - k * rx, cy + ry,
                 cx - rx, cy + k * ry});
  ops.push_back({PathOp::Kind::Cubic, cx, cy - ry, cx - rx, cy - k * ry,
                 cx - k * rx, cy - ry});
  ops.push_back({PathOp::Kind::Cubic, cx + rx, cy, cx + k * rx, cy - ry,
                 cx + rx, cy - k * ry});
  ops.push_back({PathOp::Kind::Close});
  return ops;
}

inline std::vector<PathOp> polyToOps(const std::string &pts, bool close) {
  std::vector<PathOp> ops;
  size_t i = 0;
  bool first = true;
  float x, y;
  while (scanNumber(pts, i, x) && scanNumber(pts, i, y)) {
    ops.push_back({first ? PathOp::Kind::Move : PathOp::Kind::Line, x, y});
    first = false;
  }
  if (close && !ops.empty())
    ops.push_back({PathOp::Kind::Close});
  return ops;
}

struct Shape {
  std::vector<PathOp> ops;
  Mat2x3 transform;
  bool hasFill = false;
  Color fill{0, 0, 0, 255};
  bool hasStroke = false;
  Color stroke{0, 0, 0, 255};
  float strokeWidth = 1.0f;
  bool evenOdd = false;
  int lineCap = 0;
  int lineJoin = 0;
  float miterLimit = 4.0f;
  std::vector<float> dashArray;
  float dashOffset = 0.0f;
};

struct Document {
  std::vector<Shape> shapes;
  float width = 0, height = 0;
  float viewBoxX = 0, viewBoxY = 0;
};

struct InheritedStyle {
  bool hasFill = true;
  Color fill{0, 0, 0, 255};
  float fillOpacity = 1.0f;
  bool hasStroke = false;
  Color stroke{0, 0, 0, 255};
  float strokeOpacity = 1.0f;
  float strokeWidth = 1.0f;
  float opacity = 1.0f;
  bool evenOdd = false;
  int lineCap = 0;         // 0=butt, 1=round, 2=square (SVG default: butt)
  int lineJoin = 0;        // 0=miter, 1=round, 2=bevel (SVG default: miter)
  float miterLimit = 4.0f; // SVG default
  std::vector<float> dashArray; // empty = solid (SVG default: none)
  float dashOffset = 0.0f;
};

inline std::vector<float> parseDashArray(const std::string &val) {
  std::vector<float> out;
  if (val.empty() || val == "none")
    return out;
  size_t i = 0;
  float v;
  while (scanNumber(val, i, v)) {
    skipSep(val, i);
    if (i < val.size() &&
        val[i] == '%') // percentages unsupported; skip the sign
      ++i;
    out.push_back(v);
    skipSep(val, i);
  }
  return out;
}

// selector ("." + class name) -> raw declaration-block text (same
// syntax as an inline style="" attribute, e.g. "fill:none;stroke:#000").
using Stylesheet = std::unordered_map<std::string, std::string>;

// Splits a semicolon-separated "prop:value;prop:value" block into pairs,
// trimmed. Shared by both style="" attributes and stylesheet rules so
// there's exactly one place that knows this syntax.
inline std::vector<std::pair<std::string, std::string>>
parseDeclarations(const std::string &block) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t i = 0;
  while (i < block.size()) {
    size_t semi = block.find(';', i);
    std::string decl = block.substr(
        i, semi == std::string::npos ? std::string::npos : semi - i);
    size_t colon = decl.find(':');
    if (colon != std::string::npos) {
      std::string k = decl.substr(0, colon), v = decl.substr(colon + 1);
      auto trim = [](std::string &x) {
        size_t b = x.find_first_not_of(" \t\r\n"),
               e = x.find_last_not_of(" \t\r\n");
        x = b == std::string::npos ? "" : x.substr(b, e - b + 1);
      };
      trim(k);
      trim(v);
      if (!k.empty())
        out.push_back({k, v});
    }
    if (semi == std::string::npos)
      break;
    i = semi + 1;
  }
  return out;
}

// Concatenates the text content of every <style> element anywhere in
// the tree (there's normally just one, inside <defs>, but nothing stops
// an author from writing more than one).
inline void collectStyleText(const liteui_xml::Node &node, std::string &out) {
  if (node.tag == "style") {
    out += node.text;
    out += '\n';
  }
  for (auto &child : node.children)
    collectStyleText(child, out);
}

// Deliberately minimal: only ".class { decl: value; ... }" rules are
// recognized (comma-separated selector lists are split, and each class
// gets its own entry) — covers the overwhelmingly common case of SVG
// exporters (Illustrator, Figma, Inkscape, etc.) emitting
// "<style>.cls-1{fill:none;...}</style>". Tag/id/attribute/descendant
// selectors, @media, and specificity rules are all out of scope for v1
// and are silently ignored. A class redefined by a later rule has its
// declarations appended after the earlier ones, so parseDeclarations'
// last-one-wins iteration order approximates ordinary CSS cascade
// behavior for that (extremely common) case.
inline Stylesheet parseStylesheet(const std::string &css) {
  Stylesheet sheet;
  std::string text;
  text.reserve(css.size());
  for (size_t i = 0; i < css.size();) {
    if (css.compare(i, 2, "/*") == 0) {
      size_t end = css.find("*/", i + 2);
      i = (end == std::string::npos) ? css.size() : end + 2;
    } else {
      text += css[i++];
    }
  }
  size_t i = 0;
  while (i < text.size()) {
    size_t open = text.find('{', i);
    if (open == std::string::npos)
      break;
    size_t close = text.find('}', open);
    if (close == std::string::npos)
      break;
    std::string selectors = text.substr(i, open - i);
    std::string decls = text.substr(open + 1, close - open - 1);
    size_t s = 0;
    while (s <= selectors.size()) {
      size_t comma = selectors.find(',', s);
      std::string sel = selectors.substr(
          s, comma == std::string::npos ? std::string::npos : comma - s);
      size_t b = sel.find_first_not_of(" \t\r\n");
      size_t e = sel.find_last_not_of(" \t\r\n");
      if (b != std::string::npos) {
        sel = sel.substr(b, e - b + 1);
        if (!sel.empty() && sel[0] == '.') {
          std::string &existing = sheet[sel];
          if (!existing.empty() && existing.back() != ';')
            existing += ';';
          existing += decls;
        }
      }
      if (comma == std::string::npos)
        break;
      s = comma + 1;
    }
    i = close + 1;
  }
  return sheet;
}

inline InheritedStyle
applyStyle(const std::unordered_map<std::string, std::string> &attrs,
           const InheritedStyle &inherited,
           const Stylesheet *stylesheet = nullptr) {
  InheritedStyle s = inherited;
  auto apply = [&](const std::string &key, const std::string &val) {
    Color c;
    if (key == "fill") {
      if (val == "none")
        s.hasFill = false;
      else if (parsePaint(val, c)) {
        s.hasFill = true;
        s.fill = c;
      }
    } else if (key == "stroke") {
      if (val == "none")
        s.hasStroke = false;
      else if (parsePaint(val, c)) {
        s.hasStroke = true;
        s.stroke = c;
      }
    } else if (key == "fill-opacity")
      s.fillOpacity = parseOpacity(val);
    else if (key == "stroke-opacity")
      s.strokeOpacity = parseOpacity(val);
    else if (key == "opacity")
      s.opacity = parseOpacity(val);
    else if (key == "stroke-width")
      s.strokeWidth = std::strtof(val.c_str(), nullptr);
    else if (key == "fill-rule")
      s.evenOdd = (val == "evenodd");
    else if (key == "stroke-linecap")
      s.lineCap = val == "round" ? 1 : val == "square" ? 2 : 0;
    else if (key == "stroke-linejoin")
      s.lineJoin = val == "round"   ? 1
                   : val == "bevel" ? 2
                                    : 0; // miter,
                                         // miter-clip,
                                         // arcs all
                                         // fall back
                                         // to miter
    else if (key == "stroke-miterlimit")
      s.miterLimit = std::strtof(val.c_str(), nullptr);
    else if (key == "stroke-dasharray")
      s.dashArray = parseDashArray(val);
    else if (key == "stroke-dashoffset")
      s.dashOffset = std::strtof(val.c_str(), nullptr);
  };
  auto tryAttr = [&](const char *key) {
    auto it = attrs.find(key);
    if (it != attrs.end())
      apply(key, it->second);
  };
  tryAttr("fill");
  tryAttr("stroke");
  tryAttr("fill-opacity");
  tryAttr("stroke-opacity");
  tryAttr("opacity");
  tryAttr("stroke-width");
  tryAttr("fill-rule");
  tryAttr("stroke-linecap");
  tryAttr("stroke-linejoin");
  tryAttr("stroke-miterlimit");
  tryAttr("stroke-dasharray");
  tryAttr("stroke-dashoffset");

  // Stylesheet class rules override presentation attributes but are
  // themselves overridden by an inline style="" — same precedence
  // order CSS gives inline style over a stylesheet, approximated.
  if (stylesheet && !stylesheet->empty()) {
    auto classIt = attrs.find("class");
    if (classIt != attrs.end()) {
      const std::string &cls = classIt->second;
      size_t i = 0;
      while (i < cls.size()) {
        while (i < cls.size() && (unsigned char)cls[i] <= ' ')
          ++i;
        size_t start = i;
        while (i < cls.size() && (unsigned char)cls[i] > ' ')
          ++i;
        if (i > start) {
          std::string name = cls.substr(start, i - start);
          auto sit = stylesheet->find("." + name);
          if (sit != stylesheet->end())
            for (auto &decl : parseDeclarations(sit->second))
              apply(decl.first, decl.second);
        }
      }
    }
  }

  auto styleIt = attrs.find("style");
  if (styleIt != attrs.end())
    for (auto &decl : parseDeclarations(styleIt->second))
      apply(decl.first, decl.second);

  return s;
}

inline std::string attrOr(const std::unordered_map<std::string, std::string> &a,
                          const std::string &k, const std::string &def = "") {
  auto it = a.find(k);
  return it == a.end() ? def : it->second;
}
inline float attrF(const std::unordered_map<std::string, std::string> &a,
                   const std::string &k, float def = 0) {
  auto it = a.find(k);
  return it == a.end() || it->second.empty()
             ? def
             : std::strtof(it->second.c_str(), nullptr);
}

// Builds an id -> node index over the whole tree (including inside <defs>,
// which is exactly where <use> targets usually live). Called once per
// document before walking; the returned pointers stay valid for the
// lifetime of the root Node they point into, which parseString keeps
// alive for the whole parse — Document itself stores no tree pointers.
inline void
collectIds(const liteui_xml::Node &node,
           std::unordered_map<std::string, const liteui_xml::Node *> &idMap) {
  auto it = node.attrs.find("id");
  if (it != node.attrs.end() && !it->second.empty())
    idMap.emplace(it->second, &node); // first occurrence wins on duplicate ids
  for (auto &child : node.children)
    collectIds(child, idMap);
}

// Threaded through every walk() call. idMap resolves <use href="#id">
// targets; useStack guards against reference cycles (A uses B uses A) and
// caps total nesting depth so a pathological file can't recurse unbounded.
struct WalkContext {
  const std::unordered_map<std::string, const liteui_xml::Node *> *idMap;
  std::vector<const liteui_xml::Node *> useStack;
  const Stylesheet *stylesheet = nullptr;
};

inline void walk(const liteui_xml::Node &node, Mat2x3 parentTransform,
                 InheritedStyle inherited, Document &doc, WalkContext &ctx,
                 bool isUseTarget = false) {
  Mat2x3 transform = parentTransform;
  auto txIt = node.attrs.find("transform");
  if (txIt != node.attrs.end())
    transform = transform.multiply(parseTransform(txIt->second));
  InheritedStyle style = applyStyle(node.attrs, inherited, ctx.stylesheet);

  // <defs> and <symbol> are template containers — only rendered when
  // reached as the direct resolution target of a <use> (isUseTarget),
  // never when walked as ordinary document content.
  if (!isUseTarget && (node.tag == "defs" || node.tag == "symbol"))
    return;

  if (node.tag == "use") {
    std::string href = attrOr(node.attrs, "href");
    if (href.empty())
      href = attrOr(node.attrs, "xlink:href");
    if (!href.empty() && href[0] == '#' && ctx.idMap) {
      std::string id = href.substr(1);
      auto it = ctx.idMap->find(id);
      if (it != ctx.idMap->end()) {
        const liteui_xml::Node *target = it->second;
        bool cycle = false;
        for (auto *n : ctx.useStack)
          if (n == target) {
            cycle = true;
            break;
          }
        constexpr size_t kMaxUseDepth =
            32; // guards runaway/pathological nesting
        if (!cycle && ctx.useStack.size() < kMaxUseDepth) {
          // <use x="".."" y=""..""> is an implicit extra translate applied
          // on top of the <use> element's own transform="" (already folded
          // into `transform` above).
          float ux = attrF(node.attrs, "x"), uy = attrF(node.attrs, "y");
          Mat2x3 useTransform = transform.multiply(Mat2x3::translate(ux, uy));
          ctx.useStack.push_back(target);
          walk(*target, useTransform, style, doc, ctx, /*isUseTarget=*/true);
          ctx.useStack.pop_back();
        }
      }
    }
    return; // <use> has no children of its own to walk
  }

  auto emit = [&](std::vector<PathOp> ops) {
    if (ops.empty())
      return;
    Shape shape;
    shape.ops = std::move(ops);
    shape.transform = transform;
    shape.hasFill = style.hasFill;
    if (style.hasFill) {
      shape.fill = style.fill;
      shape.fill.a =
          (uint8_t)(shape.fill.a * style.fillOpacity * style.opacity);
    }
    shape.hasStroke = style.hasStroke;
    if (style.hasStroke) {
      shape.stroke = style.stroke;
      shape.stroke.a =
          (uint8_t)(shape.stroke.a * style.strokeOpacity * style.opacity);
      shape.strokeWidth = style.strokeWidth;
      shape.lineCap = style.lineCap;
      shape.lineJoin = style.lineJoin;
      shape.miterLimit = style.miterLimit;
      shape.dashArray = style.dashArray;
      shape.dashOffset = style.dashOffset;
    }
    shape.evenOdd = style.evenOdd;
    doc.shapes.push_back(std::move(shape));
  };

  bool isShape = true;
  if (node.tag == "path")
    emit(parsePathData(attrOr(node.attrs, "d")));
  else if (node.tag == "rect") {
    float rx = attrF(node.attrs, "rx"), ry = attrF(node.attrs, "ry", rx);
    if (node.attrs.count("rx") && !node.attrs.count("ry"))
      ry = rx;
    emit(rectToOps(attrF(node.attrs, "x"), attrF(node.attrs, "y"),
                   attrF(node.attrs, "width"), attrF(node.attrs, "height"), rx,
                   ry));
  } else if (node.tag == "circle")
    emit(ellipseToOps(attrF(node.attrs, "cx"), attrF(node.attrs, "cy"),
                      attrF(node.attrs, "r"), attrF(node.attrs, "r")));
  else if (node.tag == "ellipse")
    emit(ellipseToOps(attrF(node.attrs, "cx"), attrF(node.attrs, "cy"),
                      attrF(node.attrs, "rx"), attrF(node.attrs, "ry")));
  else if (node.tag == "line")
    emit(
        {{PathOp::Kind::Move, attrF(node.attrs, "x1"), attrF(node.attrs, "y1")},
         {PathOp::Kind::Line, attrF(node.attrs, "x2"),
          attrF(node.attrs, "y2")}});
  else if (node.tag == "polyline")
    emit(polyToOps(attrOr(node.attrs, "points"), false));
  else if (node.tag == "polygon")
    emit(polyToOps(attrOr(node.attrs, "points"), true));
  else
    isShape = false;

  // <g>, <symbol> (only reached here when isUseTarget was true above),
  // nested <svg>, and unsupported containers all just recurse with the
  // accumulated transform/style. Children recurse with isUseTarget reset
  // to false (the default) — a <defs> nested inside a used <symbol> still
  // shouldn't render on its own, only via its own <use>.
  if (!isShape)
    for (auto &child : node.children)
      walk(child, transform, style, doc, ctx);
}

inline std::optional<Document> parseString(const std::string &xml) {
  auto root = liteui_xml::parse(xml);
  if (!root || root->tag != "svg")
    return std::nullopt;
  Document doc;
  float vbX = 0, vbY = 0, vbW = 0, vbH = 0;
  bool hasViewBox = false;
  auto vbIt = root->attrs.find("viewBox");
  if (vbIt != root->attrs.end()) {
    size_t i = 0;
    float vals[4];
    int n = 0;
    while (n < 4 && scanNumber(vbIt->second, i, vals[n]))
      ++n;
    if (n == 4) {
      vbX = vals[0];
      vbY = vals[1];
      vbW = vals[2];
      vbH = vals[3];
      hasViewBox = true;
    }
  }
  float attrW = attrF(root->attrs, "width", 0),
        attrH = attrF(root->attrs, "height", 0);
  doc.width = hasViewBox ? vbW : (attrW > 0 ? attrW : 100.0f);
  doc.height = hasViewBox ? vbH : (attrH > 0 ? attrH : 100.0f);
  doc.viewBoxX = hasViewBox ? vbX : 0;
  doc.viewBoxY = hasViewBox ? vbY : 0;

  std::unordered_map<std::string, const liteui_xml::Node *> idMap;
  collectIds(*root, idMap);

  std::string cssText;
  collectStyleText(*root, cssText);
  Stylesheet stylesheet = parseStylesheet(cssText);

  WalkContext ctx{&idMap, {}, &stylesheet};

  InheritedStyle rootStyle =
      applyStyle(root->attrs, InheritedStyle(), &stylesheet);
  Mat2x3 rootTf = Mat2x3::translate(-doc.viewBoxX, -doc.viewBoxY);
  for (auto &child : root->children)
    walk(child, rootTf, rootStyle, doc, ctx);
  return doc;
}

inline std::optional<Document> parseFile(const std::string &path,
                                         std::string *errorOut = nullptr) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp) {
    if (errorOut)
      *errorOut = "liteui_svg: failed to open " + path;
    return std::nullopt;
  }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::string data((size_t)std::max(0L, sz), '\0');
  if (sz > 0) {
    size_t got = fread(data.data(), 1, (size_t)sz, fp);
    data.resize(got);
  }
  fclose(fp);
  auto doc = parseString(data);
  if (!doc && errorOut)
    *errorOut = "liteui_svg: failed to parse " + path;
  return doc;
}

} // namespace liteui_svg

View View::toView(Text t) {
  View v;
  v.style = std::move(t.style);
  v.isText = true;
  v.text = std::move(t.label);
  v.textStyle.fontSize = t.fontSize;
  v.textStyle.fontWeight = t.fontWeight;
  v.textStyle.fontStyle = t.fontStyle;
  v.textStyle.fontFamily = std::move(t.fontFamily);
  v.textStyle.color = t.color;
  v.textStyle.align = t.align;
  v.textStyle.overflow = t.overflow;
  v.textStyle.wrap = t.wrap;
  v.textStyle.lineHeight = t.lineHeight;
  v.textStyle.letterSpacing = t.letterSpacing;
  v.textStyle.maxLines = t.maxLines;
  v.textStyle.underline = t.underline;
  v.textStyle.strikethrough = t.strikethrough;
  return v;
}

View View::toView(Canvas c) {
  View v;
  v.style = std::move(c.style);
  v.isCanvas = true;
  v.onPaint = std::move(c.onPaint);
  v.onClick = std::move(c.onClick);
  v.onDoubleClick = std::move(c.onDoubleClick);
  v.onAuxClick = std::move(c.onAuxClick);
  v.onContextMenu = std::move(c.onContextMenu);
  v.onPointerDown = std::move(c.onPointerDown);
  v.onPointerMove = std::move(c.onPointerMove);
  v.onPointerUp = std::move(c.onPointerUp);
  v.onPointerCancel = std::move(c.onPointerCancel);
  v.onPointerEnter = std::move(c.onPointerEnter);
  v.onPointerLeave = std::move(c.onPointerLeave);
  v.onWheel = std::move(c.onWheel);
  v.canvasDirtySource = std::move(c.canvasDirtySource);
  v.focusable = c.focusable;
  v.onFocus = std::move(c.onFocus);
  v.onBlur = std::move(c.onBlur);
  v.onKeyDown = std::move(c.onKeyDown);
  v.onTextInput = std::move(c.onTextInput);
  return v;
}

// ---------------- Text measurement/layout backend ----------------
// Platform-specific: DirectWrite on Windows, Pango/Cairo on Linux. Only
// liteui_layout::measureNatural (for sizing) and LiteUI's renderers (for
// painting) call into this.
namespace liteui_text {

struct Measurement {
  float width, height;
};

#if defined(_WIN32)

// Lazily-created, process-wide DirectWrite factory. DirectWrite objects
// aren't tied to any particular HWND/render target, so one factory serves
// every LiteUI window in the process.
inline IDWriteFactory *factory() {
  static IDWriteFactory *f = [] {
    IDWriteFactory *p = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&p))))
      throw std::runtime_error("DWriteCreateFactory failed");
    return p;
  }();
  return f;
}

inline DWRITE_FONT_WEIGHT toDWriteWeight(FontWeight w) {
  return static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(w));
}
inline DWRITE_FONT_STYLE toDWriteStyle(FontStyle s) {
  return s == FontStyle::Italic ? DWRITE_FONT_STYLE_ITALIC
                                : DWRITE_FONT_STYLE_NORMAL;
}

// Builds a ready-to-measure-or-draw layout. `availWidth`/`availHeight`
// bound wrapping/trimming — pass a huge value for "unbounded" (used when
// measuring a Fit-width node's intrinsic single-line size). Caller owns
// the returned pointer.
inline IDWriteTextLayout *makeLayout(const std::string &text,
                                     const TextStyle &style, float fontSize,
                                     FontWeight fontWeight,
                                     FontStyle fontStyleVal, float availWidth,
                                     float availHeight) {
  std::wstring wfam =
      style.fontFamily.empty() ? L"Segoe UI" : toWide(style.fontFamily);
  IDWriteTextFormat *format = nullptr;
  factory()->CreateTextFormat(wfam.c_str(), nullptr, toDWriteWeight(fontWeight),
                              toDWriteStyle(fontStyleVal),
                              DWRITE_FONT_STRETCH_NORMAL, fontSize, L"",
                              &format);
  if (!format)
    throw std::runtime_error("CreateTextFormat failed");
  format->SetWordWrapping(style.wrap == TextWrap::Wrap
                              ? DWRITE_WORD_WRAPPING_WRAP
                              : DWRITE_WORD_WRAPPING_NO_WRAP);
  switch (style.align) {
  case TextAlign::Center:
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    break;
  case TextAlign::End:
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    break;
  case TextAlign::Justify:
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_JUSTIFIED);
    break;
  default:
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    break;
  }

  std::wstring wtext = toWide(text);
  IDWriteTextLayout *layout = nullptr;
  factory()->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size()),
                              format, availWidth, availHeight, &layout);
  format->Release(); // layout holds what it needs internally
  if (!layout)
    throw std::runtime_error("CreateTextLayout failed");

  DWRITE_TEXT_RANGE full{0, static_cast<UINT32>(wtext.size())};
  if (style.letterSpacing != 0) {
    // Character spacing is IDWriteTextLayout1 (Windows 8.1+); skip
    // silently if the interface isn't available rather than failing the
    // whole layout over a cosmetic feature.
    IDWriteTextLayout1 *layout1 = nullptr;
    if (SUCCEEDED(layout->QueryInterface(&layout1)) && layout1) {
      layout1->SetCharacterSpacing(0, style.letterSpacing, 0, full);
      layout1->Release();
    }
  }
  if (style.underline)
    layout->SetUnderline(TRUE, full);
  if (style.strikethrough)
    layout->SetStrikethrough(TRUE, full);
  if (style.overflow == TextOverflow::Ellipsis) {
    IDWriteInlineObject *ellipsis = nullptr;
    factory()->CreateEllipsisTrimmingSign(layout, &ellipsis);
    if (ellipsis) {
      DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      layout->SetTrimming(&trimming, ellipsis);
      ellipsis->Release();
    }
  }
  if (style.maxLines > 0)
    layout->SetMaxHeight(availHeight);
  return layout;
}

// Used only by measureNatural — builds a throwaway layout purely to read
// metrics back. The renderer's own cached layout (built per-paint, see
// LiteUI::ensureTextLayout) is what's actually drawn.
inline Measurement measure(const std::string &text, const TextStyle &style,
                           float fontSize, FontWeight fontWeight,
                           FontStyle fontStyleVal, float availWidth) {
  float w =
      availWidth >= 0 ? availWidth : std::numeric_limits<float>::max() / 4;
  float h = std::numeric_limits<float>::max() / 4;
  IDWriteTextLayout *layout =
      makeLayout(text, style, fontSize, fontWeight, fontStyleVal, w, h);
  DWRITE_TEXT_METRICS m;
  layout->GetMetrics(&m);
  float outH = m.height;
  if (style.lineHeight > 0) {
    UINT32 lineCount = 0;
    layout->GetLineMetrics(nullptr, 0, &lineCount);
    outH = lineCount * style.lineHeight;
  }
  if (style.maxLines > 0 && style.lineHeight > 0)
    outH = std::min(outH, style.maxLines * style.lineHeight);
  Measurement result{m.widthIncludingTrailingWhitespace, outH};
  layout->Release();
  return result;
}

// Finds the character boundary whose x-position is closest to clickX
// (clickX measured from the text's own left edge). O(n) — fine for
// typical single-line field text.
size_t caretIndexForX(const std::string &text, const TextStyle &style,
                      float fontSize, FontWeight fontWeight,
                      FontStyle fontStyleVal, float clickX) {
  float best = 1e9f;
  size_t bestIdx = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    Measurement m = measure(text.substr(0, i), style, fontSize, fontWeight,
                            fontStyleVal, -1);
    float d = std::abs(m.width - clickX);
    if (d < best) {
      best = d;
      bestIdx = i;
    }
  }
  return bestIdx;
}

#else // Linux — Pango/Cairo

inline PangoFontDescription *makeFontDescription(const TextStyle &style,
                                                 float fontSize,
                                                 FontWeight fontWeight,
                                                 FontStyle fontStyleVal) {
  PangoFontDescription *desc = pango_font_description_new();
  pango_font_description_set_family(
      desc, style.fontFamily.empty() ? "Sans" : style.fontFamily.c_str());
  pango_font_description_set_weight(
      desc, static_cast<PangoWeight>(static_cast<int>(fontWeight)));
  pango_font_description_set_style(desc, fontStyleVal == FontStyle::Italic
                                             ? PANGO_STYLE_ITALIC
                                             : PANGO_STYLE_NORMAL);
  // Absolute pixel size sidesteps a DPI round-trip — everything else in
  // this file (layout, GL) already works in plain screen pixels.
  pango_font_description_set_absolute_size(desc, fontSize * PANGO_SCALE);
  return desc;
}

// One shared throwaway cairo context used only to create PangoLayouts for
// measurement — Pango requires *a* context to build a layout on, even one
// that's never painted to.
inline cairo_t *measureCr() {
  static cairo_surface_t *surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  static cairo_t *cr = cairo_create(surf);
  return cr;
}

inline PangoLayout *makeLayout(const std::string &text, const TextStyle &style,
                               float fontSize, FontWeight fontWeight,
                               FontStyle fontStyleVal, float availWidth,
                               cairo_t *cr) {
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, text.c_str(), -1);
  PangoFontDescription *desc =
      makeFontDescription(style, fontSize, fontWeight, fontStyleVal);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_width(layout,
                         (style.wrap == TextWrap::Wrap && availWidth >= 0)
                             ? static_cast<int>(availWidth * PANGO_SCALE)
                             : -1);
  if (style.maxLines > 0) {
    pango_layout_set_height(
        layout, -style.maxLines); // Pango idiom: negative = max line count
    pango_layout_set_ellipsize(layout, style.overflow == TextOverflow::Ellipsis
                                           ? PANGO_ELLIPSIZE_END
                                           : PANGO_ELLIPSIZE_NONE);
  } else if (style.overflow == TextOverflow::Ellipsis) {
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  }
  switch (style.align) {
  case TextAlign::Center:
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    break;
  case TextAlign::End:
    pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    break;
  case TextAlign::Justify:
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_justify(layout, TRUE);
    break;
  default:
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    break;
  }
  // Underline/strikethrough/letter-spacing are all plain PangoAttrList
  // entries — collected into one list and applied once.
  if (style.letterSpacing != 0 || style.underline || style.strikethrough) {
    PangoAttrList *attrs = pango_attr_list_new();
    if (style.letterSpacing != 0)
      pango_attr_list_insert(
          attrs, pango_attr_letter_spacing_new(
                     static_cast<int>(style.letterSpacing * PANGO_SCALE)));
    if (style.underline)
      pango_attr_list_insert(attrs,
                             pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    if (style.strikethrough)
      pango_attr_list_insert(attrs, pango_attr_strikethrough_new(TRUE));
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
  }
  if (style.lineHeight > 0) {
    PangoContext *ctx = pango_layout_get_context(layout);
    PangoFontMetrics *metrics = pango_context_get_metrics(
        ctx, pango_layout_get_font_description(layout), nullptr);
    double natural =
        pango_font_metrics_get_height(metrics) / (double)PANGO_SCALE;
    pango_font_metrics_unref(metrics);
    if (natural > 0)
      pango_layout_set_line_spacing(
          layout, static_cast<float>(style.lineHeight / natural));
  }
  return layout;
}

inline Measurement measure(const std::string &text, const TextStyle &style,
                           float fontSize, FontWeight fontWeight,
                           FontStyle fontStyleVal, float availWidth) {
  PangoLayout *layout = makeLayout(text, style, fontSize, fontWeight,
                                   fontStyleVal, availWidth, measureCr());
  int w, h;
  pango_layout_get_pixel_size(layout, &w, &h);
  Measurement result{static_cast<float>(w), static_cast<float>(h)};
  g_object_unref(layout);
  return result;
}

// Finds the character boundary whose x-position is closest to clickX
// (clickX measured from the text's own left edge). O(n) — fine for
// typical single-line field text. Mirrors the Windows/DirectWrite
// implementation above; only depends on measure(), so the logic is
// identical on both platforms.
size_t caretIndexForX(const std::string &text, const TextStyle &style,
                      float fontSize, FontWeight fontWeight,
                      FontStyle fontStyleVal, float clickX) {
  float best = 1e9f;
  size_t bestIdx = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    Measurement m = measure(text.substr(0, i), style, fontSize, fontWeight,
                            fontStyleVal, -1);
    float d = std::abs(m.width - clickX);
    if (d < best) {
      best = d;
      bestIdx = i;
    }
  }
  return bestIdx;
}

#endif
} // namespace liteui_text

// ---------------- Canvas 2D context: Direct2D (Windows) / Cairo (Linux) ----
//
// Two complete, independent definitions of CanvasContext follow (one per
// platform), each exposing the identical public method surface described
// in the Canvas struct's own comment above. App code that only calls the
// public API compiles unchanged on either platform.
#if defined(_WIN32)

class CanvasContextImpl : public CanvasContext {
public:
  // `dc` is a Direct2D device context (from the window's Direct3D 11
  // device) whose target the caller has already set to `target` — a GPU
  // texture sized exactly to the canvas node's inner (padding-excluded)
  // content box — and BeginDraw()'d and cleared to transparent (see
  // LiteUI::ensureCanvasTarget). `width`/`height` mirror that box's size
  // so the app can query it back via width()/height() without
  // hand-computing it. `imageCacheOwner`, when non-null, is the canvas
  // node's own View::Computed: drawImage() uses its cachedImageBitmap slot
  // to skip re-uploading the same CanvasImage on a repeat draw, and
  // getImageData() keeps its CPU readback texture in canvasStaging. Left
  // null for any CanvasContext not backed by a persistent per-node
  // Computed, in which case both fall back to uncached, per-call
  // resources.
  CanvasContextImpl(ID2D1DeviceContext *dc, ID2D1Bitmap1 *target, float width,
                    float height,
                    const View::Computed *imageCacheOwner = nullptr)
      : rt_(dc), dc_(dc), target_(target), width_(width), height_(height),
        imageCacheOwner_(imageCacheOwner) {
    rt_->GetFactory(&factory_);
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    applyBlend();
  }
  ~CanvasContextImpl() {
    // Leave the device context's layer stack balanced even if onPaint
    // forgot to restore() every save()/clip() — an imbalanced PushLayer
    // would make the caller's EndDraw() fail.
    while (!clipLayers_.empty()) {
      rt_->PopLayer();
      clipLayers_.back()->Release();
      clipLayers_.pop_back();
    }
    // The device context is shared by every canvas node, so don't leak
    // this canvas's blend mode into the next one.
    dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    if (factory_)
      factory_->Release();
  }
  CanvasContextImpl(const CanvasContextImpl &) = delete;
  CanvasContextImpl &operator=(const CanvasContextImpl &) = delete;

  float width() const { return width_; }
  float height() const { return height_; }

  // ---- state stack ----
  void save() {
    State s;
    s.transform = transform_;
    s.fill = fillPaint_;
    s.stroke = strokePaint_;
    s.lineWidth = lineWidth_;
    s.lineCap = lineCap_;
    s.lineJoin = lineJoin_;
    s.miterLimit = miterLimit_;
    s.dashes = dashes_;
    s.dashOffset = dashOffset_;
    s.globalAlpha = globalAlpha_;
    s.composite = composite_;
    s.shadowColor = shadowColor_;
    s.shadowOffsetX = shadowOffsetX_;
    s.shadowOffsetY = shadowOffsetY_;
    s.font = font_;
    s.textAlign = textAlign_;
    s.textBaseline = textBaseline_;
    s.clipDepth = clipLayers_.size();
    stack_.push_back(std::move(s));
  }
  void restore() {
    if (stack_.empty())
      return;
    State s = std::move(stack_.back());
    stack_.pop_back();
    while (clipLayers_.size() > s.clipDepth) {
      rt_->PopLayer();
      clipLayers_.back()->Release();
      clipLayers_.pop_back();
    }
    transform_ = s.transform;
    fillPaint_ = s.fill;
    strokePaint_ = s.stroke;
    lineWidth_ = s.lineWidth;
    lineCap_ = s.lineCap;
    lineJoin_ = s.lineJoin;
    miterLimit_ = s.miterLimit;
    dashes_ = s.dashes;
    dashOffset_ = s.dashOffset;
    globalAlpha_ = s.globalAlpha;
    composite_ = s.composite;
    applyBlend();
    shadowColor_ = s.shadowColor;
    shadowOffsetX_ = s.shadowOffsetX;
    shadowOffsetY_ = s.shadowOffsetY;
    font_ = s.font;
    textAlign_ = s.textAlign;
    textBaseline_ = s.textBaseline;
  }

  // ---- transform ----
  void translate(float x, float y) {
    transform_ = D2D1::Matrix3x2F::Translation(x, y) * transform_;
  }
  void rotate(float radians) {
    transform_ =
        D2D1::Matrix3x2F::Rotation(radians * 180.0f / 3.14159265358979f) *
        transform_;
  }
  void scale(float sx, float sy) {
    transform_ = D2D1::Matrix3x2F::Scale(sx, sy) * transform_;
  }
  void transformBy(float a, float b, float c, float d, float e, float f) {
    transform_ = D2D1::Matrix3x2F(a, b, c, d, e, f) * transform_;
  }
  void setTransform(float a, float b, float c, float d, float e, float f) {
    transform_ = D2D1::Matrix3x2F(a, b, c, d, e, f);
  }
  void resetTransform() { transform_ = D2D1::Matrix3x2F::Identity(); }

  // ---- paint/style ----
  void setFillColor(Color c) { fillPaint_ = Paint::solid(c); }
  void setFillGradient(const CanvasGradient &g) { fillPaint_ = Paint::grad(g); }
  void setStrokeColor(Color c) { strokePaint_ = Paint::solid(c); }
  void setStrokeGradient(const CanvasGradient &g) {
    strokePaint_ = Paint::grad(g);
  }
  void setLineWidth(float w) { lineWidth_ = std::max(0.0f, w); }
  void setLineCap(LineCap c) { lineCap_ = c; }
  void setLineJoin(LineJoin j) { lineJoin_ = j; }
  void setMiterLimit(float m) { miterLimit_ = m; }
  void setLineDash(const std::vector<float> &dashes) { dashes_ = dashes; }
  void setLineDashOffset(float offset) { dashOffset_ = offset; }
  void setGlobalAlpha(float a) { globalAlpha_ = std::clamp(a, 0.0f, 1.0f); }
  // See the Canvas limitations note: Direct2D's primitive blend modes
  // only cover SourceOver, Copy and Lighter (additive); every other
  // operator draws as SourceOver on Windows.
  void setGlobalCompositeOperation(CompositeOp op) {
    composite_ = op;
    applyBlend();
  }
  void setShadow(Color color, float /*blurPx, unsupported: unblurred*/,
                 float offsetX, float offsetY) {
    shadowColor_ = color;
    shadowOffsetX_ = offsetX;
    shadowOffsetY_ = offsetY;
  }
  void setFont(const std::string &family, float sizePx,
               FontWeight weight = FontWeight::Regular,
               FontStyle style = FontStyle::Normal) {
    font_.fontFamily = family;
    font_.fontSize = sizePx;
    font_.fontWeight = weight;
    font_.fontStyle = style;
  }
  void setTextAlign(TextAlign a) { textAlign_ = a; }
  void setTextBaseline(TextBaseline b) { textBaseline_ = b; }

  // ---- path building ----
  // Mirrors HTML canvas path semantics: fill()/stroke()/clip() below do
  // NOT clear the current path (only beginPath() does), so the same
  // shape can be filled then stroked, or extended with more segments,
  // across multiple calls.
  void beginPath() {
    segs_.clear();
    figureClosed_.clear();
    curX_ = curY_ = startX_ = startY_ = 0;
  }
  void closePath() {
    if (!figureClosed_.empty())
      figureClosed_.back() = true;
    curX_ = startX_;
    curY_ = startY_;
  }
  void moveTo(float x, float y) {
    segs_.push_back(Seg::move(x, y));
    figureClosed_.push_back(false);
    curX_ = startX_ = x;
    curY_ = startY_ = y;
  }
  void lineTo(float x, float y) {
    ensureFigure();
    segs_.push_back(Seg::line(x, y));
    curX_ = x;
    curY_ = y;
  }
  void quadraticCurveTo(float cpx, float cpy, float x, float y) {
    ensureFigure();
    float c1x = curX_ + 2.0f / 3.0f * (cpx - curX_);
    float c1y = curY_ + 2.0f / 3.0f * (cpy - curY_);
    float c2x = x + 2.0f / 3.0f * (cpx - x);
    float c2y = y + 2.0f / 3.0f * (cpy - y);
    segs_.push_back(Seg::cubic(c1x, c1y, c2x, c2y, x, y));
    curX_ = x;
    curY_ = y;
  }
  void bezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x,
                     float y) {
    ensureFigure();
    segs_.push_back(Seg::cubic(c1x, c1y, c2x, c2y, x, y));
    curX_ = x;
    curY_ = y;
  }
  void arc(float cx, float cy, float r, float startAngle, float endAngle,
           bool ccw = false) {
    appendArc(cx, cy, r, r, 0, startAngle, endAngle, ccw,
              figureClosed_.empty());
  }
  void ellipse(float cx, float cy, float rx, float ry, float rotation,
               float startAngle, float endAngle, bool ccw = false) {
    appendArc(cx, cy, rx, ry, rotation, startAngle, endAngle, ccw,
              figureClosed_.empty());
  }
  void arcTo(float x1, float y1, float x2, float y2, float radius) {
    float x0 = curX_, y0 = curY_;
    float dx1 = x0 - x1, dy1 = y0 - y1;
    float dx2 = x2 - x1, dy2 = y2 - y1;
    float len1 = std::sqrt(dx1 * dx1 + dy1 * dy1);
    float len2 = std::sqrt(dx2 * dx2 + dy2 * dy2);
    if (len1 < 1e-6f || len2 < 1e-6f || radius <= 0) {
      lineTo(x1, y1);
      return;
    }
    dx1 /= len1;
    dy1 /= len1;
    dx2 /= len2;
    dy2 /= len2;
    float cosA = std::clamp(dx1 * dx2 + dy1 * dy2, -1.0f, 1.0f);
    float angle = std::acos(cosA);
    if (angle < 1e-6f) {
      lineTo(x1, y1);
      return;
    }
    float dist = radius / std::tan(angle / 2.0f);
    float t1x = x1 + dx1 * dist, t1y = y1 + dy1 * dist;
    float t2x = x1 + dx2 * dist, t2y = y1 + dy2 * dist;
    lineTo(t1x, t1y);
    float bisx = dx1 + dx2, bisy = dy1 + dy2;
    float bisLen = std::sqrt(bisx * bisx + bisy * bisy);
    if (bisLen < 1e-6f) {
      lineTo(x1, y1);
      return;
    }
    bisx /= bisLen;
    bisy /= bisLen;
    float centerDist = std::sqrt(radius * radius + dist * dist);
    float ccx = x1 + bisx * centerDist, ccy = y1 + bisy * centerDist;
    float a0 = std::atan2(t1y - ccy, t1x - ccx);
    float a1 = std::atan2(t2y - ccy, t2x - ccx);
    float cross = dx1 * dy2 - dy1 * dx2; // winding sign, picks short way
    appendArc(ccx, ccy, radius, radius, 0, a0, a1, cross > 0, false);
  }
  void rect(float x, float y, float w, float h) {
    moveTo(x, y);
    lineTo(x + w, y);
    lineTo(x + w, y + h);
    lineTo(x, y + h);
    closePath();
  }
  void roundRect(float x, float y, float w, float h, float radius) {
    constexpr float kPi = 3.14159265358979f;
    radius = std::max(0.0f, std::min(radius, std::min(w, h) / 2.0f));
    moveTo(x + radius, y);
    lineTo(x + w - radius, y);
    appendArc(x + w - radius, y + radius, radius, radius, 0, -kPi / 2, 0, false,
              false);
    lineTo(x + w, y + h - radius);
    appendArc(x + w - radius, y + h - radius, radius, radius, 0, 0, kPi / 2,
              false, false);
    lineTo(x + radius, y + h);
    appendArc(x + radius, y + h - radius, radius, radius, 0, kPi / 2, kPi,
              false, false);
    lineTo(x, y + radius);
    appendArc(x + radius, y + radius, radius, radius, 0, kPi, kPi * 1.5f, false,
              false);
    closePath();
  }

  // ---- drawing ----
  void fill(FillRule rule = FillRule::NonZero) {
    ID2D1PathGeometry *geom = buildGeometry(toFillMode(rule));
    if (!geom)
      return;
    rt_->SetTransform(transform_);
    drawShadowThenReal([&](ID2D1Brush *b) { rt_->FillGeometry(geom, b); },
                       fillPaint_);
    geom->Release();
  }
  void stroke() {
    ID2D1PathGeometry *geom = buildGeometry(D2D1_FILL_MODE_WINDING);
    if (!geom)
      return;
    rt_->SetTransform(transform_);
    ID2D1StrokeStyle *strokeStyle = makeStrokeStyle();
    drawShadowThenReal(
        [&](ID2D1Brush *b) {
          rt_->DrawGeometry(geom, b, lineWidth_, strokeStyle);
        },
        strokePaint_);
    if (strokeStyle)
      strokeStyle->Release();
    geom->Release();
  }
  void clip(FillRule rule = FillRule::NonZero) {
    ID2D1PathGeometry *geom = buildGeometry(toFillMode(rule));
    if (!geom)
      return;
    rt_->SetTransform(transform_);
    ID2D1Layer *layer = nullptr;
    rt_->CreateLayer(nullptr, &layer);
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geom), layer);
    clipLayers_.push_back(layer);
    geom->Release();
  }
  void fillRect(float x, float y, float w, float h) {
    std::vector<Seg> savedSegs = segs_;
    std::vector<bool> savedClosed = figureClosed_;
    float savedX = curX_, savedY = curY_, savedSX = startX_, savedSY = startY_;
    beginPath();
    rect(x, y, w, h);
    fill();
    segs_ = std::move(savedSegs);
    figureClosed_ = std::move(savedClosed);
    curX_ = savedX;
    curY_ = savedY;
    startX_ = savedSX;
    startY_ = savedSY;
  }
  void strokeRect(float x, float y, float w, float h) {
    std::vector<Seg> savedSegs = segs_;
    std::vector<bool> savedClosed = figureClosed_;
    float savedX = curX_, savedY = curY_, savedSX = startX_, savedSY = startY_;
    beginPath();
    rect(x, y, w, h);
    stroke();
    segs_ = std::move(savedSegs);
    figureClosed_ = std::move(savedClosed);
    curX_ = savedX;
    curY_ = savedY;
    startX_ = savedSX;
    startY_ = savedSY;
  }
  // A true, transform-aware sub-rectangle clear (see the Canvas
  // limitations note at the top of this section). Done on the GPU as a
  // fill of transparent black with the COPY primitive blend, which
  // replaces the destination instead of blending with it: the rectangle
  // goes through the current transform and clip like any other shape, and
  // a rotated/skewed edge is antialiased.
  void clearRect(float x, float y, float w, float h) {
    float x0 = std::min(x, x + w), x1 = std::max(x, x + w);
    float y0 = std::min(y, y + h), y1 = std::max(y, y + h);
    ID2D1SolidColorBrush *brush = nullptr;
    rt_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), &brush);
    if (!brush)
      return;
    rt_->SetTransform(transform_);
    dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
    rt_->FillRectangle(D2D1::RectF(x0, y0, x1, y1), brush);
    applyBlend(); // back to whatever globalCompositeOperation asked for
    brush->Release();
  }

  // ---- text ----
  TextMetricsResult measureText(const std::string &text) {
    TextStyle ts = fontToTextStyle();
    liteui_text::Measurement m = liteui_text::measure(
        text, ts, resolveStatic(ts.fontSize), resolveStatic(ts.fontWeight),
        resolveStatic(ts.fontStyle), -1);
    return {m.width};
  }
  void fillText(const std::string &text, float x, float y,
                float maxWidth = -1) {
    drawTextImpl(text, x, y, maxWidth, false);
  }
  void strokeText(const std::string &text, float x, float y,
                  float maxWidth = -1) {
    drawTextImpl(text, x, y, maxWidth, true);
  }

  // ---- images ----
  void drawImage(const CanvasImage &img, float dx, float dy) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy,
              static_cast<float>(img.width), static_cast<float>(img.height));
  }
  void drawImage(const CanvasImage &img, float dx, float dy, float dw,
                 float dh) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy, dw, dh);
  }
  // Draws `img`, converting+uploading it to a fresh ID2D1Bitmap first
  // unless imageCacheOwner_ already has one cached for this exact pixel
  // buffer (see the constructor's comment and makeBitmap's). Caching is
  // keyed on img.pixels.data()+size+width+height rather than any explicit
  // "did this change" flag the app sets — CanvasImage carries no such
  // flag — so it's a heuristic: it correctly detects a brand new image (a
  // new allocation, e.g. every AdjustmentSet::applyTo() call) or a resized
  // one (reallocates), which covers ordinary usage, but it would miss
  // pixel values changed in place at the SAME address and size without an
  // intervening resize. No code in this file mutates a CanvasImage that
  // way between drawImage calls; an app that does would need to force a
  // miss itself (e.g. by giving the image a fresh pixels vector).
  void drawImage(const CanvasImage &img, float sx, float sy, float sw, float sh,
                 float dx, float dy, float dw, float dh) {
    if (img.width <= 0 || img.height <= 0)
      return;
    ID2D1Bitmap *bmp = nullptr;
    bool cacheHit =
        imageCacheOwner_ && imageCacheOwner_->cachedImageBitmap &&
        imageCacheOwner_->cachedImagePixels == img.pixels.data() &&
        imageCacheOwner_->cachedImagePixelsSize == img.pixels.size() &&
        imageCacheOwner_->cachedImageW == img.width &&
        imageCacheOwner_->cachedImageH == img.height;
    if (cacheHit) {
      bmp = imageCacheOwner_->cachedImageBitmap;
    } else {
      bmp = makeBitmap(img);
      if (!bmp)
        return;
      if (imageCacheOwner_) {
        if (imageCacheOwner_->cachedImageBitmap)
          imageCacheOwner_->cachedImageBitmap->Release();
        imageCacheOwner_->cachedImageBitmap = bmp;
        imageCacheOwner_->cachedImagePixels = img.pixels.data();
        imageCacheOwner_->cachedImagePixelsSize = img.pixels.size();
        imageCacheOwner_->cachedImageW = img.width;
        imageCacheOwner_->cachedImageH = img.height;
      }
    }
    rt_->SetTransform(transform_);
    D2D1_RECT_F dst = D2D1::RectF(dx, dy, dx + dw, dy + dh);
    D2D1_RECT_F src = D2D1::RectF(sx, sy, sx + sw, sy + sh);

    rt_->DrawBitmap(bmp, dst, globalAlpha_,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
    // Only release when NOT cache-owned: an uncached draw (imageCacheOwner_
    // null) still owns bmp itself and must free it here, exactly as
    // before; a cached one is now owned by imageCacheOwner_ and lives on
    // for a future hit.
    if (!imageCacheOwner_)
      bmp->Release();
  }

  // ---- pixel data ----
  // Fully supported (see the Canvas limitations note). The canvas lives in
  // a GPU texture, which can't be mapped directly, so this flushes the
  // drawing issued so far, copies the requested region into a CPU-readable
  // staging texture and maps that. Like the Cairo/Linux backend, and per
  // the HTML canvas spec, this ignores the current transform and reads raw
  // device pixels; pixels are premultiplied BGRA on the GPU and are
  // un-premultiplied into CanvasImage's straight RGBA here, mirroring the
  // Cairo path exactly. Returns std::nullopt only if there's no backing
  // texture at all (shouldn't happen for a real canvas node).
  std::optional<CanvasImage> getImageData(float x, float y, float w, float h) {
    if (!target_)
      return std::nullopt;
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    int iw = std::max(0, static_cast<int>(std::round(w)));
    int ih = std::max(0, static_cast<int>(std::round(h)));
    CanvasImage out(iw, ih);
    if (iw == 0 || ih == 0)
      return out;
    D2D1_SIZE_U sz = target_->GetPixelSize();
    int clipX0 = std::max(0, ix), clipY0 = std::max(0, iy);
    int clipX1 = std::min(static_cast<int>(sz.width), ix + iw);
    int clipY1 = std::min(static_cast<int>(sz.height), iy + ih);
    if (clipX1 <= clipX0 || clipY1 <= clipY0)
      return out; // entirely off-canvas: transparent black, like Cairo path

    // Commit everything drawn earlier in this onPaint before copying.
    rt_->Flush(nullptr, nullptr);

    // Reuse the node's cached staging texture when it matches the canvas
    // size; otherwise make a new one (kept on the node if there is one,
    // else freed again below).
    ID2D1Bitmap1 *staging = nullptr;
    bool ownsStaging = false;
    if (imageCacheOwner_ && imageCacheOwner_->canvasStaging) {
      D2D1_SIZE_U ss = imageCacheOwner_->canvasStaging->GetPixelSize();
      if (ss.width == sz.width && ss.height == sz.height)
        staging = imageCacheOwner_->canvasStaging;
    }
    if (!staging) {
      D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
          D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_PREMULTIPLIED),
          96.0f, 96.0f);
      if (FAILED(dc_->CreateBitmap(sz, nullptr, 0, &props, &staging)) ||
          !staging)
        return out;
      if (imageCacheOwner_) {
        if (imageCacheOwner_->canvasStaging)
          imageCacheOwner_->canvasStaging->Release();
        imageCacheOwner_->canvasStaging = staging;
      } else {
        ownsStaging = true;
      }
    }
    D2D1_POINT_2U dstPt =
        D2D1::Point2U(static_cast<UINT32>(clipX0), static_cast<UINT32>(clipY0));
    D2D1_RECT_U srcRect =
        D2D1::RectU(static_cast<UINT32>(clipX0), static_cast<UINT32>(clipY0),
                    static_cast<UINT32>(clipX1), static_cast<UINT32>(clipY1));
    D2D1_MAPPED_RECT mapped = {};
    if (SUCCEEDED(staging->CopyFromBitmap(&dstPt, target_, &srcRect)) &&
        SUCCEEDED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
      for (int row = clipY0; row < clipY1; ++row) {
        const BYTE *src = mapped.bits +
                          static_cast<size_t>(row) * mapped.pitch +
                          static_cast<size_t>(clipX0) * 4;
        int destRow = row - iy;
        for (int col = clipX0; col < clipX1; ++col) {
          const BYTE *p = src + static_cast<size_t>(col - clipX0) * 4;
          int destCol = col - ix;
          unsigned char b = p[0], g = p[1], r = p[2], a = p[3];
          unsigned char *dst =
              out.pixels.data() +
              (static_cast<size_t>(destRow) * iw + destCol) * 4;
          dst[0] =
              a ? static_cast<unsigned char>(std::min(255, r * 255 / a)) : 0;
          dst[1] =
              a ? static_cast<unsigned char>(std::min(255, g * 255 / a)) : 0;
          dst[2] =
              a ? static_cast<unsigned char>(std::min(255, b * 255 / a)) : 0;
          dst[3] = a;
        }
      }
      staging->Unmap();
    }
    if (ownsStaging)
      staging->Release();
    return out;
  }
  // putImageData ignores the current transform and paints the pixels
  // directly at (dx,dy), matching HTML canvas semantics.
  void putImageData(const CanvasImage &img, float dx, float dy) {
    if (img.width <= 0 || img.height <= 0)
      return;
    ID2D1Bitmap *bmp = makeBitmap(img);
    if (!bmp)
      return;
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_RECT_F dst = D2D1::RectF(dx, dy, dx + static_cast<float>(img.width),
                                  dy + static_cast<float>(img.height));
    rt_->DrawBitmap(bmp, dst, 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    bmp->Release();
  }

private:
  // ---- internal path representation ----
  struct Seg {
    enum class Kind { Move, Line, Cubic } kind;
    float x, y;               // endpoint (all kinds)
    float c1x, c1y, c2x, c2y; // control points (Cubic only)
    static Seg move(float x, float y) { return {Kind::Move, x, y, 0, 0, 0, 0}; }
    static Seg line(float x, float y) { return {Kind::Line, x, y, 0, 0, 0, 0}; }
    static Seg cubic(float c1x, float c1y, float c2x, float c2y, float x,
                     float y) {
      return {Kind::Cubic, x, y, c1x, c1y, c2x, c2y};
    }
  };

  struct Paint {
    bool isGradient = false;
    Color color{0, 0, 0, 255};
    CanvasGradient gradient;
    static Paint solid(Color c) {
      Paint p;
      p.isGradient = false;
      p.color = c;
      return p;
    }
    static Paint grad(const CanvasGradient &g) {
      Paint p;
      p.isGradient = true;
      p.gradient = g;
      return p;
    }
  };

  struct State {
    D2D1_MATRIX_3X2_F transform;
    Paint fill, stroke;
    float lineWidth;
    LineCap lineCap;
    LineJoin lineJoin;
    float miterLimit;
    std::vector<float> dashes;
    float dashOffset;
    float globalAlpha;
    CompositeOp composite;
    Color shadowColor;
    float shadowOffsetX, shadowOffsetY;
    TextStyle font;
    TextAlign textAlign;
    TextBaseline textBaseline;
    size_t clipDepth;
  };

  static Color colorOf(const Paint &p) {
    if (!p.isGradient)
      return p.color;
    return p.gradient.stops.empty() ? Color{0, 0, 0, 255}
                                    : p.gradient.stops.front().color;
  }

  void ensureFigure() {
    if (figureClosed_.empty())
      moveTo(curX_, curY_);
  }

  // Splits [startAngle, endAngle) into <=90-degree pieces and appends
  // each as a cubic Bezier via the standard circular-arc kappa
  // approximation, applied in a unit circle then mapped through
  // rotation+non-uniform-scale+translate — this is what lets the same
  // helper serve arc(), ellipse(), arcTo(), and roundRect()'s corners.
  void appendArc(float cx, float cy, float rx, float ry, float rotation,
                 float a0, float a1, bool ccw, bool asMove) {
    constexpr float kTwoPi = 6.28318530717959f;
    float delta = a1 - a0;
    if (!ccw) {
      while (delta < 0)
        delta += kTwoPi;
    } else {
      while (delta > 0)
        delta -= kTwoPi;
    }
    float cosR = std::cos(rotation), sinR = std::sin(rotation);
    auto mapPt = [&](float ux, float uy, float &px, float &py) {
      float ex = rx * ux, ey = ry * uy;
      px = cx + ex * cosR - ey * sinR;
      py = cy + ex * sinR + ey * cosR;
    };
    float p0x, p0y;
    mapPt(std::cos(a0), std::sin(a0), p0x, p0y);
    if (asMove)
      moveTo(p0x, p0y);
    else
      lineTo(p0x, p0y);
    if (std::abs(delta) < 1e-6f)
      return;
    int segCount = std::max(
        1, static_cast<int>(std::ceil(std::abs(delta) / (3.14159265f / 2))));
    float segAngle = delta / segCount;
    float ang = a0;
    for (int i = 0; i < segCount; ++i) {
      float ang1 = ang + segAngle;
      // Control-point length for a circular arc of angle (ang1 - ang): the
      // formula needs tan of HALF the sweep (a quarter made arcs and rounded
      // corners come out visibly too small).
      float t = std::tan((ang1 - ang) / 2.0f);
      float alpha =
          std::sin(ang1 - ang) * (std::sqrt(4.0f + 3.0f * t * t) - 1.0f) / 3.0f;
      float e0x = std::cos(ang), e0y = std::sin(ang);
      float e1x = std::cos(ang1), e1y = std::sin(ang1);
      float d0x = -e0y, d0y = e0x;
      float d1x = -e1y, d1y = e1x;
      float c1ex = e0x + alpha * d0x, c1ey = e0y + alpha * d0y;
      float c2ex = e1x - alpha * d1x, c2ey = e1y - alpha * d1y;
      float c1x, c1y, c2x, c2y, ex1, ey1;
      mapPt(c1ex, c1ey, c1x, c1y);
      mapPt(c2ex, c2ey, c2x, c2y);
      mapPt(e1x, e1y, ex1, ey1);
      segs_.push_back(Seg::cubic(c1x, c1y, c2x, c2y, ex1, ey1));
      curX_ = ex1;
      curY_ = ey1;
      ang = ang1;
    }
  }

  static D2D1_FILL_MODE toFillMode(FillRule rule) {
    return rule == FillRule::EvenOdd ? D2D1_FILL_MODE_ALTERNATE
                                     : D2D1_FILL_MODE_WINDING;
  }

  ID2D1PathGeometry *buildGeometry(D2D1_FILL_MODE fillMode) {
    if (segs_.empty())
      return nullptr;
    ID2D1PathGeometry *geom = nullptr;
    factory_->CreatePathGeometry(&geom);
    if (!geom)
      return nullptr;
    ID2D1GeometrySink *sink = nullptr;
    geom->Open(&sink);
    sink->SetFillMode(fillMode);
    bool figureOpen = false;
    size_t figIdx = 0;
    bool first = true;
    for (auto &s : segs_) {
      if (s.kind == Seg::Kind::Move) {
        if (figureOpen)
          sink->EndFigure(figureClosed_[figIdx] ? D2D1_FIGURE_END_CLOSED
                                                : D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(D2D1::Point2F(s.x, s.y), D2D1_FIGURE_BEGIN_FILLED);
        figureOpen = true;
        figIdx = first ? 0 : figIdx + 1;
        first = false;
      } else if (s.kind == Seg::Kind::Line) {
        sink->AddLine(D2D1::Point2F(s.x, s.y));
      } else {
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(s.c1x, s.c1y),
                                            D2D1::Point2F(s.c2x, s.c2y),
                                            D2D1::Point2F(s.x, s.y)));
      }
    }
    if (figureOpen)
      sink->EndFigure(figureClosed_[figIdx] ? D2D1_FIGURE_END_CLOSED
                                            : D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return geom;
  }

  ID2D1Brush *makeBrush(const Paint &p) {
    if (!p.isGradient) {
      ID2D1SolidColorBrush *b = nullptr;
      Color c = p.color;
      rt_->CreateSolidColorBrush(D2D1::ColorF(c.r / 255.0f, c.g / 255.0f,
                                              c.b / 255.0f,
                                              (c.a / 255.0f) * globalAlpha_),
                                 &b);
      return b;
    }
    if (p.gradient.stops.empty())
      return nullptr;
    std::vector<D2D1_GRADIENT_STOP> stops;
    for (auto &s : p.gradient.stops) {
      D2D1_GRADIENT_STOP gs;
      gs.position = std::clamp(s.offset, 0.0f, 1.0f);
      gs.color =
          D2D1::ColorF(s.color.r / 255.0f, s.color.g / 255.0f,
                       s.color.b / 255.0f, (s.color.a / 255.0f) * globalAlpha_);
      stops.push_back(gs);
    }
    if (p.gradient.kind == CanvasGradient::Kind::Radial) {
      // D2D1's radial brush models a single circle plus a focal offset
      // point, unlike canvas's two-circle (r0..r1) model — approximate
      // by rescaling stop offsets so t=0 lands at r0/r1 of the way into
      // the D2D circle (see Canvas limitations note).
      float r1 = std::max(1.0f, p.gradient.r1);
      float innerFrac = std::clamp(p.gradient.r0 / r1, 0.0f, 0.99f);
      for (auto &gs : stops)
        gs.position = innerFrac + gs.position * (1.0f - innerFrac);
    }
    ID2D1GradientStopCollection *coll = nullptr;
    rt_->CreateGradientStopCollection(stops.data(),
                                      static_cast<UINT32>(stops.size()), &coll);
    if (!coll)
      return nullptr;
    ID2D1Brush *brush = nullptr;
    if (p.gradient.kind == CanvasGradient::Kind::Linear) {
      ID2D1LinearGradientBrush *lb = nullptr;
      rt_->CreateLinearGradientBrush(
          D2D1::LinearGradientBrushProperties(
              D2D1::Point2F(p.gradient.x0, p.gradient.y0),
              D2D1::Point2F(p.gradient.x1, p.gradient.y1)),
          coll, &lb);
      brush = lb;
    } else {
      float r1 = std::max(1.0f, p.gradient.r1);
      float ox =
          std::clamp(p.gradient.x0 - p.gradient.x1, -r1 * 0.99f, r1 * 0.99f);
      float oy =
          std::clamp(p.gradient.y0 - p.gradient.y1, -r1 * 0.99f, r1 * 0.99f);
      ID2D1RadialGradientBrush *rb = nullptr;
      rt_->CreateRadialGradientBrush(
          D2D1::RadialGradientBrushProperties(
              D2D1::Point2F(p.gradient.x1, p.gradient.y1),
              D2D1::Point2F(ox, oy), r1, r1),
          coll, &rb);
      brush = rb;
    }
    coll->Release();
    return brush;
  }

  template <class Fn> void drawShadowThenReal(Fn drawFn, const Paint &paint) {
    if (shadowColor_.a > 0 && (shadowOffsetX_ != 0 || shadowOffsetY_ != 0)) {
      D2D1_MATRIX_3X2_F saved = transform_;
      rt_->SetTransform(
          D2D1::Matrix3x2F::Translation(shadowOffsetX_, shadowOffsetY_) *
          transform_);
      ID2D1Brush *shadowBrush = makeBrush(Paint::solid(shadowColor_));
      if (shadowBrush) {
        drawFn(shadowBrush);
        shadowBrush->Release();
      }
      rt_->SetTransform(saved);
    }
    ID2D1Brush *brush = makeBrush(paint);
    if (brush) {
      drawFn(brush);
      brush->Release();
    }
  }

  ID2D1StrokeStyle *makeStrokeStyle() {
    D2D1_CAP_STYLE cap = lineCap_ == LineCap::Round    ? D2D1_CAP_STYLE_ROUND
                         : lineCap_ == LineCap::Square ? D2D1_CAP_STYLE_SQUARE
                                                       : D2D1_CAP_STYLE_FLAT;
    D2D1_LINE_JOIN join = lineJoin_ == LineJoin::Round   ? D2D1_LINE_JOIN_ROUND
                          : lineJoin_ == LineJoin::Bevel ? D2D1_LINE_JOIN_BEVEL
                                                         : D2D1_LINE_JOIN_MITER;
    // D2D interprets dash array/offset values as multiples of stroke
    // width, whereas canvas's setLineDash/lineDashOffset are absolute
    // pixels — rescale to match.
    float lw = std::max(0.0001f, lineWidth_);
    std::vector<float> scaled;
    scaled.reserve(dashes_.size());
    for (float d : dashes_)
      scaled.push_back(d / lw);
    D2D1_DASH_STYLE dashStyle =
        scaled.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM;
    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        cap, cap, cap, join, miterLimit_, dashStyle, dashOffset_ / lw);
    ID2D1StrokeStyle *style = nullptr;
    factory_->CreateStrokeStyle(props, scaled.empty() ? nullptr : scaled.data(),
                                static_cast<UINT32>(scaled.size()), &style);
    return style;
  }

  ID2D1Bitmap *makeBitmap(const CanvasImage &img) {
    ID2D1Bitmap *bmp = nullptr;
    // D2D1 bitmaps require BGRA byte order AND premultiplied alpha for
    // CreateBitmap on an ID2D1RenderTarget (straight alpha is rejected
    // with D2DERR_UNSUPPORTED_PIXEL_FORMAT for this creation path).
    const size_t n = static_cast<size_t>(img.width) * img.height * 4;
    if (img.pixels.size() < n)
      return nullptr; // malformed image; don't read past the buffer
    std::vector<uint8_t> bgra(n);
    // Raw pointers (not vector operator[]) so an unoptimized Debug build
    // doesn't pay a checked function call per byte; and an opaque pixel
    // (a == 255, i.e. every pixel of a typical photo) skips the
    // premultiply math — b * 255 / 255 == b, so the result is identical.
    const uint8_t *src = img.pixels.data();
    uint8_t *dst = bgra.data();
    for (size_t i = 0; i + 3 < n; i += 4) {
      uint8_t r = src[i + 0];
      uint8_t g = src[i + 1];
      uint8_t b = src[i + 2];
      uint8_t a = src[i + 3];
      if (a == 255) {
        dst[i + 0] = b;
        dst[i + 1] = g;
        dst[i + 2] = r;
        dst[i + 3] = 255;
      } else {
        dst[i + 0] = static_cast<uint8_t>(b * a / 255);
        dst[i + 1] = static_cast<uint8_t>(g * a / 255);
        dst[i + 2] = static_cast<uint8_t>(r * a / 255);
        dst[i + 3] = a;
      }
    }
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    HRESULT hr = rt_->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(img.width),
                    static_cast<UINT32>(img.height)),
        bgra.data(), static_cast<UINT32>(img.width) * 4, props, &bmp);
    return FAILED(hr) ? nullptr : bmp;
  }
  TextStyle fontToTextStyle() const {
    TextStyle ts = font_;
    ts.wrap = TextWrap::NoWrap;
    return ts;
  }

  // maxWidth is accepted for API familiarity with HTML canvas but not
  // currently applied (no horizontal squeeze-to-fit is performed).
  void drawTextImpl(const std::string &text, float x, float y, float,
                    bool stroked) {
    TextStyle ts = fontToTextStyle();
    IDWriteTextLayout *layout = liteui_text::makeLayout(
        text, ts, resolveStatic(ts.fontSize), resolveStatic(ts.fontWeight),
        resolveStatic(ts.fontStyle), 1.0e6f, 1.0e6f);
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    float drawX = x, drawY = y;
    switch (textAlign_) {
    case TextAlign::Center:
      drawX -= m.widthIncludingTrailingWhitespace / 2.0f;
      break;
    case TextAlign::End:
      drawX -= m.widthIncludingTrailingWhitespace;
      break;
    default:
      break;
    }
    switch (textBaseline_) {
    case TextBaseline::Top:
      break;
    case TextBaseline::Middle:
      drawY -= m.height / 2.0f;
      break;
    case TextBaseline::Bottom:
      drawY -= m.height;
      break;
    case TextBaseline::Alphabetic:
    default:
      // Approximate ascent as 80% of line height (DirectWrite line
      // metrics would give an exact figure; kept simple here).
      drawY -= m.height * 0.8f;
      break;
    }
    rt_->SetTransform(transform_);
    auto paintOnce = [&](Color c) {
      ID2D1SolidColorBrush *brush = nullptr;
      rt_->CreateSolidColorBrush(D2D1::ColorF(c.r / 255.0f, c.g / 255.0f,
                                              c.b / 255.0f,
                                              (c.a / 255.0f) * globalAlpha_),
                                 &brush);
      if (brush) {
        rt_->DrawTextLayout(D2D1::Point2F(drawX, drawY), layout, brush);
        brush->Release();
      }
    };
    if (shadowColor_.a > 0 && (shadowOffsetX_ != 0 || shadowOffsetY_ != 0)) {
      D2D1_MATRIX_3X2_F saved = transform_;
      rt_->SetTransform(
          D2D1::Matrix3x2F::Translation(shadowOffsetX_, shadowOffsetY_) *
          transform_);
      paintOnce(shadowColor_);
      rt_->SetTransform(saved);
    }
    if (!stroked) {
      paintOnce(colorOf(fillPaint_));
    } else {
      // Approximates a stroked glyph outline by drawing the fill text in
      // the stroke color, nudged across a ring of directions — not a
      // true outline of the glyph contours (see Canvas limitations note).
      Color base = colorOf(strokePaint_);
      float r = std::max(1.0f, lineWidth_ / 2.0f);
      static const float offs[8][2] = {
          {1, 0},       {-1, 0},       {0, 1},        {0, -1},
          {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
      for (auto &o : offs) {
        D2D1_MATRIX_3X2_F saved = transform_;
        rt_->SetTransform(D2D1::Matrix3x2F::Translation(o[0] * r, o[1] * r) *
                          transform_);
        paintOnce(base);
        rt_->SetTransform(saved);
      }
    }
    layout->Release();
  }

  // Maps globalCompositeOperation onto Direct2D's primitive blend modes
  // (the only three it has that correspond to a canvas operator).
  void applyBlend() {
    D2D1_PRIMITIVE_BLEND b = D2D1_PRIMITIVE_BLEND_SOURCE_OVER;
    if (composite_ == CompositeOp::Copy)
      b = D2D1_PRIMITIVE_BLEND_COPY;
    else if (composite_ == CompositeOp::Lighter)
      b = D2D1_PRIMITIVE_BLEND_ADD;
    dc_->SetPrimitiveBlend(b);
  }

  // Not owned. rt_ and dc_ are the same object: rt_ is its
  // ID2D1RenderTarget face (what all the v1-style drawing calls above go
  // through), dc_ its ID2D1DeviceContext face (primitive blend, 1.1
  // bitmaps).
  ID2D1RenderTarget *rt_;
  ID2D1DeviceContext *dc_;
  // Not owned — the canvas node's GPU texture (Computed::canvasBitmap),
  // which outlives any single CanvasContext.
  ID2D1Bitmap1 *target_;
  ID2D1Factory *factory_ = nullptr;
  float width_, height_;
  // Not owned — lifetime is the canvas node's own Computed (see the
  // constructor's comment). Null when there's no per-node cache to use.
  const View::Computed *imageCacheOwner_ = nullptr;

  D2D1_MATRIX_3X2_F transform_ = D2D1::Matrix3x2F::Identity();
  Paint fillPaint_ = Paint::solid(Color{0, 0, 0, 255});
  Paint strokePaint_ = Paint::solid(Color{0, 0, 0, 255});
  float lineWidth_ = 1.0f;
  LineCap lineCap_ = LineCap::Butt;
  LineJoin lineJoin_ = LineJoin::Miter;
  float miterLimit_ = 10.0f;
  std::vector<float> dashes_;
  float dashOffset_ = 0.0f;
  float globalAlpha_ = 1.0f;
  CompositeOp composite_ = CompositeOp::SourceOver;
  Color shadowColor_{0, 0, 0, 0};
  float shadowOffsetX_ = 0.0f, shadowOffsetY_ = 0.0f;
  TextStyle font_;
  TextAlign textAlign_ = TextAlign::Start;
  TextBaseline textBaseline_ = TextBaseline::Alphabetic;

  std::vector<Seg> segs_;
  std::vector<bool> figureClosed_; // one entry per Move, parallel order
  float curX_ = 0, curY_ = 0, startX_ = 0, startY_ = 0;

  std::vector<ID2D1Layer *> clipLayers_;
  std::vector<State> stack_;
};

#elif defined(LITEUI_CANVAS_GL)

// ---------------- Canvas 2D context: OpenGL ES 3.0 (Linux, opt-in) ----------
//
// Compiled instead of the Cairo backend below when LITEUI_CANVAS_GL is
// defined (CMake: -DLITEUI_CANVAS_GL=ON). Every Canvas node renders into its
// own GPU texture through an FBO; nothing is rasterized on the CPU and no
// pixels are uploaded. Work in progress — see the status list in
// liteui_canvas_gl::Renderer's comment. Operations that are not implemented
// yet print a one-time warning to stderr and draw nothing; they never
// silently pretend to work.
namespace liteui_canvas_gl {

// One vertex. Position is in canvas pixels (y down, already transformed by
// the CTM on the CPU so that draws with different transforms can share one
// batch). The fragment colour is  paint * (r,g,b,a):  for a solid paint the
// paint is 1 and (r,g,b,a) is the premultiplied colour; for textures and
// gradients (r,g,b,a) is just the premultiplied globalAlpha. u,v are
// texture coordinates (0,0 = first row of the image), unused by other paints.
struct Vertex {
  float x, y;
  float r, g, b, a;
  float u = 0, v = 0;
};

// ---- pure CPU helpers (no GL; unit-testable) ----------------------------

// Inverse of the affine map x' = a*x + c*y + e, y' = b*x + d*y + f, written
// as a column-major 3x3 for glUniformMatrix3fv. False if it is singular.
inline bool invert2x3(float a, float b, float c, float d, float e, float f,
                      float out[9]) {
  float det = a * d - b * c;
  if (!(std::fabs(det) > 1e-12f))
    return false;
  float id = 1.0f / det;
  float ia = d * id, ib = -b * id, ic = -c * id, idd = a * id;
  float ie = -(ia * e + ic * f), iff = -(ib * e + idd * f);
  const float m[9] = {ia, ib, 0, ic, idd, 0, ie, iff, 1};
  std::copy(m, m + 9, out);
  return true;
}

// Gradient colour ramp: kRampSize premultiplied RGBA8 texels sampled by the
// shader with t in [0,1]. Colours interpolate in straight (non-premultiplied)
// RGBA and are premultiplied afterwards, like Cairo/pixman. Before the first
// stop and after the last the end colours extend. No stops -> transparent.
constexpr int kRampSize = 1024;
inline void buildRamp(const CanvasGradient &g, uint8_t *out) {
  std::vector<CanvasGradientStop> s = g.stops;
  for (auto &st : s)
    st.offset = std::clamp(st.offset, 0.0f, 1.0f);
  std::stable_sort(s.begin(), s.end(),
                   [](const CanvasGradientStop &a, const CanvasGradientStop &b) {
                     return a.offset < b.offset;
                   });
  for (int i = 0; i < kRampSize; ++i) {
    float t = static_cast<float>(i) / (kRampSize - 1);
    float c[4] = {0, 0, 0, 0};
    if (!s.empty()) {
      size_t j = 0; // first stop with offset > t
      while (j < s.size() && s[j].offset <= t)
        ++j;
      auto col = [](const Color &k, float *o) {
        o[0] = k.r;
        o[1] = k.g;
        o[2] = k.b;
        o[3] = k.a;
      };
      if (j == 0) {
        col(s.front().color, c);
      } else if (j == s.size()) {
        col(s.back().color, c);
      } else {
        float l[4], r[4];
        col(s[j - 1].color, l);
        col(s[j].color, r);
        float span = s[j].offset - s[j - 1].offset;
        float k = span > 0 ? (t - s[j - 1].offset) / span : 1.0f;
        for (int q = 0; q < 4; ++q)
          c[q] = l[q] + (r[q] - l[q]) * k;
      }
    }
    float a = c[3] / 255.0f;
    out[i * 4 + 0] = static_cast<uint8_t>(std::lround(c[0] * a));
    out[i * 4 + 1] = static_cast<uint8_t>(std::lround(c[1] * a));
    out[i * 4 + 2] = static_cast<uint8_t>(std::lround(c[2] * a));
    out[i * 4 + 3] = static_cast<uint8_t>(std::lround(c[3]));
  }
}

// Straight RGBA8 -> premultiplied RGBA8 (what the GPU textures hold).
inline void premultiplyImage(const CanvasImage &img,
                             std::vector<uint8_t> &out) {
  size_t n = static_cast<size_t>(img.width) * img.height;
  out.resize(n * 4);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t *s = &img.pixels[i * 4];
    uint8_t *d = &out[i * 4];
    d[0] = static_cast<uint8_t>((s[0] * s[3] + 127) / 255);
    d[1] = static_cast<uint8_t>((s[1] * s[3] + 127) / 255);
    d[2] = static_cast<uint8_t>((s[2] * s[3] + 127) / 255);
    d[3] = s[3];
  }
}

// Blend factors for each globalCompositeOperation (premultiplied alpha;
// "source" = the shape being drawn, "destination" = the canvas so far).
// Kept as plain data so the algebra can be unit-tested against the W3C
// compositing formulas without a GL context; the Renderer maps them to GL.
// Multiply is not expressible with blend factors (it needs both the source
// and destination colour in one product plus extra terms), so the shader
// computes it from a copy of the destination and blending just replaces.
enum class BlendFactor {
  Zero,
  One,
  SrcAlpha,
  OneMinusSrcAlpha,
  DstAlpha,
  OneMinusDstAlpha,
  DstColor,
  OneMinusDstColor
};
struct BlendFactors {
  BlendFactor srcRgb, dstRgb, srcA, dstA;
};
inline BlendFactors blendFactorsFor(CompositeOp op) {
  using F = BlendFactor;
  auto same = [](F s, F d) { return BlendFactors{s, d, s, d}; };
  switch (op) {
  case CompositeOp::SourceIn:
    return same(F::DstAlpha, F::Zero);
  case CompositeOp::SourceOut:
    return same(F::OneMinusDstAlpha, F::Zero);
  case CompositeOp::SourceAtop:
    return same(F::DstAlpha, F::OneMinusSrcAlpha);
  case CompositeOp::DestinationOver:
    return same(F::OneMinusDstAlpha, F::One);
  case CompositeOp::DestinationIn:
    return same(F::Zero, F::SrcAlpha);
  case CompositeOp::DestinationOut:
    return same(F::Zero, F::OneMinusSrcAlpha);
  case CompositeOp::DestinationAtop:
    return same(F::OneMinusDstAlpha, F::SrcAlpha);
  case CompositeOp::Lighter:
    return same(F::One, F::One);
  case CompositeOp::Copy:
  case CompositeOp::Multiply: // shader writes the finished colour: replace
    return same(F::One, F::Zero);
  case CompositeOp::Xor:
    return same(F::OneMinusDstAlpha, F::OneMinusSrcAlpha);
  case CompositeOp::Screen:
    // Cs + Cb - Cs*Cb = Cs*(1-Cb) + Cb;  alpha: as + ab - as*ab likewise.
    return {F::OneMinusDstColor, F::One, F::OneMinusDstAlpha, F::One};
  case CompositeOp::SourceOver:
  default:
    return same(F::One, F::OneMinusSrcAlpha);
  }
}

// True if `text` contains characters that commonly render as colour glyphs
// (emoji, pictographs, flags, variation selector-16, zero-width joiner).
// Only such strings pay for the colour (RGBA) rasterization path. A false
// positive is harmless: the result looks identical to the normal path.
inline bool mayHaveColorGlyphs(const std::string &s) {
  const size_t n = s.size();
  for (size_t i = 0; i < n;) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    size_t len = 1;
    if (c >= 0xF0 && c < 0xF8)
      len = 4, cp = c & 0x07;
    else if (c >= 0xE0)
      len = 3, cp = c & 0x0F;
    else if (c >= 0xC0)
      len = 2, cp = c & 0x1F;
    if (c >= 0x80 && (len == 1 || i + len > n)) { // malformed: skip a byte
      ++i;
      continue;
    }
    for (size_t k = 1; k < len; ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += len;
    if ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
        (cp >= 0x2300 && cp <= 0x23FF) || (cp >= 0x2B00 && cp <= 0x2BFF) ||
        cp == 0xFE0F || cp == 0x200D || cp == 0x00A9 || cp == 0x00AE ||
        cp == 0x203C || cp == 0x2049 || cp == 0x2122 || cp == 0x2139)
      return true;
  }
  return false;
}

// ---- text ---------------------------------------------------------------
// A shaped string rasterized to an 8-bit coverage bitmap by Pango/Cairo.
// The bitmap includes a transparent border so bilinear sampling can't pick
// up a neighbour's pixels. (offX, offY) is the bitmap's top-left corner in
// user units relative to the layout origin (the layout's own top-left);
// advance/height/baseline are the layout's size and baseline in user units
// (the same numbers the other backends use to place text).
struct RasterText {
  int w = 0, h = 0; // 0 x 0: the string inks nothing (e.g. only spaces)
  // Exactly one of these is filled. a8: 8-bit coverage, tinted by the paint
  // when drawn. rgba (color == true): premultiplied RGBA8 with the fill
  // colour already baked in, so colour glyphs (emoji) keep their own colours.
  std::vector<uint8_t> a8;
  bool color = false;
  std::vector<uint8_t> rgba;
  float offX = 0, offY = 0;
  float advance = 0, height = 0, baseline = 0;
};
// Where a RasterText lives in the atlas, plus the placement numbers above.
struct TextEntry {
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  int w = 0, h = 0;
  bool color = false; // lives in the colour atlas
  float offX = 0, offY = 0, advance = 0, height = 0, baseline = 0;
};
struct OutlineContour {
  std::vector<float> xy; // x0,y0,x1,y1,... relative to the layout origin
  bool closed = false;
};
struct TextBox {
  float advance = 0, height = 0, baseline = 0;
};

// Shelf packer for the glyph atlas: rectangles go left to right along the
// current shelf; when one doesn't fit a new shelf starts below the tallest
// rectangle of the previous one. Simple, and wasteful only when heights vary
// wildly. reset() makes the whole area free again.
struct ShelfPacker {
  int size = 0, cursorX = 0, shelfY = 0, shelfH = 0;
  void reset() { cursorX = shelfY = shelfH = 0; }
  bool allocate(int w, int h, int &x, int &y) {
    if (w <= 0 || h <= 0 || w > size || h > size)
      return false;
    if (cursorX + w > size) {
      shelfY += shelfH;
      shelfH = 0;
      cursorX = 0;
    }
    if (shelfY + h > size)
      return false;
    x = cursorX;
    y = shelfY;
    cursorX += w;
    shelfH = std::max(shelfH, h);
    return true;
  }
};

// BEGIN-PANGO-CAIRO — the only code that talks to Pango/Cairo for canvas
// text; everything else sees RasterText/OutlineContour.

// Shapes and rasterizes `text` at `scale` device pixels per user unit
// (scale 1 = unscaled). Layout, fonts and metrics come from the same
// liteui_text::makeLayout as every other text in the library, so shaping,
// fallback fonts and line metrics match. With bake == nullptr the result is
// an 8-bit coverage mask; with a colour it is a premultiplied RGBA bitmap in
// which ordinary glyphs are drawn in that colour and colour glyphs (emoji)
// in their own. False if the bitmap would be unreasonably large.
inline bool rasterizeText(const std::string &text, const TextStyle &ts,
                          float scale, const Color *bake, RasterText &out) {
  constexpr int kPad = 2, kMaxDim = 1024;
  cairo_surface_t *probeSurf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cairo_t *probe = cairo_create(probeSurf);
  cairo_scale(probe, scale, scale);
  PangoLayout *layout = liteui_text::makeLayout(
      text, ts, resolveStatic(ts.fontSize), resolveStatic(ts.fontWeight),
      resolveStatic(ts.fontStyle), -1, probe);
  PangoRectangle ink, logical;
  pango_layout_get_extents(layout, &ink, &logical);
  int pw = 0, ph = 0;
  pango_layout_get_pixel_size(layout, &pw, &ph);
  PangoLayoutIter *iter = pango_layout_get_iter(layout);
  int baseline = pango_layout_iter_get_baseline(iter) / PANGO_SCALE;
  pango_layout_iter_free(iter);
  out = RasterText{};
  out.advance = static_cast<float>(pw);
  out.height = static_cast<float>(ph);
  out.baseline = static_cast<float>(baseline);
  bool ok = true;
  if (ink.width > 0 && ink.height > 0) {
    // Whole-pixel box around the ink, in device pixels. The layout origin is
    // placed on a whole device pixel inside the bitmap so glyphs keep the
    // same pixel alignment they would have when drawn directly.
    const float k = 1.0f / PANGO_SCALE;
    int x0 = static_cast<int>(std::floor(ink.x * k * scale));
    int y0 = static_cast<int>(std::floor(ink.y * k * scale));
    int x1 = static_cast<int>(std::ceil((ink.x + ink.width) * k * scale));
    int y1 = static_cast<int>(std::ceil((ink.y + ink.height) * k * scale));
    int bw = x1 - x0 + 2 * kPad, bh = y1 - y0 + 2 * kPad;
    if (bw > kMaxDim || bh > kMaxDim) {
      ok = false;
    } else {
      const bool color = bake != nullptr;
      cairo_surface_t *surf = cairo_image_surface_create(
          color ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_A8, bw, bh);
      cairo_t *cr = cairo_create(surf);
      if (color) {
        // Greyscale antialiasing only: LCD subpixel fringes would be baked
        // into the bitmap as coloured edges.
        cairo_font_options_t *fo = cairo_font_options_create();
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
        cairo_set_font_options(cr, fo);
        cairo_font_options_destroy(fo);
      }
      cairo_translate(cr, kPad - x0, kPad - y0);
      cairo_scale(cr, scale, scale);
      pango_cairo_update_layout(cr, layout);
      if (color)
        cairo_set_source_rgba(cr, bake->r / 255.0, bake->g / 255.0,
                              bake->b / 255.0, bake->a / 255.0);
      else
        cairo_set_source_rgba(cr, 1, 1, 1, 1); // alpha only: A8 stores coverage
      cairo_move_to(cr, 0, 0);
      pango_cairo_show_layout(cr, layout);
      cairo_surface_flush(surf);
      const unsigned char *data = cairo_image_surface_get_data(surf);
      const int stride = cairo_image_surface_get_stride(surf);
      out.w = bw;
      out.h = bh;
      out.color = color;
      if (color) {
        // Cairo ARGB32 is premultiplied, stored B,G,R,A on little-endian
        // machines (a native 32-bit word otherwise); read it as a word and
        // write RGBA bytes.
        out.rgba.resize(static_cast<size_t>(bw) * bh * 4);
        for (int r = 0; r < bh; ++r) {
          const uint32_t *row = reinterpret_cast<const uint32_t *>(
              data + static_cast<size_t>(r) * stride);
          uint8_t *dst = &out.rgba[static_cast<size_t>(r) * bw * 4];
          for (int x = 0; x < bw; ++x) {
            uint32_t px = row[x];
            dst[x * 4 + 0] = static_cast<uint8_t>((px >> 16) & 0xFF);
            dst[x * 4 + 1] = static_cast<uint8_t>((px >> 8) & 0xFF);
            dst[x * 4 + 2] = static_cast<uint8_t>(px & 0xFF);
            dst[x * 4 + 3] = static_cast<uint8_t>((px >> 24) & 0xFF);
          }
        }
      } else {
        out.a8.resize(static_cast<size_t>(bw) * bh);
        for (int r = 0; r < bh; ++r)
          std::copy_n(data + static_cast<size_t>(r) * stride, bw,
                      out.a8.begin() + static_cast<size_t>(r) * bw);
      }
      out.offX = static_cast<float>(x0 - kPad) / scale;
      out.offY = static_cast<float>(y0 - kPad) / scale;
      cairo_destroy(cr);
      cairo_surface_destroy(surf);
    }
  }
  g_object_unref(layout);
  cairo_destroy(probe);
  cairo_surface_destroy(probeSurf);
  return ok;
}

// The glyph outlines of `text` as polylines (curves flattened to within
// `tolerance` user units), relative to the layout origin, plus the layout's
// size and baseline. This is what strokeText strokes.
inline bool outlineText(const std::string &text, const TextStyle &ts,
                        float tolerance, std::vector<OutlineContour> &out,
                        TextBox &box) {
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cairo_t *cr = cairo_create(surf);
  cairo_set_tolerance(cr, std::max(0.001f, tolerance));
  PangoLayout *layout = liteui_text::makeLayout(
      text, ts, resolveStatic(ts.fontSize), resolveStatic(ts.fontWeight),
      resolveStatic(ts.fontStyle), -1, cr);
  int pw = 0, ph = 0;
  pango_layout_get_pixel_size(layout, &pw, &ph);
  PangoLayoutIter *iter = pango_layout_get_iter(layout);
  int baseline = pango_layout_iter_get_baseline(iter) / PANGO_SCALE;
  pango_layout_iter_free(iter);
  box.advance = static_cast<float>(pw);
  box.height = static_cast<float>(ph);
  box.baseline = static_cast<float>(baseline);
  out.clear();
  cairo_move_to(cr, 0, 0);
  pango_cairo_layout_path(cr, layout);
  cairo_path_t *path = cairo_copy_path_flat(cr);
  for (int i = 0; i < path->num_data; i += path->data[i].header.length) {
    cairo_path_data_t *d = &path->data[i];
    switch (d->header.type) {
    case CAIRO_PATH_MOVE_TO:
      out.emplace_back();
      out.back().xy.push_back(static_cast<float>(d[1].point.x));
      out.back().xy.push_back(static_cast<float>(d[1].point.y));
      break;
    case CAIRO_PATH_LINE_TO:
      if (!out.empty()) {
        out.back().xy.push_back(static_cast<float>(d[1].point.x));
        out.back().xy.push_back(static_cast<float>(d[1].point.y));
      }
      break;
    case CAIRO_PATH_CLOSE_PATH:
      if (!out.empty())
        out.back().closed = true;
      break;
    default: // CURVE_TO can't occur in a flattened path
      break;
    }
  }
  cairo_path_destroy(path);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_destroy(surf);
  return true;
}
// END-PANGO-CAIRO

// Owns the GL objects shared by every canvas node (one program, one VAO,
// one streaming vertex buffer) and the per-frame batching state. Lives in
// LiteUIImpl, created after the EGL context is current and destroyed before
// it is released. Per-node GPU objects (texture + FBOs) live in the node's
// View::Computed and are created/freed by buildTarget/freeTarget.
//
// Implemented: per-node RGBA8 target (MSAA when available) + resolve,
// flat-colour triangle batching, CTM, globalAlpha, Porter-Duff blend modes,
// fillRect/clearRect/strokeRect, path recording, fill (NonZero/EvenOdd),
// stroke (caps, joins, miter limit, dashes), clip, flat shadows,
// measureText.
// gradients (linear, and radial with the full two-circle model),
// drawImage (cropped, bilinear, cached per node), putImageData,
// getImageData (GPU readback on request), fillText (Pango-shaped glyph
// bitmaps in a shared GPU atlas, tinted by any paint), strokeText (true
// glyph outlines through the GPU stroker).
// all twelve globalCompositeOperation modes (Screen and the Porter-Duff
// operators with blend factors; Multiply by reading back the canvas so far),
// colour (emoji) glyphs for solid fill colours.
// Notes: with a gradient fill, emoji draw as silhouettes tinted by the
// gradient (colour glyphs are baked with a solid colour).
//
// Stencil layout (8 bits per canvas target):
//   bits 7..4  clip level L (0 = unclipped, up to 15 nested clip() calls)
//   bits 3..0  scratch, used while rasterizing one fill/stroke/clip shape
// A shape is drawn in two passes ("stencil, then cover"): first its
// triangles update the scratch bits only where the clip level matches
// (winding count, parity, or plain coverage for strokes); then a covering
// quad paints (or raises the clip level) wherever the scratch bits are
// non-zero, and zeroes them again. This gives exact NonZero/EvenOdd fills
// and overlap-free strokes (no double blending at joins) with no CPU
// scan conversion. Limits: NonZero winding is counted in the 4-bit scratch
// nibble, modulo 16, so a point wound around 16 (or 32...) times by one path
// would drop out of a NonZero fill or clip; EvenOdd uses parity and strokes
// use a set-flag, so neither can wrap.
class Renderer {
public:
  static constexpr size_t kMaxVerts = 60000; // multiple of 3

  // Needs a current GLES 3.0 context. False (after printing the compiler
  // log) if the shader fails to build; the caller decides how to react.
  bool init() {
    if (prog_)
      return true;
    static constexpr const char *kVS = R"GLSL(#version 300 es
      layout(location = 0) in vec2 a_pos;
      layout(location = 1) in vec4 a_color;
      layout(location = 2) in vec2 a_uv;
      uniform vec2 u_size;   // canvas size in pixels
      out vec4 v_color;
      out vec2 v_uv;
      out vec2 v_pos;        // canvas pixels, for gradient evaluation
      void main() {
        v_color = a_color;
        v_uv = a_uv;
        v_pos = a_pos;
        // y is NOT flipped: canvas row 0 lands on framebuffer row 0, which
        // is texture row 0, which the window pass samples as the top.
        gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0,
                           a_pos.y / u_size.y * 2.0 - 1.0, 0.0, 1.0);
      }
    )GLSL";
    // One program for every paint. out = paint * v_color, where paint is 1
    // (solid), the texture sample (image) or the gradient ramp sample.
    static constexpr const char *kFS = R"GLSL(#version 300 es
      precision highp float;
      in vec4 v_color;
      in vec2 v_uv;
      in vec2 v_pos;
      uniform int u_mode;        // 0 solid, 1 image, 2 linear, 3 radial
      uniform sampler2D u_tex;   // premultiplied image
      uniform sampler2D u_ramp;  // premultiplied gradient ramp, 1 x N
      uniform mat3 u_inv;        // device pixels -> gradient (user) space
      uniform vec4 u_g;          // x0, y0, x1, y1
      uniform vec2 u_r;          // r0, r1 (radial)
      uniform int u_mask;        // 0 off, 1 mono glyph, 2 colour glyph, 3 colour glyph silhouette
      uniform sampler2D u_atlas; // 8-bit glyph coverage
      uniform sampler2D u_catlas; // premultiplied RGBA colour glyphs
      uniform int u_blend;       // 1: Multiply (needs the backdrop)
      uniform sampler2D u_backdrop; // copy of the canvas so far (Multiply)
      out vec4 o_color;
      void main() {
        vec4 s = vec4(1.0);
        if (u_mode == 1) {
          s = texture(u_tex, v_uv);
        } else if (u_mode >= 2) {
          vec2 p = (u_inv * vec3(v_pos, 1.0)).xy;
          float t = 0.0;
          bool ok = false;
          if (u_mode == 2) {
            vec2 d = u_g.zw - u_g.xy;
            float dd = dot(d, d);
            if (dd > 0.0) {
              t = dot(p - u_g.xy, d) / dd;
              ok = true;
            }
          } else {
            // Two-circle gradient: the largest s with r(s) >= 0 such that p
            // lies on the circle centre c0 + s*(c1-c0), radius r0 + s*dr.
            vec2 cd = u_g.zw - u_g.xy;
            float dr = u_r.y - u_r.x;
            vec2 pd = p - u_g.xy;
            float a = dot(cd, cd) - dr * dr;
            float b = dot(pd, cd) + u_r.x * dr;
            float c = dot(pd, pd) - u_r.x * u_r.x;
            if (abs(a) < 1e-6) {
              if (abs(b) > 1e-6) {
                t = c / (2.0 * b);
                ok = (u_r.x + t * dr >= 0.0);
              }
            } else {
              float disc = b * b - a * c;
              if (disc >= 0.0) {
                float sq = sqrt(disc);
                float s1 = (b + sq) / a, s2 = (b - sq) / a;
                float hi = max(s1, s2), lo = min(s1, s2);
                if (u_r.x + hi * dr >= 0.0) { t = hi; ok = true; }
                else if (u_r.x + lo * dr >= 0.0) { t = lo; ok = true; }
              }
            }
          }
          if (ok) {
            float n = float(textureSize(u_ramp, 0).x);
            s = texture(u_ramp, vec2((clamp(t, 0.0, 1.0) * (n - 1.0) + 0.5) / n, 0.5));
          } else {
            s = vec4(0.0);
          }
        }
        if (u_mask == 1)
          s *= texture(u_atlas, v_uv).r;
        else if (u_mask == 2)
          s = texture(u_catlas, v_uv);       // own colours: paint is ignored
        else if (u_mask == 3)
          s *= texture(u_catlas, v_uv).a;    // shadow of a colour glyph
        vec4 o = s * v_color;
        if (u_blend == 1) {
          // Multiply, premultiplied: Cs(1-ab) + Cb(1-as) + Cs*Cb. Blending is
          // set to replace, so what we output is the final pixel.
          vec4 bd = texelFetch(u_backdrop, ivec2(gl_FragCoord.xy), 0);
          o = vec4(o.rgb * (1.0 - bd.a) + bd.rgb * (1.0 - o.a) + o.rgb * bd.rgb,
                   o.a + bd.a - o.a * bd.a);
        }
        o_color = o;
      }
    )GLSL";
    GLuint vs = compile(GL_VERTEX_SHADER, kVS);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, kFS) : 0;
    if (!vs || !fs) {
      if (vs)
        glDeleteShader(vs);
      return false;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[512];
      glGetProgramInfoLog(p, sizeof(log), nullptr, log);
      fprintf(stderr, "liteui canvas: program link error: %s\n", log);
      glDeleteProgram(p);
      return false;
    }
    prog_ = p;
    uSize_ = glGetUniformLocation(prog_, "u_size");
    uMode_ = glGetUniformLocation(prog_, "u_mode");
    uMask_ = glGetUniformLocation(prog_, "u_mask");
    uBlend_ = glGetUniformLocation(prog_, "u_blend");
    uInv_ = glGetUniformLocation(prog_, "u_inv");
    uG_ = glGetUniformLocation(prog_, "u_g");
    uR_ = glGetUniformLocation(prog_, "u_r");
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex_);
    {
      // Texture units: 0 = image, 1 = gradient ramp (never changes).
      GLint prevProg = 0, prevActive = 0, prevTex1 = 0;
      glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
      glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
      glUseProgram(prog_);
      glUniform1i(glGetUniformLocation(prog_, "u_tex"), 0);
      glUniform1i(glGetUniformLocation(prog_, "u_ramp"), 1);
      glUniform1i(glGetUniformLocation(prog_, "u_atlas"), 2);
      glUniform1i(glGetUniformLocation(prog_, "u_catlas"), 3);
      glUniform1i(glGetUniformLocation(prog_, "u_backdrop"), 4);
      glActiveTexture(GL_TEXTURE1);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex1);
      glGenTextures(1, &ramp_);
      glBindTexture(GL_TEXTURE_2D, ramp_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kRampSize, 1);
      // Unit 2: the glyph atlas (8-bit coverage), shared by every canvas.
      atlasSize_ = std::min<GLint>(2048, maxTex_);
      packer_.size = atlasSize_;
      GLint prevTex2 = 0;
      glActiveTexture(GL_TEXTURE2);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2);
      glGenTextures(1, &atlas_);
      glBindTexture(GL_TEXTURE_2D, atlas_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, atlasSize_, atlasSize_);
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex2));
      // Unit 3: the colour glyph atlas (premultiplied RGBA), smaller since
      // colour glyphs are rare.
      colorAtlasSize_ = std::min<GLint>(1024, maxTex_);
      cpacker_.size = colorAtlasSize_;
      GLint prevTex3 = 0;
      glActiveTexture(GL_TEXTURE3);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex3);
      glGenTextures(1, &colorAtlas_);
      glBindTexture(GL_TEXTURE_2D, colorAtlas_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, colorAtlasSize_, colorAtlasSize_);
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex3));
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex1));
      glActiveTexture(static_cast<GLenum>(prevActive));
      glUseProgram(static_cast<GLuint>(prevProg));
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    GLint prevVao = 0, prevBuf = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, kMaxVerts * sizeof(Vertex), nullptr,
                 GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void *>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void *>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void *>(6 * sizeof(float)));
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevBuf));
    batch_.reserve(kMaxVerts);
    return true;
  }

  // Must run while the context is still current.
  void destroy() {
    if (vbo_)
      glDeleteBuffers(1, &vbo_);
    if (vao_)
      glDeleteVertexArrays(1, &vao_);
    if (ramp_)
      glDeleteTextures(1, &ramp_);
    if (atlas_)
      glDeleteTextures(1, &atlas_);
    if (colorAtlas_)
      glDeleteTextures(1, &colorAtlas_);
    if (backdrop_)
      glDeleteTextures(1, &backdrop_);
    if (prog_)
      glDeleteProgram(prog_);
    vbo_ = vao_ = prog_ = ramp_ = atlas_ = colorAtlas_ = backdrop_ = 0;
    backdropW_ = backdropH_ = 0;
    texts_.clear();
  }

  // The context is gone and every name with it: forget them WITHOUT touching
  // GL (a stale name could collide with an object of the replacement
  // context), then init() can run again on the new context.
  void abandon() {
    prog_ = vao_ = vbo_ = ramp_ = atlas_ = colorAtlas_ = backdrop_ = 0;
    backdropW_ = backdropH_ = 0;
    uSize_ = uMode_ = uInv_ = uG_ = uR_ = uMask_ = uBlend_ = -1;
    paintKind_ = PaintKind::None;
    maskMode_ = -1;
    cur_ = nullptr;
    tex_ = 0;
    trash_.clear();
    batch_.clear();
    texts_.clear();
    packer_.reset();
    cpacker_.reset();
  }

  bool ready() const { return prog_ != 0; }

  // ---- per-node GPU target ------------------------------------------
  // The node's pixels live in c.canvasTexture (RGBA8, premultiplied). It is
  // wrapped by c.canvasFbo. If the driver offers multisampling, drawing goes
  // to c.canvasMsaaFbo (4x MSAA colour + depth/stencil renderbuffers) and is
  // resolved into the texture by end(); otherwise drawing goes straight to
  // c.canvasFbo, which then owns the depth/stencil renderbuffer.
  static void freeTarget(const View::Computed &c) {
    GLuint v;
    if ((v = c.canvasFbo)) {
      glDeleteFramebuffers(1, &v);
      c.canvasFbo = 0;
    }
    if ((v = c.canvasMsaaFbo)) {
      glDeleteFramebuffers(1, &v);
      c.canvasMsaaFbo = 0;
    }
    if ((v = c.canvasMsaaColorRb)) {
      glDeleteRenderbuffers(1, &v);
      c.canvasMsaaColorRb = 0;
    }
    if ((v = c.canvasDsRb)) {
      glDeleteRenderbuffers(1, &v);
      c.canvasDsRb = 0;
    }
    if ((v = c.canvasTexture)) {
      glDeleteTextures(1, &v);
      c.canvasTexture = 0;
    }
  }

  static bool buildTarget(const View::Computed &c, int w, int h) {
    freeTarget(c);
    GLint maxRb = 0, maxSamples = 0;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRb);
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    if (w < 1 || h < 1 || w > maxRb || h > maxRb)
      return false;
    int samples = std::min<GLint>(4, maxSamples);

    GLint prevFbo = 0, prevTex = 0;
    GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat prevClear[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h); // immutable storage
    c.canvasTexture = tex;

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    c.canvasFbo = fbo;
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
              GL_FRAMEBUFFER_COMPLETE;

    bool msaa = false;
    if (ok && samples >= 2) {
      GLuint mfbo = 0, mcolor = 0, mds = 0;
      glGenFramebuffers(1, &mfbo);
      glGenRenderbuffers(1, &mcolor);
      glGenRenderbuffers(1, &mds);
      glBindRenderbuffer(GL_RENDERBUFFER, mcolor);
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w,
                                       h);
      glBindRenderbuffer(GL_RENDERBUFFER, mds);
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                       GL_DEPTH24_STENCIL8, w, h);
      glBindFramebuffer(GL_FRAMEBUFFER, mfbo);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_RENDERBUFFER, mcolor);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, mds);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
          GL_FRAMEBUFFER_COMPLETE) {
        c.canvasMsaaFbo = mfbo;
        c.canvasMsaaColorRb = mcolor;
        c.canvasDsRb = mds;
        msaa = true;
      } else {
        glDeleteFramebuffers(1, &mfbo);
        glDeleteRenderbuffers(1, &mcolor);
        glDeleteRenderbuffers(1, &mds);
      }
    }
    if (ok && !msaa) {
      GLuint ds = 0;
      glGenRenderbuffers(1, &ds);
      glBindRenderbuffer(GL_RENDERBUFFER, ds);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, ds);
      c.canvasDsRb = ds;
      ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (ok) {
      // New storage has undefined contents: start transparent so a canvas
      // without an onPaint shows nothing instead of garbage.
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glDisable(GL_SCISSOR_TEST);
      glClearColor(0, 0, 0, 0);
      glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
    if (prevScissor)
      glEnable(GL_SCISSOR_TEST);
    if (!ok)
      freeTarget(c);
    return ok;
  }

  // ---- one canvas redraw -------------------------------------------
  // Saves the window renderer's GL state, binds `c`'s draw FBO, clears it to
  // transparent and prepares for batched drawing. Pair with end().
  void begin(const View::Computed &c, int w, int h) {
    saved_.scissor = glIsEnabled(GL_SCISSOR_TEST);
    saved_.blend = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_.fbo);
    glGetIntegerv(GL_VIEWPORT, saved_.viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved_.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_.vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_.arrayBuf);
    glGetIntegerv(GL_BLEND_SRC_RGB, &saved_.srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &saved_.dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &saved_.srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &saved_.dstA);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, saved_.clear);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_.activeTex);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_.tex1);
    glBindTexture(GL_TEXTURE_2D, ramp_);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_.tex2);
    glBindTexture(GL_TEXTURE_2D, atlas_);
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_.tex3);
    glBindTexture(GL_TEXTURE_2D, colorAtlas_);
    glActiveTexture(GL_TEXTURE4);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_.tex4);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_.tex0);
    cur_ = &c;

    w_ = w;
    h_ = h;
    batch_.clear();
    glBindFramebuffer(GL_FRAMEBUFFER,
                      c.canvasMsaaFbo ? c.canvasMsaaFbo : c.canvasFbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF); // so the clear below also wipes the stencil
    mode_ = Mode::Off;
    level_ = 0;
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_BLEND);
    glUseProgram(prog_);
    glUniform2f(uSize_, static_cast<float>(w), static_cast<float>(h));
    glBindVertexArray(vao_);
    op_ = CompositeOp::SourceOver;
    applyBlend(op_);
    glUniform1i(uBlend_, 0);
    paintKind_ = PaintKind::None;
    setPaintSolid();
    maskMode_ = -1; // force the uniform to be written
    setMask(0);
  }

  // Flushes, resolves MSAA into the node's texture and restores the window
  // renderer's GL state exactly as begin() found it.
  void end(const View::Computed &c) {
    flush();
    if (!trash_.empty()) { // textures retired during this pass (safe now)
      glDeleteTextures(static_cast<GLsizei>(trash_.size()), trash_.data());
      trash_.clear();
    }
    if (c.canvasMsaaFbo) {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, c.canvasMsaaFbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c.canvasFbo);
      glBlitFramebuffer(0, 0, w_, h_, 0, 0, w_, h_, GL_COLOR_BUFFER_BIT,
                        GL_NEAREST);
    }
    // The window renderer never uses stencil or colour masks; leave them in
    // their defaults.
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    cur_ = nullptr;
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_.tex4));
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_.tex3));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_.tex2));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_.tex1));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_.tex0));
    glActiveTexture(static_cast<GLenum>(saved_.activeTex));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(saved_.fbo));
    glViewport(saved_.viewport[0], saved_.viewport[1], saved_.viewport[2],
               saved_.viewport[3]);
    glUseProgram(static_cast<GLuint>(saved_.program));
    glBindVertexArray(static_cast<GLuint>(saved_.vao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(saved_.arrayBuf));
    glBlendFuncSeparate(static_cast<GLenum>(saved_.srcRgb),
                        static_cast<GLenum>(saved_.dstRgb),
                        static_cast<GLenum>(saved_.srcA),
                        static_cast<GLenum>(saved_.dstA));
    glClearColor(saved_.clear[0], saved_.clear[1], saved_.clear[2],
                 saved_.clear[3]);
    if (saved_.blend)
      glEnable(GL_BLEND);
    else
      glDisable(GL_BLEND);
    if (saved_.scissor)
      glEnable(GL_SCISSOR_TEST);
    else
      glDisable(GL_SCISSOR_TEST);
  }

  // ---- drawing (between begin() and end()) -------------------------
  // `n` must be a multiple of 3 (independent triangles).
  // Large submissions are split into batch-sized pieces (always on triangle
  // boundaries), so one big path can't overflow the vertex buffer.
  void draw(const Vertex *v, size_t n) {
    while (n > 0) {
      size_t room = kMaxVerts - batch_.size();
      room -= room % 3;
      if (room == 0) {
        flush();
        continue;
      }
      size_t take = std::min(n, room);
      batch_.insert(batch_.end(), v, v + take);
      v += take;
      n -= take;
      // Multiply reads the canvas back per draw call, so every shape must be
      // its own call or later shapes would not see earlier ones.
      if (op_ == CompositeOp::Multiply && writesColor(mode_))
        flush();
    }
  }

  // What the stencil/colour state is currently set up to do (see the stencil
  // layout in the class comment). Changing it ends the current batch.
  enum class Mode {
    Off,          // plain colour drawing, no stencil
    ClipTest,     // colour drawing, only where the clip level == `level`
    CountWinding, // scratch bits += winding of front/back faces (NonZero)
    CountEvenOdd, // scratch bits ^= 1 per triangle (EvenOdd)
    CountUnion,   // scratch flag set by any triangle (strokes: union of pieces)
    CoverFill,    // colour where scratch != 0, then zero the scratch
    CoverClip,    // raise level -> level+1 where scratch != 0
    ReduceClip    // lower every pixel above `level` back to `level`
  };

  void setMode(Mode m, int level) {
    // ClipTest at level 0 is the same state as Off.
    if (m == Mode::ClipTest && level == 0)
      m = Mode::Off;
    if (m == mode_ && level == level_)
      return;
    flush();
    mode_ = m;
    level_ = level;
    const GLint ref = static_cast<GLint>(level) << 4;
    switch (m) {
    case Mode::Off:
      glDisable(GL_STENCIL_TEST);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      break;
    case Mode::ClipTest:
      glEnable(GL_STENCIL_TEST);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glStencilMask(0x00);
      glStencilFunc(GL_EQUAL, ref, 0xF0);
      glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
      break;
    case Mode::CountWinding:
    case Mode::CountEvenOdd:
    case Mode::CountUnion: {
      glEnable(GL_STENCIL_TEST);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glStencilMask(0x0F);
      // Only touch pixels inside the clip. For the union mode the reference
      // value's low nibble (1) is what REPLACE writes, while the compare mask
      // 0xF0 ignores it.
      glStencilFunc(GL_EQUAL, m == Mode::CountUnion ? (ref | 1) : ref, 0xF0);
      GLenum front = GL_INCR_WRAP, back = GL_INCR_WRAP;
      if (m == Mode::CountWinding)
        back = GL_DECR_WRAP;
      else if (m == Mode::CountEvenOdd)
        front = back = GL_INVERT;
      else // CountUnion: set a flag; never counts, so it can never wrap
        front = back = GL_REPLACE;
      glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, front);
      glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, back);
      break;
    }
    case Mode::CoverFill:
      glEnable(GL_STENCIL_TEST);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glStencilMask(0x0F);
      glStencilFunc(GL_NOTEQUAL, 0, 0x0F);
      glStencilOp(GL_KEEP, GL_ZERO, GL_ZERO);
      break;
    case Mode::CoverClip:
      glEnable(GL_STENCIL_TEST);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glStencilMask(0xFF);
      // Test looks at the scratch nibble only (ref's low nibble is 0);
      // REPLACE then writes the whole byte: new level, scratch cleared.
      glStencilFunc(GL_NOTEQUAL, (level + 1) << 4, 0x0F);
      glStencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
      break;
    case Mode::ReduceClip:
      glEnable(GL_STENCIL_TEST);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glStencilMask(0xFF);
      glStencilFunc(GL_LESS, ref, 0xF0); // level bits > `level`
      glStencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
      break;
    }
  }

  // ---- paint ---------------------------------------------------------
  // Selects what colour the following triangles get (see Vertex). Changing
  // the paint ends the current batch.
  void setPaintSolid() {
    if (paintKind_ == PaintKind::Solid)
      return;
    flush();
    paintKind_ = PaintKind::Solid;
    glUniform1i(uMode_, 0);
  }
  void setPaintTexture(GLuint tex) {
    if (paintKind_ == PaintKind::Texture && tex_ == tex)
      return;
    flush();
    paintKind_ = PaintKind::Texture;
    tex_ = tex;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(uMode_, 1);
  }
  // `inv` = column-major device->user matrix (see invert2x3).
  void setPaintGradient(const CanvasGradient &g, const float inv[9]) {
    if (paintKind_ == PaintKind::Gradient && sameGradient(g, inv))
      return;
    flush();
    paintKind_ = PaintKind::Gradient;
    grad_ = g;
    std::copy(inv, inv + 9, inv_);
    rampBuf_.resize(static_cast<size_t>(kRampSize) * 4);
    buildRamp(g, rampBuf_.data());
    glActiveTexture(GL_TEXTURE1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kRampSize, 1, GL_RGBA,
                    GL_UNSIGNED_BYTE, rampBuf_.data());
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uMode_, g.kind == CanvasGradient::Kind::Linear ? 2 : 3);
    glUniformMatrix3fv(uInv_, 1, GL_FALSE, inv);
    glUniform4f(uG_, g.x0, g.y0, g.x1, g.y1);
    glUniform2f(uR_, g.r0, g.r1);
  }

  // ---- images --------------------------------------------------------
  // Uploads `img` (straight RGBA8) as a premultiplied RGBA8 texture, so
  // bilinear filtering doesn't bleed colour out of transparent pixels.
  // 0 if it is too large for the GPU or its pixel buffer is too short.
  GLuint uploadImage(const CanvasImage &img) {
    if (img.width <= 0 || img.height <= 0 || img.width > maxTex_ ||
        img.height > maxTex_ ||
        img.pixels.size() < static_cast<size_t>(img.width) * img.height * 4)
      return 0;
    flush(); // binding a texture below must not affect queued draws
    premultiplyImage(img, upBuf_);
    GLuint t = 0;
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, upBuf_.data());
    paintKind_ = PaintKind::None; // unit 0's binding changed under us
    return t;
  }
  // Frees a texture once the current pass has been flushed (it may still be
  // referenced by queued draws until then).
  void deferDelete(GLuint t) { trash_.push_back(t); }

  // Copies the w x h region at (x, y) of the node's finished pixels into
  // `dst` (premultiplied RGBA8, row 0 = canvas row y). Flushes queued work,
  // resolves just that region from the MSAA target, and reads it back: the
  // only point where pixels leave the GPU.
  void readback(const View::Computed &c, int x, int y, int w, int h,
                uint8_t *dst) {
    flush();
    if (c.canvasMsaaFbo) {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, c.canvasMsaaFbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c.canvasFbo);
      glBlitFramebuffer(x, y, x + w, y + h, x, y, x + w, y + h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, c.canvasFbo);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    // Back to drawing into the node's draw target.
    glBindFramebuffer(GL_FRAMEBUFFER,
                      c.canvasMsaaFbo ? c.canvasMsaaFbo : c.canvasFbo);
  }

  // ---- text ----------------------------------------------------------
  // Selects how fragments use the glyph atlases at their (u,v): turns
  // textured quads into tinted glyph masks or full-colour glyphs.
  // 0 off, 1 mono glyph (coverage x paint), 2 colour glyph (own colours),
  // 3 colour glyph silhouette (its alpha x paint — used for shadows).
  void setMask(int mode) {
    if (mode == maskMode_)
      return;
    flush();
    maskMode_ = mode;
    glUniform1i(uMask_, mode);
  }

  // Hooks the canvas context calls through its renderer type, so a test
  // renderer can substitute fake glyph bitmaps and outlines.
  static bool rasterizeText(const std::string &text, const TextStyle &ts,
                            float scale, const Color *bake, RasterText &out) {
    return liteui_canvas_gl::rasterizeText(text, ts, scale, bake, out);
  }
  static bool outlineText(const std::string &text, const TextStyle &ts,
                          float tolerance, std::vector<OutlineContour> &out,
                          TextBox &box) {
    return liteui_canvas_gl::outlineText(text, ts, tolerance, out, box);
  }

  bool findText(const std::string &key, TextEntry &out) const {
    auto it = texts_.find(key);
    if (it == texts_.end())
      return false;
    out = it->second;
    return true;
  }

  // Packs `rt` into the shared atlas (shelf packing) and remembers it under
  // `key`. When the atlas is full everything is discarded and re-created on
  // demand — after flushing, because queued draws may still sample the old
  // contents. False if the bitmap can't fit even in an empty atlas.
  bool addText(const std::string &key, const RasterText &rt, TextEntry &out) {
    TextEntry e;
    e.color = rt.color;
    e.w = rt.w;
    e.h = rt.h;
    e.offX = rt.offX;
    e.offY = rt.offY;
    e.advance = rt.advance;
    e.height = rt.height;
    e.baseline = rt.baseline;
    if (texts_.size() > 8192) { // bound the map even for ink-less strings
      flush();
      resetAtlas();
    }
    if (rt.w > 0 && rt.h > 0) {
      ShelfPacker &pk = rt.color ? cpacker_ : packer_;
      int x = 0, y = 0;
      if (!pk.allocate(rt.w, rt.h, x, y)) {
        flush();
        resetAtlas();
        if (!pk.allocate(rt.w, rt.h, x, y))
          return false;
      }
      glPixelStorei(GL_UNPACK_ALIGNMENT, rt.color ? 4 : 1);
      if (rt.color) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, colorAtlas_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, rt.w, rt.h, GL_RGBA,
                        GL_UNSIGNED_BYTE, rt.rgba.data());
      } else {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, atlas_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, rt.w, rt.h, GL_RED,
                        GL_UNSIGNED_BYTE, rt.a8.data());
      }
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glActiveTexture(GL_TEXTURE0);
      const float inv = 1.0f / static_cast<float>(rt.color ? colorAtlasSize_
                                                           : atlasSize_);
      e.u0 = x * inv;
      e.v0 = y * inv;
      e.u1 = (x + rt.w) * inv;
      e.v1 = (y + rt.h) * inv;
    }
    texts_[key] = e;
    out = e;
    return true;
  }

  // Drops the clip back to `level` (used by restore()).
  void reduceClip(int level) {
    setMode(Mode::ReduceClip, level);
    const float W = static_cast<float>(w_), H = static_cast<float>(h_);
    const Vertex q[6] = {{0, 0, 0, 0, 0, 0}, {W, 0, 0, 0, 0, 0},
                         {W, H, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0},
                         {W, H, 0, 0, 0, 0}, {0, H, 0, 0, 0, 0}};
    draw(q, 6);
  }

  // Changing the blend mode ends the current batch: the GPU applies one
  // blend state per draw call, and order must be preserved.
  void setBlend(CompositeOp op) {
    if (op == op_)
      return;
    flush();
    op_ = op;
    applyBlend(op);
    glUniform1i(uBlend_, op == CompositeOp::Multiply ? 1 : 0);
  }

  void flush() {
    if (batch_.empty())
      return;
    if (op_ == CompositeOp::Multiply && writesColor(mode_))
      captureBackdrop();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    // Re-specifying the whole store ("orphaning") lets the driver hand us
    // fresh memory instead of stalling on the previous frame's draw.
    glBufferData(GL_ARRAY_BUFFER, kMaxVerts * sizeof(Vertex), nullptr,
                 GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(batch_.size() * sizeof(Vertex)),
                    batch_.data());
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch_.size()));
    batch_.clear();
  }

private:
  // Modes whose draws write colour (as opposed to stencil-only passes).
  static bool writesColor(Mode m) {
    return m == Mode::Off || m == Mode::ClipTest || m == Mode::CoverFill;
  }

  // Copies the node's pixels drawn so far into backdrop_ so the shader can
  // read them (Multiply). A multisampled target can't be copied directly,
  // so it is resolved into the node's texture first; the single-sample FBO
  // is then the copy source either way. Sampling a separate texture (never
  // the one being drawn into) avoids a feedback loop.
  void captureBackdrop() {
    if (!cur_)
      return;
    const View::Computed &c = *cur_;
    if (!backdrop_ || backdropW_ != w_ || backdropH_ != h_) {
      if (backdrop_)
        glDeleteTextures(1, &backdrop_);
      glGenTextures(1, &backdrop_);
      glActiveTexture(GL_TEXTURE4);
      glBindTexture(GL_TEXTURE_2D, backdrop_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w_, h_);
      backdropW_ = w_;
      backdropH_ = h_;
    }
    if (c.canvasMsaaFbo) {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, c.canvasMsaaFbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c.canvasFbo);
      glBlitFramebuffer(0, 0, w_, h_, 0, 0, w_, h_, GL_COLOR_BUFFER_BIT,
                        GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, c.canvasFbo);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, backdrop_);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w_, h_);
    glActiveTexture(GL_TEXTURE0);
    // Back to drawing into the node's draw target (read + draw).
    glBindFramebuffer(GL_FRAMEBUFFER,
                      c.canvasMsaaFbo ? c.canvasMsaaFbo : c.canvasFbo);
  }

  static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512];
      glGetShaderInfoLog(s, sizeof(log), nullptr, log);
      fprintf(stderr, "liteui canvas: shader compile error: %s\n", log);
      glDeleteShader(s);
      return 0;
    }
    return s;
  }

  static GLenum glFactor(BlendFactor f) {
    switch (f) {
    case BlendFactor::Zero:
      return GL_ZERO;
    case BlendFactor::One:
      return GL_ONE;
    case BlendFactor::SrcAlpha:
      return GL_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha:
      return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha:
      return GL_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha:
      return GL_ONE_MINUS_DST_ALPHA;
    case BlendFactor::DstColor:
      return GL_DST_COLOR;
    case BlendFactor::OneMinusDstColor:
      return GL_ONE_MINUS_DST_COLOR;
    }
    return GL_ONE;
  }
  static void applyBlend(CompositeOp op) {
    const BlendFactors f = blendFactorsFor(op);
    glBlendFuncSeparate(glFactor(f.srcRgb), glFactor(f.dstRgb),
                        glFactor(f.srcA), glFactor(f.dstA));
  }

  struct Saved {
    GLboolean scissor = GL_FALSE, blend = GL_FALSE;
    GLint fbo = 0, program = 0, vao = 0, arrayBuf = 0;
    GLint srcRgb = GL_ONE, dstRgb = GL_ZERO, srcA = GL_ONE, dstA = GL_ZERO;
    GLint activeTex = GL_TEXTURE0, tex0 = 0, tex1 = 0, tex2 = 0, tex3 = 0,
          tex4 = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLfloat clear[4] = {0, 0, 0, 0};
  } saved_;

  enum class PaintKind { None, Solid, Texture, Gradient };
  bool sameGradient(const CanvasGradient &g, const float inv[9]) const {
    if (g.kind != grad_.kind || g.x0 != grad_.x0 || g.y0 != grad_.y0 ||
        g.x1 != grad_.x1 || g.y1 != grad_.y1 || g.r0 != grad_.r0 ||
        g.r1 != grad_.r1 || g.stops.size() != grad_.stops.size())
      return false;
    for (int i = 0; i < 9; ++i)
      if (inv[i] != inv_[i])
        return false;
    for (size_t i = 0; i < g.stops.size(); ++i) {
      const auto &a = g.stops[i], &c = grad_.stops[i];
      if (a.offset != c.offset || a.color.r != c.color.r ||
          a.color.g != c.color.g || a.color.b != c.color.b ||
          a.color.a != c.color.a)
        return false;
    }
    return true;
  }

  GLuint prog_ = 0, vao_ = 0, vbo_ = 0, ramp_ = 0;
  GLint uSize_ = -1, uMode_ = -1, uInv_ = -1, uG_ = -1, uR_ = -1;
  GLint maxTex_ = 2048;
  PaintKind paintKind_ = PaintKind::None;
  GLuint tex_ = 0;
  CanvasGradient grad_;
  float inv_[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::vector<uint8_t> rampBuf_, upBuf_;
  std::vector<GLuint> trash_;

  // Glyph atlas bookkeeping.
  void resetAtlas() {
    texts_.clear();
    packer_.reset();
    cpacker_.reset();
  }
  GLuint atlas_ = 0, colorAtlas_ = 0, backdrop_ = 0;
  GLint atlasSize_ = 1024, colorAtlasSize_ = 512;
  GLint uMask_ = -1, uBlend_ = -1;
  int maskMode_ = 0;
  int backdropW_ = 0, backdropH_ = 0;
  const View::Computed *cur_ = nullptr; // node being drawn (between begin/end)
  ShelfPacker packer_, cpacker_;
  std::unordered_map<std::string, TextEntry> texts_;
  int w_ = 0, h_ = 0;
  CompositeOp op_ = CompositeOp::SourceOver;
  Mode mode_ = Mode::Off;
  int level_ = 0;
  std::vector<Vertex> batch_;
};

} // namespace liteui_canvas_gl

// The canvas context is a template over the renderer it draws through. The
// production instantiation (CanvasContextImpl, below) uses the real GL
// Renderer; the unit tests instantiate it with a software renderer so the
// path, stroke, clip, gradient, image and text logic can be checked without a
// GPU (see tests/cases/canvas_gl_context.inc). A renderer type `Gl` must
// provide: an enum `Mode`; setMode/reduceClip/setBlend/draw; setPaintSolid/
// setPaintTexture/setPaintGradient; setMask/findText/addText; uploadImage/
// deferDelete/readback; and static rasterizeText/outlineText.
template <class Gl> class GlCanvasContext : public CanvasContext {
public:
  using Mode = typename Gl::Mode;
  // `imageCacheOwner` is the canvas node's own View::Computed: its GPU
  // target is what getImageData() reads back, and its cachedImage* slot is
  // where drawImage() keeps the last uploaded texture. Null for a context
  // not backed by a node (images are then uploaded per call and
  // getImageData() returns nullopt).
  GlCanvasContext(Gl &gl, float width, float height,
                    const View::Computed *imageCacheOwner = nullptr)
      : gl_(gl), width_(width), height_(height),
        imageCacheOwner_(imageCacheOwner) {}
  ~GlCanvasContext() = default;
  GlCanvasContext(const GlCanvasContext &) = delete;
  GlCanvasContext &operator=(const GlCanvasContext &) = delete;

  float width() const { return width_; }
  float height() const { return height_; }

  // ---- state stack ----
  void save() {
    State s;
    s.ctm = ctm_;
    s.fill = fillPaint_;
    s.stroke = strokePaint_;
    s.lineWidth = lineWidth_;
    s.lineCap = lineCap_;
    s.lineJoin = lineJoin_;
    s.miterLimit = miterLimit_;
    s.dashes = dashes_;
    s.dashOffset = dashOffset_;
    s.globalAlpha = globalAlpha_;
    s.composite = composite_;
    s.shadowColor = shadowColor_;
    s.shadowOffsetX = shadowOffsetX_;
    s.shadowOffsetY = shadowOffsetY_;
    s.font = font_;
    s.textAlign = textAlign_;
    s.textBaseline = textBaseline_;
    s.clipLevel = clipLevel_;
    stack_.push_back(std::move(s));
  }
  void restore() {
    if (stack_.empty())
      return;
    State s = std::move(stack_.back());
    stack_.pop_back();
    ctm_ = s.ctm;
    fillPaint_ = s.fill;
    strokePaint_ = s.stroke;
    lineWidth_ = s.lineWidth;
    lineCap_ = s.lineCap;
    lineJoin_ = s.lineJoin;
    miterLimit_ = s.miterLimit;
    dashes_ = s.dashes;
    dashOffset_ = s.dashOffset;
    globalAlpha_ = s.globalAlpha;
    composite_ = s.composite;
    shadowColor_ = s.shadowColor;
    shadowOffsetX_ = s.shadowOffsetX;
    shadowOffsetY_ = s.shadowOffsetY;
    font_ = s.font;
    textAlign_ = s.textAlign;
    textBaseline_ = s.textBaseline;
    if (s.clipLevel < clipLevel_) { // clips made since save() end here
      gl_.reduceClip(s.clipLevel);
      clipLevel_ = s.clipLevel;
    }
  }

  // ---- transform ----
  // HTML semantics: each call post-multiplies the current matrix, i.e. the
  // new operation happens in the already-transformed coordinate system.
  void translate(float x, float y) { ctm_ = ctm_.mul({1, 0, 0, 1, x, y}); }
  void rotate(float radians) {
    float c = std::cos(radians), s = std::sin(radians);
    ctm_ = ctm_.mul({c, s, -s, c, 0, 0});
  }
  void scale(float sx, float sy) { ctm_ = ctm_.mul({sx, 0, 0, sy, 0, 0}); }
  void transformBy(float a, float b, float c, float d, float e, float f) {
    ctm_ = ctm_.mul({a, b, c, d, e, f});
  }
  void setTransform(float a, float b, float c, float d, float e, float f) {
    ctm_ = Mat{a, b, c, d, e, f};
  }
  void resetTransform() { ctm_ = Mat{}; }

  // ---- paint/style ----
  void setFillColor(Color c) { fillPaint_ = Paint::solid(c); }
  void setFillGradient(const CanvasGradient &g) { fillPaint_ = Paint::grad(g); }
  void setStrokeColor(Color c) { strokePaint_ = Paint::solid(c); }
  void setStrokeGradient(const CanvasGradient &g) {
    strokePaint_ = Paint::grad(g);
  }
  void setLineWidth(float w) { lineWidth_ = std::max(0.0f, w); }
  void setLineCap(LineCap c) { lineCap_ = c; }
  void setLineJoin(LineJoin j) { lineJoin_ = j; }
  void setMiterLimit(float m) { miterLimit_ = m; }
  void setLineDash(const std::vector<float> &dashes) { dashes_ = dashes; }
  void setLineDashOffset(float offset) { dashOffset_ = offset; }
  void setGlobalAlpha(float a) { globalAlpha_ = std::clamp(a, 0.0f, 1.0f); }
  void setGlobalCompositeOperation(CompositeOp op) { composite_ = op; }
  void setShadow(Color color, float /*blurPx: unblurred, as on Windows*/,
                 float offsetX, float offsetY) {
    shadowColor_ = color;
    shadowOffsetX_ = offsetX;
    shadowOffsetY_ = offsetY;
  }
  void setFont(const std::string &family, float sizePx,
               FontWeight weight = FontWeight::Regular,
               FontStyle style = FontStyle::Normal) {
    font_.fontFamily = family;
    font_.fontSize = sizePx;
    font_.fontWeight = weight;
    font_.fontStyle = style;
  }
  void setTextAlign(TextAlign a) { textAlign_ = a; }
  void setTextBaseline(TextBaseline b) { textBaseline_ = b; }

  // ---- path building ----
  // Recorded exactly like the Windows backend (cubic segments, one flag per
  // figure); fill()/stroke()/clip() will consume it in the next step.
  void beginPath() {
    segs_.clear();
    figureClosed_.clear();
    curX_ = curY_ = startX_ = startY_ = 0;
  }
  void closePath() {
    if (!figureClosed_.empty())
      figureClosed_.back() = true;
    curX_ = startX_;
    curY_ = startY_;
  }
  void moveTo(float x, float y) {
    segs_.push_back(Seg::move(x, y));
    figureClosed_.push_back(false);
    curX_ = startX_ = x;
    curY_ = startY_ = y;
  }
  void lineTo(float x, float y) {
    ensureFigure();
    segs_.push_back(Seg::line(x, y));
    curX_ = x;
    curY_ = y;
  }
  void quadraticCurveTo(float cpx, float cpy, float x, float y) {
    ensureFigure();
    float c1x = curX_ + 2.0f / 3.0f * (cpx - curX_);
    float c1y = curY_ + 2.0f / 3.0f * (cpy - curY_);
    float c2x = x + 2.0f / 3.0f * (cpx - x);
    float c2y = y + 2.0f / 3.0f * (cpy - y);
    segs_.push_back(Seg::cubic(c1x, c1y, c2x, c2y, x, y));
    curX_ = x;
    curY_ = y;
  }
  void bezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x,
                     float y) {
    ensureFigure();
    segs_.push_back(Seg::cubic(c1x, c1y, c2x, c2y, x, y));
    curX_ = x;
    curY_ = y;
  }
  void arc(float cx, float cy, float r, float startAngle, float endAngle,
           bool ccw = false) {
    appendArc(cx, cy, r, r, 0, startAngle, endAngle, ccw,
              figureClosed_.empty());
  }
  void ellipse(float cx, float cy, float rx, float ry, float rotation,
               float startAngle, float endAngle, bool ccw = false) {
    appendArc(cx, cy, rx, ry, rotation, startAngle, endAngle, ccw,
              figureClosed_.empty());
  }
  void arcTo(float x1, float y1, float x2, float y2, float radius) {
    float x0 = curX_, y0 = curY_;
    float dx1 = x0 - x1, dy1 = y0 - y1;
    float dx2 = x2 - x1, dy2 = y2 - y1;
    float len1 = std::sqrt(dx1 * dx1 + dy1 * dy1);
    float len2 = std::sqrt(dx2 * dx2 + dy2 * dy2);
    if (len1 < 1e-6f || len2 < 1e-6f || radius <= 0) {
      lineTo(x1, y1);
      return;
    }
    dx1 /= len1;
    dy1 /= len1;
    dx2 /= len2;
    dy2 /= len2;
    float cosA = std::clamp(dx1 * dx2 + dy1 * dy2, -1.0f, 1.0f);
    float angle = std::acos(cosA);
    if (angle < 1e-6f) {
      lineTo(x1, y1);
      return;
    }
    float dist = radius / std::tan(angle / 2.0f);
    float t1x = x1 + dx1 * dist, t1y = y1 + dy1 * dist;
    float t2x = x1 + dx2 * dist, t2y = y1 + dy2 * dist;
    lineTo(t1x, t1y);
    float bisx = dx1 + dx2, bisy = dy1 + dy2;
    float bisLen = std::sqrt(bisx * bisx + bisy * bisy);
    if (bisLen < 1e-6f) {
      lineTo(x1, y1);
      return;
    }
    bisx /= bisLen;
    bisy /= bisLen;
    float centerDist = std::sqrt(radius * radius + dist * dist);
    float ccx = x1 + bisx * centerDist, ccy = y1 + bisy * centerDist;
    float a0 = std::atan2(t1y - ccy, t1x - ccx);
    float a1 = std::atan2(t2y - ccy, t2x - ccx);
    float cross = dx1 * dy2 - dy1 * dx2; // winding sign, picks short way
    appendArc(ccx, ccy, radius, radius, 0, a0, a1, cross > 0, false);
  }
  void rect(float x, float y, float w, float h) {
    moveTo(x, y);
    lineTo(x + w, y);
    lineTo(x + w, y + h);
    lineTo(x, y + h);
    closePath();
  }
  void roundRect(float x, float y, float w, float h, float radius) {
    constexpr float kPi = 3.14159265358979f;
    radius = std::max(0.0f, std::min(radius, std::min(w, h) / 2.0f));
    moveTo(x + radius, y);
    lineTo(x + w - radius, y);
    appendArc(x + w - radius, y + radius, radius, radius, 0, -kPi / 2, 0, false,
              false);
    lineTo(x + w, y + h - radius);
    appendArc(x + w - radius, y + h - radius, radius, radius, 0, 0, kPi / 2,
              false, false);
    lineTo(x + radius, y + h);
    appendArc(x + radius, y + h - radius, radius, radius, 0, kPi / 2, kPi,
              false, false);
    lineTo(x, y + radius);
    appendArc(x + radius, y + radius, radius, radius, 0, kPi, kPi * 1.5f, false,
              false);
    closePath();
  }

  // ---- drawing ----
  void fill(FillRule rule = FillRule::NonZero) {
    std::vector<Contour> cs = flattenPath();
    if (cs.empty())
      return;
    gl_.setBlend(composite_);
    if (hasShadow())
      fillContours(cs, rule, Paint::solid(shadowColor_), shadowOffsetX_,
                   shadowOffsetY_);
    fillContours(cs, rule, fillPaint_, 0, 0);
  }
  void stroke() { strokeContours(flattenPath()); }

  // Intersects the clip with the current path (through the current
  // transform). Clips nest up to 15 deep; restore() undoes them.
  void clip(FillRule rule = FillRule::NonZero) {
    if (clipLevel_ >= 15) {
      warnOnce("clip(): more than 15 nested clips; the extra clips are ignored");
      return;
    }
    std::vector<liteui_canvas_gl::Vertex> fan;
    Bounds bb;
    buildFan(flattenPath(), 0, 0, fan, bb);
    // An empty path clips everything away: the level still goes up, but no
    // pixel is promoted to it, so nothing matches the new level any more.
    if (!fan.empty() && bb.clampTo(width_, height_)) {
      gl_.setMode(rule == FillRule::EvenOdd ? Mode::CountEvenOdd
                                            : Mode::CountWinding,
                  clipLevel_);
      gl_.draw(fan.data(), fan.size());
      gl_.setMode(Mode::CoverClip, clipLevel_);
      emitDeviceQuad(bb, {0, 0, 0, 0});
    }
    ++clipLevel_;
  }

  void fillRect(float x, float y, float w, float h) {
    prepareNormal();
    gl_.setBlend(composite_);
    if (hasShadow()) {
      gl_.setPaintSolid();
      emitQuad(x + shadowOffsetX_, y + shadowOffsetY_, w, h,
               premul(shadowColor_));
    }
    emitQuad(x, y, w, h, bindPaint(fillPaint_));
  }
  // Strokes a rectangle without touching the current path, like the other
  // backends.
  void strokeRect(float x, float y, float w, float h) {
    Contour c;
    c.pts = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    c.closed = true;
    std::vector<Contour> cs;
    cs.push_back(std::move(c));
    strokeContours(cs);
  }
  // Clears through the current transform to transparent black. Done as a
  // Copy-blended quad, so the rectangle is antialiased by MSAA like any
  // other shape and honours rotation/skew.
  void clearRect(float x, float y, float w, float h) {
    prepareNormal();
    gl_.setBlend(CompositeOp::Copy);
    gl_.setPaintSolid();
    emitQuad(x, y, w, h, {0, 0, 0, 0});
    gl_.setBlend(composite_);
  }

  // ---- text ----
  TextMetricsResult measureText(const std::string &text) {
    TextStyle ts = fontToTextStyle();
    liteui_text::Measurement m = liteui_text::measure(
        text, ts, resolveStatic(ts.fontSize), resolveStatic(ts.fontWeight),
        resolveStatic(ts.fontStyle), -1);
    return {m.width};
  }
  // maxWidth is accepted for API familiarity but not applied, as in the
  // other backends. Text is a single unwrapped line.
  void fillText(const std::string &text, float x, float y, float = -1) {
    drawTextImpl(text, x, y, false);
  }
  void strokeText(const std::string &text, float x, float y, float = -1) {
    drawTextImpl(text, x, y, true);
  }

  // ---- images / pixel data ----
  void drawImage(const CanvasImage &img, float dx, float dy) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy,
              static_cast<float>(img.width), static_cast<float>(img.height));
  }
  void drawImage(const CanvasImage &img, float dx, float dy, float dw,
                 float dh) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy, dw, dh);
  }
  // Draws the (sx,sy,sw,sh) part of `img` into (dx,dy,dw,dh), bilinearly
  // filtered, through the CTM, clip, globalAlpha and composite operation.
  // Like the HTML spec and the Windows backend (and unlike the Cairo one),
  // the source rectangle really crops: it is first clipped to the image and
  // the destination shrinks in proportion. The image lives in a GPU texture
  // cached on the canvas node (same identity heuristic as the other
  // backends: same pixel buffer address, size and dimensions — mutating
  // pixels in place without changing the buffer is not noticed).
  void drawImage(const CanvasImage &img, float sx, float sy, float sw, float sh,
                 float dx, float dy, float dw, float dh) {
    if (img.width <= 0 || img.height <= 0 || sw <= 0 || sh <= 0 || dw == 0 ||
        dh == 0)
      return;
    const float iw = static_cast<float>(img.width),
                ih = static_cast<float>(img.height);
    float cx0 = std::max(sx, 0.0f), cy0 = std::max(sy, 0.0f);
    float cx1 = std::min(sx + sw, iw), cy1 = std::min(sy + sh, ih);
    if (cx1 <= cx0 || cy1 <= cy0)
      return; // nothing of the image is inside the source rectangle
    float rx = dw / sw, ry = dh / sh;
    GLuint tex = imageTexture(img);
    if (!tex)
      return;
    prepareNormal();
    gl_.setBlend(composite_);
    gl_.setPaintTexture(tex);
    emitTexQuad(ctm_, dx + (cx0 - sx) * rx, dy + (cy0 - sy) * ry,
                (cx1 - cx0) * rx, (cy1 - cy0) * ry, cx0 / iw, cy0 / ih,
                cx1 / iw, cy1 / ih, {globalAlpha_, globalAlpha_, globalAlpha_,
                                     globalAlpha_});
  }

  // Reads the canvas's finished pixels back from the GPU (the only place
  // they leave it). Ignores the transform, like the HTML spec; pixels come
  // back as straight (un-premultiplied) RGBA, mirroring the other backends.
  // Needs the node's GPU target, so it returns nullopt for a context that
  // isn't backed by one.
  std::optional<CanvasImage> getImageData(float x, float y, float w, float h) {
    if (!imageCacheOwner_ || !imageCacheOwner_->canvasFbo)
      return std::nullopt;
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    int iw = std::max(0, static_cast<int>(std::round(w)));
    int ih = std::max(0, static_cast<int>(std::round(h)));
    CanvasImage out(iw, ih);
    if (iw == 0 || ih == 0)
      return out;
    int x0 = std::max(0, ix), y0 = std::max(0, iy);
    int x1 = std::min(static_cast<int>(width_), ix + iw);
    int y1 = std::min(static_cast<int>(height_), iy + ih);
    if (x1 <= x0 || y1 <= y0)
      return out; // entirely off-canvas: transparent black
    const int rw = x1 - x0, rh = y1 - y0;
    std::vector<uint8_t> px(static_cast<size_t>(rw) * rh * 4);
    gl_.readback(*imageCacheOwner_, x0, y0, rw, rh, px.data());
    for (int row = 0; row < rh; ++row) {
      for (int col = 0; col < rw; ++col) {
        const uint8_t *p = &px[(static_cast<size_t>(row) * rw + col) * 4];
        uint8_t *d = out.pixels.data() +
                     ((static_cast<size_t>(y0 + row - iy)) * iw +
                      (x0 + col - ix)) *
                         4;
        int a = p[3];
        d[0] = a ? static_cast<uint8_t>(std::min(255, p[0] * 255 / a)) : 0;
        d[1] = a ? static_cast<uint8_t>(std::min(255, p[1] * 255 / a)) : 0;
        d[2] = a ? static_cast<uint8_t>(std::min(255, p[2] * 255 / a)) : 0;
        d[3] = static_cast<uint8_t>(a);
      }
    }
    return out;
  }

  // Replaces the destination pixels with `img` (straight RGBA, alpha
  // included), at (dx,dy) rounded to whole pixels. Per the spec it ignores
  // the transform, clip, globalAlpha and composite operation, which is what
  // the Cairo backend does too.
  void putImageData(const CanvasImage &img, float dx, float dy) {
    if (img.width <= 0 || img.height <= 0)
      return;
    GLuint tex = gl_.uploadImage(img);
    if (!tex) {
      warnOnce("putImageData(): the image is larger than the GPU supports; nothing drawn");
      return;
    }
    gl_.deferDelete(tex);
    gl_.setMode(Mode::Off, 0); // ignore the clip
    gl_.setBlend(CompositeOp::Copy);
    gl_.setPaintTexture(tex);
    emitTexQuad(Mat{}, std::round(dx), std::round(dy),
                static_cast<float>(img.width), static_cast<float>(img.height),
                0, 0, 1, 1, {1, 1, 1, 1});
    gl_.setBlend(composite_);
  }

private:
  // ---- matrix: x' = a*x + c*y + e, y' = b*x + d*y + f ----
  struct Mat {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    // this * o: o is applied first, then this.
    Mat mul(const Mat &o) const {
      return {a * o.a + c * o.b,       b * o.a + d * o.b,
              a * o.c + c * o.d,       b * o.c + d * o.d,
              a * o.e + c * o.f + e,   b * o.e + d * o.f + f};
    }
    void apply(float x, float y, float &ox, float &oy) const {
      ox = a * x + c * y + e;
      oy = b * x + d * y + f;
    }
  };

  struct Seg {
    enum class Kind { Move, Line, Cubic } kind;
    float x, y;               // endpoint (all kinds)
    float c1x, c1y, c2x, c2y; // control points (Cubic only)
    static Seg move(float x, float y) { return {Kind::Move, x, y, 0, 0, 0, 0}; }
    static Seg line(float x, float y) { return {Kind::Line, x, y, 0, 0, 0, 0}; }
    static Seg cubic(float c1x, float c1y, float c2x, float c2y, float x,
                     float y) {
      return {Kind::Cubic, x, y, c1x, c1y, c2x, c2y};
    }
  };

  struct Paint {
    bool isGradient = false;
    Color color{0, 0, 0, 255};
    CanvasGradient gradient;
    static Paint solid(Color c) {
      Paint p;
      p.color = c;
      return p;
    }
    static Paint grad(const CanvasGradient &g) {
      Paint p;
      p.isGradient = true;
      p.gradient = g;
      return p;
    }
  };

  struct State {
    Mat ctm;
    Paint fill, stroke;
    float lineWidth;
    LineCap lineCap;
    LineJoin lineJoin;
    float miterLimit;
    std::vector<float> dashes;
    float dashOffset;
    float globalAlpha;
    CompositeOp composite;
    Color shadowColor;
    float shadowOffsetX, shadowOffsetY;
    TextStyle font;
    TextAlign textAlign;
    TextBaseline textBaseline;
    int clipLevel;
  };

  struct Rgba {
    float r, g, b, a;
  };

  // Straight 8-bit colour -> premultiplied float, scaled by globalAlpha.
  Rgba premul(Color c) const {
    float a = (c.a / 255.0f) * globalAlpha_;
    return {c.r / 255.0f * a, c.g / 255.0f * a, c.b / 255.0f * a, a};
  }

  // Makes `p` the renderer's active paint and returns the vertex colour to
  // draw with: the premultiplied colour for a solid paint, or just the
  // globalAlpha scale for a gradient (the shader supplies the colour).
  // Gradient coordinates live in the user space current at draw time, so
  // the shader gets the inverse CTM. A gradient with no stops, or a
  // singular transform, paints transparent black (draws nothing).
  Rgba bindPaint(const Paint &p) {
    if (!p.isGradient) {
      gl_.setPaintSolid();
      return premul(p.color);
    }
    float inv[9];
    if (p.gradient.stops.empty() ||
        !liteui_canvas_gl::invert2x3(ctm_.a, ctm_.b, ctm_.c, ctm_.d, ctm_.e,
                                     ctm_.f, inv)) {
      gl_.setPaintSolid();
      return {0, 0, 0, 0};
    }
    gl_.setPaintGradient(p.gradient, inv);
    return {globalAlpha_, globalAlpha_, globalAlpha_, globalAlpha_};
  }

  // The texture for `img`: this node's cached one if it is for the same
  // pixel buffer, else a fresh upload that replaces it. With no node to
  // cache on, the texture is retired after this pass.
  GLuint imageTexture(const CanvasImage &img) {
    const View::Computed *o = imageCacheOwner_;
    if (o && o->cachedImageTexture && o->cachedImagePixels == img.pixels.data() &&
        o->cachedImagePixelsSize == img.pixels.size() &&
        o->cachedImageW == img.width && o->cachedImageH == img.height)
      return o->cachedImageTexture;
    GLuint t = gl_.uploadImage(img);
    if (!t) {
      warnOnce("drawImage(): the image is larger than the GPU supports; nothing drawn");
      return 0;
    }
    if (o) {
      if (o->cachedImageTexture)
        gl_.deferDelete(o->cachedImageTexture);
      o->cachedImageTexture = t;
      o->cachedImagePixels = img.pixels.data();
      o->cachedImagePixelsSize = img.pixels.size();
      o->cachedImageW = img.width;
      o->cachedImageH = img.height;
    } else {
      gl_.deferDelete(t);
    }
    return t;
  }

  // Textured version of emitQuad: (u0,v0)-(u1,v1) is the part of the image
  // mapped onto the rectangle.
  void emitTexQuad(const Mat &m, float x, float y, float w, float h, float u0,
                   float v0, float u1, float v1, Rgba c) {
    float px[4], py[4];
    m.apply(x, y, px[0], py[0]);
    m.apply(x + w, y, px[1], py[1]);
    m.apply(x + w, y + h, px[2], py[2]);
    m.apply(x, y + h, px[3], py[3]);
    const float us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
    auto V = [&](int i) {
      return liteui_canvas_gl::Vertex{px[i], py[i], c.r, c.g, c.b, c.a,
                                      us[i],  vs[i]};
    };
    const liteui_canvas_gl::Vertex tri[6] = {V(0), V(1), V(2), V(0), V(2), V(3)};
    gl_.draw(tri, 6);
  }

  // Two triangles covering the CTM-transformed rectangle (a parallelogram
  // when rotated or skewed).
  void emitQuad(float x, float y, float w, float h, Rgba c) {
    float px[4], py[4];
    ctm_.apply(x, y, px[0], py[0]);
    ctm_.apply(x + w, y, px[1], py[1]);
    ctm_.apply(x + w, y + h, px[2], py[2]);
    ctm_.apply(x, y + h, px[3], py[3]);
    auto V = [&](int i) {
      return liteui_canvas_gl::Vertex{px[i], py[i], c.r, c.g, c.b, c.a};
    };
    const liteui_canvas_gl::Vertex tri[6] = {V(0), V(1), V(2), V(0), V(2), V(3)};
    gl_.draw(tri, 6);
  }

  void ensureFigure() {
    if (figureClosed_.empty())
      moveTo(curX_, curY_);
  }

  // ---- stencil-and-cover plumbing ----
  struct Pt {
    float x, y;
  };
  struct Contour {
    std::vector<Pt> pts;
    bool closed = false;
  };
  // Device-space bounding box of a shape.
  struct Bounds {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    void add(float x, float y) {
      x0 = std::min(x0, x);
      y0 = std::min(y0, y);
      x1 = std::max(x1, x);
      y1 = std::max(y1, y);
    }
    // Intersects with the canvas; false if nothing is left.
    bool clampTo(float w, float h) {
      x0 = std::max(x0, 0.0f);
      y0 = std::max(y0, 0.0f);
      x1 = std::min(x1, w);
      y1 = std::min(y1, h);
      return x1 > x0 && y1 > y0;
    }
  };

  // Colour drawing that respects the current clip.
  void prepareNormal() {
    gl_.setMode(Mode::ClipTest, clipLevel_);
  }
  bool hasShadow() const {
    return shadowColor_.a > 0 && (shadowOffsetX_ != 0 || shadowOffsetY_ != 0);
  }
  float scaleFactor() const {
    float sx = std::hypot(ctm_.a, ctm_.b), sy = std::hypot(ctm_.c, ctm_.d);
    return std::max(std::max(sx, sy), 1e-6f);
  }

  void emitDeviceQuad(const Bounds &b, Rgba c) {
    using V = liteui_canvas_gl::Vertex;
    const V q[6] = {{b.x0, b.y0, c.r, c.g, c.b, c.a},
                    {b.x1, b.y0, c.r, c.g, c.b, c.a},
                    {b.x1, b.y1, c.r, c.g, c.b, c.a},
                    {b.x0, b.y0, c.r, c.g, c.b, c.a},
                    {b.x1, b.y1, c.r, c.g, c.b, c.a},
                    {b.x0, b.y1, c.r, c.g, c.b, c.a}};
    gl_.draw(q, 6);
  }

  // Recorded path -> user-space polylines. Curves are subdivided so the
  // flattening error stays under ~0.2 device pixels at the current scale.
  std::vector<Contour> flattenPath() const {
    const float tol = 0.2f / scaleFactor();
    std::vector<Contour> out;
    size_t fig = 0;
    bool first = true;
    Pt cur{0, 0};
    for (const Seg &s : segs_) {
      if (s.kind == Seg::Kind::Move) {
        if (!first)
          ++fig; // one figureClosed_ entry per Move, in order
        first = false;
        Contour c;
        c.closed = fig < figureClosed_.size() && figureClosed_[fig];
        c.pts.push_back({s.x, s.y});
        out.push_back(std::move(c));
        cur = {s.x, s.y};
      } else if (out.empty()) {
        continue;
      } else if (s.kind == Seg::Kind::Line) {
        out.back().pts.push_back({s.x, s.y});
        cur = {s.x, s.y};
      } else {
        // Error of a uniform n-piece polyline for a cubic is bounded by
        // 0.75 * (largest control-polygon second difference) / n^2.
        float d1x = cur.x - 2 * s.c1x + s.c2x, d1y = cur.y - 2 * s.c1y + s.c2y;
        float d2x = s.c1x - 2 * s.c2x + s.x, d2y = s.c1y - 2 * s.c2y + s.y;
        float dd = std::max(std::hypot(d1x, d1y), std::hypot(d2x, d2y));
        int n = std::clamp(
            static_cast<int>(std::ceil(std::sqrt(0.75f * dd / tol))), 1, 200);
        for (int i = 1; i <= n; ++i) {
          float t = static_cast<float>(i) / n, u = 1 - t;
          float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t,
                b3 = t * t * t;
          out.back().pts.push_back(
              {b0 * cur.x + b1 * s.c1x + b2 * s.c2x + b3 * s.x,
               b0 * cur.y + b1 * s.c1y + b2 * s.c2y + b3 * s.y});
        }
        cur = {s.x, s.y};
      }
    }
    return out;
  }

  // Triangle fans (one per contour, from its first point) in device space,
  // plus their bounding box. Open contours are closed implicitly, as fill
  // and clip require.
  void buildFan(const std::vector<Contour> &cs, float offX, float offY,
                std::vector<liteui_canvas_gl::Vertex> &fan, Bounds &bb) const {
    std::vector<Pt> dev;
    for (const Contour &c : cs) {
      if (c.pts.size() < 3)
        continue;
      dev.clear();
      for (const Pt &p : c.pts) {
        Pt d;
        ctm_.apply(p.x + offX, p.y + offY, d.x, d.y);
        dev.push_back(d);
        bb.add(d.x, d.y);
      }
      for (size_t i = 1; i + 1 < dev.size(); ++i) {
        fan.push_back({dev[0].x, dev[0].y, 0, 0, 0, 0});
        fan.push_back({dev[i].x, dev[i].y, 0, 0, 0, 0});
        fan.push_back({dev[i + 1].x, dev[i + 1].y, 0, 0, 0, 0});
      }
    }
  }

  void fillContours(const std::vector<Contour> &cs, FillRule rule,
                    const Paint &paint, float offX, float offY) {
    std::vector<liteui_canvas_gl::Vertex> fan;
    Bounds bb;
    buildFan(cs, offX, offY, fan, bb);
    if (fan.empty() || !bb.clampTo(width_, height_))
      return;
    gl_.setMode(rule == FillRule::EvenOdd ? Mode::CountEvenOdd
                                          : Mode::CountWinding,
                clipLevel_);
    gl_.draw(fan.data(), fan.size());
    gl_.setMode(Mode::CoverFill, clipLevel_);
    emitDeviceQuad(bb, bindPaint(paint));
  }

  // ---- stroking ----
  // The stroke outline is built in USER space (so a non-uniform scale
  // squashes the pen correctly), as a pile of overlapping triangles:
  // segment rectangles, join wedges/discs, cap pieces. Overlaps are
  // harmless because the pile is rasterized as a union via the stencil
  // (Mode::CountUnion), so translucent strokes don't darken at joins.
  void strokeContours(const std::vector<Contour> &cs) {
    if (lineWidth_ <= 0 || cs.empty())
      return;
    gl_.setBlend(composite_);
    if (hasShadow())
      strokeContoursAt(cs, Paint::solid(shadowColor_), shadowOffsetX_,
                       shadowOffsetY_);
    strokeContoursAt(cs, strokePaint_, 0, 0);
  }

  void strokeContoursAt(const std::vector<Contour> &cs, const Paint &paint,
                        float offX, float offY) {
    strokeScale_ = scaleFactor();
    // Dash first (open pieces), if a usable pattern is set.
    std::vector<Contour> pieces;
    std::vector<float> pat;
    float patTotal = 0;
    bool dashed = !dashes_.empty();
    for (float d : dashes_) {
      if (!(d >= 0) || !std::isfinite(d))
        dashed = false;
      patTotal += d;
    }
    if (dashed && patTotal > 1e-6f) {
      // An odd-length list is repeated to make it even, as in HTML canvas.
      const int repeats = (dashes_.size() % 2) ? 2 : 1;
      pat.reserve(dashes_.size() * repeats);
      for (int r = 0; r < repeats; ++r)
        for (float d : dashes_)
          pat.push_back(d);
      patTotal = 0;
      for (float d : pat)
        patTotal += d;
      for (const Contour &c : cs)
        dashContour(c, pat, patTotal, pieces);
    } else {
      pieces = cs;
    }

    std::vector<Pt> tris; // user space, 3 per triangle
    for (const Contour &c : pieces)
      strokePolyline(c, tris);
    if (tris.empty())
      return;

    std::vector<liteui_canvas_gl::Vertex> verts;
    verts.reserve(tris.size());
    Bounds bb;
    for (const Pt &p : tris) {
      float dx, dy;
      ctm_.apply(p.x + offX, p.y + offY, dx, dy);
      verts.push_back({dx, dy, 0, 0, 0, 0});
      bb.add(dx, dy);
    }
    if (!bb.clampTo(width_, height_))
      return;
    gl_.setMode(Mode::CountUnion, clipLevel_);
    gl_.draw(verts.data(), verts.size());
    gl_.setMode(Mode::CoverFill, clipLevel_);
    emitDeviceQuad(bb, bindPaint(paint));
  }

  static float len(Pt a, Pt b) { return std::hypot(b.x - a.x, b.y - a.y); }
  static Pt dirOf(Pt a, Pt b) {
    float l = len(a, b);
    return l > 0 ? Pt{(b.x - a.x) / l, (b.y - a.y) / l} : Pt{1, 0};
  }
  static void tri(std::vector<Pt> &o, Pt a, Pt b, Pt c) {
    o.push_back(a);
    o.push_back(b);
    o.push_back(c);
  }

  // A filled circle as a triangle fan; segment count follows its size on
  // screen so big round caps stay smooth and tiny ones stay cheap.
  void disc(std::vector<Pt> &o, Pt c, float r) const {
    float rd = r * strokeScale_;
    int n = 8;
    if (rd > 0.2f) {
      float step = std::acos(std::max(0.0f, 1.0f - 0.2f / rd));
      if (step > 1e-4f)
        n = std::clamp(static_cast<int>(std::ceil(3.14159265f / step)), 8, 64);
      else
        n = 64;
    }
    for (int i = 0; i < n; ++i) {
      float a0 = 6.28318530f * i / n, a1 = 6.28318530f * (i + 1) / n;
      tri(o, c, {c.x + r * std::cos(a0), c.y + r * std::sin(a0)},
          {c.x + r * std::cos(a1), c.y + r * std::sin(a1)});
    }
  }

  void capAt(std::vector<Pt> &o, Pt e, Pt outward, float hw) const {
    if (lineCap_ == LineCap::Round) {
      disc(o, e, hw);
    } else if (lineCap_ == LineCap::Square) {
      Pt n{-outward.y * hw, outward.x * hw};
      Pt f{e.x + outward.x * hw, e.y + outward.y * hw};
      tri(o, {e.x + n.x, e.y + n.y}, {e.x - n.x, e.y - n.y},
          {f.x + n.x, f.y + n.y});
      tri(o, {e.x - n.x, e.y - n.y}, {f.x - n.x, f.y - n.y},
          {f.x + n.x, f.y + n.y});
    }
  }

  void joinAt(std::vector<Pt> &o, Pt prev, Pt v, Pt next, float hw) const {
    Pt d0 = dirOf(prev, v), d1 = dirOf(v, next);
    float cross = d0.x * d1.y - d0.y * d1.x;
    float dot = d0.x * d1.x + d0.y * d1.y;
    if (lineJoin_ == LineJoin::Round) {
      // Skip joins whose gap would be invisible (nearly straight); a full
      // reversal (dot < 0) still needs its disc.
      if (dot < 0 || std::fabs(cross) * hw * strokeScale_ > 0.05f)
        disc(o, v, hw);
      return;
    }
    if (std::fabs(cross) < 1e-6f)
      return; // straight on, or a hairpin (nothing to fill)
    // Outer side of the turn: opposite the side the path bends toward.
    float s = cross > 0 ? -1.0f : 1.0f;
    Pt n0{-d0.y, d0.x}, n1{-d1.y, d1.x};
    Pt A{v.x + s * hw * n0.x, v.y + s * hw * n0.y};
    Pt B{v.x + s * hw * n1.x, v.y + s * hw * n1.y};
    tri(o, v, A, B); // bevel
    if (lineJoin_ == LineJoin::Miter && 1.0f + dot > 1e-6f) {
      // miterLength / lineWidth == 1 / sin(theta/2) == sqrt(2 / (1 + dot))
      float ratio = std::sqrt(2.0f / (1.0f + dot));
      if (ratio <= miterLimit_) {
        float k = s * hw / (1.0f + dot);
        Pt M{v.x + k * (n0.x + n1.x), v.y + k * (n0.y + n1.y)};
        tri(o, A, M, B);
      }
    }
  }

  void strokePolyline(const Contour &c, std::vector<Pt> &o) const {
    const float hw = lineWidth_ * 0.5f;
    // Points closer than a small fraction of a device pixel are the same
    // point. (A fixed tiny epsilon is not enough: the end of a full-circle arc
    // lands a few millionths away from its start, and the near-zero-length
    // segment that results has a meaningless direction, which breaks the
    // joins on both sides of it.)
    const float same = 0.002f / strokeScale_;
    std::vector<Pt> q;
    for (const Pt &p : c.pts)
      if (q.empty() || len(q.back(), p) > same)
        q.push_back(p);
    bool closed = c.closed;
    if (closed && q.size() > 1 && len(q.front(), q.back()) <= same)
      q.pop_back();
    if (q.empty())
      return;
    if (q.size() == 1) { // zero-length subpath: a dot for round/square caps
      if (lineCap_ == LineCap::Round) {
        disc(o, q[0], hw);
      } else if (lineCap_ == LineCap::Square) {
        Pt a{q[0].x - hw, q[0].y - hw}, b{q[0].x + hw, q[0].y - hw};
        Pt cc{q[0].x + hw, q[0].y + hw}, d{q[0].x - hw, q[0].y + hw};
        tri(o, a, b, cc);
        tri(o, a, cc, d);
      }
      return;
    }
    const size_t n = q.size();
    const size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i) {
      Pt a = q[i], b = q[(i + 1) % n];
      Pt d = dirOf(a, b);
      Pt nn{-d.y * hw, d.x * hw};
      Pt a1{a.x + nn.x, a.y + nn.y}, a2{a.x - nn.x, a.y - nn.y};
      Pt b1{b.x + nn.x, b.y + nn.y}, b2{b.x - nn.x, b.y - nn.y};
      tri(o, a1, a2, b1);
      tri(o, a2, b2, b1);
    }
    for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); ++i)
      joinAt(o, q[(i + n - 1) % n], q[i], q[(i + 1) % n], hw);
    if (!closed) {
      Pt s0 = dirOf(q[1], q[0]), s1 = dirOf(q[n - 2], q[n - 1]);
      capAt(o, q[0], s0, hw);
      capAt(o, q[n - 1], s1, hw);
    }
  }

  // Splits one contour into dash pieces (each an open polyline) following
  // `pat` (even entry count, positive total), starting dashOffset_ into the
  // pattern. A zero-length "on" entry yields a single-point piece, which
  // strokePolyline turns into a dot for round/square caps.
  void dashContour(const Contour &c, const std::vector<float> &pat,
                   float total, std::vector<Contour> &out) const {
    std::vector<Pt> P = c.pts;
    if (c.closed && !P.empty())
      P.push_back(P.front());
    if (P.size() < 2) {
      out.push_back(c);
      return;
    }
    // Refuse absurd dash counts (tiny dashes on a huge path): draw solid.
    float pathLen = 0;
    for (size_t i = 0; i + 1 < P.size(); ++i)
      pathLen += len(P[i], P[i + 1]);
    if (pathLen / total * static_cast<float>(pat.size()) > 200000.0f) {
      out.push_back(c);
      return;
    }
    // Position inside the pattern at the start of the path.
    float phase = std::fmod(dashOffset_, total);
    if (phase < 0)
      phase += total;
    size_t idx = 0;
    float remaining = pat[0];
    while (phase > 0) {
      if (phase >= remaining) {
        phase -= remaining;
        idx = (idx + 1) % pat.size();
        remaining = pat[idx];
      } else {
        remaining -= phase;
        phase = 0;
      }
    }
    bool on = (idx % 2) == 0;
    Contour cur;
    if (on)
      cur.pts.push_back(P[0]);
    for (size_t i = 0; i + 1 < P.size(); ++i) {
      Pt a = P[i], b = P[i + 1];
      float segLen = len(a, b);
      Pt d = dirOf(a, b);
      float pos = 0;
      while (pos < segLen) {
        float step = std::min(remaining, segLen - pos);
        Pt pt{a.x + d.x * (pos + step), a.y + d.y * (pos + step)};
        if (on)
          cur.pts.push_back(pt);
        pos += step;
        remaining -= step;
        if (remaining <= 1e-6f) { // reached a dash/gap boundary
          if (on) {
            out.push_back(std::move(cur));
            cur = Contour{};
          }
          idx = (idx + 1) % pat.size();
          remaining = pat[idx];
          on = !on;
          if (on)
            cur.pts.push_back(pt);
        }
      }
    }
    if (on && !cur.pts.empty())
      out.push_back(std::move(cur));
  }

  float strokeScale_ = 1.0f;

  // Splits [startAngle, endAngle) into <=90-degree pieces and appends each
  // as a cubic Bezier (identical to the Windows backend's helper).
  void appendArc(float cx, float cy, float rx, float ry, float rotation,
                 float a0, float a1, bool ccw, bool asMove) {
    constexpr float kTwoPi = 6.28318530717959f;
    float delta = a1 - a0;
    if (!ccw) {
      while (delta < 0)
        delta += kTwoPi;
    } else {
      while (delta > 0)
        delta -= kTwoPi;
    }
    float cosR = std::cos(rotation), sinR = std::sin(rotation);
    auto mapPt = [&](float ux, float uy, float &px, float &py) {
      float ex = rx * ux, ey = ry * uy;
      px = cx + ex * cosR - ey * sinR;
      py = cy + ex * sinR + ey * cosR;
    };
    float p0x, p0y;
    mapPt(std::cos(a0), std::sin(a0), p0x, p0y);
    if (asMove)
      moveTo(p0x, p0y);
    else
      lineTo(p0x, p0y);
    if (std::abs(delta) < 1e-6f)
      return;
    int segCount = std::max(
        1, static_cast<int>(std::ceil(std::abs(delta) / (3.14159265f / 2))));
    float segAngle = delta / segCount;
    float ang = a0;
    for (int i = 0; i < segCount; ++i) {
      float ang1 = ang + segAngle;
      // Control-point length for a circular arc of angle (ang1 - ang): the
      // formula needs tan of HALF the sweep.
      float t = std::tan((ang1 - ang) / 2.0f);
      float alpha =
          std::sin(ang1 - ang) * (std::sqrt(4.0f + 3.0f * t * t) - 1.0f) / 3.0f;
      float e0x = std::cos(ang), e0y = std::sin(ang);
      float e1x = std::cos(ang1), e1y = std::sin(ang1);
      float d0x = -e0y, d0y = e0x;
      float d1x = -e1y, d1y = e1x;
      float c1ex = e0x + alpha * d0x, c1ey = e0y + alpha * d0y;
      float c2ex = e1x - alpha * d1x, c2ey = e1y - alpha * d1y;
      float c1x, c1y, c2x, c2y, ex1, ey1;
      mapPt(c1ex, c1ey, c1x, c1y);
      mapPt(c2ex, c2ey, c2x, c2y);
      mapPt(e1x, e1y, ex1, ey1);
      segs_.push_back(Seg::cubic(c1x, c1y, c2x, c2y, ex1, ey1));
      curX_ = ex1;
      curY_ = ey1;
      ang = ang1;
    }
  }

  // Where the layout's top-left goes so the string lands at (x,y) with the
  // current textAlign/textBaseline (same rules as the other backends).
  void textOrigin(float advance, float height, float baseline, float x, float y,
                  float &bx, float &by) const {
    bx = x;
    by = y;
    switch (textAlign_) {
    case TextAlign::Center:
      bx -= advance / 2.0f;
      break;
    case TextAlign::End:
      bx -= advance;
      break;
    default:
      break;
    }
    switch (textBaseline_) {
    case TextBaseline::Top:
      break;
    case TextBaseline::Middle:
      by -= height / 2.0f;
      break;
    case TextBaseline::Bottom:
      by -= height;
      break;
    case TextBaseline::Alphabetic:
    default:
      by -= baseline;
      break;
    }
  }

  // Glyph bitmaps are rasterized at the transform's scale (quantized, so
  // slightly different scales share cache entries) to stay crisp when the
  // canvas is zoomed; rotation and skew are applied by the quad.
  float textRasterScale() const {
    return std::clamp(std::round(scaleFactor() * 16.0f) / 16.0f, 0.25f, 16.0f);
  }
  std::string textKey(const std::string &text, const TextStyle &ts,
                      float scale) const {
    char head[160];
    std::snprintf(head, sizeof(head), "|%g|%d|%d|%d%d|%g|%g|%g|",
                  static_cast<double>(resolveStatic(ts.fontSize)),
                  static_cast<int>(resolveStatic(ts.fontWeight)),
                  static_cast<int>(resolveStatic(ts.fontStyle)),
                  ts.underline ? 1 : 0, ts.strikethrough ? 1 : 0,
                  static_cast<double>(ts.letterSpacing),
                  static_cast<double>(ts.lineHeight),
                  static_cast<double>(scale));
    return ts.fontFamily + head + text;
  }

  void drawTextImpl(const std::string &text, float x, float y, bool stroked) {
    if (text.empty())
      return;
    TextStyle ts = fontToTextStyle();
    if (stroked) {
      strokeTextOutline(text, ts, x, y);
      return;
    }
    const float s = textRasterScale();
    // Strings that may contain colour glyphs (emoji) are rasterized as RGBA
    // with the (solid) fill colour baked in, so emoji keep their own colours.
    // A gradient fill can't be baked; those draw as tinted silhouettes.
    const bool bake =
        !fillPaint_.isGradient && liteui_canvas_gl::mayHaveColorGlyphs(text);
    std::string key = textKey(text, ts, s);
    if (bake) {
      char suffix[40];
      std::snprintf(suffix, sizeof(suffix), "#%02x%02x%02x%02x",
                    fillPaint_.color.r, fillPaint_.color.g, fillPaint_.color.b,
                    fillPaint_.color.a);
      key += suffix;
    }
    liteui_canvas_gl::TextEntry e;
    if (!gl_.findText(key, e)) {
      liteui_canvas_gl::RasterText rt;
      if (!Gl::rasterizeText(
              text, ts, s, bake ? &fillPaint_.color : nullptr, rt) ||
          !gl_.addText(key, rt, e)) {
        warnOnce("fillText(): the text is too large for the glyph atlas; nothing drawn");
        return;
      }
    }
    if (e.w == 0)
      return; // nothing inked (e.g. only spaces)
    float bx, by;
    textOrigin(e.advance, e.height, e.baseline, x, y, bx, by);
    // With an axis-aligned transform, put the origin on a whole device pixel
    // so the glyph bitmap lands 1:1 on the pixel grid (crisp text).
    if (ctm_.b == 0 && ctm_.c == 0 && ctm_.a != 0 && ctm_.d != 0) {
      float ox, oy;
      ctm_.apply(bx, by, ox, oy);
      bx += (std::round(ox) - ox) / ctm_.a;
      by += (std::round(oy) - oy) / ctm_.d;
    }
    prepareNormal();
    gl_.setBlend(composite_);
    const float qx = bx + e.offX, qy = by + e.offY;
    const float qw = static_cast<float>(e.w) / s, qh = static_cast<float>(e.h) / s;
    if (hasShadow()) {
      gl_.setMask(e.color ? 3 : 1); // the glyph's silhouette, in shadow colour
      gl_.setPaintSolid();
      emitTexQuad(ctm_, qx + shadowOffsetX_, qy + shadowOffsetY_, qw, qh, e.u0,
                  e.v0, e.u1, e.v1, premul(shadowColor_));
    }
    gl_.setMask(e.color ? 2 : 1);
    Rgba c;
    if (e.color) { // paint is baked into the bitmap; only globalAlpha applies
      gl_.setPaintSolid();
      c = {globalAlpha_, globalAlpha_, globalAlpha_, globalAlpha_};
    } else {
      c = bindPaint(fillPaint_);
    }
    emitTexQuad(ctm_, qx, qy, qw, qh, e.u0, e.v0, e.u1, e.v1, c);
    gl_.setMask(0);
  }

  // strokeText traces the real glyph outlines with the normal stroker, so
  // lineWidth, joins, dashes, gradients, shadow and clip all apply. (The
  // Cairo and Windows backends only approximate this with offset copies.)
  void strokeTextOutline(const std::string &text, const TextStyle &ts, float x,
                         float y) {
    std::vector<liteui_canvas_gl::OutlineContour> oc;
    liteui_canvas_gl::TextBox box;
    if (!Gl::outlineText(text, ts, 0.1f / scaleFactor(), oc,
                                       box)) {
      warnOnce("strokeText(): the glyph outlines could not be obtained; nothing drawn");
      return;
    }
    float bx, by;
    textOrigin(box.advance, box.height, box.baseline, x, y, bx, by);
    std::vector<Contour> cs;
    for (const auto &o : oc) {
      Contour c;
      c.closed = o.closed;
      for (size_t i = 0; i + 1 < o.xy.size(); i += 2)
        c.pts.push_back({bx + o.xy[i], by + o.xy[i + 1]});
      cs.push_back(std::move(c));
    }
    strokeContours(cs);
  }

  TextStyle fontToTextStyle() const {
    TextStyle ts = font_;
    ts.wrap = TextWrap::NoWrap;
    return ts;
  }

  // One stderr line per distinct missing feature per process, so a canvas
  // that repaints every frame doesn't flood the log.
  static void warnOnce(const char *what) {
    static std::vector<std::string> seen;
    for (const auto &s : seen)
      if (s == what)
        return;
    seen.emplace_back(what);
    fprintf(stderr,
            "liteui [LITEUI_CANVAS_GL]: %s\n",
            what);
  }

  Gl &gl_;
  float width_, height_;
  const View::Computed *imageCacheOwner_;

  Mat ctm_;
  Paint fillPaint_ = Paint::solid(Color{0, 0, 0, 255});
  Paint strokePaint_ = Paint::solid(Color{0, 0, 0, 255});
  float lineWidth_ = 1.0f;
  LineCap lineCap_ = LineCap::Butt;
  LineJoin lineJoin_ = LineJoin::Miter;
  float miterLimit_ = 10.0f;
  std::vector<float> dashes_;
  float dashOffset_ = 0.0f;
  float globalAlpha_ = 1.0f;
  CompositeOp composite_ = CompositeOp::SourceOver;
  Color shadowColor_{0, 0, 0, 0};
  float shadowOffsetX_ = 0.0f, shadowOffsetY_ = 0.0f;
  TextStyle font_;
  TextAlign textAlign_ = TextAlign::Start;
  TextBaseline textBaseline_ = TextBaseline::Alphabetic;

  std::vector<Seg> segs_;
  std::vector<bool> figureClosed_; // one entry per Move, parallel order
  float curX_ = 0, curY_ = 0, startX_ = 0, startY_ = 0;

  int clipLevel_ = 0; // current clip nesting (see the stencil layout)
  std::vector<State> stack_;
};

using CanvasContextImpl = GlCanvasContext<liteui_canvas_gl::Renderer>;

#else // Linux — Cairo

class CanvasContextImpl : public CanvasContext {
public:
  // `cr` is a Cairo context targeting an image surface sized exactly to
  // the canvas node's inner (padding-excluded) content box, already
  // cleared to transparent by the caller (see LiteUI::ensureCanvasSurface).
  // `imageCacheOwner`, when non-null, is the canvas node's own
  // View::Computed — drawImage() below uses its cachedImageSurface slot
  // to skip reconverting the same CanvasImage on a repeat draw (see
  // drawImage's own comment). Left null for any CanvasContext not backed
  // by a persistent per-node Computed.
  CanvasContextImpl(cairo_t *cr, float width, float height,
                    const View::Computed *imageCacheOwner = nullptr)
      : cr_(cr), width_(width), height_(height),
        imageCacheOwner_(imageCacheOwner) {}
  ~CanvasContextImpl() = default;
  CanvasContextImpl(const CanvasContextImpl &) = delete;
  CanvasContextImpl &operator=(const CanvasContextImpl &) = delete;

  float width() const { return width_; }
  float height() const { return height_; }

  // ---- state stack ----
  // Cairo's own save/restore already covers the CTM, clip, line width/
  // cap/join/miter/dash — only the fields HTML canvas tracks that Cairo
  // has no native slot for (fill vs. stroke paint, globalAlpha, font,
  // text align/baseline, shadow) need to be mirrored by hand here.
  void save() {
    cairo_save(cr_);
    ExtraState s;
    s.fill = fillPaint_;
    s.stroke = strokePaint_;
    s.globalAlpha = globalAlpha_;
    s.font = font_;
    s.textAlign = textAlign_;
    s.textBaseline = textBaseline_;
    s.shadowColor = shadowColor_;
    s.shadowOffsetX = shadowOffsetX_;
    s.shadowOffsetY = shadowOffsetY_;
    extraStack_.push_back(std::move(s));
  }
  void restore() {
    cairo_restore(cr_);
    if (extraStack_.empty())
      return;
    ExtraState s = std::move(extraStack_.back());
    extraStack_.pop_back();
    fillPaint_ = s.fill;
    strokePaint_ = s.stroke;
    globalAlpha_ = s.globalAlpha;
    font_ = s.font;
    textAlign_ = s.textAlign;
    textBaseline_ = s.textBaseline;
    shadowColor_ = s.shadowColor;
    shadowOffsetX_ = s.shadowOffsetX;
    shadowOffsetY_ = s.shadowOffsetY;
  }

  // ---- transform ----
  void translate(float x, float y) { cairo_translate(cr_, x, y); }
  void rotate(float radians) { cairo_rotate(cr_, radians); }
  void scale(float sx, float sy) { cairo_scale(cr_, sx, sy); }
  void transformBy(float a, float b, float c, float d, float e, float f) {
    cairo_matrix_t m;
    m.xx = a;
    m.yx = b;
    m.xy = c;
    m.yy = d;
    m.x0 = e;
    m.y0 = f;
    cairo_transform(cr_, &m);
  }
  void setTransform(float a, float b, float c, float d, float e, float f) {
    cairo_matrix_t m;
    m.xx = a;
    m.yx = b;
    m.xy = c;
    m.yy = d;
    m.x0 = e;
    m.y0 = f;
    cairo_set_matrix(cr_, &m);
  }
  void resetTransform() { cairo_identity_matrix(cr_); }

  // ---- paint/style ----
  void setFillColor(Color c) { fillPaint_ = Paint::solid(c); }
  void setFillGradient(const CanvasGradient &g) { fillPaint_ = Paint::grad(g); }
  void setStrokeColor(Color c) { strokePaint_ = Paint::solid(c); }
  void setStrokeGradient(const CanvasGradient &g) {
    strokePaint_ = Paint::grad(g);
  }
  void setLineWidth(float w) { cairo_set_line_width(cr_, std::max(0.0f, w)); }
  void setLineCap(LineCap c) {
    cairo_set_line_cap(cr_, c == LineCap::Round    ? CAIRO_LINE_CAP_ROUND
                            : c == LineCap::Square ? CAIRO_LINE_CAP_SQUARE
                                                   : CAIRO_LINE_CAP_BUTT);
  }
  void setLineJoin(LineJoin j) {
    cairo_set_line_join(cr_, j == LineJoin::Round   ? CAIRO_LINE_JOIN_ROUND
                             : j == LineJoin::Bevel ? CAIRO_LINE_JOIN_BEVEL
                                                    : CAIRO_LINE_JOIN_MITER);
  }
  void setMiterLimit(float m) { cairo_set_miter_limit(cr_, m); }
  void setLineDash(const std::vector<float> &dashes) {
    std::vector<double> d(dashes.begin(), dashes.end());
    cairo_set_dash(cr_, d.empty() ? nullptr : d.data(),
                   static_cast<int>(d.size()), dashOffset_);
  }
  void setLineDashOffset(float offset) {
    dashOffset_ = offset;
    int n = cairo_get_dash_count(cr_);
    std::vector<double> d(static_cast<size_t>(std::max(0, n)));
    if (n > 0)
      cairo_get_dash(cr_, d.data(), nullptr);
    cairo_set_dash(cr_, n > 0 ? d.data() : nullptr, n, offset);
  }
  void setGlobalAlpha(float a) { globalAlpha_ = std::clamp(a, 0.0f, 1.0f); }
  void setGlobalCompositeOperation(CompositeOp op) {
    cairo_set_operator(cr_, toCairoOp(op));
  }
  void setShadow(Color color, float /*blurPx, unsupported: unblurred*/,
                 float offsetX, float offsetY) {
    shadowColor_ = color;
    shadowOffsetX_ = offsetX;
    shadowOffsetY_ = offsetY;
  }
  void setFont(const std::string &family, float sizePx,
               FontWeight weight = FontWeight::Regular,
               FontStyle style = FontStyle::Normal) {
    font_.fontFamily = family;
    font_.fontSize = sizePx;
    font_.fontWeight = weight;
    font_.fontStyle = style;
  }
  void setTextAlign(TextAlign a) { textAlign_ = a; }
  void setTextBaseline(TextBaseline b) { textBaseline_ = b; }

  // ---- path building ----
  void beginPath() { cairo_new_path(cr_); }
  void closePath() { cairo_close_path(cr_); }
  void moveTo(float x, float y) { cairo_move_to(cr_, x, y); }
  void lineTo(float x, float y) { cairo_line_to(cr_, x, y); }
  void quadraticCurveTo(float cpx, float cpy, float x, float y) {
    double cx0 = 0, cy0 = 0;
    if (cairo_has_current_point(cr_))
      cairo_get_current_point(cr_, &cx0, &cy0);
    float c1x =
        static_cast<float>(cx0) + 2.0f / 3.0f * (cpx - static_cast<float>(cx0));
    float c1y =
        static_cast<float>(cy0) + 2.0f / 3.0f * (cpy - static_cast<float>(cy0));
    float c2x = x + 2.0f / 3.0f * (cpx - x);
    float c2y = y + 2.0f / 3.0f * (cpy - y);
    cairo_curve_to(cr_, c1x, c1y, c2x, c2y, x, y);
  }
  void bezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x,
                     float y) {
    cairo_curve_to(cr_, c1x, c1y, c2x, c2y, x, y);
  }
  void arc(float cx, float cy, float r, float startAngle, float endAngle,
           bool ccw = false) {
    if (ccw)
      cairo_arc_negative(cr_, cx, cy, r, startAngle, endAngle);
    else
      cairo_arc(cr_, cx, cy, r, startAngle, endAngle);
  }
  void arcTo(float x1, float y1, float x2, float y2, float radius) {
    double x0 = 0, y0 = 0;
    if (cairo_has_current_point(cr_))
      cairo_get_current_point(cr_, &x0, &y0);
    float dx1 = static_cast<float>(x0) - x1, dy1 = static_cast<float>(y0) - y1;
    float dx2 = x2 - x1, dy2 = y2 - y1;
    float len1 = std::sqrt(dx1 * dx1 + dy1 * dy1);
    float len2 = std::sqrt(dx2 * dx2 + dy2 * dy2);
    if (len1 < 1e-6f || len2 < 1e-6f || radius <= 0) {
      cairo_line_to(cr_, x1, y1);
      return;
    }
    dx1 /= len1;
    dy1 /= len1;
    dx2 /= len2;
    dy2 /= len2;
    float cosA = std::clamp(dx1 * dx2 + dy1 * dy2, -1.0f, 1.0f);
    float angle = std::acos(cosA);
    if (angle < 1e-6f) {
      cairo_line_to(cr_, x1, y1);
      return;
    }
    float dist = radius / std::tan(angle / 2.0f);
    float t1x = x1 + dx1 * dist, t1y = y1 + dy1 * dist;
    float t2x = x1 + dx2 * dist, t2y = y1 + dy2 * dist;
    cairo_line_to(cr_, t1x, t1y);
    float bisx = dx1 + dx2, bisy = dy1 + dy2;
    float bisLen = std::sqrt(bisx * bisx + bisy * bisy);
    if (bisLen < 1e-6f) {
      cairo_line_to(cr_, x1, y1);
      return;
    }
    bisx /= bisLen;
    bisy /= bisLen;
    float centerDist = std::sqrt(radius * radius + dist * dist);
    float ccx = x1 + bisx * centerDist, ccy = y1 + bisy * centerDist;
    float a0 = std::atan2(t1y - ccy, t1x - ccx);
    float a1 = std::atan2(t2y - ccy, t2x - ccx);
    float cross = dx1 * dy2 - dy1 * dx2;
    if (cross > 0)
      cairo_arc_negative(cr_, ccx, ccy, radius, a0, a1);
    else
      cairo_arc(cr_, ccx, ccy, radius, a0, a1);
  }
  void ellipse(float cx, float cy, float rx, float ry, float rotation,
               float startAngle, float endAngle, bool ccw = false) {
    // Standard Cairo idiom: build the arc against a unit circle under a
    // temporarily scaled/rotated/translated CTM, then restore — the path
    // points get baked into the OUTER coordinate system as this shape's
    // true (rx,ry)-scaled ellipse, and any path built before/after this
    // call in the outer frame stays correctly connected.
    cairo_save(cr_);
    cairo_translate(cr_, cx, cy);
    cairo_rotate(cr_, rotation);
    cairo_scale(cr_, std::max(1e-6f, rx), std::max(1e-6f, ry));
    if (ccw)
      cairo_arc_negative(cr_, 0, 0, 1, startAngle, endAngle);
    else
      cairo_arc(cr_, 0, 0, 1, startAngle, endAngle);
    cairo_restore(cr_);
  }
  void rect(float x, float y, float w, float h) {
    cairo_rectangle(cr_, x, y, w, h);
  }
  void roundRect(float x, float y, float w, float h, float radius) {
    constexpr float kPi = 3.14159265358979f;
    radius = std::max(0.0f, std::min(radius, std::min(w, h) / 2.0f));
    cairo_move_to(cr_, x + radius, y);
    cairo_line_to(cr_, x + w - radius, y);
    cairo_arc(cr_, x + w - radius, y + radius, radius, -kPi / 2, 0);
    cairo_line_to(cr_, x + w, y + h - radius);
    cairo_arc(cr_, x + w - radius, y + h - radius, radius, 0, kPi / 2);
    cairo_line_to(cr_, x + radius, y + h);
    cairo_arc(cr_, x + radius, y + h - radius, radius, kPi / 2, kPi);
    cairo_line_to(cr_, x, y + radius);
    cairo_arc(cr_, x + radius, y + radius, radius, kPi, kPi * 1.5);
    cairo_close_path(cr_);
  }

  // ---- drawing ----
  // fill()/stroke()/clip() preserve the current path (via the *_preserve
  // variants), matching HTML canvas semantics where only beginPath()
  // clears it.
  void fill(FillRule rule = FillRule::NonZero) {
    cairo_set_fill_rule(cr_, rule == FillRule::EvenOdd
                                 ? CAIRO_FILL_RULE_EVEN_ODD
                                 : CAIRO_FILL_RULE_WINDING);
    drawShadowThenReal([&] { cairo_fill_preserve(cr_); }, fillPaint_);
  }
  void stroke() {
    drawShadowThenReal([&] { cairo_stroke_preserve(cr_); }, strokePaint_);
  }
  void clip(FillRule rule = FillRule::NonZero) {
    cairo_set_fill_rule(cr_, rule == FillRule::EvenOdd
                                 ? CAIRO_FILL_RULE_EVEN_ODD
                                 : CAIRO_FILL_RULE_WINDING);
    cairo_clip_preserve(cr_);
  }
  void fillRect(float x, float y, float w, float h) {
    cairo_path_t *orig = cairo_copy_path(cr_);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, x, y, w, h);
    drawShadowThenReal([&] { cairo_fill(cr_); }, fillPaint_);
    cairo_new_path(cr_);
    cairo_append_path(cr_, orig);
    cairo_path_destroy(orig);
  }
  void strokeRect(float x, float y, float w, float h) {
    cairo_path_t *orig = cairo_copy_path(cr_);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, x, y, w, h);
    drawShadowThenReal([&] { cairo_stroke(cr_); }, strokePaint_);
    cairo_new_path(cr_);
    cairo_append_path(cr_, orig);
    cairo_path_destroy(orig);
  }
  // A true partial-area clear (unlike the Windows backend — see the
  // Canvas limitations note), implemented via CAIRO_OPERATOR_CLEAR.
  void clearRect(float x, float y, float w, float h) {
    cairo_path_t *orig = cairo_copy_path(cr_);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, x, y, w, h);
    cairo_operator_t prevOp = cairo_get_operator(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_fill(cr_);
    cairo_set_operator(cr_, prevOp);
    cairo_new_path(cr_);
    cairo_append_path(cr_, orig);
    cairo_path_destroy(orig);
  }

  // ---- text ----
  TextMetricsResult measureText(const std::string &text) {
    TextStyle ts = fontToTextStyle();
    PangoLayout *layout = liteui_text::makeLayout(
        text, ts, resolveStatic(ts.fontSize), resolveStatic(ts.fontWeight),
        resolveStatic(ts.fontStyle), -1, cr_);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    return {static_cast<float>(w)};
  }
  void fillText(const std::string &text, float x, float y,
                float maxWidth = -1) {
    drawTextImpl(text, x, y, maxWidth, false);
  }
  void strokeText(const std::string &text, float x, float y,
                  float maxWidth = -1) {
    drawTextImpl(text, x, y, maxWidth, true);
  }

  // ---- images ----
  void drawImage(const CanvasImage &img, float dx, float dy) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy,
              static_cast<float>(img.width), static_cast<float>(img.height));
  }
  void drawImage(const CanvasImage &img, float dx, float dy, float dw,
                 float dh) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy, dw, dh);
  }
  // Draws `img`, converting it to a fresh Cairo image surface first unless
  // imageCacheOwner_ already has one cached for this exact pixel buffer
  // (see the constructor's comment and the Windows drawImage's — same
  // caching heuristic and the same caveat about in-place pixel mutation
  // applies here).
  void drawImage(const CanvasImage &img, float sx, float sy, float sw, float sh,
                 float dx, float dy, float dw, float dh) {
    if (img.width <= 0 || img.height <= 0)
      return;
    cairo_surface_t *surf = nullptr;
    bool cacheHit =
        imageCacheOwner_ && imageCacheOwner_->cachedImageSurface &&
        imageCacheOwner_->cachedImagePixels == img.pixels.data() &&
        imageCacheOwner_->cachedImagePixelsSize == img.pixels.size() &&
        imageCacheOwner_->cachedImageW == img.width &&
        imageCacheOwner_->cachedImageH == img.height;
    if (cacheHit) {
      surf = imageCacheOwner_->cachedImageSurface;
    } else {
      surf = imageToCairoSurface(img);
      if (imageCacheOwner_) {
        if (imageCacheOwner_->cachedImageSurface)
          cairo_surface_destroy(imageCacheOwner_->cachedImageSurface);
        imageCacheOwner_->cachedImageSurface = surf;
        imageCacheOwner_->cachedImagePixels = img.pixels.data();
        imageCacheOwner_->cachedImagePixelsSize = img.pixels.size();
        imageCacheOwner_->cachedImageW = img.width;
        imageCacheOwner_->cachedImageH = img.height;
      }
    }
    cairo_save(cr_);
    cairo_translate(cr_, dx, dy);
    cairo_scale(cr_, dw / std::max(1.0f, sw), dh / std::max(1.0f, sh));
    cairo_translate(cr_, -sx, -sy);
    cairo_set_source_surface(cr_, surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_BILINEAR);
    cairo_paint_with_alpha(cr_, globalAlpha_);
    cairo_restore(cr_);
    // Only destroy when NOT cache-owned — see the Windows drawImage's
    // matching comment.
    if (!imageCacheOwner_)
      cairo_surface_destroy(surf);
  }

  // ---- pixel data ----
  // Fully supported: reads straight out of this canvas's own backing
  // Cairo image surface (the render target `cr_` draws into).
  std::optional<CanvasImage> getImageData(float x, float y, float w, float h) {
    cairo_surface_t *target = cairo_get_target(cr_);
    cairo_surface_flush(target);
    int sw = cairo_image_surface_get_width(target);
    int sh = cairo_image_surface_get_height(target);
    int ix = static_cast<int>(std::floor(x)),
        iy = static_cast<int>(std::floor(y));
    int iw = std::max(0, static_cast<int>(std::round(w)));
    int ih = std::max(0, static_cast<int>(std::round(h)));
    CanvasImage out(iw, ih);
    unsigned char *src = cairo_image_surface_get_data(target);
    int stride = cairo_image_surface_get_stride(target);
    for (int row = 0; row < ih; ++row) {
      int sy = iy + row;
      if (sy < 0 || sy >= sh)
        continue;
      for (int col = 0; col < iw; ++col) {
        int sx = ix + col;
        if (sx < 0 || sx >= sw)
          continue;
        unsigned char *p = src + sy * stride + sx * 4;
        unsigned char b = p[0], g = p[1], r = p[2], a = p[3];
        unsigned char *dst =
            out.pixels.data() + (static_cast<size_t>(row) * iw + col) * 4;
        dst[0] = a ? static_cast<unsigned char>(std::min(255, r * 255 / a)) : 0;
        dst[1] = a ? static_cast<unsigned char>(std::min(255, g * 255 / a)) : 0;
        dst[2] = a ? static_cast<unsigned char>(std::min(255, b * 255 / a)) : 0;
        dst[3] = a;
      }
    }
    return out;
  }
  // putImageData ignores the current transform/compositing and pokes
  // pixels directly, matching HTML canvas semantics.
  void putImageData(const CanvasImage &img, float dx, float dy) {
    if (img.width <= 0 || img.height <= 0)
      return;
    cairo_surface_t *target = cairo_get_target(cr_);
    cairo_surface_flush(target);
    int sw = cairo_image_surface_get_width(target);
    int sh = cairo_image_surface_get_height(target);
    unsigned char *dst = cairo_image_surface_get_data(target);
    int stride = cairo_image_surface_get_stride(target);
    int ix = static_cast<int>(std::round(dx)),
        iy = static_cast<int>(std::round(dy));
    for (int row = 0; row < img.height; ++row) {
      int ty = iy + row;
      if (ty < 0 || ty >= sh)
        continue;
      for (int col = 0; col < img.width; ++col) {
        int tx = ix + col;
        if (tx < 0 || tx >= sw)
          continue;
        const unsigned char *s =
            img.pixels.data() +
            (static_cast<size_t>(row) * img.width + col) * 4;
        unsigned char r = s[0], g = s[1], b = s[2], a = s[3];
        unsigned char *p = dst + ty * stride + tx * 4;
        p[0] = static_cast<unsigned char>(b * a / 255);
        p[1] = static_cast<unsigned char>(g * a / 255);
        p[2] = static_cast<unsigned char>(r * a / 255);
        p[3] = a;
      }
    }
    cairo_surface_mark_dirty(target);
  }

private:
  struct Paint {
    bool isGradient = false;
    Color color{0, 0, 0, 255};
    CanvasGradient gradient;
    static Paint solid(Color c) {
      Paint p;
      p.isGradient = false;
      p.color = c;
      return p;
    }
    static Paint grad(const CanvasGradient &g) {
      Paint p;
      p.isGradient = true;
      p.gradient = g;
      return p;
    }
  };

  struct ExtraState {
    Paint fill, stroke;
    float globalAlpha;
    TextStyle font;
    TextAlign textAlign;
    TextBaseline textBaseline;
    Color shadowColor;
    float shadowOffsetX, shadowOffsetY;
  };

  static Color colorOf(const Paint &p) {
    if (!p.isGradient)
      return p.color;
    return p.gradient.stops.empty() ? Color{0, 0, 0, 255}
                                    : p.gradient.stops.front().color;
  }

  static cairo_operator_t toCairoOp(CompositeOp op) {
    switch (op) {
    case CompositeOp::SourceOver:
      return CAIRO_OPERATOR_OVER;
    case CompositeOp::SourceIn:
      return CAIRO_OPERATOR_IN;
    case CompositeOp::SourceOut:
      return CAIRO_OPERATOR_OUT;
    case CompositeOp::SourceAtop:
      return CAIRO_OPERATOR_ATOP;
    case CompositeOp::DestinationOver:
      return CAIRO_OPERATOR_DEST_OVER;
    case CompositeOp::DestinationIn:
      return CAIRO_OPERATOR_DEST_IN;
    case CompositeOp::DestinationOut:
      return CAIRO_OPERATOR_DEST_OUT;
    case CompositeOp::DestinationAtop:
      return CAIRO_OPERATOR_DEST_ATOP;
    case CompositeOp::Lighter:
      return CAIRO_OPERATOR_ADD;
    case CompositeOp::Copy:
      return CAIRO_OPERATOR_SOURCE;
    case CompositeOp::Xor:
      return CAIRO_OPERATOR_XOR;
    case CompositeOp::Multiply:
      return CAIRO_OPERATOR_MULTIPLY;
    case CompositeOp::Screen:
      return CAIRO_OPERATOR_SCREEN;
    }
    return CAIRO_OPERATOR_OVER;
  }

  void applyPaint(const Paint &p) {
    if (!p.isGradient) {
      Color c = p.color;
      cairo_set_source_rgba(cr_, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                            (c.a / 255.0) * globalAlpha_);
      return;
    }
    cairo_pattern_t *pat =
        p.gradient.kind == CanvasGradient::Kind::Linear
            ? cairo_pattern_create_linear(p.gradient.x0, p.gradient.y0,
                                          p.gradient.x1, p.gradient.y1)
            : cairo_pattern_create_radial(p.gradient.x0, p.gradient.y0,
                                          p.gradient.r0, p.gradient.x1,
                                          p.gradient.y1, p.gradient.r1);
    for (auto &s : p.gradient.stops)
      cairo_pattern_add_color_stop_rgba(pat, std::clamp(s.offset, 0.0f, 1.0f),
                                        s.color.r / 255.0, s.color.g / 255.0,
                                        s.color.b / 255.0,
                                        (s.color.a / 255.0) * globalAlpha_);
    cairo_set_source(cr_, pat);
    cairo_pattern_destroy(pat);
  }

  // Cairo paths are stored as literal coordinate data, not re-evaluated
  // against a later CTM — cairo_copy_path()/cairo_append_path() is the
  // standard way to "replay" the current path under a temporarily
  // different transform (used here to offset a shadow copy) and to put
  // the app's original path back afterward, since fill()/stroke() must
  // leave the current path exactly as HTML canvas would (untouched,
  // aside from the intentional *_preserve semantics).
  template <class Fn> void drawShadowThenReal(Fn drawFn, const Paint &paint) {
    cairo_path_t *orig = cairo_copy_path(cr_);
    if (shadowColor_.a > 0 && (shadowOffsetX_ != 0 || shadowOffsetY_ != 0)) {
      cairo_save(cr_);
      cairo_translate(cr_, shadowOffsetX_, shadowOffsetY_);
      cairo_new_path(cr_);
      cairo_append_path(cr_, orig);
      applyPaint(Paint::solid(shadowColor_));
      drawFn();
      cairo_restore(cr_);
    }
    cairo_new_path(cr_);
    cairo_append_path(cr_, orig);
    applyPaint(paint);
    drawFn();
    cairo_path_destroy(orig);
  }

  static cairo_surface_t *imageToCairoSurface(const CanvasImage &img) {
    int w = img.width, h = img.height;
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int row = 0; row < h; ++row) {
      unsigned char *dst = data + row * stride;
      const unsigned char *src =
          img.pixels.data() + static_cast<size_t>(row) * w * 4;
      for (int col = 0; col < w; ++col) {
        unsigned char r = src[col * 4 + 0], g = src[col * 4 + 1],
                      b = src[col * 4 + 2], a = src[col * 4 + 3];
        // Cairo's ARGB32 wants premultiplied, native-endian 32-bit
        // pixels — byte order B,G,R,A on the little-endian platforms
        // this file targets (same convention as ensureTextTexture above).
        unsigned char *px = dst + col * 4;
        if (a == 255) { // opaque: premultiply is a no-op (x * 255 / 255 == x)
          px[0] = b;
          px[1] = g;
          px[2] = r;
          px[3] = 255;
        } else {
          px[0] = static_cast<unsigned char>(b * a / 255);
          px[1] = static_cast<unsigned char>(g * a / 255);
          px[2] = static_cast<unsigned char>(r * a / 255);
          px[3] = a;
        }
      }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
  }

  TextStyle fontToTextStyle() const {
    TextStyle ts = font_;
    ts.wrap = TextWrap::NoWrap;
    return ts;
  }

  // maxWidth is accepted for API familiarity with HTML canvas but not
  // currently applied (no horizontal squeeze-to-fit is performed).
  void drawTextImpl(const std::string &text, float x, float y, float,
                    bool stroked) {
    TextStyle ts = fontToTextStyle();
    PangoLayout *layout = liteui_text::makeLayout(
        text, ts, resolveStatic(ts.fontSize), resolveStatic(ts.fontWeight),
        resolveStatic(ts.fontStyle), -1, cr_);
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    int baseline = pango_layout_iter_get_baseline(iter) / PANGO_SCALE;
    pango_layout_iter_free(iter);
    float baseX = x, baseY = y;
    switch (textAlign_) {
    case TextAlign::Center:
      baseX -= pw / 2.0f;
      break;
    case TextAlign::End:
      baseX -= static_cast<float>(pw);
      break;
    default:
      break;
    }
    switch (textBaseline_) {
    case TextBaseline::Top:
      break;
    case TextBaseline::Middle:
      baseY -= ph / 2.0f;
      break;
    case TextBaseline::Bottom:
      baseY -= static_cast<float>(ph);
      break;
    case TextBaseline::Alphabetic:
    default:
      baseY -= static_cast<float>(baseline);
      break;
    }
    auto paintOnce = [&](Color c, float ox, float oy) {
      cairo_set_source_rgba(cr_, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                            (c.a / 255.0) * globalAlpha_);
      cairo_move_to(cr_, baseX + ox, baseY + oy);
      pango_cairo_show_layout(cr_, layout);
    };
    if (shadowColor_.a > 0 && (shadowOffsetX_ != 0 || shadowOffsetY_ != 0))
      paintOnce(shadowColor_, shadowOffsetX_, shadowOffsetY_);
    if (!stroked) {
      paintOnce(colorOf(fillPaint_), 0, 0);
    } else {
      // Approximates a stroked glyph outline the same way the Windows
      // backend does (see its drawTextImpl comment) — not a true glyph
      // contour outline.
      Color base = colorOf(strokePaint_);
      double lw = cairo_get_line_width(cr_);
      float r = static_cast<float>(std::max(1.0, lw / 2.0));
      static const float offs[8][2] = {
          {1, 0},       {-1, 0},       {0, 1},        {0, -1},
          {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
      for (auto &o : offs)
        paintOnce(base, o[0] * r, o[1] * r);
    }
    g_object_unref(layout);
  }

  cairo_t *cr_;
  float width_, height_;
  // Not owned — lifetime is the canvas node's own Computed (see the
  // constructor's comment). Null when there's no per-node cache to use.
  const View::Computed *imageCacheOwner_ = nullptr;

  Paint fillPaint_ = Paint::solid(Color{0, 0, 0, 255});
  Paint strokePaint_ = Paint::solid(Color{0, 0, 0, 255});
  float dashOffset_ = 0.0f;
  float globalAlpha_ = 1.0f;
  Color shadowColor_{0, 0, 0, 0};
  float shadowOffsetX_ = 0.0f, shadowOffsetY_ = 0.0f;
  TextStyle font_;
  TextAlign textAlign_ = TextAlign::Start;
  TextBaseline textBaseline_ = TextBaseline::Alphabetic;

  std::vector<ExtraState> extraStack_;
};

#endif

// ---------------- Layout algorithm (flexbox subset) ----------------
//
// Two passes per subtree:
//  Pass 1. measureNatural() — bottom-up. Resolves a node's own border-box size:
//     Fixed/Percentage/Full resolve directly against the parent's available
//     space; Fit sums/maxes its children's own natural sizes.
//  Pass 2. placeNode() — top-down. Given a node's already-decided final box
//  (from
//     pass 1 at the root, or from the parent's flex distribution below it),
//     computes each child's basis size, distributes leftover space via
//     flexGrow/flexShrink, positions via justifyContent/alignItems, and
//     recurses.

// Known limitations (fine for v1, revisit if needed): percentages resolve to
// 0 when an ancestor's size is itself Fit (indefinite). flex-wrap only
// affects placeNode's placement pass — measureNatural's Fit sizing still
// assumes a single line, since a Fit main axis has no definite width to
// wrap against in the first place; give the container a definite/Full main
// size if you want Fit-height wrapping content to actually wrap.
namespace liteui_layout {

struct Natural {
  float w, h;
};

inline float resolveAxis(const Size &s, float available, bool definite,
                         float fitValue) {
  switch (s.kind) {
  case Size::Kind::Fixed:
    return s.value;
  case Size::Kind::Percentage:
    return definite ? available * s.value / 100.0f : fitValue;
  case Size::Kind::Full:
    return definite ? available : fitValue;
  case Size::Kind::Fit:
    return fitValue;
  }
  return fitValue;
}

// Clamps a resolved axis size into [minV, maxV], guarding against a
// misconfigured maxV < minV by falling back to minV.
inline float clampSize(float v, float minV, float maxV) {
  return std::clamp(v, minV, std::max(minV, maxV));
}

// Whether a given axis's scrollbar should actually be drawn/interactive:
// Scroll always shows it, Auto shows it only once content overflows the
// viewport (the "+ 0.5" is just slack against float rounding so a
// perfectly-fitting container doesn't flicker a bar on and off), and
// Hidden/Visible never do. Used by both placeNode (to decide whether to
// reserve gutter space for it) and every renderer/hit-tester (to decide
// whether to draw/click it) — kept as one function so those two places
// can never disagree about whether a bar is showing.
inline bool axisScrollbarVisible(Overflow ov, float content, float viewport) {
  if (ov == Overflow::Scroll)
    return true;
  if (ov == Overflow::Auto)
    return content > viewport + 0.5f;
  return false; // Visible, Hidden
}

// Takes node by non-const reference (unlike the rest of this "measure"
// pass, which is conceptually read-only) for exactly one reason: a scroll
// container's natural content size — the scrollable extent — needs to be
// recorded somewhere for later use by placeNode() (to offset/clip
// children), by the scrollbar-thumb sizing code, and by input handling
// (to know how far a drag/wheel event is allowed to move the scroll
// offset). node.computed.contentW/contentH is that somewhere. Everything
// else this function does is still the same bottom-up size query it always
// was.
inline Natural measureNatural(View &node, float availW, float availH,
                              bool wDefinite, bool hDefinite) {
  const Size &styleWidth =
      resolveDynamic(node.style.width, node.computed.resolvedWidth);
  const Size &styleHeight =
      resolveDynamic(node.style.height, node.computed.resolvedHeight);
  if (node.isText) {
    const EdgeInsets &pad =
        resolveDynamic(node.style.padding, node.computed.resolvedPadding);
    bool widthIsFit = styleWidth.kind == Size::Kind::Fit;
    bool widthIndefinitePercentage =
        styleWidth.kind == Size::Kind::Percentage && !wDefinite;
    float outerW = clampSize(resolveAxis(styleWidth, availW, wDefinite, 0),
                             node.style.minWidth, node.style.maxWidth);
    float measureWidth = (widthIsFit || widthIndefinitePercentage)
                             ? -1.0f
                             : std::max(0.0f, outerW - pad.left - pad.right);
    liteui_text::Measurement m = liteui_text::measure(
        resolveDynamic(node.text, node.computed.resolvedText), node.textStyle,
        resolveDynamic(node.textStyle.fontSize, node.computed.resolvedFontSize),
        resolveDynamic(node.textStyle.fontWeight,
                       node.computed.resolvedFontWeight),
        resolveDynamic(node.textStyle.fontStyle,
                       node.computed.resolvedFontStyle),
        measureWidth);
    float w = (widthIsFit || widthIndefinitePercentage)
                  ? clampSize(m.width + pad.left + pad.right,
                              node.style.minWidth, node.style.maxWidth)
                  : outerW;
    float naturalH = m.height + pad.top + pad.bottom;
    float h =
        clampSize(styleHeight.kind == Size::Kind::Fit
                      ? naturalH
                      : resolveAxis(styleHeight, availH, hDefinite, naturalH),
                  node.style.minHeight, node.style.maxHeight);
    return {w, h};
  }
  bool horizontal = node.style.direction == FlexDirection::Row;
  const EdgeInsets &pad =
      resolveDynamic(node.style.padding, node.computed.resolvedPadding);
  bool needW = styleWidth.kind == Size::Kind::Fit;
  bool needH = styleHeight.kind == Size::Kind::Fit;
  bool scrollX = node.scrollsX();
  bool scrollY = node.scrollsY();

  float w = clampSize(resolveAxis(styleWidth, availW, wDefinite, 0),
                      node.style.minWidth, node.style.maxWidth);
  float h = clampSize(resolveAxis(styleHeight, availH, hDefinite, 0),
                      node.style.minHeight, node.style.maxHeight);
  // A scroll container must still visit its children even when neither
  // axis is Fit (e.g. a fixed-size scrollable box) — that's the whole
  // point: we need to know how big the content *wants* to be so we know
  // how far it can scroll, even though the container's own box size
  // doesn't depend on that at all.
  if ((!needW && !needH && !scrollX && !scrollY) || node.children.empty())
    return {w, h};

  // Along an axis this node scrolls, children are measured against
  // effectively unbounded space so they report their true desired size
  // instead of being squeezed into the viewport — that natural total is
  // exactly the "scrollable extent". Along a non-scrolling axis, sizing is
  // unchanged from before (children measured against the resolved inner
  // box, or against availW/availH while this node's own size is still
  // being figured out).
  constexpr float kUnbounded = std::numeric_limits<float>::max() / 4;
  float innerW = scrollX ? kUnbounded
                 : (wDefinite && !needW)
                     ? std::max(0.0f, w - pad.left - pad.right)
                     : availW;
  float innerH = scrollY ? kUnbounded
                 : (hDefinite && !needH)
                     ? std::max(0.0f, h - pad.top - pad.bottom)
                     : availH;

  float mainTotal = 0, crossMax = 0;
  bool firstFlow = true;
  for (size_t i = 0; i < node.children.size(); ++i) {
    View &c = node.children[i];
    if (c.style.position == Position::Absolute ||
        resolveDynamic(c.style.display, c.computed.resolvedDisplay) ==
            Display::None)
      continue;
    Natural cn = measureNatural(c, innerW, innerH,
                                scrollX ? false : (wDefinite || !needW),
                                scrollY ? false : (hDefinite || !needH));
    const EdgeInsets &cMargin =
        resolveDynamic(c.style.margin, c.computed.resolvedMargin);
    float mm = cMargin.left + cMargin.right;
    float mv = cMargin.top + cMargin.bottom;
    float childMain = horizontal ? cn.w + mm : cn.h + mv;
    float childCross = horizontal ? cn.h + mv : cn.w + mm;
    if (!firstFlow)
      mainTotal += node.style.gap;
    mainTotal += childMain;
    crossMax = std::max(crossMax, childCross);
    firstFlow = false;
  }
  if (needW)
    w = clampSize((horizontal ? mainTotal : crossMax) + pad.left + pad.right,
                  node.style.minWidth, node.style.maxWidth);

  if (needH)
    h = clampSize((horizontal ? crossMax : mainTotal) + pad.top + pad.bottom,
                  node.style.minHeight, node.style.maxHeight);

  // Content extent is deliberately NOT run through clampSize/min-max: a
  // container's max-width doesn't shrink its *content*, only its own box
  // (that's the entire reason overflow is a thing).
  if (scrollX)
    node.computed.contentW =
        (horizontal ? mainTotal : crossMax) + pad.left + pad.right;
  if (scrollY)
    node.computed.contentH =
        (horizontal ? crossMax : mainTotal) + pad.top + pad.bottom;
  return {w, h};
}

inline void placeNode(View &node, float x, float y, float w, float h) {
  // contentW/contentH here already existed as "content's natural size" on
  // node.computed before this call, set by the measureNatural() pass that
  // ran over this same node earlier (either from the parent's per-child
  // loop, or from layoutRoot() for the root). We're about to overwrite
  // x/y/w/h with the final viewport box; contentW/contentH and
  // scrollX/scrollY are left untouched by this assignment.
  node.computed.x = x;
  node.computed.y = y;
  node.computed.w = w;
  node.computed.h = h;
  if (node.onLayout)
    node.onLayout(x, y, w, h);

  if (node.children.empty()) {
    // No children means nothing to scroll regardless of overflow setting;
    // pin the offset at 0 so a container that briefly had children (and
    // therefore a scroll offset) doesn't leave a stale one behind if it's
    // ever emptied out.
    node.computed.scrollX = node.computed.scrollY = 0;
    return;
  }

  bool horizontal = node.style.direction == FlexDirection::Row;
  const EdgeInsets &pad =
      resolveDynamic(node.style.padding, node.computed.resolvedPadding);
  float contentW = std::max(0.0f, w - pad.left - pad.right);
  float contentH = std::max(0.0f, h - pad.top - pad.bottom);

  // ---- Scrolling: gutter reservation, offset clamping, content origin ----
  // Whether each axis's scrollbar is actually showing determines whether it
  // eats into the space available for children — same rule CSS uses (a
  // visible scrollbar shrinks the content box on the OTHER axis; the
  // scrolling axis itself is unbounded so its own bar doesn't need to
  // "make room" against itself).
  bool showVBar =
      node.scrollsY() &&
      axisScrollbarVisible(node.style.overflowY, node.computed.contentH, h);
  bool showHBar =
      node.scrollsX() &&
      axisScrollbarVisible(node.style.overflowX, node.computed.contentW, w);
  if (showVBar)
    contentW = std::max(0.0f, contentW - kScrollbarThickness);
  if (showHBar)
    contentH = std::max(0.0f, contentH - kScrollbarThickness);

  // Scroll offsets are user/input-driven state that can go stale the
  // instant content size or viewport size changes (a window resize, or
  // content shrinking), so every relayout re-clamps them into range rather
  // than trusting whatever a previous frame left behind.
  if (node.scrollsX())
    node.computed.scrollX =
        std::clamp(node.computed.scrollX, 0.0f, node.maxScrollX());
  else
    node.computed.scrollX = 0;
  if (node.scrollsY())
    node.computed.scrollY =
        std::clamp(node.computed.scrollY, 0.0f, node.maxScrollY());
  else
    node.computed.scrollY = 0;

  // The content origin simply shifts by the (clamped) scroll offset —
  // children are positioned exactly as they would be at scroll (0,0), then
  // this single subtraction slides the whole subtree. Clipping (handled by
  // the renderers/hit-testers, not here) is what actually hides the part
  // that scrolls out of view.
  float contentX = x + pad.left - node.computed.scrollX;
  float contentY = y + pad.top - node.computed.scrollY;

  float mainAvail = horizontal ? contentW : contentH;
  float crossAvail = horizontal ? contentH : contentW;
  // A scrolling main axis must never let flexShrink squeeze children below
  // their natural size just because the viewport is smaller than the
  // content — that's the entire point of scrolling instead of shrinking.
  // Widening mainAvail to at least the natural content total makes
  // "leftover" in the flex-resolution pass below >= 0, which keeps every
  // shrink factor's candidate at-or-above its unclamped basis.
  if (horizontal && node.scrollsX())
    mainAvail =
        std::max(mainAvail, node.computed.contentW - pad.left - pad.right);
  if (!horizontal && node.scrollsY())
    mainAvail =
        std::max(mainAvail, node.computed.contentH - pad.top - pad.bottom);

  size_t n = node.children.size();
  bool wrap = node.style.flexWrap == FlexWrap::Wrap;
  std::vector<size_t> flowIdx;
  flowIdx.reserve(node.children.size());
  for (size_t i = 0; i < node.children.size(); ++i) {
    const Style &cs = node.children[i].style;
    if (cs.position != Position::Absolute &&
        resolveDynamic(cs.display, node.children[i].computed.resolvedDisplay) !=
            Display::None)
      flowIdx.push_back(i);
  }
  n = flowIdx.size();
  std::vector<float> basis(n), cross(n), mMainS(n), mMainE(n), mCrossS(n),
      mCrossE(n), minMain(n), maxMain(n), marginMain(n);

  for (size_t k = 0; k < n; ++k) {
    View &c = node.children[flowIdx[k]];
    Natural cn = measureNatural(c, contentW, contentH, true, true);
    const EdgeInsets &cMargin =
        resolveDynamic(c.style.margin, c.computed.resolvedMargin);
    basis[k] = horizontal ? cn.w : cn.h;
    cross[k] = horizontal ? cn.h : cn.w;
    mMainS[k] = horizontal ? cMargin.left : cMargin.top;
    mMainE[k] = horizontal ? cMargin.right : cMargin.bottom;
    mCrossS[k] = horizontal ? cMargin.top : cMargin.left;
    mCrossE[k] = horizontal ? cMargin.bottom : cMargin.right;
    minMain[k] = horizontal ? c.style.minWidth : c.style.minHeight;
    maxMain[k] = horizontal ? c.style.maxWidth : c.style.maxHeight;
    marginMain[k] = mMainS[k] + mMainE[k];
  }

  // ---- Line breaking ----
  // With wrap disabled this is always one line spanning every child (the
  // original single-line behavior, byte-for-byte). With wrap enabled,
  // children are greedily packed onto a line until the next child's basis
  // would overflow mainAvail, at which point a new line starts. A line
  // always takes at least one child, even an oversized one, so a single
  // giant child can't stall the packer.
  struct Line {
    size_t begin, end; // half-open [begin, end) into node.children
  };
  std::vector<Line> lines;
  if (!wrap) {
    lines.push_back({0, n});
  } else {
    size_t start = 0;
    float used = 0;
    for (size_t i = 0; i < n; ++i) {
      float itemMain = basis[i] + marginMain[i];
      float withGap = (i > start) ? node.style.gap : 0.0f;
      if (i > start && used + withGap + itemMain > mainAvail) {
        lines.push_back({start, i});
        start = i;
        used = itemMain;
      } else {
        used += withGap + itemMain;
      }
    }
    lines.push_back({start, n});
  }

  // ---- Per-line main-axis flex resolution ----
  // Resolve flexGrow/flexShrink into final main-axis sizes, honoring each
  // child's own min/max — this is CSS flexbox's "resolve flexible lengths"
  // algorithm, scoped to one line's children at a time. A single pass
  // (basis + share of leftover, then clamp) would silently drop whatever a
  // clamped child couldn't absorb; instead, any item whose share would
  // violate its own bound gets frozen at that bound and removed from the
  // pool, and the remaining free space is recalculated and redistributed
  // among the still-flexible siblings. Repeats until nothing new freezes
  // (at most one extra item freezes per pass, so ln+1 passes always
  // suffices). Also tracks each line's cross size (max child cross extent)
  // for the cross-axis distribution pass below.
  std::vector<float> finalMain(n);
  std::vector<float> lineCross(lines.size());
  for (size_t li = 0; li < lines.size(); ++li) {
    size_t lb = lines[li].begin, le = lines[li].end;
    size_t ln = le - lb;
    std::vector<bool> frozen(ln, false);
    std::vector<float> lineFinal(ln);
    float gapTotal = ln > 1 ? node.style.gap * (ln - 1) : 0.0f;

    for (size_t pass = 0; pass <= ln; ++pass) {
      float used = gapTotal, gsum = 0, ssum = 0;
      for (size_t k = 0; k < ln; ++k) {
        size_t i = lb + k;
        used += (frozen[k] ? lineFinal[k] : basis[i]) + marginMain[i];
        if (!frozen[k]) {
          gsum += node.children[flowIdx[i]].style.flexGrow;
          ssum += node.children[flowIdx[i]].style.flexShrink;
        }
      }
      float leftover = mainAvail - used;
      if (leftover == 0 || (leftover > 0 && gsum <= 0) ||
          (leftover < 0 && ssum <= 0)) {
        for (size_t k = 0; k < ln; ++k)
          if (!frozen[k])
            lineFinal[k] =
                clampSize(basis[lb + k], minMain[lb + k], maxMain[lb + k]);
        break;
      }
      bool frozeAny = false;
      for (size_t k = 0; k < ln; ++k) {
        if (frozen[k])
          continue;
        size_t i = lb + k;
        const Style &cs = node.children[flowIdx[i]].style;
        float extra = leftover > 0 ? leftover * (cs.flexGrow / gsum)
                                   : leftover * (cs.flexShrink / ssum);
        float candidate = std::max(0.0f, basis[i] + extra);
        float clamped = clampSize(candidate, minMain[i], maxMain[i]);
        lineFinal[k] = clamped;
        if (clamped != candidate) {
          frozen[k] = true;
          frozeAny = true;
        }
      }
      if (!frozeAny)
        break; // this pass's candidates all satisfied their bounds — done
    }

    float maxCross = 0;
    for (size_t k = 0; k < ln; ++k) {
      size_t i = lb + k;
      finalMain[i] = lineFinal[k];
      maxCross = std::max(maxCross, cross[i] + mCrossS[i] + mCrossE[i]);
    }
    lineCross[li] = maxCross;
  }

  // ---- Distribute lines along the cross axis (align-content) ----
  // With exactly one line this collapses to the old behavior: Stretch
  // grows that line to fill crossAvail (matching the previous unconditional
  // stretch-to-container-cross-size), everything else just packs the one
  // line at the start.
  size_t numLines = lines.size();
  float lineGapTotal = numLines > 1 ? node.style.gap * (numLines - 1) : 0.0f;
  float linesTotal = lineGapTotal;
  for (float lc : lineCross)
    linesTotal += lc;
  float crossFree = std::max(0.0f, crossAvail - linesTotal);

  std::vector<float> lineOffset(numLines), lineSize(numLines);
  float crossStart = 0, crossBetween = node.style.gap;
  switch (node.style.alignContent) {
  case AlignContent::Start:
    lineSize = lineCross;
    break;
  case AlignContent::End:
    crossStart = crossFree;
    lineSize = lineCross;
    break;
  case AlignContent::Center:
    crossStart = crossFree / 2;
    lineSize = lineCross;
    break;
  case AlignContent::SpaceBetween:
    if (numLines > 1)
      crossBetween += crossFree / (numLines - 1);
    lineSize = lineCross;
    break;
  case AlignContent::SpaceAround: {
    float each = numLines ? crossFree / numLines : 0;
    crossStart = each / 2;
    crossBetween += each;
    lineSize = lineCross;
    break;
  }
  case AlignContent::SpaceEvenly: {
    float each = crossFree / (numLines + 1);
    crossStart = each;
    crossBetween += each;
    lineSize = lineCross;
    break;
  }
  case AlignContent::Stretch: {
    float extra = numLines ? crossFree / numLines : 0;
    for (size_t li = 0; li < numLines; ++li)
      lineSize[li] = lineCross[li] + extra;
    break;
  }
  }
  {
    float pos = crossStart;
    for (size_t li = 0; li < numLines; ++li) {
      lineOffset[li] = pos;
      pos += lineSize[li] + crossBetween;
    }
  }

  // ---- Per-line: justify main axis, align children within the line's
  // cross extent, then recurse ----
  for (size_t li = 0; li < numLines; ++li) {
    size_t lb = lines[li].begin, le = lines[li].end;
    size_t ln = le - lb;
    float lineCrossAvail = lineSize[li];
    float lineCrossPos = (horizontal ? contentY : contentX) + lineOffset[li];

    float totalUsed = 0;
    for (size_t k = 0; k < ln; ++k) {
      size_t i = lb + k;
      totalUsed += finalMain[i] + mMainS[i] + mMainE[i];
      if (k + 1 < ln)
        totalUsed += node.style.gap;
    }
    float freeSpace = std::max(0.0f, mainAvail - totalUsed);
    float startOffset = 0, between = node.style.gap;
    switch (node.style.justifyContent) {
    case Justify::Start:
      break;
    case Justify::End:
      startOffset = freeSpace;
      break;
    case Justify::Center:
      startOffset = freeSpace / 2;
      break;
    case Justify::SpaceBetween:
      if (ln > 1)
        between += freeSpace / (ln - 1);
      break;
    case Justify::SpaceAround: {
      float each = ln ? freeSpace / ln : 0;
      startOffset = each / 2;
      between += each;
      break;
    }
    case Justify::SpaceEvenly: {
      float each = freeSpace / (ln + 1);
      startOffset = each;
      between += each;
      break;
    }
    }

    float cursor = (horizontal ? contentX : contentY) + startOffset;
    for (size_t k = 0; k < ln; ++k) {
      size_t i = lb + k;
      View &ch = node.children[flowIdx[i]];
      cursor += mMainS[i];

      bool explicitCross =
          horizontal
              ? resolveDynamic(ch.style.height, ch.computed.resolvedHeight)
                        .kind != Size::Kind::Fit
              : resolveDynamic(ch.style.width, ch.computed.resolvedWidth)
                        .kind != Size::Kind::Fit;
      float finalCross = cross[i];
      if (node.style.alignItems == Align::Stretch && !explicitCross)
        finalCross = std::max(0.0f, lineCrossAvail - mCrossS[i] - mCrossE[i]);

      float minCross = horizontal ? ch.style.minHeight : ch.style.minWidth;
      float maxCross = horizontal ? ch.style.maxHeight : ch.style.maxWidth;
      finalCross = clampSize(finalCross, minCross, maxCross);

      float crossOffset;
      switch (node.style.alignItems) {
      case Align::End:
        crossOffset = lineCrossAvail - finalCross - mCrossE[i];
        break;
      case Align::Center:
        crossOffset = (lineCrossAvail - finalCross) / 2;
        break;
      default:
        crossOffset = mCrossS[i];
        break; // Start & Stretch
      }

      float cx = horizontal ? cursor : lineCrossPos + crossOffset;
      float cy = horizontal ? lineCrossPos + crossOffset : cursor;
      float cw = horizontal ? finalMain[i] : finalCross;
      float chh = horizontal ? finalCross : finalMain[i];

      placeNode(ch, cx, cy, cw, chh);
      cursor += finalMain[i] + mMainE[i] + between;
    }
  }

  // ---- Position::Absolute children ----
  // Placed against this node's content box, entirely independent of the
  // flex distribution above. Sizing reuses measureNatural: Fixed/
  // Percentage/Full resolve normally against contentW/contentH (always
  // definite here, since this node's own box is already finalized);
  // Fit falls back to natural content size unless both opposing edges
  // are set, in which case size is derived from them (CSS's "left+right
  // implies width" rule).
  for (auto &ch : node.children) {
    if (ch.style.position != Position::Absolute ||
        resolveDynamic(ch.style.display, ch.computed.resolvedDisplay) ==
            Display::None)
      continue;
    const Style &cs = ch.style;
    const Size &csWidth = resolveDynamic(cs.width, ch.computed.resolvedWidth);
    const Size &csHeight =
        resolveDynamic(cs.height, ch.computed.resolvedHeight);
    const EdgeInsets &csMargin =
        resolveDynamic(cs.margin, ch.computed.resolvedMargin);
    float csLeft = resolveDynamic(cs.left, ch.computed.resolvedLeft);
    float csTop = resolveDynamic(cs.top, ch.computed.resolvedTop);
    float csRight = resolveDynamic(cs.right, ch.computed.resolvedRight);
    float csBottom = resolveDynamic(cs.bottom, ch.computed.resolvedBottom);
    bool hasL = !std::isnan(csLeft), hasR = !std::isnan(csRight);
    bool hasT = !std::isnan(csTop), hasB = !std::isnan(csBottom);

    Natural probe = measureNatural(ch, contentW, contentH, true, true);
    float aw = (csWidth.kind == Size::Kind::Fit && hasL && hasR)
                   ? contentW - csLeft - csRight
                   : probe.w;
    float ah = (csHeight.kind == Size::Kind::Fit && hasT && hasB)
                   ? contentH - csTop - csBottom
                   : probe.h;
    aw = clampSize(aw, cs.minWidth, cs.maxWidth);
    ah = clampSize(ah, cs.minHeight, cs.maxHeight);

    float ax = hasL   ? contentX + csLeft + csMargin.left
               : hasR ? contentX + contentW - csRight - csMargin.right - aw
                      : contentX + csMargin.left;
    float ay = hasT   ? contentY + csTop + csMargin.top
               : hasB ? contentY + contentH - csBottom - csMargin.bottom - ah
                      : contentY + csMargin.top;

    placeNode(ch, ax, ay, aw, ah);
  }
}

// originX/originY let a caller reserve space above/left of the root — used
// on Linux to keep content out of the custom titlebar strip; Windows (no
// custom titlebar) always passes the defaults of (0, 0).
inline void layoutRoot(View &root, float windowW, float windowH,
                       float originX = 0, float originY = 0) {
  Natural n = measureNatural(root, windowW, windowH, true, true);
  placeNode(root, originX, originY, n.w, n.h);
}

} // namespace liteui_layout

// ---------------------------------------------------------------------------
// liteui_pointer — pointer hit-testing and event routing, with no window,
// platform or renderer dependency (so the unit tests can drive it directly).
// LiteUIImpl owns one Router and its platform backends translate native
// mouse messages into PointerEvent / WheelEvent calls on it. See the
// "Pointer events" comment above PointerEvent for the semantics.
// ---------------------------------------------------------------------------
namespace liteui_pointer {

// A rectangle used to accumulate the intersection of every scrollable
// ancestor's viewport as we walk down the tree, so painting and
// hit-testing can both tell "is this point/pixel actually visible, or
// has it scrolled behind a clipping ancestor". Defaults to "the whole
// plane" so the root of any walk starts unclipped.
struct ClipRect {
  float x0 = -std::numeric_limits<float>::infinity();
  float y0 = -std::numeric_limits<float>::infinity();
  float x1 = std::numeric_limits<float>::infinity();
  float y1 = std::numeric_limits<float>::infinity();
  bool contains(float px, float py) const {
    return px >= x0 && px < x1 && py >= y0 && py < y1;
  }
  // Narrows this clip to also be inside the given box — used every time
  // we descend into a scroll container's children.
  ClipRect intersect(float bx, float by, float bw, float bh) const {
    return {std::max(x0, bx), std::max(y0, by), std::min(x1, bx + bw),
            std::min(y1, by + bh)};
  }
};

// Global z-index stacking, shared by both renderers and by hit-testing.
struct AbsoluteEntry {
  const View *view;
  int order;     // document/discovery order, for stable z-index ties
  ClipRect clip; // intersection of every scrollable ancestor's viewport
};

// Returns whether (px, py) lies within v's already-computed border-box.
inline bool containsPoint(const View &v, float px, float py) {
  return px >= v.computed.x && px < v.computed.x + v.computed.w &&
         py >= v.computed.y && py < v.computed.y + v.computed.h;
}

// Walks the whole tree (not just direct children) collecting every
// Position::Absolute node, tagged with its pre-order discovery index.
// Recurses into every node regardless of its own position, so nested
// absolutes (an absolute inside another absolute's subtree) still get
// their own top-level slot in the global list. Each entry also carries
// the clip of its scrollable ancestors, so absolutes scrolled out of
// view are neither painted nor hit-tested.
inline void collectAbsolutes(const View &v, std::vector<AbsoluteEntry> &out,
                             ClipRect clip = ClipRect{}) {
  ClipRect childClip = (v.scrollsX() || v.scrollsY())
                           ? clip.intersect(v.computed.x, v.computed.y,
                                            v.computed.w, v.computed.h)
                           : clip;
  for (const auto &child : v.children) {
    if (resolveDynamic(child.style.display, child.computed.resolvedDisplay) ==
        Display::None)
      continue; // a display:none subtree contributes no absolutes either
    if (child.style.position == Position::Absolute)
      out.push_back({&child, static_cast<int>(out.size()),
                     child.style.escapeClip ? ClipRect{} : childClip});
    collectAbsolutes(child, out, childClip);
  }
}

// Sorts absolute entries by zIndex ascending, document order breaking ties.
inline void sortAbsolutes(std::vector<AbsoluteEntry> &absolutes) {
  std::stable_sort(absolutes.begin(), absolutes.end(),
                   [](const AbsoluteEntry &a, const AbsoluteEntry &b) {
                     int za = resolveDynamic(a.view->style.zIndex,
                                             a.view->computed.resolvedZIndex);
                     int zb = resolveDynamic(b.view->style.zIndex,
                                             b.view->computed.resolvedZIndex);
                     if (za != zb)
                       return za < zb;
                     return a.order < b.order;
                   });
}

// The id of the mouse; PointerEvent::pointerId defaults to it.
inline constexpr uint32_t kMousePointerId = 1;

class Router {
public:
  // ---- Wiring (set by the owner) -----------------------------------------
  View *root = nullptr; // null until a tree exists
  // Polls dynamic sources and relayouts (LiteUIImpl::pollAndRelayout).
  // Returns whether anything changed. Called after dispatch, never between
  // two handlers of one event.
  std::function<bool()> poll;
  // Moves keyboard focus (LiteUIImpl::setFocus); called on a left press.
  std::function<void(View *)> focus;

  static constexpr uint64_t kDoubleClickMs = 500;
  static constexpr float kDoubleClickSlop = 4.0f;

  Router() {
    pointers_.push_back(
        std::make_unique<PointerState>(kMousePointerId, PointerType::Mouse));
  }

  // ---- Introspection (tests, and the platform layer) ---------------------
  // Mouse unless a pointer id is given.
  uint8_t buttonsHeld(uint32_t id = kMousePointerId) const {
    const PointerState *s = find(id);
    return s ? s->held : 0;
  }
  View *capturedView(uint32_t id = kMousePointerId) const {
    const PointerState *s = find(id);
    return s ? s->captured : nullptr;
  }
  View *pressedView(MouseButton b, uint32_t id = kMousePointerId) const {
    const PointerState *s = find(id);
    return (s && b != MouseButton::None) ? s->pressed[idx(b)] : nullptr;
  }
  // Pointers the router currently tracks: the mouse, plus every finger down
  // and every pen that is in range.
  size_t pointerCount() const { return pointers_.size(); }
  bool knows(uint32_t id) const { return find(id) != nullptr; }

  // Drops every raw View* this router holds WITHOUT calling anything. Use
  // whenever views may already be destroyed (tree rebuilt, setRoot). No
  // onPointerCancel is delivered. A dispatch in progress notices
  // (see generation_) and stops bubbling. Pointers stay known (their buttons
  // are still physically held); the releases that follow are no-ops.
  void invalidate() {
    ++generation_;
    for (auto &p : pointers_) {
      for (int i = 0; i < 3; ++i)
        p->pressed[i] = nullptr;
      p->captured = nullptr;
      p->captureChain.clear();
      p->downMask = 0;
    }
    for (auto &c : lastClick_)
      c = {};
  }

  // A press-in-scrollable-content has turned into a pan: the eventual
  // release must not count as a click. The pointer's capture stays armed, so
  // a view that is dragging keeps receiving moves while the content pans.
  void dropClick(MouseButton b, uint32_t id = kMousePointerId) {
    if (PointerState *s = find(id); s && b != MouseButton::None)
      s->pressed[idx(b)] = nullptr;
  }

  // ---- Events ------------------------------------------------------------
  // e.windowX/windowY, e.button (down/up), e.mods, e.type, e.pointerId and
  // e.isPrimary must be filled by the caller; e.timeMs may be 0 (stamped with
  // the steady clock). A pen/touch contact is MouseButton::Left (see the
  // "Pointer events" comment).

  void down(PointerEvent e) {
    if (e.button == MouseButton::None)
      return;
    const MouseButton btn = e.button;
    const int i = idx(btn);
    PointerState &st = stateFor(e);
    prepare(st, e);
    st.held |= buttonBit(btn);
    st.downMask |= buttonBit(btn);
    e.buttons = st.held;
    fixPressure(e);
    e.coalesced = {};

    Scratch chain(*this);
    chainAt(e.windowX, e.windowY,
            [btn](const View &v) { return wantsPress(v, btn); }, chain.v);
    View *target = chain.v.empty() ? nullptr : chain.v.front();
    st.pressed[i] = target;

    // Focus before any handler, as it always was: a left press focuses the
    // target if it's focusable and otherwise clears focus.
    if (btn == MouseButton::Left && focus)
      focus((target && target->focusable) ? target : nullptr);

    const uint64_t gen = generation_;
    View *stopper = nullptr, *deepestDown = nullptr;
    bool aborted = false;
    for (View *v : chain.v) {
      if (v->onPointerDown) {
        if (!deepestDown)
          deepestDown = v;
        setLocal(e, *v);
        v->onPointerDown(e);
        if (gen != generation_) {
          aborted = true;
          break;
        }
        if (e.propagationStopped()) {
          stopper = v;
          break;
        }
      }
    }

    if (!aborted && !st.captured) {
      if (View *cap = stopper ? stopper : deepestDown) {
        st.captured = cap;
        st.captureChain.clear();
        st.captureChain.push_back(cap);
        ancestorsOf(*cap, st.captureChain);
      }
    }
    if (!aborted && poll)
      poll(); // reconcile what the handlers changed; may invalidate()
  }

  // Returns whether anything changed (caller should repaint).
  bool move(PointerEvent e) {
    PointerState &st = stateFor(e); // a pen hovering creates its state
    prepare(st, e);
    e.button = MouseButton::None;
    e.buttons = st.held;
    fixPressure(e);
    if (!root)
      return false;

    // A move always carries at least its own position as a sample.
    PointerSample own{e.windowX, e.windowY, e.pressure, e.tiltX,
                      e.tiltY,   e.twist,   e.timeMs};
    if (e.coalesced.empty())
      e.coalesced = std::span<const PointerSample>(&own, 1);

    const uint64_t gen = generation_;
    bool ran = false;
    {
      Scratch chain(*this);
      if (st.captured)
        chain.v = st.captureChain;
      else
        chainAt(e.windowX, e.windowY,
                [](const View &v) {
                  return (bool)v.onPointerMove && !isDisabled(v);
                },
                chain.v);
      ran |= bubble(chain.v, e, &View::onPointerMove, gen, st.captured != nullptr);
    }
    if (gen != generation_)
      return poll ? poll() : false;

    return ran && poll ? poll() : false;
  }

  // Returns whether anything changed (caller should repaint).
  bool up(PointerEvent e) {
    if (e.button == MouseButton::None)
      return false;
    PointerState *stp = find(e.pointerId);
    if (!stp)
      return false;
    PointerState &st = *stp;
    const MouseButton btn = e.button;
    const int i = idx(btn);
    prepare(st, e);
    st.held &= static_cast<uint8_t>(~buttonBit(btn));
    e.buttons = st.held;
    fixPressure(e);
    e.coalesced = {};
    const bool hadDown = (st.downMask & buttonBit(btn)) != 0;
    st.downMask &= static_cast<uint8_t>(~buttonBit(btn));
    // A finger's pointer ceases to exist when it lifts.
    const uint32_t id = st.id;
    const bool transient = st.type == PointerType::Touch && st.held == 0;
    auto finish = [&](bool r) {
      if (transient)
        erase(id);
      return r;
    };
    if (!hadDown || !root)
      return finish(false); // a release with no matching press we saw

    const uint64_t gen = generation_;

    // 1. pointerup — to the capture target, else to whatever is under it.
    {
      Scratch chain(*this);
      const bool viaCapture = st.captured != nullptr;
      if (viaCapture)
        chain.v = st.captureChain;
      else
        chainAt(e.windowX, e.windowY,
                [](const View &v) {
                  return (bool)v.onPointerUp && !isDisabled(v);
                },
                chain.v);
      bubble(chain.v, e, &View::onPointerUp, gen, viaCapture);
    }
    if (gen != generation_)
      return finish(poll ? poll() : false);
    if (st.held == 0) { // capture lasts until the last button is released
      st.captured = nullptr;
      st.captureChain.clear();
    }

    // 2. click — only if the release landed on the view the press did.
    View *pressedTarget = st.pressed[i];
    st.pressed[i] = nullptr;
    if (pressedTarget) {
      Scratch chain(*this);
      chainAt(e.windowX, e.windowY,
              [btn](const View &v) { return wantsPress(v, btn); }, chain.v);
      if (!chain.v.empty() && chain.v.front() == pressedTarget) {
        e.clickCount = countClick(pressedTarget, e);
        dispatchClick(chain.v, e, gen);
      }
    }
    return finish(poll ? poll() : false);
  }

  // MOUSE: the press of `b` can no longer complete (capture lost, an OS move
  // loop swallowed the release, the release landed in window chrome).
  // Delivers onPointerCancel to the capture target when the last button goes
  // away.
  void cancel(MouseButton b) {
    if (b == MouseButton::None)
      return;
    PointerState &st = mouse();
    const int i = idx(b);
    const uint8_t bit = buttonBit(b);
    st.pressed[i] = nullptr;
    st.held &= static_cast<uint8_t>(~bit);
    st.downMask &= static_cast<uint8_t>(~bit);
    if (st.held == 0)
      cancelCapture(st);
  }

  void cancelAll() {
    for (int i = 0; i < 3; ++i)
      cancel(static_cast<MouseButton>(i));
    cancelCapture(mouse());
  }

  // ANY pointer: the system took it away (a compositor gesture, a lost
  // capture, a tool leaving the tablet mid-stroke). onPointerCancel to its
  // capture target, and a touch ceases to exist. For the mouse this is
  // cancelAll().
  void cancelPointer(uint32_t id) {
    if (id == kMousePointerId) {
      cancelAll();
      return;
    }
    PointerState *stp = find(id);
    if (!stp)
      return;
    PointerState &st = *stp;
    for (int i = 0; i < 3; ++i)
      st.pressed[i] = nullptr;
    st.held = 0;
    st.downMask = 0;
    cancelCapture(st);
    erase(id);
  }

  // A pen left the tablet's range (or any non-mouse pointer stopped
  // existing without a press in flight): drop its state. Nothing is
  // delivered; a press still in flight should be cancelPointer()'d instead.
  void forget(uint32_t id) {
    PointerState *s = find(id);
    if (s && id != kMousePointerId && s->held == 0)
      erase(id);
  }

  // Runs the wheel handlers. Returns whether any handler ran (caller should
  // repaint). If !e.propagationStopped() afterwards, the caller applies the
  // built-in overflow scrolling.
  bool wheel(WheelEvent &e) {
    if (!root || (e.dx == 0.0f && e.dy == 0.0f))
      return false;
    if (e.timeMs == 0)
      e.timeMs = nowMs();
    const uint64_t gen = generation_;
    bool ran = false;
    Scratch chain(*this);
    // Wheel handlers don't care whether a view is disabled.
    chainAt(e.windowX, e.windowY, [](const View &v) { return (bool)v.onWheel; },
            chain.v);
    for (View *v : chain.v) {
      e.x = e.windowX - v->computed.x;
      e.y = e.windowY - v->computed.y;
      v->onWheel(e);
      ran = true;
      if (gen != generation_ || e.propagationStopped())
        break;
    }
    if (ran && poll)
      poll();
    return ran;
  }

  // The cursor to show with the MOUSE at window (x, y): the nearest
  // non-Inherit Style::cursor walking from the deepest view under the pointer
  // up through its ancestors (every view counts, whether or not it has a
  // handler), else Cursor::Default. While a view holds the mouse's capture —
  // a press with an onPointerDown — the walk starts from the capture target
  // instead, wherever the pointer is. Dynamic
  // cursors are read from the cache the owner's poll keeps fresh
  // (computed.resolvedCursor).
  Cursor cursorAt(float x, float y) {
    if (!root)
      return Cursor::Default;
    PointerState &st = mouse();
    Scratch chain(*this);
    if (st.captured)
      chain.v = st.captureChain;
    else
      chainAt(x, y, [](const View &) { return true; }, chain.v);
    for (const View *v : chain.v) {
      Cursor c = resolveDynamic(v->style.cursor, v->computed.resolvedCursor);
      if (c != Cursor::Inherit)
        return c;
    }
    return Cursor::Default;
  }

  // Refreshes every view's hover flag from the pointer position (clip-aware,
  // so a view scrolled out of view is never hovered) and fires
  // onPointerEnter/Leave. Returns whether any flag flipped. *needPoll is set when a handler may have changed app
  // state, so the caller should pollAndRelayout(). Call it for the mouse and
  // for a hovering pen; a finger has no hover.
  bool hover(PointerEvent e, bool *needPoll) {
    if (!root)
      return false;
    PointerState &st = stateFor(e);
    prepare(st, e);
    e.button = MouseButton::None;
    e.buttons = st.held;
    fixPressure(e);
    e.coalesced = {};
    return updateHover(*root, e, ClipRect{}, needPoll);
  }

private:
  struct ClickRecord {
    View *target = nullptr;
    MouseButton button = MouseButton::None;
    float x = 0, y = 0;
    uint64_t timeMs = 0;
    uint8_t count = 0;
  };

  // Everything that is per pointer: which buttons it holds, what each press
  // landed on, its capture, and its click history. The mouse's lives for the
  // router's lifetime; a finger's from down to up; a pen's while in range.
  struct PointerState {
    PointerState(uint32_t i, PointerType t) : id(i), type(t) {}
    uint32_t id;
    PointerType type;
    uint8_t held = 0;     // buttons physically held (buttonBit)
    uint8_t downMask = 0; // buttons whose press this router handled
    View *pressed[3] = {nullptr, nullptr, nullptr}; // click pairing, per button
    View *captured = nullptr;          // pointer-capture target
    std::vector<View *> captureChain;  // captured first, then tree ancestors
    float lastX = 0, lastY = 0;
    KeyModifiers lastMods;
  };

  static int idx(MouseButton b) { return static_cast<int>(b); }

  PointerState &mouse() { return *pointers_.front(); }
  const PointerState *find(uint32_t id) const {
    for (const auto &p : pointers_)
      if (p->id == id)
        return p.get();
    return nullptr;
  }
  PointerState *find(uint32_t id) {
    return const_cast<PointerState *>(std::as_const(*this).find(id));
  }
  PointerState &stateFor(const PointerEvent &e) {
    if (PointerState *s = find(e.pointerId))
      return *s;
    pointers_.push_back(std::make_unique<PointerState>(e.pointerId, e.type));
    return *pointers_.back();
  }
  void erase(uint32_t id) {
    if (id == kMousePointerId)
      return;
    std::erase_if(pointers_, [id](const auto &p) { return p->id == id; });
  }

  static uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  static void prepare(PointerState &st, PointerEvent &e) {
    if (e.timeMs == 0)
      e.timeMs = nowMs();
    e.x = e.windowX;
    e.y = e.windowY;
    st.lastX = e.windowX;
    st.lastY = e.windowY;
    st.lastMods = e.mods;
  }
  // A mouse has no pressure sensor: 0.5 while any button is held, else 0.
  // (Pen and touch backends report their own.)
  static void fixPressure(PointerEvent &e) {
    if (e.type == PointerType::Mouse)
      e.pressure = e.buttons ? 0.5f : 0.0f;
  }
  static void setLocal(PointerEvent &e, const View &v) {
    e.x = e.windowX - v.computed.x;
    e.y = e.windowY - v.computed.y;
  }
  static bool isDisabled(const View &v) {
    return resolveDynamic(v.disabled, v.computed.resolvedDisabled);
  }

  // ---- Which views care about a button ---------------------------------------
  static bool hasClickFamily(const View &v, MouseButton b) {
    switch (b) {
    case MouseButton::Left:
      return (bool)v.onClick || (bool)v.onDoubleClick;
    case MouseButton::Middle:
      return (bool)v.onAuxClick;
    case MouseButton::Right:
      return (bool)v.onContextMenu;
    default:
      return false;
    }
  }
  // Whether v takes part in pressing `b`: it handles the down, or a click of
  // that button. Disabled views are transparent.
  static bool wantsPress(const View &v, MouseButton b) {
    if (isDisabled(v))
      return false;
    return (bool)v.onPointerDown || hasClickFamily(v, b);
  }

  // ---- Chains ------------------------------------------------------------------
  // A reusable vector for building a chain without allocating per event.
  // Handlers can re-enter (and two chains can be alive at once), so each
  // user takes the pooled vector for its lifetime and hands it back.
  struct Scratch {
    Router &r;
    std::vector<View *> v;
    explicit Scratch(Router &rr) : r(rr) { v.swap(r.pool_); }
    ~Scratch() {
      v.clear();
      r.pool_.swap(v);
    }
  };

  // Mirrors the paint order: the topmost Position::Absolute layer (highest
  // zIndex, latest in document order) that has a matching view under the
  // point wins; otherwise the in-flow tree. `out` receives the matching view
  // deepest under the point first, then its matching ancestors up to the
  // root. A layer with no match under the point falls through to the layers
  // below, so a view without a handler never blocks one beneath it.
  template <class Pred>
  void chainAt(float x, float y, Pred pred, std::vector<View *> &out) {
    out.clear();
    if (!root)
      return;
    absolutes_.clear();
    collectAbsolutes(*root, absolutes_);
    sortAbsolutes(absolutes_);
    for (auto it = absolutes_.rbegin(); it != absolutes_.rend(); ++it) {
      View &av = const_cast<View &>(*it->view);
      if (walk(av, x, y, it->clip, pred, out)) {
        // An absolute view bubbles to its parent in the tree (like the DOM).
        ancestors_.clear();
        ancestorsOf(av, ancestors_);
        for (View *a : ancestors_)
          if (pred(*a))
            out.push_back(a);
        return;
      }
    }
    walk(*root, x, y, ClipRect{}, pred, out);
  }

  // Clip-aware descent, same geometry rules as painting. Children are tried
  // last-to-first (later siblings paint on top); absolute children are
  // separate layers handled by chainAt. Pushes post-order, so deeper matches
  // come first. Returns whether anything matched.
  template <class Pred>
  static bool walk(View &v, float x, float y, ClipRect clip, Pred &pred,
                   std::vector<View *> &out) {
    if (resolveDynamic(v.style.visibility, v.computed.resolvedVisibility) ==
        Visibility::Hidden)
      return false;
    if (!clip.contains(x, y) || !containsPoint(v, x, y))
      return false;
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    bool found = false;
    for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
      if (it->style.position == Position::Absolute ||
          resolveDynamic(it->style.display, it->computed.resolvedDisplay) ==
              Display::None)
        continue;
      if (walk(*it, x, y, childClip, pred, out)) {
        found = true;
        break;
      }
    }
    if (pred(v)) {
      out.push_back(&v);
      found = true;
    }
    return found;
  }

  // Appends target's ancestors (parent first, root last) to `out`.
  bool ancestorsOf(View &target, std::vector<View *> &out) {
    return root && pathTo(*root, &target, out);
  }
  static bool pathTo(View &v, const View *target, std::vector<View *> &out) {
    for (auto &c : v.children) {
      if (&c == target || pathTo(c, target, out)) {
        out.push_back(&v);
        return true;
      }
    }
    return false;
  }

  // ---- Delivery ------------------------------------------------------------------
  // Fires `h` along `chain` (deepest first) until a handler stops the event.
  // Returns whether any handler ran. `filter` is for capture chains, which
  // (unlike hit chains) were not filtered for disabled views when built.
  bool bubble(const std::vector<View *> &chain, PointerEvent e,
              PointerHandler View::*h, uint64_t gen, bool filter) {
    bool ran = false;
    for (View *v : chain) {
      if (!(v->*h) || (filter && isDisabled(*v)))
        continue;
      setLocal(e, *v);
      (v->*h)(e);
      ran = true;
      if (gen != generation_ || e.propagationStopped())
        break;
    }
    return ran;
  }

  // Counts consecutive clicks on the same view by the same KIND of pointer
  // (DOM's `detail`). Kept per type rather than per pointer: each finger
  // contact is a new, short-lived pointer, so a double-tap is two different
  // pointers; mouse, pen and touch each keep their own history.
  uint8_t countClick(View *target, const PointerEvent &e) {
    ClickRecord &c = lastClick_[static_cast<int>(e.type)];
    const bool again = c.target == target && c.button == e.button &&
                       e.timeMs >= c.timeMs &&
                       e.timeMs - c.timeMs <= kDoubleClickMs &&
                       std::abs(e.windowX - c.x) <= kDoubleClickSlop &&
                       std::abs(e.windowY - c.y) <= kDoubleClickSlop;
    const uint8_t n = again ? static_cast<uint8_t>(std::min(c.count + 1, 255)) : 1;
    c = {target, e.button, e.windowX, e.windowY, e.timeMs, n};
    return n;
  }

  // Click-family dispatch: onClick (left) / onAuxClick (middle) /
  // onContextMenu (right), then onDoubleClick on the second left click.
  void dispatchClick(const std::vector<View *> &chain, PointerEvent e,
                     uint64_t gen) {
    const MouseButton btn = e.button;
    const PointerEvent pristine = e.fresh();
    for (View *v : chain) {
      setLocal(e, *v);
      bool stop = false;
      switch (btn) {
      case MouseButton::Left:
        if (v->onClick) {
          v->onClick(e);
          stop = v->onClick.endsBubbling();
        }
        break;
      case MouseButton::Middle:
        if (v->onAuxClick)
          v->onAuxClick(e);
        break;
      case MouseButton::Right:
        if (v->onContextMenu)
          v->onContextMenu(e);
        break;
      default:
        break;
      }
      if (gen != generation_ || stop || e.propagationStopped())
        break;
    }
    if (gen == generation_ && btn == MouseButton::Left &&
        pristine.clickCount == 2)
      bubble(chain, pristine, &View::onDoubleClick, gen, false);
  }

  // Delivers onPointerCancel to the capture chain and drops the capture.
  void cancelCapture(PointerState &st) {
    if (!st.captured)
      return;
    std::vector<View *> chain = std::move(st.captureChain);
    st.captured = nullptr;
    st.captureChain.clear();
    PointerEvent e;
    e.pointerId = st.id;
    e.type = st.type;
    e.windowX = st.lastX;
    e.windowY = st.lastY;
    e.mods = st.lastMods;
    e.buttons = st.held;
    e.timeMs = nowMs();
    fixPressure(e);
    bubble(chain, e, &View::onPointerCancel, generation_, false);
  }

  // ---- Hover ---------------------------------------------------------------------
  static void fireBoundary(View &v, PointerHandler View::*h,
                           const PointerEvent &proto, bool *needPoll) {
    if (!(v.*h))
      return;
    PointerEvent e = proto;
    e.x = proto.windowX - v.computed.x;
    e.y = proto.windowY - v.computed.y;
    (v.*h)(e);
    if (needPoll)
      *needPoll = true;
  }

  // Updates v's (and its descendants') isHovered flag based on the pointer
  // position, mirroring walk()'s clip-aware descent so a node scrolled out of
  // view is never marked hovered. Returns whether any flag actually flipped,
  // so callers only repaint when hover state visibly changes.
  static bool updateHover(View &v, const PointerEvent &p, ClipRect clip,
                          bool *needPoll) {
    if (resolveDynamic(v.style.visibility, v.computed.resolvedVisibility) ==
        Visibility::Hidden) {
      bool changed = v.computed.isHovered;
      if (changed)
        fireBoundary(v, &View::onPointerLeave, p, needPoll);
      v.computed.isHovered = false; // scrolled-out/None already relied on
                                    // this same "force false" idiom
      return changed;
    }
    bool inside = clip.contains(p.windowX, p.windowY) &&
                  containsPoint(v, p.windowX, p.windowY);
    bool changed = false;
    if (inside != v.computed.isHovered) {
      v.computed.isHovered = inside;
      if (v.style.hoverColor)
        v.computed.dirty = true;
      changed = true;
      fireBoundary(v, inside ? &View::onPointerEnter : &View::onPointerLeave, p,
                   needPoll);
    }
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (auto &c : v.children)
      changed |= updateHover(c, p, childClip, needPoll);
    return changed;
  }

  // ---- State -----------------------------------------------------------------------
  // Bumped by invalidate(); a dispatch that sees it change mid-flight stops
  // touching its (possibly dangling) chain.
  uint64_t generation_ = 0;
  ClickRecord lastClick_[3]; // indexed by PointerType
  // pointers_[0] is always the mouse. unique_ptr keeps a PointerState's
  // address stable while another pointer is added.
  std::vector<std::unique_ptr<PointerState>> pointers_;
  std::vector<View *> pool_, ancestors_;
  std::vector<AbsoluteEntry> absolutes_;
};

} // namespace liteui_pointer

#if !defined(_WIN32)
inline Key xkbKeysymToKey(xkb_keysym_t sym) {
  if (sym >= XKB_KEY_a && sym <= XKB_KEY_z)
    return static_cast<Key>(static_cast<int>(Key::A) + (sym - XKB_KEY_a));
  if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)
    return static_cast<Key>(static_cast<int>(Key::A) + (sym - XKB_KEY_A));
  if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9)
    return static_cast<Key>(static_cast<int>(Key::N0) + (sym - XKB_KEY_0));
  if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12)
    return static_cast<Key>(static_cast<int>(Key::F1) + (sym - XKB_KEY_F1));
  switch (sym) {
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    return Key::Enter;
  case XKB_KEY_Escape:
    return Key::Escape;
  case XKB_KEY_BackSpace:
    return Key::Backspace;
  case XKB_KEY_Tab:
    return Key::Tab;
  case XKB_KEY_space:
    return Key::Space;
  case XKB_KEY_Delete:
    return Key::Delete;
  case XKB_KEY_Left:
    return Key::Left;
  case XKB_KEY_Right:
    return Key::Right;
  case XKB_KEY_Up:
    return Key::Up;
  case XKB_KEY_Down:
    return Key::Down;
  case XKB_KEY_Home:
    return Key::Home;
  case XKB_KEY_End:
    return Key::End;
  case XKB_KEY_Prior:
    return Key::PageUp;
  case XKB_KEY_Next:
    return Key::PageDown;
  case XKB_KEY_Shift_L:
  case XKB_KEY_Shift_R:
    return Key::Shift;
  case XKB_KEY_Control_L:
  case XKB_KEY_Control_R:
    return Key::Control;
  case XKB_KEY_Alt_L:
  case XKB_KEY_Alt_R:
    return Key::Alt;
  case XKB_KEY_Super_L:
  case XKB_KEY_Super_R:
    return Key::Super;
  case XKB_KEY_minus:
    return Key::Minus;
  case XKB_KEY_equal:
    return Key::Equal;
  case XKB_KEY_bracketleft:
    return Key::LeftBracket;
  case XKB_KEY_bracketright:
    return Key::RightBracket;
  case XKB_KEY_backslash:
    return Key::Backslash;
  case XKB_KEY_semicolon:
    return Key::Semicolon;
  case XKB_KEY_apostrophe:
    return Key::Quote;
  case XKB_KEY_comma:
    return Key::Comma;
  case XKB_KEY_period:
    return Key::Period;
  case XKB_KEY_slash:
    return Key::Slash;
  case XKB_KEY_grave:
    return Key::Grave;
  default:
    return Key::Unknown;
  }
}
#endif

void View::freeTextResources() {
#if defined(_WIN32)
  if (computed.textLayout) {
    computed.textLayout->Release();
    computed.textLayout = nullptr;
  }
  if (computed.canvasBitmap) {
    computed.canvasBitmap->Release();
    computed.canvasBitmap = nullptr;
  }
  if (computed.canvasStaging) {
    computed.canvasStaging->Release();
    computed.canvasStaging = nullptr;
  }
  if (computed.cachedImageBitmap) {
    computed.cachedImageBitmap->Release();
    computed.cachedImageBitmap = nullptr;
  }
#else
  if (computed.textTexture) {
    glDeleteTextures(1, &computed.textTexture);
    computed.textTexture = 0;
  }
  if (computed.cachedImageTexture) {
    glDeleteTextures(1, &computed.cachedImageTexture);
    computed.cachedImageTexture = 0;
    computed.cachedImagePixels = nullptr;
  }
  if (computed.canvasFbo) {
    glDeleteFramebuffers(1, &computed.canvasFbo);
    computed.canvasFbo = 0;
  }
  if (computed.canvasMsaaFbo) {
    glDeleteFramebuffers(1, &computed.canvasMsaaFbo);
    computed.canvasMsaaFbo = 0;
  }
  if (computed.canvasMsaaColorRb) {
    glDeleteRenderbuffers(1, &computed.canvasMsaaColorRb);
    computed.canvasMsaaColorRb = 0;
  }
  if (computed.canvasDsRb) {
    glDeleteRenderbuffers(1, &computed.canvasDsRb);
    computed.canvasDsRb = 0;
  }
  if (computed.canvasTexture) {
    glDeleteTextures(1, &computed.canvasTexture);
    computed.canvasTexture = 0;
  }
  if (computed.canvasSurface) {
    cairo_surface_destroy(computed.canvasSurface);
    computed.canvasSurface = nullptr;
  }
  if (computed.cachedImageSurface) {
    cairo_surface_destroy(computed.cachedImageSurface);
    computed.cachedImageSurface = nullptr;
  }
#endif
  for (auto &c : children)
    c.freeTextResources();
}

class LiteUIImpl {

public:
  // Constructor: explicit prevents accidental implicit conversions from a bare

  explicit LiteUIImpl(const std::string &title = "Window", int width = -1,
                      int height = -1, bool hideTitlebar = false);
  // Destructor: tears down whatever platform resources were created.
  ~LiteUIImpl();
  // Copying is disabled — a LiteUI owns unique OS handles that can't be
  // safely duplicated.
  LiteUIImpl(const LiteUIImpl &) = delete;
  // Copy-assignment is disabled too.
  LiteUIImpl &operator=(const LiteUIImpl &) = delete;

  // Public entry point that blocks the calling thread and pumps the platform's
  // event loop.
  void run(); // blocks, runs the event loop

  // Sets (or replaces) the root of the layout tree, lays it out immediately,
  // and triggers a repaint. Ownership of the tree is copied/moved in.
  void setRoot(View view);

  void requestClose() {
#if defined(_WIN32)
    if (hwnd_)
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
#else
    running_ = false;
#endif
  }

  void requestMinimize() {
#if defined(_WIN32)
    if (hwnd_)
      ShowWindow(hwnd_, SW_MINIMIZE);
#else
    if (toplevel_)
      xdg_toplevel_set_minimized(toplevel_);
#endif
  }

  // Toggles: maximizes if the window is normal, restores it if it's maximized.
  void requestMaximize() {
#if defined(_WIN32)
    if (hwnd_)
      ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
#else
    if (!toplevel_)
      return;
    if (maximized_)
      xdg_toplevel_unset_maximized(toplevel_);
    else
      xdg_toplevel_set_maximized(toplevel_);
    maximized_ = !maximized_;
#endif
  }

  void requestMove() {
#if defined(_WIN32)
    // Can't start the move from here: we're still inside WM_LBUTTONDOWN,
    // which is about to grab the mouse. Leave a note for that handler.
    moveRequested_ = true;
#else
    if (seat_ && toplevel_)
      xdg_toplevel_move(toplevel_, seat_, lastPressSerial_);
#endif
  }

  // Installs a handler invoked whenever the user asks to close the window
  // (the system close button, the custom titlebar/menu-bar close button
  // via requestClose(), Alt+F4, or a window-manager "Close" action) —
  // instead of the window closing immediately. The handler is fully
  // responsible for deciding what happens next: call forceClose() right
  // away if there's nothing to lose, or show a confirmation UI first and
  // call forceClose() later once the user has responded. With no handler
  // installed, a close request closes the window immediately (the
  // previous, unconditional behavior).
  void setOnCloseRequest(std::function<void()> fn) {
    onCloseRequest_ = std::move(fn);
  }

  // Closes the window immediately and unconditionally, bypassing
  // onCloseRequest — call this from within the onCloseRequest handler
  // once it's safe to actually close.
  void forceClose() {
#if defined(_WIN32)
    if (hwnd_)
      DestroyWindow(hwnd_);
#else
    running_ = false;
#endif
  }

  KeyModifiers modifiers() const {
#if defined(_WIN32)
    return currentModifiers();
#else
    return modState_;
#endif
  }

  // Everything below is internal implementation detail.
private:
  // Requested window width in pixels, stored so pixel-drawing helpers can
  // bounds-check.
  int width_;
  // Requested window height in pixels, same purpose as width_.
  int height_;
  bool startMaximized_ = false;
  bool hideTitlebar_ = false;

  View root_;
  bool hasRoot_ = false;
  static inline LiteUIImpl *activeInstance_ = nullptr;

  std::function<void()> wakeHandler_; // see setWakeHandler
  std::function<void()> onCloseRequest_;
  void handleCloseRequested() {
    if (onCloseRequest_)
      onCloseRequest_();
    else
      forceClose();
  }

  // Re-runs the layout algorithm over root_ against the current window size.
  void relayout() {
    if (!hasRoot_)
      return;
#if defined(_WIN32)
    // Native window decorations — the root view owns the whole client area.
    liteui_layout::layoutRoot(root_, static_cast<float>(width_),
                              static_cast<float>(height_));
#else
    // Custom (client-side) titlebar occupies the top kTitlebarHeight
    // pixels, drawn as an opaque overlay after content (see redraw()).
    // The root view must be laid out *below* that strip, not underneath
    // it — otherwise a child near the top of root (e.g. a heading) can
    // extend far enough to peek out past the titlebar's bottom edge,
    // which is exactly what happened here: the heading's box reached
    // deeper than kTitlebarHeight, so its lower slice (including the
    // text) rendered outside the titlebar's opaque cover instead of
    // safely underneath it.
    float top = static_cast<float>(titlebarHeight());
    float availH = std::max(0.0f, static_cast<float>(height_) - top);
    liteui_layout::layoutRoot(root_, static_cast<float>(width_), availH, 0.0f,
                              top);
#endif
  }

  // Hit-testing geometry and the pointer router live in liteui_pointer (no
  // window needed, so the tests can drive them); these are the names the
  // renderers have always used.
  using ClipRect = liteui_pointer::ClipRect;
  using AbsoluteEntry = liteui_pointer::AbsoluteEntry;

  // Plain float rectangle for scrollbar geometry (track/thumb), kept
  // separate from Wayland's integer-pixel `Rect` below since scrollbar math
  // wants to stay in the same float space as View::Computed.
  struct PixRect {
    float x, y, w, h;
  };

  // Whether view v's vertical/horizontal scrollbar is currently showing —
  // thin wrappers around axisScrollbarVisible() using v's own already-
  // computed sizes, so callers don't have to repeat the h vs. contentH /
  // w vs. contentW pairing correctly every time.
  static bool wantVBar(const View &v) {
    return v.scrollsY() &&
           liteui_layout::axisScrollbarVisible(
               v.style.overflowY, v.computed.contentH, v.computed.h);
  }
  static bool wantHBar(const View &v) {
    return v.scrollsX() &&
           liteui_layout::axisScrollbarVisible(
               v.style.overflowX, v.computed.contentW, v.computed.w);
  }

  // Track rectangles run the full length of their edge, minus the corner
  // square where both bars would otherwise overlap (only relevant when
  // both axes scroll at once).
  static PixRect vTrackRect(const View &v) {
    float h = v.computed.h - (wantHBar(v) ? kScrollbarThickness : 0.0f);
    return {v.computed.x + v.computed.w - kScrollbarThickness, v.computed.y,
            kScrollbarThickness, std::max(0.0f, h)};
  }
  static PixRect hTrackRect(const View &v) {
    float w = v.computed.w - (wantVBar(v) ? kScrollbarThickness : 0.0f);
    return {v.computed.x, v.computed.y + v.computed.h - kScrollbarThickness,
            std::max(0.0f, w), kScrollbarThickness};
  }

  // Minimum thumb length so a very long scrollable area doesn't shrink the
  // thumb down to an unclickable sliver.
  static constexpr float kMinThumb = 20.0f;

  // Thumb length is proportional to viewport/content (how much of the
  // content is visible at once); thumb position is proportional to how far
  // through the scrollable range the current offset is.
  static PixRect vThumbRect(const View &v) {
    PixRect track = vTrackRect(v);
    float minThumb = std::min(kMinThumb, track.h); // never exceeds track
    float thumbH =
        v.computed.contentH > 0
            ? std::clamp(track.h * (v.computed.h / v.computed.contentH),
                         minThumb, track.h)
            : track.h;
    float maxScroll = v.maxScrollY();
    float pos = maxScroll > 0
                    ? (v.computed.scrollY / maxScroll) * (track.h - thumbH)
                    : 0.0f;
    return {track.x, track.y + pos, track.w, thumbH};
  }
  static PixRect hThumbRect(const View &v) {
    PixRect track = hTrackRect(v);
    float minThumb = std::min(kMinThumb, track.w);
    float thumbW =
        v.computed.contentW > 0
            ? std::clamp(track.w * (v.computed.w / v.computed.contentW),
                         minThumb, track.w)
            : track.w;
    float maxScroll = v.maxScrollX();
    float pos = maxScroll > 0
                    ? (v.computed.scrollX / maxScroll) * (track.w - thumbW)
                    : 0.0f;
    return {track.x + pos, track.y, thumbW, track.h};
  }
  static bool pixRectContains(const PixRect &r, float px, float py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
  }

  // ---- Press/drag/wheel resolution against scrollbars & scrollable content
  // ---- What a press (or a wheel event, which reuses this to find its target)
  // landed on. VThumb/HThumb mean "start dragging that thumb"; VTrack/
  // HTrack mean "clicked empty track — jump the thumb to click position;
  // Content means "this is inside some scrollable view's content area
  // (not its scrollbar)" — used both to start a possible pan-to-scroll
  // drag and, for wheel events, as the view to scroll.
  enum class ScrollHit { None, VThumb, HThumb, VTrack, HTrack, Content };
  struct ScrollPress {
    ScrollHit kind = ScrollHit::None;
    View *view = nullptr;
    float trackFrac = 0; // 0..1 position along the track, for VTrack/HTrack
  };

  // Walks down from v looking for the deepest relevant scroll interaction
  // under (x, y). Each node's own scrollbars are checked before recursing
  // into its children, which is always correct because layout already
  // reserves the scrollbar's gutter out of the children's placement area —
  // a child box can never overlap its parent's own scrollbar. Known
  // limitation shared with hitTestFlow below: Position::Absolute
  // descendants aren't threaded through this clip-aware walk at all (they
  // bypass the normal recursion entirely, same as elsewhere in this file),
  // so an absolute view inside a scrolled-out region can still be found —
  // acceptable for v1, matching this file's existing absolute-positioning
  // trade-offs.
  static ScrollPress resolveScrollTarget(View &v, float x, float y,
                                         ClipRect clip) {
    if (resolveDynamic(v.style.visibility, v.computed.resolvedVisibility) ==
        Visibility::Hidden)
      return {};
    if (!clip.contains(x, y) || !containsPoint(v, x, y))
      return {};
    if (wantVBar(v)) {
      PixRect track = vTrackRect(v);
      if (pixRectContains(track, x, y)) {
        PixRect thumb = vThumbRect(v);
        if (pixRectContains(thumb, x, y))
          return {ScrollHit::VThumb, &v, 0};
        float frac = track.h > 0 ? (y - track.y) / track.h : 0;
        return {ScrollHit::VTrack, &v, frac};
      }
    }
    if (wantHBar(v)) {
      PixRect track = hTrackRect(v);
      if (pixRectContains(track, x, y)) {
        PixRect thumb = hThumbRect(v);
        if (pixRectContains(thumb, x, y))
          return {ScrollHit::HThumb, &v, 0};
        float frac = track.w > 0 ? (x - track.x) / track.w : 0;
        return {ScrollHit::HTrack, &v, frac};
      }
    }
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
      if (it->style.position == Position::Absolute ||
          resolveDynamic(it->style.display, it->computed.resolvedDisplay) ==
              Display::None)
        continue;
      ScrollPress r = resolveScrollTarget(*it, x, y, childClip);
      if (r.kind != ScrollHit::None)
        return r;
    }
    if (v.scrollsX() || v.scrollsY())
      return {ScrollHit::Content, &v, 0};
    return {};
  }
  // How a click-and-drag inside scrollable content is currently being
  // interpreted. Mirrors pressedView_'s press/release pairing, but for
  // scroll interactions instead of onClick.
  enum class DragMode { None, VThumb, HThumb, ContentPan };
  struct ScrollDrag {
    DragMode mode = DragMode::None;
    View *target = nullptr;
    float startPointerX = 0, startPointerY = 0;
    float startScrollX = 0, startScrollY = 0;
    // ContentPan only: whether the pointer has moved past the click/drag
    // threshold yet. Until it does, this might still turn out to be an
    // ordinary click (see beginScrollPress/updateScrollDrag/endScrollPress).
    bool moved = false;
  } scrollDrag_;
  // Which pointer (PointerEvent::pointerId) armed scrollDrag_: only it may
  // advance or end the drag, so a finger panning a list isn't moved by mouse
  // motion and vice versa. Meaningful only while scrollDrag_.mode != None.
  uint32_t scrollOwner_ = liteui_pointer::kMousePointerId;

  // Pixels of pointer movement before a press-in-content is treated as a
  // pan-to-scroll drag rather than a click.
  static constexpr float kDragThreshold = 4.0f;

  // Call on every left-button press, before the existing beginPress(). If
  // this returns true, it fully owns the press (a scrollbar grab, or a
  // track-click jump) and the caller must NOT also call beginPress() —
  // there's nothing left to click. If it returns false, the caller should
  // fall through to its normal beginPress(x, y) — resolveScrollTarget()
  // found either nothing scrollable, or plain scrollable *content*, in
  // which case a ContentPan drag is armed here but the ordinary click path
  // still runs too, since a small movement should behave as a click, not a
  // pan (see updateScrollDrag/endScrollPress).
  bool beginScrollPress(float x, float y,
                        uint32_t pointerId = liteui_pointer::kMousePointerId) {
    if (!hasRoot_)
      return false;
    ScrollPress r = resolveScrollTarget(root_, x, y, ClipRect{});
    switch (r.kind) {
    case ScrollHit::VThumb:
      scrollDrag_ = {
          DragMode::VThumb,         r.view, x, y, r.view->computed.scrollX,
          r.view->computed.scrollY, true};
      scrollOwner_ = pointerId;
      return true;
    case ScrollHit::HThumb:
      scrollDrag_ = {
          DragMode::HThumb,         r.view, x, y, r.view->computed.scrollX,
          r.view->computed.scrollY, true};
      scrollOwner_ = pointerId;
      return true;
    case ScrollHit::VTrack: {
      PixRect track = vTrackRect(*r.view), thumb = vThumbRect(*r.view);
      float target = r.trackFrac * track.h - thumb.h / 2;
      float maxScroll = r.view->maxScrollY();
      float range = track.h - thumb.h;
      r.view->computed.scrollY = std::clamp(
          range > 0 ? (target / range) * maxScroll : 0.0f, 0.0f, maxScroll);
      relayout();
      return true;
    }
    case ScrollHit::HTrack: {
      PixRect track = hTrackRect(*r.view), thumb = hThumbRect(*r.view);
      float target = r.trackFrac * track.w - thumb.w / 2;
      float maxScroll = r.view->maxScrollX();
      float range = track.w - thumb.w;
      r.view->computed.scrollX = std::clamp(
          range > 0 ? (target / range) * maxScroll : 0.0f, 0.0f, maxScroll);
      relayout();
      return true;
    }
    case ScrollHit::Content:
      if (r.view->style.contentPanEnabled) {
        scrollDrag_ = {
            DragMode::ContentPan,     r.view, x, y, r.view->computed.scrollX,
            r.view->computed.scrollY, false};
        scrollOwner_ = pointerId;
      }
      return false; // ordinary click press still proceeds too
    case ScrollHit::None:
      return false;
    }
    return false;
  }

  // Call on every pointer-motion event while a button is held. Advances an
  // in-progress scrollbar drag or content pan; a no-op if scrollDrag_ isn't
  // active. Returns true if it changed anything (caller should repaint).
  bool updateScrollDrag(float x, float y,
                        uint32_t pointerId = liteui_pointer::kMousePointerId) {
    if (scrollDrag_.mode == DragMode::None || scrollOwner_ != pointerId)
      return false;
    View &v = *scrollDrag_.target;
    float dx = x - scrollDrag_.startPointerX;
    float dy = y - scrollDrag_.startPointerY;
    if (scrollDrag_.mode == DragMode::ContentPan) {
      if (!scrollDrag_.moved && std::abs(dx) < kDragThreshold &&
          std::abs(dy) < kDragThreshold)
        return false; // still within click tolerance — not a pan yet
      if (!scrollDrag_.moved) {
        scrollDrag_.moved = true;
        // It just became a drag, not a click — cancel any pending onClick
        // so the eventual release doesn't also fire it.
        ptrRouter_.dropClick(MouseButton::Left, pointerId);
      }
      if (v.scrollsX())
        v.computed.scrollX =
            std::clamp(scrollDrag_.startScrollX - dx, 0.0f, v.maxScrollX());
      if (v.scrollsY())
        v.computed.scrollY =
            std::clamp(scrollDrag_.startScrollY - dy, 0.0f, v.maxScrollY());
    } else if (scrollDrag_.mode == DragMode::VThumb) {
      PixRect track = vTrackRect(v), thumb = vThumbRect(v);
      float range = track.h - thumb.h;
      float delta = range > 0 ? (dy / range) * v.maxScrollY() : 0.0f;
      v.computed.scrollY =
          std::clamp(scrollDrag_.startScrollY + delta, 0.0f, v.maxScrollY());
    } else if (scrollDrag_.mode == DragMode::HThumb) {
      PixRect track = hTrackRect(v), thumb = hThumbRect(v);
      float range = track.w - thumb.w;
      float delta = range > 0 ? (dx / range) * v.maxScrollX() : 0.0f;
      v.computed.scrollX =
          std::clamp(scrollDrag_.startScrollX + delta, 0.0f, v.maxScrollX());
    }
    relayout();
    return true;
  }

  // Call on every left-button release. Resolves a ContentPan that never
  // crossed the drag threshold back into an ordinary click via the
  // existing endPress(); anything that already became a real drag (a
  // scrollbar grab, or a pan that moved) just ends quietly. Always clears
  // scrollDrag_ so a stale target can't leak into some unrelated later
  // press.
  //
  // `e` is the release as a PointerEvent for a pen or finger (null = the
  // mouse). Only the pointer that armed scrollDrag_ may end it: a release by
  // any other pointer is an ordinary release and leaves the drag alone.
  bool endScrollPress(float x, float y, const PointerEvent *e = nullptr) {
    bool changed = false;
    const uint32_t pid = e ? e->pointerId : liteui_pointer::kMousePointerId;
    const bool owner = scrollDrag_.mode != DragMode::None && scrollOwner_ == pid;
    if (!owner ||
        (scrollDrag_.mode == DragMode::ContentPan && !scrollDrag_.moved))
      changed = e ? ptrRouter_.up(*e) : endPress(x, y);
    if (owner)
      scrollDrag_ = {};
    return changed;
  }

  // Call on every wheel/scroll event; deltaX/deltaY are in pixels (already
  // sign-adjusted so positive means "scroll right"/"scroll down" — each
  // platform's wheel callback is responsible for that conversion). Finds
  // the deepest scrollable view under the cursor via the same walk used
  // for press resolution — scrollbar vs. content doesn't matter for wheel
  // input, both count as "the pointer is over this scrollable view".
  // Returns true if it changed anything (caller should repaint).
  //
  // If the resolved view has style.wheelScrollEnabled == false, this is a
  // no-op — the wheel no longer moves that view's scroll position at all,
  // even though it's still scrollable via its scrollbar. This function is
  // entirely separate from dispatchWheel()'s onWheel handlers: a caller that
  // wants "wheel does something custom instead of scrolling" (e.g. zoom)
  // can either call stopPropagation() in its onWheel, or turn this off and
  // rely on onWheel, which fires regardless of wheelScrollEnabled.
  bool applyWheelScroll(float x, float y, float deltaX, float deltaY) {
    if (!hasRoot_)
      return false;
    ScrollPress r = resolveScrollTarget(root_, x, y, ClipRect{});
    if (r.kind == ScrollHit::None || !r.view)
      return false;
    View &v = *r.view;
    if (!v.style.wheelScrollEnabled)
      return false; // pixel scroll disabled for this view; scrollbar
                    // drag/track-click still work normally, and
                    // onWheel handlers still fire via dispatchWheel()
    bool changed = false;
    if (v.scrollsY() && deltaY != 0.0f) {
      float ns = std::clamp(v.computed.scrollY + deltaY, 0.0f, v.maxScrollY());
      changed |= ns != v.computed.scrollY;
      v.computed.scrollY = ns;
    }
    if (v.scrollsX() && deltaX != 0.0f) {
      float ns = std::clamp(v.computed.scrollX + deltaX, 0.0f, v.maxScrollX());
      changed |= ns != v.computed.scrollX;
      v.computed.scrollX = ns;
    }
    if (changed)
      relayout();
    return changed;
  }

  void collectAbsolutes(const View &v, std::vector<AbsoluteEntry> &out) {
    liteui_pointer::collectAbsolutes(v, out);
  }
  static void sortAbsolutes(std::vector<AbsoluteEntry> &absolutes) {
    liteui_pointer::sortAbsolutes(absolutes);
  }

  static bool containsPoint(const View &v, float px, float py) {
    return liteui_pointer::containsPoint(v, px, py);
  }

  static View *hitTestTooltipFlow(View &v, float x, float y, ClipRect clip) {
    if (resolveDynamic(v.style.visibility, v.computed.resolvedVisibility) ==
        Visibility::Hidden)
      return nullptr;
    if (!clip.contains(x, y) || !containsPoint(v, x, y))
      return nullptr;
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
      if (it->style.position == Position::Absolute ||
          resolveDynamic(it->style.display, it->computed.resolvedDisplay) ==
              Display::None)
        continue;
      if (View *hit = hitTestTooltipFlow(*it, x, y, childClip))
        return hit;
    }
    if (!v.tooltip.empty() &&
        !resolveDynamic(v.disabled, v.computed.resolvedDisabled))
      return &v;
    return nullptr;
  }

  View *hitTestTooltip(float x, float y) {
    if (!hasRoot_)
      return nullptr;
    std::vector<AbsoluteEntry> absolutes;
    collectAbsolutes(root_, absolutes);
    sortAbsolutes(absolutes);
    for (auto it = absolutes.rbegin(); it != absolutes.rend(); ++it)
      if (View *hit =
              hitTestTooltipFlow(const_cast<View &>(*it->view), x, y, it->clip))
        return hit;
    return hitTestTooltipFlow(root_, x, y, ClipRect{});
  }

  // Polls every dynamic source in the subtree rooted at v, writing
  // changes back in place and marking the affected node dirty. Called
  // after a click is dispatched, since the handler may have mutated the
  // plain variables these sources read from. Returns whether anything
  // changed, so the caller knows whether to relayout/repaint.
  bool checkForUpdates(View &v) {
    bool changed = false;

    if (v.keysSource) {
      std::vector<std::string> keys = v.keysSource();
      if (keys != v.computed.resolvedKeys) {
        v.computed.resolvedKeys = keys;
        if (reconcileChildren(v, keys)) {
          v.computed.dirty = true;
          changed = true;
          structureChanged_ = true;
        }
      }
    }

    if (auto *fn = std::get_if<std::function<std::string()>>(&v.text)) {
      std::string next = (*fn)();
      if (next != v.computed.resolvedText) {
        v.computed.resolvedText = std::move(next);
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<Color()>>(&v.textStyle.color)) {
      Color next = (*fn)();
      Color &cur = v.computed.resolvedTextColor;
      if (next.r != cur.r || next.g != cur.g || next.b != cur.b ||
          next.a != cur.a) {
        cur = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<float()>>(&v.textStyle.fontSize)) {
      float next = (*fn)();
      if (next != v.computed.resolvedFontSize) {
        v.computed.resolvedFontSize = next;
        v.computed.dirty = true;
        changed =
            true; // affects text measurement — relayout() picked up by caller
      }
    }
    if (auto *fn =
            std::get_if<std::function<FontWeight()>>(&v.textStyle.fontWeight)) {
      FontWeight next = (*fn)();
      if (next != v.computed.resolvedFontWeight) {
        v.computed.resolvedFontWeight = next;
        v.computed.dirty = true;
        changed =
            true; // affects text measurement — relayout() picked up by caller
      }
    }
    if (auto *fn =
            std::get_if<std::function<FontStyle()>>(&v.textStyle.fontStyle)) {
      FontStyle next = (*fn)();
      if (next != v.computed.resolvedFontStyle) {
        v.computed.resolvedFontStyle = next;
        v.computed.dirty = true;
        changed =
            true; // affects text measurement — relayout() picked up by caller
      }
    }
    if (auto *fn = std::get_if<std::function<Cursor()>>(&v.style.cursor)) {
      Cursor next = (*fn)();
      if (next != v.computed.resolvedCursor) {
        v.computed.resolvedCursor = next;
        // Needs no relayout or repaint: just a cursor refresh, which
        // pollAndRelayout() does when it sees this flag.
        cursorDirty_ = true;
      }
    }
    if (auto *fn = std::get_if<std::function<bool()>>(&v.disabled)) {
      bool next = (*fn)();
      if (next != v.computed.resolvedDisabled) {
        v.computed.resolvedDisabled = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<Size()>>(&v.style.width)) {
      Size next = (*fn)();
      Size &cur = v.computed.resolvedWidth;
      if (next.kind != cur.kind || next.value != cur.value) {
        cur = next;
        v.computed.dirty = true;
        changed = true; // affects layout — relayout() picked up by caller
      }
    }
    if (auto *fn = std::get_if<std::function<Size()>>(&v.style.height)) {
      Size next = (*fn)();
      Size &cur = v.computed.resolvedHeight;
      if (next.kind != cur.kind || next.value != cur.value) {
        cur = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<float()>>(&v.style.borderWidth)) {
      float next = (*fn)();
      if (next != v.computed.resolvedBorderWidth) {
        v.computed.resolvedBorderWidth = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<Color()>>(&v.style.borderColor)) {
      Color next = (*fn)();
      Color &cur = v.computed.resolvedBorderColor;
      if (next.r != cur.r || next.g != cur.g || next.b != cur.b ||
          next.a != cur.a) {
        cur = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<float()>>(&v.style.borderRadius)) {
      float next = (*fn)();
      if (next != v.computed.resolvedBorderRadius) {
        v.computed.resolvedBorderRadius = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<float()>>(&v.style.left)) {
      float next = (*fn)();
      if (next != v.computed.resolvedLeft) {
        v.computed.resolvedLeft = next;
        v.computed.dirty = true;
        changed = true; // affects layout — relayout() picked up by caller
      }
    }
    if (auto *fn = std::get_if<std::function<float()>>(&v.style.top)) {
      float next = (*fn)();
      if (next != v.computed.resolvedTop) {
        v.computed.resolvedTop = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<float()>>(&v.style.right)) {
      float next = (*fn)();
      if (next != v.computed.resolvedRight) {
        v.computed.resolvedRight = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<float()>>(&v.style.bottom)) {
      float next = (*fn)();
      if (next != v.computed.resolvedBottom) {
        v.computed.resolvedBottom = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn =
            std::get_if<std::function<Color()>>(&v.style.backgroundColor)) {
      Color next = (*fn)();
      Color &cur = v.computed.resolvedBackgroundColor;
      if (next.r != cur.r || next.g != cur.g || next.b != cur.b ||
          next.a != cur.a) {
        cur = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<std::optional<Color>()>>(
            &v.style.scrollTrackColor)) {
      std::optional<Color> next = (*fn)();
      if (next != v.computed.resolvedScrollTrackColor) {
        v.computed.resolvedScrollTrackColor = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<std::optional<Color>()>>(
            &v.style.scrollThumbColor)) {
      std::optional<Color> next = (*fn)();
      if (next != v.computed.resolvedScrollThumbColor) {
        v.computed.resolvedScrollThumbColor = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<Display()>>(&v.style.display)) {
      Display next = (*fn)();
      if (next != v.computed.resolvedDisplay) {
        v.computed.resolvedDisplay = next;
        v.computed.dirty = true;
        changed = true; // affects layout — relayout() picked up by caller
      }
    }
    if (auto *fn =
            std::get_if<std::function<Visibility()>>(&v.style.visibility)) {
      Visibility next = (*fn)();
      if (next != v.computed.resolvedVisibility) {
        v.computed.resolvedVisibility = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<int()>>(&v.style.zIndex)) {
      int next = (*fn)();
      if (next != v.computed.resolvedZIndex) {
        v.computed.resolvedZIndex = next;
        v.computed.dirty = true;
        changed = true; // affects stacking order — repaint picks it up
      }
    }
    if (auto *fn = std::get_if<std::function<EdgeInsets()>>(&v.style.margin)) {
      EdgeInsets next = (*fn)();
      if (next != v.computed.resolvedMargin) {
        v.computed.resolvedMargin = next;
        v.computed.dirty = true;
        changed = true; // affects layout — relayout() picked up by caller
      }
    }
    if (auto *fn = std::get_if<std::function<EdgeInsets()>>(&v.style.padding)) {
      EdgeInsets next = (*fn)();
      if (next != v.computed.resolvedPadding) {
        v.computed.resolvedPadding = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (v.canvasDirtySource && v.canvasDirtySource()) {
      v.computed.canvasNeedsRedraw = true;
      v.computed.dirty = true;
      changed = true;
    }
    if (v.wantsFocus && v.wantsFocus()) {
      pendingFocusRequest_ = &v;
      changed = true;
    }
    for (auto &c : v.children)
      changed |= checkForUpdates(c);
    return changed;
  }

  // Rebuilds v.children to match `keys`, reusing any existing child whose
  // `key` matches. Survivors are std::move'd, so their computed cache
  // carries over; only genuinely new keys are built, and only dropped
  // children are freed. Returns whether anything actually changed.
  //
  // Moved-from entries in `old` keep dangling copies of the survivor's
  // handles (GL textures, IDWriteTextLayout, cairo surfaces), which is
  // harmless precisely because View frees nothing in a destructor (see
  // View::freeTextResources) — we only free the entries we did NOT move
  // out of.
  //
  // Matching is O(new * old). Fine for the list sizes a scroll container
  // realistically holds; swap in a key->index map if that stops being true.
  static bool reconcileChildren(View &v, const std::vector<std::string> &keys) {
    if (!v.itemBuilder)
      return false;
    std::vector<View> old = std::move(v.children);
    v.children.clear();
    v.children.reserve(keys.size());
    std::vector<bool> reused(old.size(), false);
    bool changed = old.size() != keys.size();

    for (size_t i = 0; i < keys.size(); ++i) {
      size_t match = old.size();
      for (size_t j = 0; j < old.size(); ++j)
        if (!reused[j] && old[j].key == keys[i]) {
          match = j;
          break;
        }
      if (match < old.size()) {
        reused[match] = true;
        if (match != i)
          changed = true; // same node, new position — still needs relayout
        v.children.push_back(std::move(old[match]));
      } else {
        View built = v.itemBuilder(keys[i]);
        built.key = keys[i];
        v.children.push_back(std::move(built));
        changed = true;
      }
    }
    for (size_t j = 0; j < old.size(); ++j)
      if (!reused[j]) {
        old[j].freeTextResources();
        changed = true;
      }
    return changed;
  }

  // Every raw View* we hold points into a children vector that a reconcile
  // may have just reallocated — or at a row that no longer exists. Drop
  // them all rather than risk resolving a dangling one on the next
  // release/motion/key event. focusedView_ is nulled directly rather than
  // via setFocus(nullptr): the old target may already be freed, so firing
  // its onBlur would be a use-after-free. The pointer router is dropped
  // silently for the same reason: no onPointerCancel here.
  void invalidateViewPointers() {
    ptrRouter_.invalidate(); // press and capture targets, silently
    scrollDrag_ = {};
    focusedView_ = nullptr;
    hideTooltip(); // clears tooltipTarget_ and cancels its timer
  }

  // The one place that polls dynamic sources and applies the result.
  bool pollAndRelayout() {
    if (!hasRoot_)
      return false;
    structureChanged_ = false;
    pendingFocusRequest_ = nullptr;
    cursorDirty_ = false;
    if (!checkForUpdates(root_)) {
      if (cursorDirty_)
        refreshCursor(); // only a dynamic Style::cursor changed
      return false;
    }
    if (structureChanged_)
      invalidateViewPointers();
    if (pendingFocusRequest_)
      setFocus(pendingFocusRequest_);
    relayout();
    refreshCursor(); // the view under a stationary pointer may have changed
    return true;
  }

  // Set by checkForUpdates when a callback-valued Style::cursor changed.
  bool cursorDirty_ = false;

  // All pointer state (press/capture/click pairing) and event
  // routing. The platform backends below only translate native mouse
  // messages into calls on this; see liteui_pointer::Router.
  liteui_pointer::Router ptrRouter_;


  // Set by checkForUpdates whenever reconcileChildren actually rebuilt a
  // child list; consumed (and cleared) by pollAndRelayout above.
  bool structureChanged_ = false;

  static int btnIdx(MouseButton b) { return static_cast<int>(b); }

  // The one view currently receiving keyboard events, or nullptr if
  // nothing has focus. Unlike pressedView_[3] (per-button, click-only)
  // there's exactly one keyboard focus at a time, independent of mouse
  // button state.
  View *focusedView_ = nullptr;
  View *pendingFocusRequest_ = nullptr;

  TooltipStyle tooltipStyle_;
  View *tooltipTarget_ = nullptr;
  int tooltipTimerHandle_ = -1;
  bool tooltipVisible_ = false;
  float tooltipPointerX_ = 0, tooltipPointerY_ = 0;

  Color windowBg_{255, 255, 255, 255};
  Color scrollTrack_{0xE0, 0xE0, 0xE0, 255};
  Color scrollThumb_{0x90, 0x90, 0x90, 255};

  void requestTooltipRepaint() {
#if defined(_WIN32)
    if (hwnd_)
      InvalidateRect(hwnd_, nullptr, FALSE);
#else
    if (eglReady_)
      redraw();
#endif
  }

  void showTooltip() {
    tooltipVisible_ = true;
    requestTooltipRepaint();
  }

  void hideTooltip() {
    if (tooltipTimerHandle_ >= 0) {
      removeInterval(tooltipTimerHandle_);
      tooltipTimerHandle_ = -1;
    }
    bool wasVisible = tooltipVisible_;
    tooltipVisible_ = false;
    tooltipTarget_ = nullptr;
    if (wasVisible)
      requestTooltipRepaint();
  }

  // Call on every pointer-motion event.
  void updateTooltipHover(float x, float y) {
    View *hit = hitTestTooltip(x, y);
    tooltipPointerX_ = x;
    tooltipPointerY_ = y;
    if (hit == tooltipTarget_)
      return; // same target (or same "nothing") — leave timer/visibility alone

    if (tooltipTimerHandle_ >= 0) {
      removeInterval(tooltipTimerHandle_);
      tooltipTimerHandle_ = -1;
    }
    bool wasVisible = tooltipVisible_;
    tooltipVisible_ = false;
    tooltipTarget_ = hit;
    if (wasVisible)
      requestTooltipRepaint();

    if (hit) {
      tooltipTimerHandle_ = addInterval(tooltipStyle_.delayMs, [this] {
        showTooltip();
        // addInterval's timers repeat — cancel the underlying OS timer
        // now that it's done its one job, or it'd keep firing.
        int h = tooltipTimerHandle_;
        tooltipTimerHandle_ = -1;
        if (h >= 0)
          removeInterval(h);
      });
    }
  }

  // Modifier state, kept current by whichever platform backend is
  // handling raw key events (Win32's GetKeyState polling on each message,
  // or Wayland's xkb_state on every "modifiers" event).
  KeyModifiers modState_;

  void setFocus(View *v) {
    if (v == focusedView_)
      return;
    if (focusedView_ && focusedView_->onBlur)
      focusedView_->onBlur();
    focusedView_ = v;
    if (focusedView_ && focusedView_->onFocus)
      focusedView_->onFocus();
  }

  // App-global shortcut: fires regardless of what has focus (or whether
  // anything does), checked before the focused view's own onKeyDown. Use
  // this for things like Ctrl+S that should work even while a button (not
  // a text field) has focus.
  struct Shortcut {
    KeyModifiers mods;
    Key key;
    std::function<void()> fn;
  };
  std::vector<Shortcut> shortcuts_;

public:
  // Runs fn every `ms` milliseconds until removeInterval() is called with
  // the returned handle. Each tick automatically triggers the same
  // checkForUpdates+relayout+repaint cycle as a click/key dispatch, so fn
  // can freely flip plain bools/state and have it show up on screen.
  int addInterval(int ms, std::function<void()> fn);
  void removeInterval(int handle);
  void setTooltipStyle(TooltipStyle s) { tooltipStyle_ = std::move(s); }
  void setWindowBackground(Color c) {
    windowBg_ = c;
    requestRepaint();
  }
  // Sets the window-wide default scrollbar colors. A view with
  // Style::scrollTrackColor/scrollThumbColor set overrides these defaults
  // for itself only (see paintScrollbars/renderScrollbars) — there is no
  // separate "global" color to opt back into once a view overrides one.
  void setScrollbarColors(Color track, Color thumb) {
    scrollTrack_ = track;
    scrollThumb_ = thumb;
    requestRepaint();
  }
  void addShortcut(KeyModifiers mods, Key key, std::function<void()> fn) {
    shortcuts_.push_back({mods, key, std::move(fn)});
  }
  void requestRepaint();
  static LiteUIImpl *current() { return activeInstance_; }

  // Wakes the event loop from ANY thread — the one LiteUI call that is safe
  // off the UI thread. The loop normally sleeps until the OS hands it an
  // event, so a background thread that has finished something (a render, a
  // decode, a download) has no other way to make the UI notice. wake() makes
  // the UI thread run the wake handler (if set) and then poll + repaint,
  // exactly as after a click or an interval tick.
  //
  // Wakes coalesce: several wake() calls before the UI thread gets to them
  // produce ONE callback, so a chatty worker cannot flood the loop. A wake
  // is never lost: one issued while the UI thread is busy (or not yet in its
  // loop) is delivered as soon as it next waits.
  //
  // Lifetime: the caller must stop every thread that can still call wake()
  // (join them) before this LiteUI is destroyed.
  void wake();
  // Installs the handler wake() triggers; runs on the UI thread, once per
  // coalesced wake, before the repaint. Optional — with none installed a wake
  // still polls the Dynamic<> sources and repaints, which is all a worker
  // needs when the UI reads its results from a canvasDirtySource. Set it from
  // the UI thread before the first wake().
  void setWakeHandler(std::function<void()> fn) {
    wakeHandler_ = std::move(fn);
  }

private:
  // Called by both platform backends on every key press. Checks
  // shortcuts_ first (global, focus-independent), then falls through to
  // focusedView_->onKeyDown if nothing claimed it. Polls dynamic sources
  // afterward, same idiom as dispatchClick/dispatchScroll, since a
  // handler may have mutated app state a Dynamic<> field reads from.
  bool dispatchKeyDown(KeyEvent e) {
    for (auto &sc : shortcuts_) {
      if (sc.key == e.key && sc.mods == e.mods) {
        if (sc.fn)
          sc.fn();
        pollAndRelayout();
        return true;
      }
    }
    if (focusedView_ && focusedView_->onKeyDown) {
      focusedView_->onKeyDown(e);
      pollAndRelayout();
      return true;
    }
    return false;
  }

  bool dispatchKeyUp(KeyEvent e) {
    if (focusedView_ && focusedView_->onKeyUp) {
      focusedView_->onKeyUp(e);
      pollAndRelayout();
      return true;
    }
    return false;
  }

  bool dispatchTextInput(uint32_t codepoint) {
    if (focusedView_ && focusedView_->onTextInput) {
      focusedView_->onTextInput(codepoint);
      pollAndRelayout();
      return true;
    }
    return false;
  }

  // A PointerEvent for the router from a window-space position. Timestamp is
  // stamped by the router; modifiers come from the backend's key state.
  PointerEvent makePointerEvent(float x, float y,
                                MouseButton btn = MouseButton::None) const {
    PointerEvent e;
    e.windowX = e.x = x;
    e.windowY = e.y = y;
    e.button = btn;
    e.mods = modifiers();
    e.type = PointerType::Mouse;
    return e;
  }

  // Button `btn` went down at (x, y). Focus, onPointerDown (bubbling),
  // capture, and click pairing — all in the router.
  void beginPress(float x, float y, MouseButton btn = MouseButton::Left) {
    ptrRouter_.down(makePointerEvent(x, y, btn));
    refreshCursor(); // a press may start a capture, which pins the cursor
  }

  // Call on every pointer-motion event: onPointerMove (to the capture target
  // while a button is held, else to the view under the pointer). Cheap when
  // nothing cares.
  // Returns true if it changed anything (caller should repaint).
  bool dispatchPointerMove(float x, float y) {
    return ptrRouter_.move(makePointerEvent(x, y));
  }

  // Hover flags and onPointerEnter/Leave.
  // Returns whether any hover flag flipped; *needPoll is set if a handler may
  // have changed app state (caller then runs pollAndRelayout()).
  bool updateHoverAt(float x, float y, bool *needPoll) {
    return ptrRouter_.hover(makePointerEvent(x, y), needPoll);
  }

  // Button `btn` went up at (x, y): onPointerUp, then the click family if the
  // release landed on the press target. Returns whether anything changed.
  bool endPress(float x, float y, MouseButton btn = MouseButton::Left) {
    bool changed = ptrRouter_.up(makePointerEvent(x, y, btn));
    refreshCursor();
    return changed;
  }

  // The press of `btn` can't complete (capture lost, OS move loop, release
  // in the titlebar): delivers onPointerCancel.
  void cancelPress(MouseButton btn) {
    ptrRouter_.cancel(btn);
    refreshCursor();
  }
  void cancelAllPresses() {
    ptrRouter_.cancelAll();
    refreshCursor();
  }

  // ---- Pen and touch ----------------------------------------------------------
  // Backends deliver pen/finger input as *frames*: the full state of one
  // pointer at one instant, plus every position it reported since the last
  // frame. feedPointer() turns that into router events, so the backends only
  // translate native data and never reimplement press/release bookkeeping.

  // Id ranges: the mouse is 1; fingers and pens are numbered by the backend
  // from these bases so they can never collide.
  static constexpr uint32_t kTouchIdBase = 0x1000;
  static constexpr uint32_t kPenIdBase = 0x2000;

  struct PointerFrame {
    uint32_t id = 0;
    PointerType type = PointerType::Touch;
    bool primary = true;
    float x = 0, y = 0;     // window coordinates of the latest position
    uint8_t buttons = 0;    // buttonBit mask that should be held NOW
                            // (tip/finger contact = Left, barrel = Right,
                            // second barrel = Middle)
    float pressure = 0, tiltX = 0, tiltY = 0, twist = 0;
    bool eraser = false;
    uint64_t timeMs = 0;
    // Every position since the previous frame, oldest first, ending with
    // (x, y). Empty = no motion in this frame (a pure button change).
    std::span<const PointerSample> samples;
  };

  // Buttons feedPointer believes each non-mouse pointer holds. Separate from
  // the router's own record because a press that scrolled instead of
  // clicking (a scrollbar grab) is never handed to the router.
  std::unordered_map<uint32_t, uint8_t> fedButtons_;

  // Applies one frame: a move (coalesced samples, hover for a pen, scroll
  // pans), then any button releases, then any presses. Returns true if
  // anything changed (caller should repaint).
  bool feedPointer(const PointerFrame &f) {
    using liteui_pointer::kMousePointerId;
    if (f.id == kMousePointerId)
      return false; // the mouse has its own entry points
    PointerEvent base;
    base.pointerId = f.id;
    base.type = f.type;
    base.isPrimary = f.primary;
    base.windowX = base.x = f.x;
    base.windowY = base.y = f.y;
    base.mods = modifiers();
    base.pressure = f.pressure;
    base.tiltX = f.tiltX;
    base.tiltY = f.tiltY;
    base.twist = f.twist;
    base.eraser = f.eraser;
    base.timeMs = f.timeMs;

    bool changed = false;
    uint8_t &fed = fedButtons_[f.id];

    if (!f.samples.empty()) {
      PointerEvent m = base;
      m.coalesced = f.samples;
      if (updateScrollDrag(f.x, f.y, f.id))
        changed = true;
      if (ptrRouter_.move(m))
        changed = true;
      if (f.type == PointerType::Pen && hasRoot_) { // a pen hovers like a mouse
        bool needPoll = false;
        if (ptrRouter_.hover(m, &needPoll))
          changed = true;
        if (needPoll) {
          pollAndRelayout();
          changed = true;
        }
      }
    }

    for (int i = 0; i < 3; ++i) { // releases first, then presses
      const uint8_t bit = static_cast<uint8_t>(1u << i);
      if (!(fed & bit) || (f.buttons & bit))
        continue;
      fed &= static_cast<uint8_t>(~bit);
      PointerEvent u = base;
      u.button = static_cast<MouseButton>(i);
      u.coalesced = {};
      bool panConsumed = false;
      if (u.button == MouseButton::Left) {
        panConsumed = scrollDrag_.mode == DragMode::ContentPan &&
                      scrollDrag_.moved && scrollOwner_ == f.id;
        if (endScrollPress(f.x, f.y, &u))
          changed = true;
      } else if (ptrRouter_.up(u)) {
        changed = true;
      }
      // A pan swallowed the release (the router never saw an up): end the
      // pointer's press state the way a cancelled press ends.
      if (panConsumed) {
        ptrRouter_.cancelPointer(f.id);
        changed = true;
      }
    }
    for (int i = 0; i < 3; ++i) {
      const uint8_t bit = static_cast<uint8_t>(1u << i);
      if ((fed & bit) || !(f.buttons & bit))
        continue;
      fed |= bit;
      PointerEvent d = base;
      d.button = static_cast<MouseButton>(i);
      d.coalesced = {};
      if (d.button == MouseButton::Left && beginScrollPress(f.x, f.y, f.id)) {
        changed = true; // a scrollbar grab / track click owns this press
        continue;
      }
      ptrRouter_.down(d);
      changed = true;
    }
    if (f.type == PointerType::Touch && fed == 0)
      fedButtons_.erase(f.id); // a finger's record ends when it lifts
    return changed;
  }

  // The system took pointer `id` away (compositor gesture, lost capture):
  // onPointerCancel, and the pointer is forgotten.
  bool cancelPointerInput(uint32_t id) {
    ptrRouter_.cancelPointer(id);
    if (scrollDrag_.mode != DragMode::None && scrollOwner_ == id)
      scrollDrag_ = {};
    fedButtons_.erase(id);
    return true;
  }

  // A pen left the tablet's range: cancels a press still in flight, drops its
  // state, and un-hovers (the mouse's hover, if it's in the window, is
  // restored by refreshMouseHover()).
  bool penLeftRange(uint32_t id) {
    auto it = fedButtons_.find(id);
    if (it != fedButtons_.end() && it->second != 0)
      cancelPointerInput(id);
    ptrRouter_.forget(id);
    fedButtons_.erase(id);
    bool changed = false;
    if (hasRoot_) {
      bool needPoll = false;
      changed = refreshMouseHover(&needPoll);
      if (needPoll) {
        pollAndRelayout();
        changed = true;
      }
    }
    return changed;
  }

  // A backend's 32-bit millisecond event time (it wraps every ~49 days) as
  // the 64-bit monotonic milliseconds PointerEvent::timeMs uses. Assumes the
  // backend's clock is the same one the steady clock reads (true for
  // compositors and for Windows' tick count modulo 2^32); if it isn't, times
  // are still monotonic and correctly spaced, just on an arbitrary epoch.
  static uint64_t widenMs32(uint32_t t) {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    uint64_t cand = (now & ~0xFFFFFFFFull) | t;
    if (cand > now + (1ull << 31))
      cand -= (1ull << 32);
    else if (cand + (1ull << 31) < now)
      cand += (1ull << 32);
    return cand;
  }

  // Wheel / trackpad scroll at (x, y); dx/dy in pixels (+dy = scroll down,
  // +dx = right — each backend converts to that). Runs onWheel (bubbling),
  // then, unless a handler called stopPropagation(), the built-in overflow
  // scrolling. Returns true if anything changed (caller should repaint).
  bool dispatchWheel(float x, float y, float dx, float dy,
                     bool precise = false) {
    WheelEvent e;
    e.windowX = e.x = x;
    e.windowY = e.y = y;
    e.dx = dx;
    e.dy = dy;
    e.precise = precise;
    e.mods = modifiers();
    bool changed = ptrRouter_.wheel(e);
    if (!e.propagationStopped() && applyWheelScroll(x, y, dx, dy))
      changed = true;
    return changed;
  }

// Windows-only member/method block.
#if defined(_WIN32)
  // Native window handle; null until CreateWindowExW succeeds.
  HWND hwnd_ = nullptr;
  bool moveRequested_ = false;
  std::unordered_map<UINT_PTR, std::function<void()>> timers_;

  // wake(): a private window message posted from any thread. `wakePending_`
  // keeps at most one such message in the queue (see wake / WndProc).
  static constexpr UINT kWakeMsg = WM_APP + 1;
  std::atomic<bool> wakePending_{false};

  // Device-independent: created once in the constructor, lives for the
  // process (well, the LiteUI instance's) lifetime.
  ID2D1Factory1 *d2dFactory_ = nullptr;

  // ---- GPU device objects (Direct3D 11 + Direct2D 1.1) ----------------
  // Device-dependent. Everything the window and every Canvas node draw
  // with hangs off one Direct3D 11 device, so a canvas texture can be
  // drawn straight into the window without ever leaving the GPU:
  //
  //   d3dDevice_ -> d2dDevice_ -> renderTarget_ (window context)
  //                            \-> canvasDc_    (canvas-node context)
  //   d3dDevice_ + hwnd_ -> swapChain_ -> backBuffer_ (renderTarget_'s target)
  //
  // The window and the canvases use separate device contexts because a
  // canvas is rendered (from paintCanvas) while the window's context is
  // already inside BeginDraw()/EndDraw(), and a context can't switch
  // targets or start a second draw in the middle of one.
  ID3D11Device *d3dDevice_ = nullptr;
  IDXGISwapChain1 *swapChain_ = nullptr;
  ID2D1Device *d2dDevice_ = nullptr;
  // The window's context (kept under its old name: it is still "the render
  // target" every paint function draws through; a device context is an
  // ID2D1RenderTarget).
  ID2D1DeviceContext *renderTarget_ = nullptr;
  ID2D1Bitmap1 *backBuffer_ = nullptr;
  ID2D1DeviceContext *canvasDc_ = nullptr;
  // Set when something reports the device is gone (D2DERR_RECREATE_TARGET,
  // DXGI_ERROR_DEVICE_REMOVED/RESET); WM_PAINT/WM_SIZE then tear every
  // device object down and the next paint rebuilds them.
  bool deviceLost_ = false;

  template <class T> static void releaseCom(T *&p) {
    if (p) {
      p->Release();
      p = nullptr;
    }
  }

  // Wraps the swap chain's current back buffer as renderTarget_'s target.
  HRESULT createBackBufferTarget() {
    IDXGISurface *surface = nullptr;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr))
      return hr;
    // 96 dpi: the library lays everything out in client-area pixels, so
    // one Direct2D unit is one pixel (as with the old HwndRenderTarget).
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    hr = renderTarget_->CreateBitmapFromDxgiSurface(surface, &props,
                                                    &backBuffer_);
    surface->Release();
    if (SUCCEEDED(hr))
      renderTarget_->SetTarget(backBuffer_);
    return hr;
  }

  HRESULT createDeviceResources() {
    // BGRA support is required for Direct2D interop. Try the GPU first,
    // fall back to WARP (Microsoft's software rasterizer) so the library
    // still runs on a machine without a usable GPU/driver.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = E_FAIL;
    const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE,
                                     D3D_DRIVER_TYPE_WARP};
    for (D3D_DRIVER_TYPE type : types) {
      hr = D3D11CreateDevice(nullptr, type, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                             ARRAYSIZE(levels), D3D11_SDK_VERSION,
                             &d3dDevice_, nullptr, nullptr);
      if (SUCCEEDED(hr))
        break;
    }
    if (FAILED(hr))
      return hr;

    IDXGIDevice *dxgiDevice = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *dxgiFactory = nullptr;
    hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(hr))
      hr = d2dFactory_->CreateDevice(dxgiDevice, &d2dDevice_);
    if (SUCCEEDED(hr))
      hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                           &renderTarget_);
    if (SUCCEEDED(hr))
      hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                           &canvasDc_);
    if (SUCCEEDED(hr))
      hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr))
      hr = adapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (SUCCEEDED(hr)) {
      RECT rc;
      GetClientRect(hwnd_, &rc);
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      desc.Width = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
      desc.Height = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
      desc.Scaling = DXGI_SCALING_STRETCH;
      hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice_, hwnd_, &desc,
                                               nullptr, nullptr, &swapChain_);
      if (SUCCEEDED(hr))
        dxgiFactory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    }
    releaseCom(dxgiFactory);
    releaseCom(adapter);
    releaseCom(dxgiDevice);
    if (FAILED(hr))
      return hr;
    renderTarget_->SetDpi(96.0f, 96.0f);
    canvasDc_->SetDpi(96.0f, 96.0f);
    return createBackBufferTarget();
  }

  // Drops every device-dependent object, including each Canvas node's GPU
  // texture (the tree is walked via freeTextResources; canvases rebuild and
  // repaint themselves on demand because their texture pointer is null).
  // Also the destructor's teardown path. Safe to call repeatedly.
  void releaseDeviceResources() {
    if (hasRoot_)
      root_.freeTextResources();
    if (renderTarget_)
      renderTarget_->SetTarget(nullptr);
    if (canvasDc_)
      canvasDc_->SetTarget(nullptr);
    releaseCom(backBuffer_);
    releaseCom(canvasDc_);
    releaseCom(renderTarget_);
    releaseCom(swapChain_);
    releaseCom(d2dDevice_);
    releaseCom(d3dDevice_);
    deviceLost_ = false;
  }

  // Lazily (re)creates the whole device stack against hwnd_'s current
  // client size. A no-op once it exists; WM_SIZE resizes the swap chain
  // instead of tearing anything down, so this only really runs at startup
  // and after a device loss.
  void ensureRenderTarget() {
    if (renderTarget_)
      return;
    if (FAILED(createDeviceResources())) {
      releaseDeviceResources();
      throw std::runtime_error("Direct3D 11 / Direct2D device creation failed");
    }
  }

  // Swap-chain buffers can only be resized once nothing references them,
  // so drop the Direct2D wrapper first and re-wrap afterwards.
  void resizeBackBuffer(UINT w, UINT h) {
    if (!swapChain_ || !renderTarget_)
      return;
    renderTarget_->SetTarget(nullptr);
    releaseCom(backBuffer_);
    HRESULT hr = swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr))
      hr = createBackBufferTarget();
    if (FAILED(hr))
      deviceLost_ = true;
  }

  // Converts our own Color into the D2D1::ColorF Direct2D brushes want.
  static D2D1::ColorF toD2DColor(Color c) {
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
  }

  // Draws a filled rectangle in one shot: create brush, fill, release.
  // Mirrors the old gdiFillRect's create-use-delete pattern 1:1.
  static void d2dFillRect(ID2D1RenderTarget *rt, float x, float y, float w,
                          float h, Color c) {
    ID2D1SolidColorBrush *brush = nullptr;
    rt->CreateSolidColorBrush(toD2DColor(c), &brush);
    if (brush) {
      rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
      brush->Release();
    }
  }

  // The Win32 window procedure: Windows calls this for every message sent to
  // hwnd.
  // Win32 resource for a Style::cursor value. Windows has no stock "grab"
  // cursors or zoom cursors: Grab/Grabbing use the hand / four-way arrow, and
  // ZoomIn/ZoomOut fall back to the crosshair.
  static LPCTSTR cursorResource(Cursor c) {
    switch (c) {
    case Cursor::Pointer:
    case Cursor::Grab:
      return IDC_HAND;
    case Cursor::Text:
      return IDC_IBEAM;
    case Cursor::Crosshair:
    case Cursor::ZoomIn:
    case Cursor::ZoomOut:
      return IDC_CROSS;
    case Cursor::Move:
    case Cursor::Grabbing:
      return IDC_SIZEALL;
    case Cursor::NotAllowed:
      return IDC_NO;
    case Cursor::Wait:
      return IDC_WAIT;
    case Cursor::Progress:
      return IDC_APPSTARTING;
    case Cursor::Help:
      return IDC_HELP;
    case Cursor::ResizeEW:
      return IDC_SIZEWE;
    case Cursor::ResizeNS:
      return IDC_SIZENS;
    case Cursor::ResizeNESW:
      return IDC_SIZENESW;
    case Cursor::ResizeNWSE:
      return IDC_SIZENWSE;
    default:
      return IDC_ARROW;
    }
  }

  // The cursor to show with the pointer at client (x, y); null hides it.
  HCURSOR cursorHandleAt(float x, float y) {
    Cursor c = ptrRouter_.cursorAt(x, y);
    return c == Cursor::None ? nullptr : LoadCursor(nullptr, cursorResource(c));
  }

  // Applies the right cursor right now, without waiting for the next mouse
  // message — after a press/release/cancel changes capture, or a relayout
  // puts a different view under a stationary pointer. WM_SETCURSOR covers
  // ordinary motion.
  void refreshCursor() {
    if (!hwnd_)
      return;
    POINT pt;
    if (!GetCursorPos(&pt))
      return;
    const bool captured = GetCapture() == hwnd_;
    if (!captured && WindowFromPoint(pt) != hwnd_)
      return;
    ScreenToClient(hwnd_, &pt);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (!captured && !PtInRect(&rc, pt))
      return; // over the titlebar/border: leave it to Windows
    SetCursor(cursorHandleAt(static_cast<float>(pt.x), static_cast<float>(pt.y)));
  }

  // ---- Pen and touch (WM_POINTER*, Windows 8+) ---------------------------------
  // Pen and finger input arrives as WM_POINTERDOWN/UPDATE/UP. We handle those
  // two pointer types ourselves and return 0, which also stops Windows from
  // promoting them to fake mouse messages; mouse and touchpad pointers are
  // passed to DefWindowProc, which promotes them to the WM_MOUSE* messages the
  // rest of this file already handles.

  // Hover of the mouse (so a pen leaving range doesn't leave a stale hover):
  // re-derive it from where the mouse is, or un-hover everything if it isn't
  // over the client area.
  bool refreshMouseHover(bool *needPoll) {
    float x = -1e6f, y = -1e6f;
    POINT pt;
    RECT rc;
    if (hwnd_ && GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt) &&
        GetClientRect(hwnd_, &rc) && PtInRect(&rc, pt)) {
      x = static_cast<float>(pt.x);
      y = static_cast<float>(pt.y);
    }
    return updateHoverAt(x, y, needPoll);
  }

  // Returns true if the message was a pen or finger and has been handled.
  bool handleWinPointer(UINT msg, WPARAM wp) {
    if (!hwnd_)
      return false;
    const UINT32 wid = GET_POINTERID_WPARAM(wp);
    POINTER_INPUT_TYPE type;
    if (!GetPointerType(wid, &type) || (type != PT_PEN && type != PT_TOUCH))
      return false;

    auto clientPos = [this](const POINTER_INFO &pi, float &x, float &y) {
      POINT p = pi.ptPixelLocation; // device pixels, screen space
      ScreenToClient(hwnd_, &p);
      x = static_cast<float>(p.x);
      y = static_cast<float>(p.y);
    };
    PointerFrame f;
    std::vector<PointerSample> samples;

    if (type == PT_PEN) {
      POINTER_PEN_INFO pen;
      if (!GetPointerPenInfo(wid, &pen))
        return false;
      f.id = kPenIdBase + wid;
      f.type = PointerType::Pen;
      // One PointerSample from a POINTER_PEN_INFO.
      auto sampleOf = [&](const POINTER_PEN_INFO &pi) {
        PointerSample sm;
        clientPos(pi.pointerInfo, sm.x, sm.y);
        const bool contact = (pi.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
        sm.pressure = (pi.penMask & PEN_MASK_PRESSURE)
                          ? static_cast<float>(pi.pressure) / 1024.0f // 0..1024
                          : (contact ? 0.5f : 0.0f);
        if (pi.penMask & PEN_MASK_TILT_X)
          sm.tiltX = static_cast<float>(pi.tiltX); // degrees, -90..90
        if (pi.penMask & PEN_MASK_TILT_Y)
          sm.tiltY = static_cast<float>(pi.tiltY);
        if (pi.penMask & PEN_MASK_ROTATION)
          sm.twist = static_cast<float>(pi.rotation); // degrees, 0..359
        sm.timeMs = pi.pointerInfo.dwTime ? widenMs32(pi.pointerInfo.dwTime) : 0;
        return sm;
      };
      // Every sample since the last message, oldest first (the history call
      // returns newest first and includes the current one).
      if (msg == WM_POINTERUPDATE) {
        UINT32 count = 0;
        if (GetPointerPenInfoHistory(wid, &count, nullptr) && count > 1) {
          std::vector<POINTER_PEN_INFO> hist(count);
          if (GetPointerPenInfoHistory(wid, &count, hist.data()))
            for (UINT32 i = count; i-- > 0;)
              samples.push_back(sampleOf(hist[i]));
        }
        if (samples.empty())
          samples.push_back(sampleOf(pen));
      }
      const PointerSample cur = sampleOf(pen);
      f.x = cur.x;
      f.y = cur.y;
      f.pressure = cur.pressure;
      f.tiltX = cur.tiltX;
      f.tiltY = cur.tiltY;
      f.twist = cur.twist;
      f.timeMs = cur.timeMs;
      f.eraser = (pen.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
      // msg == WM_POINTERUP has no INCONTACT, so the tip comes off here.
      if (pen.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT)
        f.buttons |= buttonBit(MouseButton::Left);
      if (pen.penFlags & PEN_FLAG_BARREL)
        f.buttons |= buttonBit(MouseButton::Right);
    } else {
      POINTER_TOUCH_INFO touch;
      if (!GetPointerTouchInfo(wid, &touch))
        return false;
      f.id = kTouchIdBase + wid;
      f.type = PointerType::Touch;
      f.primary = (touch.pointerInfo.pointerFlags & POINTER_FLAG_PRIMARY) != 0;
      auto sampleOf = [&](const POINTER_TOUCH_INFO &ti) {
        PointerSample sm;
        clientPos(ti.pointerInfo, sm.x, sm.y);
        sm.pressure = (ti.touchMask & TOUCH_MASK_PRESSURE)
                          ? static_cast<float>(ti.pressure) / 1024.0f
                          : 0.5f;
        sm.timeMs = ti.pointerInfo.dwTime ? widenMs32(ti.pointerInfo.dwTime) : 0;
        return sm;
      };
      // No stray move on the down or up itself; those carry the position.
      if (msg == WM_POINTERUPDATE) {
        UINT32 count = 0;
        if (GetPointerTouchInfoHistory(wid, &count, nullptr) && count > 1) {
          std::vector<POINTER_TOUCH_INFO> hist(count);
          if (GetPointerTouchInfoHistory(wid, &count, hist.data()))
            for (UINT32 i = count; i-- > 0;)
              samples.push_back(sampleOf(hist[i]));
        }
        if (samples.empty())
          samples.push_back(sampleOf(touch));
      }
      const PointerSample cur = sampleOf(touch);
      f.x = cur.x;
      f.y = cur.y;
      f.pressure = cur.pressure;
      f.timeMs = cur.timeMs;
      if (touch.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT)
        f.buttons |= buttonBit(MouseButton::Left);
    }
    f.samples = samples;
    if (feedPointer(f))
      InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
  }

  // A pen left range (WM_POINTERLEAVE) or any pointer lost its capture
  // (WM_POINTERCAPTURECHANGED). Both ids are tried because the pointer type
  // can no longer be queried at this point.
  void winPointerGone(UINT msg, UINT32 wid) {
    bool changed = false;
    const uint32_t penId = kPenIdBase + wid, touchId = kTouchIdBase + wid;
    if (msg == WM_POINTERLEAVE) {
      if (ptrRouter_.knows(penId))
        changed = penLeftRange(penId);
    } else {
      if (ptrRouter_.knows(touchId))
        changed |= cancelPointerInput(touchId);
      if (ptrRouter_.knows(penId))
        changed |= cancelPointerInput(penId);
    }
    if (changed && hwnd_)
      InvalidateRect(hwnd_, nullptr, FALSE);
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Will hold the LiteUI instance associated with this hwnd, if any.
    LiteUIImpl *self = nullptr;

    // WM_NCCREATE arrives before any other message and carries the creation
    // parameters.
    if (msg == WM_NCCREATE) {
      // WM_NCCREATE is the very first message a window receives, sent
      // during CreateWindowExW itself, before the window is usable.
      // Reinterpret the message's lParam as the CREATESTRUCTW Windows built for
      // us.
      auto cs = reinterpret_cast<CREATESTRUCTW *>(lp);
      // Recover the `this` pointer we passed as the lpParam argument to
      // CreateWindowExW.
      self = static_cast<LiteUIImpl *>(cs->lpCreateParams);
      // Stash that pointer in the window's user-data slot so future messages
      // can retrieve it.
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    // For every other message, the pointer was already stored by the
    // WM_NCCREATE branch above.
    else {
      // Fetch the previously stored `this` pointer back out of the window's
      // user-data slot.
      self = reinterpret_cast<LiteUIImpl *>(
          GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // Dispatch on the specific message type.
    switch (msg) {
    // Repaint request: paint everything into the off-screen back buffer,
    // then blit it to the screen in a single BitBlt — see memDC_'s comment
    // for why (this is the actual flicker fix).
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      bool recreate = false;
      if (self) {
        self->ensureRenderTarget();
        LITEUI_PROF_START(profBuild);
        self->renderTarget_->BeginDraw();
        self->renderTarget_->Clear(toD2DColor(self->windowBg_));
        self->paintRoot(self->renderTarget_);
        self->paintTooltip(self->renderTarget_);
        LITEUI_PROF_ADD(build, profBuild);
        LITEUI_PROF_START(profEnd);
        HRESULT hr = self->renderTarget_->EndDraw();
        // Hand the finished frame to the display (vsync'd). Skipped when
        // drawing already failed or a canvas reported device loss.
        if (SUCCEEDED(hr) && !self->deviceLost_)
          hr = self->swapChain_->Present(1, 0);
        LITEUI_PROF_ADD(endDraw, profEnd);
        // The GPU/driver went away (reset, removal, TDR): drop every device
        // object — canvas textures included — and rebuild on the next
        // paint, which we request below once EndPaint has run.
        if (self->deviceLost_ || hr == D2DERR_RECREATE_TARGET ||
            hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
          self->releaseDeviceResources();
          recreate = true;
        }
      }
      EndPaint(hwnd, &ps);
      if (recreate)
        InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    // Tells Windows we're handling the background ourselves (see
    // WM_PAINT), so it should skip its own default erase-to-brush pass —
    // that default erase is what caused a visible white/gray flash right
    // before every repaint.
    case WM_ERASEBKGND:
      return 1;

      // Left mouse button pressed: first give scrollbars/scrollable content a
      // chance to claim the press (beginScrollPress) — a thumb grab or track
      // click consumes it entirely; a press over plain scrollable content
      // arms a possible pan but still falls through to the ordinary
      // beginPress() below, since a small movement should still resolve as a
      // click (see updateScrollDrag/endScrollPress). Capture the mouse so we
      // still get the matching WM_MOUSEMOVE/WM_LBUTTONUP even if the cursor
      // leaves the window before the button is released.
    case WM_LBUTTONDOWN: {
      if (self) {
        self->hideTooltip();
        self->moveRequested_ =
            false; // only honor a request made during THIS press
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (!self->beginScrollPress(x, y))
          self->beginPress(x, y);

        if (self->moveRequested_) {
          self->moveRequested_ = false;
          // Hand the mouse to
          // Windows' own move loop, WITHOUT calling SetCapture first. The
          // cursor position goes in lParam so the drag starts exactly where
          // the user pressed. This call blocks until the button is released.
          POINT pt;
          GetCursorPos(&pt);
          SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                       MAKELPARAM(pt.x, pt.y));

          // The OS loop swallowed the release, so drop our pending press
          // instead of leaving it dangling.
          self->cancelPress(MouseButton::Left);
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }

        SetCapture(hwnd);
        if (self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP: {
      if (self && self->handleWinPointer(msg, wp))
        return 0; // pen/finger handled here: no mouse promotion
      return DefWindowProcW(hwnd, msg, wp, lp); // mouse/touchpad: promote
    }
    case WM_POINTERLEAVE:
    case WM_POINTERCAPTURECHANGED: {
      if (self)
        self->winPointerGone(msg, GET_POINTERID_WPARAM(wp));
      return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // Windows asks for the cursor on every mouse move. Inside the client area
    // we answer from Style::cursor; elsewhere (borders, titlebar) the default
    // handling picks the resize/arrow cursor.
    case WM_SETCURSOR: {
      if (self && LOWORD(lp) == HTCLIENT) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        SetCursor(
            self->cursorHandleAt(static_cast<float>(pt.x), static_cast<float>(pt.y)));
        return TRUE;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // Pointer moved with a button held: advances an in-progress scrollbar
    // drag or content pan. No-op (returns false) if neither is active, so
    // this costs nothing on ordinary hover.
    case WM_MOUSEMOVE: {
      if (self) {
        LITEUI_PROF_START(profMove);
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        bool changed = self->updateScrollDrag(x, y);
        if (self->dispatchPointerMove(x, y))
          changed = true;
        bool needPoll = false;
        if (self->hasRoot_ && self->updateHoverAt(x, y, &needPoll))
          changed = true;
        self->updateTooltipHover(x, y);
        if (needPoll) {
          self->pollAndRelayout(); // a hover handler changed app state
          changed = true;
        }
        if (changed && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
        LITEUI_PROF_ADD(move, profMove);
      }
      return 0;
    }

    // Left mouse button released: resolve the pending press against
    // whatever's under the cursor now, firing onClick only if it matches
    // the original press target (endScrollPress handles the scroll-drag
    // side of this and defers to endPress() when a pan never actually
    // moved, i.e. it was really just a click).
    case WM_LBUTTONUP: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (self->endScrollPress(x, y) && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      ReleaseCapture();
      return 0;
    }

    // Middle/right buttons don't interact with scrollbars — that's a
    // left-drag convention — so these go straight through the ordinary
    // press/click path.
    case WM_MBUTTONDOWN: {
      if (self) {
        self->hideTooltip();
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        self->beginPress(x, y, MouseButton::Middle);
        SetCapture(hwnd);
        if (self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    case WM_MBUTTONUP: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (self->endPress(x, y, MouseButton::Middle) && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      ReleaseCapture();
      return 0;
    }

    case WM_RBUTTONDOWN: {
      if (self) {
        self->hideTooltip();
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        self->beginPress(x, y, MouseButton::Right);
        SetCapture(hwnd);
        if (self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    case WM_RBUTTONUP: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (self->endPress(x, y, MouseButton::Right) && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      ReleaseCapture();
      return 0;
    }

    // Vertical mouse-wheel rotation. WM_MOUSEWHEEL's cursor coordinates are
    // in *screen* space (unlike every other mouse message here, which are
    // client-space) — ScreenToClient converts before hit-testing. One
    // notch (WHEEL_DELTA = 120) scrolls a fixed 40px step; larger/precision
    // wheels report multiples/fractions of that.
    case WM_MOUSEWHEEL: {
      if (self) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float notches =
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        // Wheel-up (positive notches) should scroll content up, i.e.
        // decrease scrollY — hence the negation. A delta that isn't a whole
        // number of notches comes from a precision touchpad/wheel.
        bool precise = std::fmod(std::abs(notches), 1.0f) != 0.0f;
        bool changed = self->dispatchWheel(static_cast<float>(pt.x),
                                           static_cast<float>(pt.y), 0.0f,
                                           -notches * 40.0f, precise);
        if (changed && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Horizontal mouse-wheel rotation (tilt-wheel or shift+wheel on most
    // drivers). Same coordinate/notch handling as WM_MOUSEWHEEL, but
    // positive notches scroll right, so no negation here.
    case WM_MOUSEHWHEEL: {
      if (self) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float notches =
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        bool precise = std::fmod(std::abs(notches), 1.0f) != 0.0f;
        if (self->dispatchWheel(static_cast<float>(pt.x),
                                static_cast<float>(pt.y), notches * 40.0f,
                                0.0f, precise) &&
            self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Capture was taken away from us mid-press (e.g. alt-tab, a system
    // dialog popping up) — the click/drag can't complete normally, so drop
    // both the pending click and any in-progress scroll drag rather than
    // let a later, unrelated event resolve them.
    case WM_CAPTURECHANGED:
      if (self) {
        self->cancelAllPresses(); // onPointerCancel
        self->scrollDrag_ = {};
      }
      return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if (self) {
        KeyEvent e{vkToKey(wp), currentModifiers()};
        if (self->dispatchKeyDown(e) && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      // Fall through to DefWindowProcW so system accelerators (Alt+F4
      // etc.) still work; we're not fully intercepting the message.
      return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
      if (self) {
        KeyEvent e{vkToKey(wp), currentModifiers()};
        if (self->dispatchKeyUp(e) && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // WM_CHAR gives already-composed text as UTF-16 code units — handle
    // surrogate pairs since anything outside the BMP (emoji, etc.) arrives
    // as a high/low surrogate pair across two messages.
    case WM_CHAR: {
      if (self) {
        uint32_t cu = static_cast<uint32_t>(wp);
        static uint32_t pendingHighSurrogate = 0;
        if (cu >= 0xD800 && cu <= 0xDBFF) {
          pendingHighSurrogate = cu;
        } else if (cu >= 0xDC00 && cu <= 0xDFFF && pendingHighSurrogate) {
          uint32_t cp =
              0x10000 + ((pendingHighSurrogate - 0xD800) << 10) + (cu - 0xDC00);
          pendingHighSurrogate = 0;
          if (self->dispatchTextInput(cp) && self->hwnd_)
            InvalidateRect(self->hwnd_, nullptr, FALSE);
        } else if (cu >= 0x20 || cu == '\t') { // skip raw control chars
          pendingHighSurrogate = 0;
          if (self->dispatchTextInput(cu) && self->hwnd_)
            InvalidateRect(self->hwnd_, nullptr, FALSE);
        }
      }
      return 0;
    }

    // Window lost OS-level focus entirely (alt-tab, clicking another app)
    // — blur our own focused view so a background window doesn't keep
    // silently eating keystrokes meant for something else.
    case WM_KILLFOCUS:
      if (self) {
        self->setFocus(nullptr);
        self->hideTooltip();
      }
      return 0;

      // Keeps a frameless window from covering the taskbar when maximized.
    case WM_GETMINMAXINFO: {
      if (self && self->hideTitlebar_) {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
          auto *mmi = reinterpret_cast<MINMAXINFO *>(lp);
          mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
          mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
          mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
          mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
          return 0;
        }
      }
      break; // fall out to DefWindowProcW
    }

      // Window was resized (including maximize/restore/snap): update our
      // stored dimensions and re-run layout against the new size. GDI needs
      // no buffer reallocation (it paints straight into the window's DC), so
      // this is just relayout + repaint.
    case WM_SIZE: {
      if (wp == SIZE_MINIMIZED)
        return 0;
      if (self) {
        self->width_ = LOWORD(lp);
        self->height_ = HIWORD(lp);
        if (self->swapChain_) {
          self->resizeBackBuffer(
              static_cast<UINT>(std::max(1, self->width_)),
              static_cast<UINT>(std::max(1, self->height_)));
          if (self->deviceLost_)
            self->releaseDeviceResources(); // rebuilt by the next WM_PAINT
        }
        self->relayout();
        self->pollAndRelayout();
        self->hideTooltip();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case kWakeMsg: {
      if (self) {
        // Clear the flag BEFORE running the handler: a wake() issued while
        // the handler runs must post a fresh message, not be swallowed.
        self->wakePending_.store(false);
        if (self->wakeHandler_)
          self->wakeHandler_();
        self->requestRepaint();
      }
      return 0;
    }
    case WM_TIMER: {
      if (self) {
        auto it = self->timers_.find(wp);
        if (it != self->timers_.end()) {
          it->second();
          self->requestRepaint();
        }
      }
      return 0;
    }
    case WM_CLOSE:
      if (self)
        self->handleCloseRequested();
      return 0;
    // When the window is being destroyed...
    case WM_DESTROY:
      // ...tell Windows to post a WM_QUIT message, which ends the GetMessage
      // loop in run().
      PostQuitMessage(0);
      // Indicate this message was fully handled.
      return 0;
    }
    // Any message not explicitly handled above falls through to Windows'
    // default behavior.
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  // Draws the layout tree (if any). Border-radius is handled natively by
  // D2D1_ROUNDED_RECT — no manual pixel math needed on this platform,
  // same as the old RoundRect approach, just anti-aliased for free.
  void paintRoot(ID2D1RenderTarget *rt) {
    if (hasRoot_) {
      paintView(rt, root_, ClipRect{});
      std::vector<AbsoluteEntry> absolutes;
      collectAbsolutes(root_, absolutes);
      sortAbsolutes(absolutes);
      for (const auto &e : absolutes)
        paintView(rt, *e.view, e.clip);
    }
  }

  void paintTooltip(ID2D1RenderTarget *rt) {
    if (!tooltipVisible_ || !tooltipTarget_ || tooltipTarget_->tooltip.empty())
      return;
    TextStyle ts;
    ts.fontSize = tooltipStyle_.fontSize;
    ts.fontFamily = tooltipStyle_.fontFamily;
    ts.wrap = TextWrap::NoWrap;
    liteui_text::Measurement m = liteui_text::measure(
        tooltipTarget_->tooltip, ts, resolveStatic(ts.fontSize),
        resolveStatic(ts.fontWeight), resolveStatic(ts.fontStyle), -1);
    float pad = tooltipStyle_.padding;
    float boxW = m.width + pad * 2, boxH = m.height + pad * 2;
    float maxX = std::max(2.0f, width_ - boxW - 2.0f);
    float maxY = std::max(2.0f, height_ - boxH - 2.0f);
    float bx = std::clamp(tooltipPointerX_ + 12.0f, 2.0f, maxX);
    float by = std::clamp(tooltipPointerY_ + 18.0f, 2.0f, maxY);

    d2dFillRect(rt, bx, by, boxW, boxH, tooltipStyle_.background);
    IDWriteTextLayout *layout = liteui_text::makeLayout(
        tooltipTarget_->tooltip, ts, resolveStatic(ts.fontSize),
        resolveStatic(ts.fontWeight), resolveStatic(ts.fontStyle), m.width + 4,
        m.height + 4);
    ID2D1SolidColorBrush *brush = nullptr;
    rt->CreateSolidColorBrush(toD2DColor(tooltipStyle_.textColor), &brush);
    if (brush) {
      rt->SetTransform(D2D1::Matrix3x2F::Identity());
      rt->DrawTextLayout(D2D1::Point2F(bx + pad, by + pad), layout, brush);
      brush->Release();
    }
    layout->Release();
  }

  // Draws v's vertical/horizontal scrollbar (whichever are currently
  // showing) using flat fills — same classic fixed track+thumb styling as
  // before, unclipped by the content clip beyond whatever `clip` already
  // restricts (a scrollbar always sits fully within its own view's box,
  // which is itself already visible or this function wouldn't have been
  // reached).
  void paintScrollbars(ID2D1RenderTarget *rt, const View &v) {
    // Auto-hide: painted only while hovered, or while this exact view's
    // thumb is mid-drag (so a fast drag that briefly outruns the pointer
    // doesn't make the bar vanish out from under it).
    if (v.style.scrollbarAutoHide && !v.computed.isHovered &&
        !((scrollDrag_.mode == DragMode::VThumb ||
           scrollDrag_.mode == DragMode::HThumb) &&
          scrollDrag_.target == &v))
      return;
    // Per-view override falls back to the window-wide default independently
    // for track and thumb (see Style::scrollTrackColor/scrollThumbColor).
    Color track = resolveDynamic(v.style.scrollTrackColor,
                                 v.computed.resolvedScrollTrackColor)
                      .value_or(scrollTrack_);
    Color thumb = resolveDynamic(v.style.scrollThumbColor,
                                 v.computed.resolvedScrollThumbColor)
                      .value_or(scrollThumb_);
    auto box = [&](const PixRect &r, Color c) {
      d2dFillRect(rt, r.x, r.y, r.w, r.h, c);
    };
    if (wantVBar(v)) {
      box(vTrackRect(v), track);
      box(vThumbRect(v), thumb);
    }
    if (wantHBar(v)) {
      box(hTrackRect(v), track);
      box(hThumbRect(v), thumb);
    }
    if (wantVBar(v) && wantHBar(v))
      d2dFillRect(rt, v.computed.x + v.computed.w - kScrollbarThickness,
                  v.computed.y + v.computed.h - kScrollbarThickness,
                  kScrollbarThickness, kScrollbarThickness, track);
  }

  // Rebuilds v.computed.textLayout if it's missing or was built for a
  // different final width — a resize/relayout can change how the text
  // wraps even with no string/style change. `v` is const here (called
  // from paintView), which is exactly why the cache fields are mutable.
  static void ensureTextLayout(const View &v) {
    const std::string &text = resolveDynamic(v.text, v.computed.resolvedText);
    float fontSize =
        resolveDynamic(v.textStyle.fontSize, v.computed.resolvedFontSize);
    FontWeight fontWeight =
        resolveDynamic(v.textStyle.fontWeight, v.computed.resolvedFontWeight);
    FontStyle fontStyleVal =
        resolveDynamic(v.textStyle.fontStyle, v.computed.resolvedFontStyle);
    if (v.computed.textLayout &&
        v.computed.textLayoutBuiltForWidth == v.computed.w &&
        v.computed.textLayoutBuiltForText == text &&
        v.computed.textLayoutBuiltForFontSize == fontSize &&
        v.computed.textLayoutBuiltForFontWeight == fontWeight &&
        v.computed.textLayoutBuiltForFontStyle == fontStyleVal)
      return;
    if (v.computed.textLayout) {
      v.computed.textLayout->Release();
      v.computed.textLayout = nullptr;
    }
    const EdgeInsets &pad =
        resolveDynamic(v.style.padding, v.computed.resolvedPadding);
    float innerW = std::max(0.0f, v.computed.w - pad.left - pad.right);
    float innerH = std::max(0.0f, v.computed.h - pad.top - pad.bottom);
    v.computed.textLayout = liteui_text::makeLayout(
        text, v.textStyle, fontSize, fontWeight, fontStyleVal, innerW, innerH);
    v.computed.textLayoutBuiltForWidth = v.computed.w;
    v.computed.textLayoutBuiltForText = text;
    v.computed.textLayoutBuiltForFontSize = fontSize;
    v.computed.textLayoutBuiltForFontWeight = fontWeight;
    v.computed.textLayoutBuiltForFontStyle = fontStyleVal;
  }

  void paintText(ID2D1RenderTarget *rt, const View &v) {
    ensureTextLayout(v);
    ID2D1SolidColorBrush *brush = nullptr;
    rt->CreateSolidColorBrush(
        toD2DColor(
            resolveDynamic(v.textStyle.color, v.computed.resolvedTextColor)),
        &brush);
    if (brush) {
      const EdgeInsets &pad =
          resolveDynamic(v.style.padding, v.computed.resolvedPadding);
      float x = v.computed.x + pad.left;
      float y = v.computed.y + pad.top;
      rt->DrawTextLayout(D2D1::Point2F(x, y), v.computed.textLayout, brush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
      brush->Release();
    }
  }

  // (Re)builds v.computed.canvasBitmap — a GPU render-target texture sized
  // to v's inner (padding-excluded) content box, living on the same
  // Direct3D 11 device as the window — whenever that size has changed
  // since it was last built, then v.onPaint is (re)invoked into it
  // whenever the size changed OR the app requested a redraw via
  // View::requestCanvasRedraw() (canvasNeedsRedraw starts true, so this
  // always happens at least once). A plain repaint with neither condition
  // true is a no-op: the previous frame's texture is simply reused.
  void ensureCanvasTarget(const View &v) {
    if (!canvasDc_)
      return;
    const EdgeInsets &pad =
        resolveDynamic(v.style.padding, v.computed.resolvedPadding);
    float innerW = std::max(1.0f, v.computed.w - pad.left - pad.right);
    float innerH = std::max(1.0f, v.computed.h - pad.top - pad.bottom);
    bool resized = !v.computed.canvasBitmap ||
                   v.computed.canvasBuiltForWidth != innerW ||
                   v.computed.canvasBuiltForHeight != innerH;
    if (resized) {
      releaseCom(v.computed.canvasBitmap);
      releaseCom(v.computed.canvasStaging); // sized for the old texture
      // cachedImageBitmap is deliberately kept: it is a device resource,
      // not tied to the old texture, so a resize can reuse it.
      D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
          D2D1_BITMAP_OPTIONS_TARGET,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_PREMULTIPLIED),
          96.0f, 96.0f);
      canvasDc_->CreateBitmap(
          D2D1::SizeU(static_cast<UINT32>(innerW),
                      static_cast<UINT32>(innerH)),
          nullptr, 0, &props, &v.computed.canvasBitmap);
      v.computed.canvasBuiltForWidth = innerW;
      v.computed.canvasBuiltForHeight = innerH;
      v.computed.canvasNeedsRedraw = true;
    }
    if (!v.computed.canvasBitmap)
      return;
    if (v.computed.canvasNeedsRedraw && v.onPaint) {
      LITEUI_PROF_START(profCanvas);
      canvasDc_->SetTarget(v.computed.canvasBitmap);
      canvasDc_->BeginDraw();
      canvasDc_->Clear(D2D1::ColorF(0, 0, 0, 0));
      {
        // Scoped so ~CanvasContextImpl (which pops any clip layers onPaint
        // left open) runs BEFORE EndDraw().
        CanvasContextImpl ctx(canvasDc_, v.computed.canvasBitmap, innerW,
                              innerH, &v.computed);
        v.onPaint(ctx);
      }
      HRESULT hr = canvasDc_->EndDraw();
      canvasDc_->SetTarget(nullptr);
      if (hr == D2DERR_RECREATE_TARGET)
        deviceLost_ = true; // WM_PAINT tears the device down after this frame
      v.computed.canvasNeedsRedraw = false;
      LITEUI_PROF_ADD(canvas, profCanvas);
    }
  }

  // Draws v's canvas texture into the window's render target at v's final
  // on-screen position (inset by padding, same convention as paintText's
  // x/y). Both contexts share one Direct2D device, so this is a GPU-to-GPU
  // textured quad: no pixel is copied or uploaded.
  void paintCanvas(ID2D1RenderTarget *rt, const View &v) {
    ensureCanvasTarget(v);
    ID2D1Bitmap1 *bmp = v.computed.canvasBitmap;
    if (!bmp)
      return;
    LITEUI_PROF_START(profBlit);
    const EdgeInsets &pad =
        resolveDynamic(v.style.padding, v.computed.resolvedPadding);
    float x = v.computed.x + pad.left;
    float y = v.computed.y + pad.top;
    D2D1_SIZE_F sz = bmp->GetSize();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->DrawBitmap(bmp, D2D1::RectF(x, y, x + sz.width, y + sz.height));
    LITEUI_PROF_ADD(blit, profBlit);
  }

  // `clip` is the accumulated visible region from scrollable ancestors.
  // Direct2D's clip stack is push/pop rather than GDI's set-and-restore,
  // but since this function is itself called recursively (one call frame
  // per View), pushing on entry and popping on exit naturally nests
  // correctly with the call tree — no need to save/restore a previous
  // clip handle the way SelectClipRgn did.
  void paintView(ID2D1RenderTarget *rt, const View &v, ClipRect clip) {
    if (resolveDynamic(v.style.visibility, v.computed.resolvedVisibility) ==
        Visibility::Hidden)
      return; // space already reserved by layout; just don't draw it

    const Style &s = v.style;
    float x = v.computed.x, y = v.computed.y, w = v.computed.w,
          h = v.computed.h;

    bool clipped = clip.x0 != -std::numeric_limits<float>::infinity() ||
                   clip.y0 != -std::numeric_limits<float>::infinity() ||
                   clip.x1 != std::numeric_limits<float>::infinity() ||
                   clip.y1 != std::numeric_limits<float>::infinity();
    if (clipped)
      rt->PushAxisAlignedClip(D2D1::RectF(clip.x0, clip.y0, clip.x1, clip.y1),
                              D2D1_ANTIALIAS_MODE_ALIASED);

    // Text leaves never paint their own background/border — Text::style is
    // layout-only (see the struct's comment); a solid backgroundColor here
    // would otherwise paint an opaque box under the glyphs on every text
    // node, since Style::backgroundColor defaults to opaque white rather
    // than "none" (Color has no alpha channel to express transparent).
    if (v.isText) {
      paintText(rt, v);
      if (clipped)
        rt->PopAxisAlignedClip();
      return;
    }

    float borderRadius =
        resolveDynamic(s.borderRadius, v.computed.resolvedBorderRadius);
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x, y, x + w, y + h), borderRadius,
                            borderRadius};
    Color bg = (v.computed.isHovered && s.hoverColor)
                   ? *s.hoverColor
                   : resolveDynamic(s.backgroundColor,
                                    v.computed.resolvedBackgroundColor);
    ID2D1SolidColorBrush *bgBrush = nullptr;
    rt->CreateSolidColorBrush(toD2DColor(bg), &bgBrush);
    if (bgBrush) {
      rt->FillRoundedRectangle(rr, bgBrush);
      bgBrush->Release();
    }
    float borderWidth =
        resolveDynamic(s.borderWidth, v.computed.resolvedBorderWidth);
    if (borderWidth > 0) {
      ID2D1SolidColorBrush *borderBrush = nullptr;
      rt->CreateSolidColorBrush(
          toD2DColor(
              resolveDynamic(s.borderColor, v.computed.resolvedBorderColor)),
          &borderBrush);
      if (borderBrush) {
        rt->DrawRoundedRectangle(rr, borderBrush, borderWidth);
        borderBrush->Release();
      }
    }

    // A canvas node paints its own Style background/border like any
    // other box (above), then has its cached onPaint bitmap blitted on
    // top — unlike a text leaf, which skips background/border entirely.
    // Canvas nodes aren't expected to have children, so control falls
    // through to the (harmless, normally no-op) children loop below.
    if (v.isCanvas)
      paintCanvas(rt, v);

    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (const auto &child : v.children)
      if (child.style.position != Position::Absolute &&
          resolveDynamic(child.style.display, child.computed.resolvedDisplay) !=
              Display::None)
        paintView(rt, child, childClip);

    // Scrollbars are drawn after children, still under v's own (ancestor,
    // not child-narrowed) clip — we haven't popped it yet, so this is
    // still exactly `clip`, matching the old GDI re-selection semantics.
    // Scrollbars sit in the gutter layout already reserved outside the
    // children's placement area, so drawing them after children never
    // overlaps content, and keeps them on top the way an overlay
    // scrollbar should be.
    if (v.scrollsX() || v.scrollsY())
      paintScrollbars(rt, v);

    if (clipped)
      rt->PopAxisAlignedClip();
  }

#else // Linux / Wayland

  wl_display *display_ =
      nullptr; // Connection handle to the Wayland compositor; null until
               // wl_display_connect succeeds.
  wl_compositor *compositor_ =
      nullptr; // The compositor global, used to create surfaces.
  xdg_wm_base *wm_base_ = nullptr;  // The xdg-shell global, used to turn a raw
                                    // surface into a desktop window.
  wl_surface *surface_ = nullptr;   // The raw drawable surface for this window.
  xdg_surface *xdg_surf_ = nullptr; // The xdg_surface wrapping surface_, needed
                                    // for configure/ack handshakes.
  xdg_toplevel *toplevel_ =
      nullptr; // The xdg_toplevel representing this as a top-level
               // (movable/resizable/closable) window.
  wl_shm *shm_ =
      nullptr; // The shared-memory global used to allocate pixel buffers.

  wl_seat *seat_ = nullptr; // The seat global, representing one user's set of
                            // input devices (keyboard/mouse/etc.).
  wl_keyboard *keyboard_ = nullptr;
  xkb_context *xkbContext_ = nullptr;
  xkb_keymap *xkbKeymap_ = nullptr;
  xkb_state *xkbState_ = nullptr;

  struct IntervalTimer {
    int fd;
    std::function<void()> fn;
  };
  std::vector<IntervalTimer> intervalTimers_;

  // wake(): an eventfd folded into run()'s poll() set. write() from any
  // thread makes it readable (writes add to a counter, so they coalesce);
  // the loop drains it with one read(). Created with the object, so the
  // descriptor is valid before any other thread can possibly call wake().
  int wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  int nextIntervalHandle_ = 1;
  std::unordered_map<int, int>
      intervalHandleToFd_; // handle -> fd, for removeInterval

  // Key-repeat: Wayland does NOT auto-repeat key events for you (unlike
  // Win32). The compositor's repeat_info event gives the rate and delay,
  // and the client drives its own timer. Here that is a POSIX timerfd,
  // armed on key press (armRepeatTimer) and polled in run() alongside
  // the Wayland display fd; each expiry calls fireRepeatIfDue(), which
  // re-dispatches the held key.
  int repeatTimerFd_ = -1;
  uint32_t repeatKeycode_ = 0;
  int32_t repeatRateHz_ = 0; // 0 = repeat disabled by compositor
  int32_t repeatDelayMs_ = 0;

  wl_pointer *pointer_ =
      nullptr; // The pointer (mouse) device obtained from
               // the seat, once it announces pointer capability.
  wl_cursor_theme *cursorTheme_ =
      nullptr; // Loaded cursor theme (arrow, resize handles, etc.), used to
               // look up the pixel images for wl_pointer_set_cursor.
  wl_surface *cursorSurface_ =
      nullptr; // Dedicated surface that holds whichever cursor image is
               // currently active; the compositor renders this at the
               // pointer position once we call wl_pointer_set_cursor.
  uint32_t pointerEnterSerial_ =
      0; // Serial from the most recent pointer-enter event;
         // wl_pointer_set_cursor requires one and it's not resent on motion, so
         // we cache it.
  uint32_t lastPressSerial_ = 0;
  std::string currentCursorName_; // Name of the cursor image currently shown,
                                  // so we don't reissue set_cursor every
                                  // single motion event for no reason.

  zxdg_decoration_manager_v1 *decoration_manager_ =
      nullptr; // Global for negotiating who draws window decorations (server vs
               // client).

  zxdg_toplevel_decoration_v1 *toplevel_decoration_ =
      nullptr; // Per-window decoration-mode object obtained from
               // decoration_manager_.

  // software rendered CSD titlebar. No cairo here, so buttons are drawn with
  // raw pixel writes straight into the shm buffer.

  static constexpr int kTitlebarHeight =
      32; // Height in pixels reserved at the top of the window for the custom
  // titlebar.
  int titlebarHeight() const { return hideTitlebar_ ? 0 : kTitlebarHeight; }

  static constexpr int kButtonSize =
      18; // Width/height in pixels of each titlebar button's square hitbox.

  static constexpr int kButtonMargin =
      8; // Horizontal gap in pixels between buttons and between the last button
         // and the window edge.
  // Raw Linux input-event code for the left mouse button (from
  // linux/input-event-codes.h).
  static constexpr uint32_t BTN_LEFT_CODE = 0x110;  // linux/input-event-codes.h
  static constexpr uint32_t BTN_RIGHT_CODE = 0x111; // linux/input-event-codes.h
  static constexpr uint32_t BTN_MIDDLE_CODE =
      0x112; // linux/input-event-codes.h

  double pointer_x_ = 0,
         pointer_y_ = 0; // Last known pointer position within
                         // the surface, in surface-local coordinates.

  // Set by pointerMotion; cleared by flushPendingMotion. Motion events only
  // record the newest position — the real work (drag handlers, hover,
  // tooltip, cursor, redraw) runs once per event-loop iteration instead of
  // once per event. See flushPendingMotion for why.
  bool motionPending_ = false;

  bool maximized_ =
      false; // Tracks whether the window currently believes
             // itself to be maximized (for toggling and icon state).

  // ---- EGL / GLES 3.0 ----
  // GPU-accelerated replacement for the old wl_shm double buffer. The
  // compositor owns presentation timing via eglSwapBuffers (backed by
  // wayland-egl's own internal buffer queue), so there's no manual
  // busy-tracking/retry logic to maintain anymore — swap is synchronous
  // from our point of view.
  wl_egl_window *eglWindow_ = nullptr;
  EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
  EGLContext eglContext_ = EGL_NO_CONTEXT;
  EGLSurface eglSurface_ = EGL_NO_SURFACE;
  bool eglReady_ = false; // true once initEgl() has completed
  EGLConfig eglConfig_ = nullptr; // kept so a lost context can be recreated
  bool recoveringContext_ = false;
  int contextLossStreak_ = 0; // consecutive lost swaps (recovery gives up at 3)

  GLuint rectProgram_ = 0, flatProgram_ = 0;
  GLuint texProgram_ = 0;
#if defined(LITEUI_CANVAS_GL)
  liteui_canvas_gl::Renderer canvasGl_; // shared by every canvas node
#endif
  GLuint canvasProgram_ = 0; // samples RGBA directly, no alpha-tint (unlike
                             // texProgram_'s glyph-coverage tinting)
  GLuint quadVbo_ = 0; // static unit quad, reused by every rectProgram_ draw
  GLuint flatVbo_ = 0; // rewritten per-call for flatProgram_ (line quads)
  GLint rectAPos_ = -1, rectUPos_ = -1, rectUSize_ = -1, rectUScreen_ = -1,
        rectURadius_ = -1, rectUColor_ = -1;
  GLint flatAPos_ = -1, flatUScreen_ = -1, flatUColor_ = -1;

  GLint texAPos_ = -1, texUPos_ = -1, texUSize_ = -1, texUScreen_ = -1,
        texUTex_ = -1, texUColor_ = -1;
  GLint canvasAPos_ = -1, canvasUPos_ = -1, canvasUSize_ = -1,
        canvasUScreen_ = -1, canvasUTex_ = -1;

  bool configured_ =
      false; // Set true once the compositor has sent its first
             // configure event, meaning we're allowed to attach a buffer.
  int pendingWidth_ = 0;  // Size most recently suggested by
  int pendingHeight_ = 0; // toplevelConfigure; 0 means "no suggestion yet"
                          // (the compositor may send 0x0 to mean "you decide").
  bool running_ = true;   // Controls the event loop in run(); set false to
                          // request a clean exit.

  // ---- GLES 3.0 rendering ----
  // Two tiny shader programs cover everything this UI draws:
  //  - rectProgram_: an axis-aligned box with an optional rounded-corner
  //    radius, via a signed-distance test in the fragment shader. Used for
  //    all backgrounds/borders/scrollbars (radius 0 == a plain rect).
  //  - flatProgram_: an arbitrary pixel-space quad with a flat color. Used
  //    only for drawLine's thick-line quads (titlebar icons).
  static constexpr const char *kRectVS = R"(
    precision mediump float;
    attribute vec2 a_pos;      // unit quad, 0..1
    uniform vec2 u_rectPos;    // top-left, window pixels
    uniform vec2 u_rectSize;   // pixels
    uniform vec2 u_screen;     // window size, pixels
    varying vec2 v_local;      // pixel offset within the rect
    void main() {
      v_local = a_pos * u_rectSize;
      vec2 px = u_rectPos + a_pos * u_rectSize;
      gl_Position = vec4(px.x / u_screen.x * 2.0 - 1.0,
                         1.0 - px.y / u_screen.y * 2.0, 0.0, 1.0);
    }
  )";
  static constexpr const char *kRectFS = R"(
    precision mediump float;
    varying vec2 v_local;
    uniform vec2 u_rectSize;
    uniform float u_radius;
    uniform vec4 u_color;
    void main() {
      vec2 half_ = u_rectSize * 0.5;
      vec2 d = abs(v_local - half_) - (half_ - u_radius);
      float dist = length(max(d, 0.0)) - u_radius;
      float alpha = 1.0 - smoothstep(-1.0, 1.0, dist);
      gl_FragColor = vec4(u_color.rgb, u_color.a * alpha);
    }
  )";
  static constexpr const char *kFlatVS = R"(
    precision mediump float;
    attribute vec2 a_pos;   // window pixels, explicit per-vertex
    uniform vec2 u_screen;
    void main() {
      gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0,
                         1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0);
    }
  )";
  static constexpr const char *kFlatFS = R"(
    precision mediump float;
    uniform vec4 u_color;
    void main() { gl_FragColor = u_color; }
  )";

  // Draws a cached glyph-coverage texture (see ensureTextTexture) tinted
  // by u_color. The texture stores coverage in its alpha channel only —
  // rasterized once in plain white so the same texture can be recolored
  // without re-rendering glyphs.
  static constexpr const char *kTexVS = R"(
    precision mediump float;
    attribute vec2 a_pos;   // unit quad, 0..1 — doubles as UV
    uniform vec2 u_pos;
    uniform vec2 u_size;
    uniform vec2 u_screen;
    varying vec2 v_uv;
    void main() {
      v_uv = a_pos;
      vec2 px = u_pos + a_pos * u_size;
      gl_Position = vec4(px.x / u_screen.x * 2.0 - 1.0,
                         1.0 - px.y / u_screen.y * 2.0, 0.0, 1.0);
    }
  )";
  static constexpr const char *kTexFS = R"(
    precision mediump float;
    varying vec2 v_uv;
    uniform sampler2D u_tex;
    uniform vec4 u_color;
    void main() {
      float a = texture2D(u_tex, v_uv).a;
      gl_FragColor = vec4(u_color.rgb, u_color.a * a);
    }
  )";

  // Draws a Canvas node's cached backing texture (see ensureCanvasSurface).
  // Unlike kTexFS (which stores only glyph coverage in alpha and tints it
  // with a separate u_color), a canvas texture already carries real RGBA
  // color data straight out of Cairo, so this simply samples it as-is —
  // reuses kTexVS unchanged since the vertex stage (position a unit quad,
  // pass through UV) is identical.
  // The GPU canvas backend's targets hold plain RGBA (no Cairo BGRA
  // byte order), already premultiplied: sample as-is.
  static constexpr const char *kCanvasRgbaFS = R"(
    precision mediump float;
    varying vec2 v_uv;
    uniform sampler2D u_tex;
    void main() {
      gl_FragColor = texture2D(u_tex, v_uv);
    }
  )";
  static constexpr const char *kCanvasFS = R"(
    precision mediump float;
    varying vec2 v_uv;
    uniform sampler2D u_tex;
    void main() {
      // Cairo ARGB32 bytes are B,G,R,A (little-endian) and premultiplied;
      // uploaded as GL_RGBA they arrive as (r=B, g=G, b=R, a=A), so swizzle back.
      gl_FragColor = texture2D(u_tex, v_uv).bgra;
    }
  )";

  static GLuint compileShader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512];
      glGetShaderInfoLog(s, sizeof(log), nullptr, log);
      fprintf(stderr, "shader compile error: %s\n", log);
      throw std::runtime_error("GLSL shader compile failed");
    }
    return s;
  }
  static GLuint linkProgram(const char *vs, const char *fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[512];
      glGetProgramInfoLog(p, sizeof(log), nullptr, log);
      fprintf(stderr, "program link error: %s\n", log);
      throw std::runtime_error("GLSL program link failed");
    }
    return p;
  }

  // xdg_wm_base ping handler: the compositor periodically checks we're alive.
  static void wmBasePing(void *, xdg_wm_base *base, uint32_t serial) {
    // Echo the serial straight back so the compositor knows we're responsive.
    xdg_wm_base_pong(base, serial);
  }
  // Listener struct binding the ping callback above to xdg_wm_base's single
  // event.
  static constexpr xdg_wm_base_listener wmBaseListener = {wmBasePing};

  // Called when the compositor wants us to (re)configure our xdg_surface.
  static void surfaceConfigure(void *data, xdg_surface *xs, uint32_t serial) {
    // Recover the LiteUI instance this callback belongs to.
    auto *self = static_cast<LiteUIImpl *>(data);
    // Acknowledge the configure event with its serial, as the protocol
    // requires.
    xdg_surface_ack_configure(xs, serial);
    // Remember that we're now allowed to attach a buffer.
    self->configured_ = true;
    // If the compositor suggested a size different from what we have, resize
    // (reallocate the buffer + relayout) to match. Otherwise, on the very
    // first configure, no buffer exists yet, so create and attach one now.
    if (self->pendingWidth_ > 0 && self->pendingHeight_ > 0 &&
        (self->pendingWidth_ != self->width_ ||
         self->pendingHeight_ != self->height_)) {
      self->resize(self->pendingWidth_, self->pendingHeight_);
    } else if (!self->eglReady_)
      self->initEgl();
  }
  // Listener struct binding surfaceConfigure to xdg_surface's single event.
  static constexpr xdg_surface_listener surfListener = {surfaceConfigure};

  // Called when the compositor suggests a new size/state for the toplevel.
  static void toplevelConfigure(void *data, xdg_toplevel *, int32_t width,
                                int32_t height, wl_array *states) {
    auto *self = static_cast<LiteUIImpl *>(data);
    if (width > 0 && height > 0) {
      self->pendingWidth_ = width;
      self->pendingHeight_ = height;
    }
    bool max = false;
    auto *s = static_cast<uint32_t *>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i)
      if (s[i] == XDG_TOPLEVEL_STATE_MAXIMIZED)
        max = true;
    self->maximized_ = max;
  }
  // Called when the compositor/user requests the window be closed (e.g. via a
  // taskbar close action).
  static void toplevelClose(void *data, xdg_toplevel *) {
    // Flip the running flag so run()'s dispatch loop exits on its next check.
    static_cast<LiteUIImpl *>(data)->running_ = false;
  }
  // Listener struct binding the two callbacks above to xdg_toplevel's
  // configure/close events.
  static constexpr xdg_toplevel_listener toplevelListener = {toplevelConfigure,
                                                             toplevelClose};

  // wl_seat/wl_pointer

  // Called when the seat announces which input capabilities
  // (pointer/keyboard/touch) it has.
  static void seatCapabilities(void *data, wl_seat *seat, uint32_t caps) {
    auto *self = static_cast<LiteUIImpl *>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_) {
      self->pointer_ = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(self->pointer_, &pointerListener, self);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->keyboard_) {
      self->keyboard_ = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(self->keyboard_, &keyboardListener, self);
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !self->touch_) {
      self->touch_ = wl_seat_get_touch(seat);
      wl_touch_add_listener(self->touch_, &touchListener, self);
    }
  }

  // Called when the seat reports its human-readable name; unused here.
  static void seatName(void *, wl_seat *, const char *) {}
  // Listener struct binding the two seat callbacks above to wl_seat's events.
  static constexpr wl_seat_listener seatListener = {seatCapabilities, seatName};

  // ---- Touch (wl_touch) and pen (zwp_tablet_v2) ---------------------------------
  // Both report in *frames*: a burst of events describing one instant, closed
  // by a frame event. We fold each frame into a state per finger / pen tool,
  // and hand it to feedPointer(). A frame that only moves things is parked and
  // applied once per event-loop iteration with every sample it collected (so a
  // 400 Hz pen costs one handler call per display frame yet loses nothing —
  // PointerEvent::coalesced); a frame that presses, lifts, or changes a button
  // first applies everything older, so ordering is preserved.

  // One finger currently on the surface.
  struct TouchPoint {
    int32_t wlId = 0;     // the compositor's id for this contact
    uint32_t id = 0;      // ours: unique for the process lifetime
    bool primary = false; // the first finger down while no other was
    float x = 0, y = 0;
    bool isNew = true;    // down seen, not yet delivered
    bool lifted = false;  // up seen, not yet delivered
    uint64_t timeMs = 0;
    std::vector<PointerSample> pending; // motion not yet delivered
  };
  std::vector<TouchPoint> touchPoints_;
  uint32_t nextTouchId_ = kTouchIdBase;

  // One pen/eraser/etc. tool the tablet has announced.
  struct TabletTool {
    LiteUIImpl *owner = nullptr;
    zwp_tablet_tool_v2 *tool = nullptr;
    uint32_t id = 0;
    bool eraser = false;
    // Latest raw values (events of a frame update these).
    float x = 0, y = 0, pressure = 0, tiltX = 0, tiltY = 0, twist = 0;
    bool havePosition = false, inProximity = false, contact = false,
         stylus1 = false, stylus2 = false;
    // Set by the events of the frame being assembled; consumed at frame().
    bool dirty = false, proxIn = false, proxOut = false;
    uint32_t proxSerial = 0;
    uint8_t desired = 0;   // buttonBit mask the pen should hold after the frame
    uint64_t lastMs = 0;
    std::vector<PointerSample> pending;
  };
  std::vector<std::unique_ptr<TabletTool>> tabletTools_;
  uint32_t nextPenId_ = kPenIdBase;

  // Motion parked by a touch/tablet frame, waiting for the loop's next flush.
  bool penTouchPending_ = false;

  zwp_tablet_manager_v2 *tabletManager_ = nullptr;
  zwp_tablet_seat_v2 *tabletSeat_ = nullptr;
  wl_touch *touch_ = nullptr;
  wl_surface *toolCursorSurface_ = nullptr; // a tablet tool's cursor needs its
                                            // own surface (a different role
                                            // from the pointer's)
  bool pointerInside_ = false; // whether the mouse pointer is over the surface

  // Hover of the mouse (so a pen leaving range doesn't leave a stale hover):
  // re-derive it from where the mouse is, or un-hover everything if it
  // isn't in the window.
  bool refreshMouseHover(bool *needPoll) {
    const float x = pointerInside_ ? static_cast<float>(pointer_x_) : -1e6f;
    const float y = pointerInside_ ? static_cast<float>(pointer_y_) : -1e6f;
    return updateHoverAt(x, y, needPoll);
  }

  // Sends a touch point's parked motion (it is held down).
  bool flushTouch(TouchPoint &p) {
    if (p.pending.empty() || p.isNew)
      return false; // a finger that hasn't gone down yet has nothing to move
    PointerFrame f;
    f.id = p.id;
    f.type = PointerType::Touch;
    f.primary = p.primary;
    f.x = p.x;
    f.y = p.y;
    f.buttons = buttonBit(MouseButton::Left);
    f.pressure = 0.5f;
    f.timeMs = p.pending.back().timeMs;
    f.samples = p.pending;
    bool changed = feedPointer(f);
    p.pending.clear();
    return changed;
  }

  // Sends a pen's parked motion together with its current button state.
  bool flushTool(TabletTool &t) {
    if (!t.havePosition || (!t.inProximity && t.desired == 0 && t.pending.empty()))
      return false;
    PointerFrame f;
    f.id = t.id;
    f.type = PointerType::Pen;
    f.x = t.x;
    f.y = t.y;
    f.buttons = t.desired;
    f.pressure = t.pressure;
    f.tiltX = t.tiltX;
    f.tiltY = t.tiltY;
    f.twist = t.twist;
    f.eraser = t.eraser;
    f.timeMs = t.pending.empty() ? t.lastMs : t.pending.back().timeMs;
    f.samples = t.pending;
    bool changed = feedPointer(f);
    t.pending.clear();
    return changed;
  }

  // Applies everything parked. Returns whether anything changed.
  bool flushPenTouchPending() {
    penTouchPending_ = false;
    bool changed = false;
    for (auto &t : tabletTools_)
      if (!t->pending.empty())
        changed |= flushTool(*t);
    for (auto &p : touchPoints_)
      changed |= flushTouch(p);
    return changed;
  }
  // The event loop's once-per-iteration flush (see run()).
  void flushPendingPen() {
    if (penTouchPending_ && flushPenTouchPending())
      redraw();
  }

  // -- wl_touch --
  static void touchDown(void *data, wl_touch *, uint32_t, uint32_t time,
                        wl_surface *, int32_t wlId, wl_fixed_t sx,
                        wl_fixed_t sy) {
    auto *self = static_cast<LiteUIImpl *>(data);
    TouchPoint p;
    p.wlId = wlId;
    p.id = self->nextTouchId_++;
    p.primary = true;
    for (const auto &o : self->touchPoints_)
      if (!o.lifted)
        p.primary = false;
    p.x = static_cast<float>(wl_fixed_to_double(sx));
    p.y = static_cast<float>(wl_fixed_to_double(sy));
    p.timeMs = widenMs32(time);
    self->touchPoints_.push_back(std::move(p));
  }
  static void touchUp(void *data, wl_touch *, uint32_t, uint32_t time,
                      int32_t wlId) {
    auto *self = static_cast<LiteUIImpl *>(data);
    for (auto &p : self->touchPoints_)
      if (p.wlId == wlId && !p.lifted) {
        p.lifted = true;
        p.timeMs = widenMs32(time);
      }
  }
  static void touchMotion(void *data, wl_touch *, uint32_t time, int32_t wlId,
                          wl_fixed_t sx, wl_fixed_t sy) {
    auto *self = static_cast<LiteUIImpl *>(data);
    for (auto &p : self->touchPoints_)
      if (p.wlId == wlId && !p.lifted) {
        p.x = static_cast<float>(wl_fixed_to_double(sx));
        p.y = static_cast<float>(wl_fixed_to_double(sy));
        p.timeMs = widenMs32(time);
        p.pending.push_back({p.x, p.y, 0.5f, 0, 0, 0, p.timeMs});
      }
  }
  static void touchFrame(void *data, wl_touch *) {
    static_cast<LiteUIImpl *>(data)->processTouchFrame();
  }
  // The compositor took the touches over (e.g. a system gesture): every
  // contact is cancelled, and no up will follow.
  static void touchCancel(void *data, wl_touch *) {
    auto *self = static_cast<LiteUIImpl *>(data);
    for (auto &p : self->touchPoints_)
      self->cancelPointerInput(p.id);
    self->touchPoints_.clear();
    if (self->eglReady_)
      self->redraw();
  }
  static constexpr wl_touch_listener touchListener = {
      touchDown, touchUp, touchMotion, touchFrame, touchCancel};

  void processTouchFrame() {
    bool transition = false;
    for (const auto &p : touchPoints_)
      if (p.isNew || p.lifted)
        transition = true;
    if (!transition) { // motion only: park it for the loop's flush
      for (const auto &p : touchPoints_)
        if (!p.pending.empty())
          penTouchPending_ = true;
      return;
    }
    bool changed = flushPenTouchPending(); // older motion first, in order
    for (size_t i = 0; i < touchPoints_.size();) {
      TouchPoint &p = touchPoints_[i];
      PointerFrame f;
      f.id = p.id;
      f.type = PointerType::Touch;
      f.primary = p.primary;
      f.x = p.x;
      f.y = p.y;
      f.pressure = 0.5f;
      f.timeMs = p.timeMs;
      if (p.isNew) {
        p.isNew = false;
        p.pending.clear(); // a press at the latest position; no stray move
        f.buttons = buttonBit(MouseButton::Left);
        changed |= feedPointer(f);
      }
      if (p.lifted) {
        f.buttons = 0;
        changed |= feedPointer(f);
        touchPoints_.erase(touchPoints_.begin() + static_cast<long>(i));
        continue;
      }
      ++i;
    }
    if (changed && eglReady_)
      redraw();
  }

  // -- zwp_tablet_v2 --
  // Shows an arrow for a tool entering range: the compositor leaves a tablet
  // tool's cursor to the client.
  void setToolCursor(TabletTool &t) {
    if (!cursorTheme_ || !compositor_)
      return;
    if (!toolCursorSurface_)
      toolCursorSurface_ = wl_compositor_create_surface(compositor_);
    wl_cursor *c = wl_cursor_theme_get_cursor(cursorTheme_, "left_ptr");
    if (!c)
      c = wl_cursor_theme_get_cursor(cursorTheme_, "default");
    if (!c || c->image_count == 0 || !toolCursorSurface_) {
      zwp_tablet_tool_v2_set_cursor(t.tool, t.proxSerial, nullptr, 0, 0);
      return;
    }
    wl_cursor_image *image = c->images[0];
    wl_buffer *buf = wl_cursor_image_get_buffer(image);
    if (!buf)
      return;
    wl_surface_attach(toolCursorSurface_, buf, 0, 0);
    wl_surface_damage_buffer(toolCursorSurface_, 0, 0, image->width,
                             image->height);
    wl_surface_commit(toolCursorSurface_);
    zwp_tablet_tool_v2_set_cursor(t.tool, t.proxSerial, toolCursorSurface_,
                                  image->hotspot_x, image->hotspot_y);
  }

  void processToolFrame(TabletTool &t, uint64_t ms) {
    if (t.proxIn)
      setToolCursor(t);
    t.lastMs = ms;
    if (t.dirty && t.havePosition)
      t.pending.push_back({t.x, t.y, t.pressure, t.tiltX, t.tiltY, t.twist, ms});

    uint8_t desired = 0;
    if (t.inProximity && !t.proxOut) {
      if (t.contact)
        desired |= buttonBit(MouseButton::Left); // tip (or eraser end)
      if (t.stylus1)
        desired |= buttonBit(MouseButton::Right); // barrel button
      if (t.stylus2)
        desired |= buttonBit(MouseButton::Middle);
    }
    const bool buttonsChanged = desired != t.desired;
    t.desired = desired;
    const bool leaving = t.proxOut;
    t.dirty = t.proxIn = t.proxOut = false;
    if (leaving)
      t.inProximity = false;

    if (!buttonsChanged && !leaving) { // motion only: park it
      if (!t.pending.empty())
        penTouchPending_ = true;
      return;
    }
    bool changed = flushPenTouchPending(); // older motion first, in order
    changed |= flushTool(t);               // then this frame's transitions
    if (leaving)
      changed |= penLeftRange(t.id);
    if (changed && eglReady_)
      redraw();
  }

  static TabletTool *toolOf(void *data) { return static_cast<TabletTool *>(data); }
  static void toolType(void *data, zwp_tablet_tool_v2 *, uint32_t type) {
    toolOf(data)->eraser = type == ZWP_TABLET_TOOL_V2_TYPE_ERASER;
  }
  static void toolIgnore2(void *, zwp_tablet_tool_v2 *, uint32_t, uint32_t) {}
  static void toolIgnore1(void *, zwp_tablet_tool_v2 *, uint32_t) {}
  static void toolIgnoreI(void *, zwp_tablet_tool_v2 *, int32_t) {}
  static void toolIgnoreNone(void *, zwp_tablet_tool_v2 *) {}
  static void toolIgnoreWheel(void *, zwp_tablet_tool_v2 *, wl_fixed_t,
                              int32_t) {}
  static void toolRemoved(void *data, zwp_tablet_tool_v2 *tool) {
    TabletTool *t = toolOf(data);
    LiteUIImpl *self = t->owner;
    const bool changed = self->penLeftRange(t->id);
    zwp_tablet_tool_v2_destroy(tool);
    std::erase_if(self->tabletTools_,
                  [t](const auto &p) { return p.get() == t; });
    if (changed && self->eglReady_)
      self->redraw();
  }
  static void toolProximityIn(void *data, zwp_tablet_tool_v2 *, uint32_t serial,
                              zwp_tablet_v2 *, wl_surface *) {
    TabletTool *t = toolOf(data);
    t->proxIn = true;
    t->inProximity = true;
    t->proxSerial = serial;
  }
  static void toolProximityOut(void *data, zwp_tablet_tool_v2 *) {
    toolOf(data)->proxOut = true;
  }
  static void toolDown(void *data, zwp_tablet_tool_v2 *, uint32_t) {
    toolOf(data)->contact = true;
  }
  static void toolUp(void *data, zwp_tablet_tool_v2 *) {
    toolOf(data)->contact = false;
  }
  static void toolMotion(void *data, zwp_tablet_tool_v2 *, wl_fixed_t x,
                         wl_fixed_t y) {
    TabletTool *t = toolOf(data);
    t->x = static_cast<float>(wl_fixed_to_double(x));
    t->y = static_cast<float>(wl_fixed_to_double(y));
    t->havePosition = t->dirty = true;
  }
  static void toolPressure(void *data, zwp_tablet_tool_v2 *, uint32_t p) {
    TabletTool *t = toolOf(data);
    t->pressure = static_cast<float>(p) / 65535.0f; // protocol range 0..65535
    t->dirty = true;
  }
  static void toolTilt(void *data, zwp_tablet_tool_v2 *, wl_fixed_t tx,
                       wl_fixed_t ty) {
    TabletTool *t = toolOf(data);
    t->tiltX = static_cast<float>(wl_fixed_to_double(tx)); // degrees, -90..90
    t->tiltY = static_cast<float>(wl_fixed_to_double(ty));
    t->dirty = true;
  }
  static void toolRotation(void *data, zwp_tablet_tool_v2 *, wl_fixed_t deg) {
    TabletTool *t = toolOf(data);
    t->twist = static_cast<float>(wl_fixed_to_double(deg));
    t->dirty = true;
  }
  static void toolButton(void *data, zwp_tablet_tool_v2 *, uint32_t,
                         uint32_t button, uint32_t state) {
    TabletTool *t = toolOf(data);
    const bool pressed = state == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED;
    if (button == 0x14b) // BTN_STYLUS  (linux/input-event-codes.h)
      t->stylus1 = pressed;
    else if (button == 0x14c) // BTN_STYLUS2
      t->stylus2 = pressed;
  }
  static void toolFrame(void *data, zwp_tablet_tool_v2 *, uint32_t time) {
    TabletTool *t = toolOf(data);
    t->owner->processToolFrame(*t, widenMs32(time));
  }
  static constexpr zwp_tablet_tool_v2_listener toolListener = {
      toolType,       toolIgnore2,    toolIgnore2,    toolIgnore1,
      toolIgnoreNone, toolRemoved,    toolProximityIn, toolProximityOut,
      toolDown,       toolUp,         toolMotion,     toolPressure,
      toolIgnore1 /*distance*/,       toolTilt,       toolRotation,
      toolIgnoreI /*slider*/,         toolIgnoreWheel, toolButton,
      toolFrame};

  static void tabletSeatTabletAdded(void *, zwp_tablet_seat_v2 *,
                                    zwp_tablet_v2 *) {}
  static void tabletSeatToolAdded(void *data, zwp_tablet_seat_v2 *,
                                  zwp_tablet_tool_v2 *tool) {
    auto *self = static_cast<LiteUIImpl *>(data);
    auto t = std::make_unique<TabletTool>();
    t->owner = self;
    t->tool = tool;
    t->id = self->nextPenId_++;
    zwp_tablet_tool_v2_add_listener(tool, &toolListener, t.get());
    self->tabletTools_.push_back(std::move(t));
  }
  static void tabletSeatPadAdded(void *, zwp_tablet_seat_v2 *,
                                 zwp_tablet_pad_v2 *) {}
  static constexpr zwp_tablet_seat_v2_listener tabletSeatListener = {
      tabletSeatTabletAdded, tabletSeatToolAdded, tabletSeatPadAdded};

  // The tablet protocol hangs off the seat, and the registry may announce
  // the manager and the seat in either order.
  void maybeCreateTabletSeat() {
    if (tabletSeat_ || !tabletManager_ || !seat_)
      return;
    tabletSeat_ = zwp_tablet_manager_v2_get_tablet_seat(tabletManager_, seat_);
    zwp_tablet_seat_v2_add_listener(tabletSeat_, &tabletSeatListener, this);
  }

  // Called when the pointer enters this surface.
  static void pointerEnter(void *data, wl_pointer *, uint32_t serial,
                           wl_surface *, wl_fixed_t sx, wl_fixed_t sy) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUIImpl *>(data);
    // Convert Wayland's fixed-point x coordinate to a double and store it.
    self->pointer_x_ = wl_fixed_to_double(sx);
    // Same for the y coordinate.
    self->pointer_y_ = wl_fixed_to_double(sy);

    // wl_pointer_set_cursor requires the serial of an enter event, and it
    // isn't resent on plain motion, so remember it for later calls.
    self->pointerEnterSerial_ = serial;
    // Force the next setCursor() call to actually apply, since re-entering
    // (e.g. after a resize/move) may land us back on the same cursor name
    // the compositor's default already reset to something else.
    self->currentCursorName_.clear();
    self->pointerInside_ = true;
    self->refreshCursor();
  }
  // Called when the pointer leaves this surface. Reset the cached cursor
  // name so re-entering always re-applies one, rather than skipping the
  // very next setCursor() as a no-op change.
  static void pointerLeave(void *data, wl_pointer *, uint32_t, wl_surface *) {
    static_cast<LiteUIImpl *>(data)->flushPendingMotion();
    static_cast<LiteUIImpl *>(data)->currentCursorName_.clear();
    static_cast<LiteUIImpl *>(data)->pointerInside_ = false;
  }
  // Called on every pointer movement while over this surface.
  // Called on every pointer movement while over this surface.
  static void pointerMotion(void *data, wl_pointer *, uint32_t, wl_fixed_t sx,
                            wl_fixed_t sy) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUIImpl *>(data);
    // Only remember where the pointer is now. A compositor can deliver
    // several motion events per frame (and a high-polling-rate mouse many
    // more); doing the drag/hover/redraw work for each one made a slow
    // onPointerMove handler (e.g. a photo editor's slider recompute) run many
    // times per frame, with vsync-blocked redraws in between. The work is
    // done once per loop iteration by flushPendingMotion() instead, always
    // with the newest position.
    self->pointer_x_ = wl_fixed_to_double(sx);
    self->pointer_y_ = wl_fixed_to_double(sy);
    self->motionPending_ = true;
  }

  // Applies the most recent pointer position recorded by pointerMotion, if
  // any: advances scrollbar drags / content pans and onPointerMove handlers,
  // refreshes hover and the tooltip timer, redraws once if anything
  // changed, and updates the cursor image. Called once per event-loop
  // iteration (see run()), and first thing in any handler that acts on the
  // pointer position (button, axis, leave) so a press/release never sees a
  // drag that is a few motion events behind.
  //
  // MUST NOT be called while a wl_display_prepare_read() is outstanding:
  // redraw() -> eglSwapBuffers can read the display itself, which would
  // wait forever on our own pending read intent.
  void flushPendingMotion() {
    if (!motionPending_)
      return;
    motionPending_ = false;

    const float x = static_cast<float>(pointer_x_);
    const float y = static_cast<float>(pointer_y_);

    // Advance any in-progress scrollbar drag / content pan (see
    // beginScrollPress/handlePress). A no-op, and cheap, when nothing's
    // being dragged.
    bool changed = updateScrollDrag(x, y);
    if (dispatchPointerMove(x, y))
      changed = true;
    bool needPoll = false;
    if (hasRoot_ && updateHoverAt(x, y, &needPoll))
      changed = true;

    // arms/cancels/re-targets the tooltip timer. Handles its own
    // repaint scheduling internally (via requestTooltipRepaint), so it
    // doesn't need to participate in the `changed` flag above.
    updateTooltipHover(x, y);
    if (needPoll) {
      pollAndRelayout(); // a hover handler changed app state
      changed = true;
    }

    if (changed)
      redraw();

    // Re-derive the cursor (window-edge resize handle, else Style::cursor of
    // what's under the pointer) so the user sees it before they even click.
    refreshCursor();
  }
  // Called on scroll/axis events — mouse wheel rotation or a touchpad's
  // continuous scroll gesture. `value` is already a relative-movement
  // amount in the same coordinate space as pointer motion (per the
  // wl_pointer protocol), so it's usable directly as a pixel delta with no
  // extra scaling, unlike Windows' notch-based WM_MOUSEWHEEL.
  static void pointerAxis(void *data, wl_pointer *, uint32_t, uint32_t axis,
                          wl_fixed_t value) {
    auto *self = static_cast<LiteUIImpl *>(data);
    self->flushPendingMotion(); // hit-test against the up-to-date position
    float delta = static_cast<float>(wl_fixed_to_double(value));
    float dx = 0, dy = 0;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
      dy = delta;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
      dx = delta;
    // Wayland's axis value is already positive for scroll-down/right, which
    // is WheelEvent's sign convention. This listener has no axis_source
    // event, so it can't tell a trackpad from a notched wheel: precise stays
    // false.
    bool changed =
        self->dispatchWheel(static_cast<float>(self->pointer_x_),
                            static_cast<float>(self->pointer_y_), dx, dy);
    if (changed)
      self->redraw();
  }
  // Called on every pointer button press/release.
  static void pointerButton(void *data, wl_pointer *, uint32_t serial, uint32_t,
                            uint32_t button, uint32_t state) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUIImpl *>(data);
    // Bring any drag up to the final pointer position first, so a release
    // that arrives in the same batch as the last few motion events ends the
    // drag where the user actually let go.
    self->flushPendingMotion();
    MouseButton btn;
    if (button == BTN_LEFT_CODE)
      btn = MouseButton::Left;
    else if (button == BTN_MIDDLE_CODE)
      btn = MouseButton::Middle;
    else if (button == BTN_RIGHT_CODE)
      btn = MouseButton::Right;
    else
      return; // some other device button we don't handle
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
      // Route the press to the custom titlebar hit-testing logic, passing
      // the event serial (needed for interactive resize/move grabs).
      self->handlePress(serial, btn);
    else if (state == WL_POINTER_BUTTON_STATE_RELEASED)
      self->handleRelease(btn);
  }
  // Listener struct binding all five pointer callbacks above to wl_pointer's
  // events, in the order the interface expects.
  static constexpr wl_pointer_listener pointerListener = {
      pointerEnter, pointerLeave, pointerMotion, pointerButton, pointerAxis};

  static void keyboardKeymap(void *data, wl_keyboard *, uint32_t format,
                             int32_t fd, uint32_t size) {
    auto *self = static_cast<LiteUIImpl *>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
      close(fd);
      return;
    }
    char *map =
        static_cast<char *>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED)
      return;
    if (!self->xkbContext_)
      self->xkbContext_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (self->xkbKeymap_)
      xkb_keymap_unref(self->xkbKeymap_);
    self->xkbKeymap_ = xkb_keymap_new_from_string(self->xkbContext_, map,
                                                  XKB_KEYMAP_FORMAT_TEXT_V1,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (self->xkbState_)
      xkb_state_unref(self->xkbState_);
    self->xkbState_ =
        self->xkbKeymap_ ? xkb_state_new(self->xkbKeymap_) : nullptr;
  }

  static void keyboardEnter(void *, wl_keyboard *, uint32_t, wl_surface *,
                            wl_array *) {}

  // Keyboard focus left our surface entirely (switched to another window)
  // — blur, and stop any in-flight repeat, same reasoning as WM_KILLFOCUS.
  static void keyboardLeave(void *data, wl_keyboard *, uint32_t, wl_surface *) {
    auto *self = static_cast<LiteUIImpl *>(data);
    self->setFocus(nullptr);
    self->repeatKeycode_ = 0;
    self->hideTooltip();
  }

  static void keyboardKey(void *data, wl_keyboard *, uint32_t, uint32_t,
                          uint32_t key, uint32_t state) {
    auto *self = static_cast<LiteUIImpl *>(data);
    if (!self->xkbState_)
      return;
    // Wayland keycodes are evdev-based, offset by 8 from the X11/xkb
    // numbering xkbcommon expects.
    xkb_keycode_t xkbCode = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(self->xkbState_, xkbCode);
    KeyEvent e{xkbKeysymToKey(sym), self->modState_};
    bool changed = false;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      changed = self->dispatchKeyDown(e);
      uint32_t cp = xkb_state_key_get_utf32(self->xkbState_, xkbCode);
      if ((cp >= 0x20 || cp == '\t') && !self->modState_.alt)
        changed |= self->dispatchTextInput(cp);
      // Arm repeat for this key if the compositor wants repeat and this
      // key actually produces repeatable input (xkb flags this).
      if (self->repeatRateHz_ > 0 &&
          xkb_keymap_key_repeats(self->xkbKeymap_, xkbCode)) {
        self->repeatKeycode_ = key;
        self->armRepeatTimer(self->repeatDelayMs_);
      }
    } else {
      changed = self->dispatchKeyUp(e);
      if (self->repeatKeycode_ == key)
        self->repeatKeycode_ = 0;
    }
    if (changed)
      self->redraw();
  }

  static void keyboardModifiers(void *data, wl_keyboard *, uint32_t,
                                uint32_t modsDepressed, uint32_t modsLatched,
                                uint32_t modsLocked, uint32_t group) {
    auto *self = static_cast<LiteUIImpl *>(data);
    if (!self->xkbState_)
      return;
    xkb_state_update_mask(self->xkbState_, modsDepressed, modsLatched,
                          modsLocked, 0, 0, group);
    KeyModifiers m;
    m.shift = xkb_state_mod_name_is_active(self->xkbState_, XKB_MOD_NAME_SHIFT,
                                           XKB_STATE_MODS_EFFECTIVE) > 0;
    m.ctrl = xkb_state_mod_name_is_active(self->xkbState_, XKB_MOD_NAME_CTRL,
                                          XKB_STATE_MODS_EFFECTIVE) > 0;
    m.alt = xkb_state_mod_name_is_active(self->xkbState_, XKB_MOD_NAME_ALT,
                                         XKB_STATE_MODS_EFFECTIVE) > 0;
    m.super = xkb_state_mod_name_is_active(self->xkbState_, XKB_MOD_NAME_LOGO,
                                           XKB_STATE_MODS_EFFECTIVE) > 0;
    self->modState_ = m;
  }

  static void keyboardRepeatInfo(void *data, wl_keyboard *, int32_t rate,
                                 int32_t delay) {
    auto *self = static_cast<LiteUIImpl *>(data);
    self->repeatRateHz_ = rate;
    self->repeatDelayMs_ = delay;
  }

  static constexpr wl_keyboard_listener keyboardListener = {
      keyboardKeymap, keyboardEnter,     keyboardLeave,
      keyboardKey,    keyboardModifiers, keyboardRepeatInfo};

  void armRepeatTimer(int delayMs) {
    if (repeatTimerFd_ < 0)
      repeatTimerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (repeatTimerFd_ < 0)
      return;
    itimerspec spec{};
    spec.it_value.tv_sec = delayMs / 1000;
    spec.it_value.tv_nsec = (delayMs % 1000) * 1000000L;
    if (repeatRateHz_ > 0) {
      spec.it_interval.tv_sec = 0;
      spec.it_interval.tv_nsec = 1000000000L / repeatRateHz_;
    }
    timerfd_settime(repeatTimerFd_, 0, &spec, nullptr);
  }

  void fireRepeatIfDue() {
    if (repeatTimerFd_ < 0 || repeatKeycode_ == 0 || !xkbState_)
      return;
    uint64_t expirations;
    if (read(repeatTimerFd_, &expirations, sizeof(expirations)) <= 0)
      return; // not due yet (EAGAIN) or error
    xkb_keycode_t xkbCode = repeatKeycode_ + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkbState_, xkbCode);
    KeyEvent e{xkbKeysymToKey(sym), modState_};
    bool changed = dispatchKeyDown(e);
    uint32_t cp = xkb_state_key_get_utf32(xkbState_, xkbCode);
    if ((cp >= 0x20 || cp == '\t') && !modState_.alt)
      changed |= dispatchTextInput(cp);
    if (changed)
      redraw();
  }

  // Called once per global object the compositor advertises via the registry.
  static void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                             const char *interface, uint32_t) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUIImpl *>(data);
    // If this global is the compositor interface...
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
      // ...bind to it at version 4 and store the resulting proxy.
      self->compositor_ = static_cast<wl_compositor *>(
          wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    }
    // If instead this global is the xdg_wm_base (window-shell) interface...
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
      // ...bind to it at version 1...
      self->wm_base_ = static_cast<xdg_wm_base *>(
          wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      // ...and register the ping/pong listener on it immediately.
      xdg_wm_base_add_listener(self->wm_base_, &wmBaseListener, self);
    }
    // If instead this global is the shared-memory interface...
    else if (strcmp(interface, wl_shm_interface.name) == 0) {
      // ...bind to it so we can later allocate pixel buffers.
      self->shm_ = static_cast<wl_shm *>(
          wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
    // If instead this global is the seat (input devices) interface...
    else if (strcmp(interface, wl_seat_interface.name) == 0) {
      // ...bind to it...
      self->seat_ = static_cast<wl_seat *>(
          wl_registry_bind(registry, name, &wl_seat_interface, 1));
      // ...and register the capabilities/name listener so we learn about the
      // pointer.
      wl_seat_add_listener(self->seat_, &seatListener, self);
      self->maybeCreateTabletSeat();
    }
    // If instead this global is the tablet manager (pens)...
    else if (strcmp(interface, zwp_tablet_manager_v2_interface.name) == 0) {
      self->tabletManager_ = static_cast<zwp_tablet_manager_v2 *>(
          wl_registry_bind(registry, name, &zwp_tablet_manager_v2_interface, 1));
      self->maybeCreateTabletSeat();
    }
    // If instead this global is the decoration-manager interface...
    else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) ==
             0) {
      // ...bind to it so we can later request client-side (or server-side)
      // decorations.
      self->decoration_manager_ =
          static_cast<zxdg_decoration_manager_v1 *>(wl_registry_bind(
              registry, name, &zxdg_decoration_manager_v1_interface, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
      self->seat_ = static_cast<wl_seat *>(
          wl_registry_bind(registry, name, &wl_seat_interface,
                           4)); // bump to 4 for keyboard repeat_info
      wl_seat_add_listener(self->seat_, &seatListener, self);
    }
  }
  // Called when a global is removed; this minimal window doesn't need to react.
  static void registryRemove(void *, wl_registry *, uint32_t) {}
  // Listener struct binding the two registry callbacks above to wl_registry's
  // events.
  static constexpr wl_registry_listener registryListener = {registryGlobal,
                                                            registryRemove};

  // How close (in pixels) the pointer must be to the window's outer edge
  // before we treat it as a resize grab instead of ordinary titlebar
  // content. Kept well inside the button icons (which start ~7px into a
  // 32px titlebar) so it never steals clicks from minimize/maximize/close.
  static constexpr int kResizeMargin = 6;

  // Maps a pointer position to which edge (if any) an interactive resize
  // should grab, mirroring how most CSD toolkits treat a thin strip along
  // each window edge as a resize handle rather than ordinary content.
  uint32_t resizeEdgeAt(double px, double py) const {
    bool left = px < kResizeMargin;
    bool right = px >= width_ - kResizeMargin;
    bool top = py < kResizeMargin;
    bool bottom = py >= height_ - kResizeMargin;
    if (top && left)
      return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    if (top && right)
      return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    if (bottom && left)
      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    if (bottom && right)
      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    if (left)
      return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    if (right)
      return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    if (top)
      return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    if (bottom)
      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
  }

  // Standard XCursor names for each edge/corner; "left_ptr" is the ordinary
  // arrow shown everywhere else.
  static const char *cursorNameForEdge(uint32_t edge) {
    switch (edge) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
      return "top_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
      return "bottom_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
      return "left_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
      return "right_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
      return "top_left_corner";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
      return "top_right_corner";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
      return "bottom_left_corner";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT:
      return "bottom_right_corner";
    default:
      return "left_ptr";
    }
  }

  // XCursor names for a Style::cursor value: the CSS-style name most
  // current themes ship, then the classic X11 name older themes use.
  struct CursorNames {
    const char *primary;
    const char *alt;
  };
  static CursorNames xcursorNames(Cursor c) {
    switch (c) {
    case Cursor::None:
      return {"none", nullptr}; // special-cased in setCursor: hides it
    case Cursor::Pointer:
      return {"pointer", "hand2"};
    case Cursor::Text:
      return {"text", "xterm"};
    case Cursor::Crosshair:
      return {"crosshair", "cross"};
    case Cursor::Move:
      return {"move", "fleur"};
    case Cursor::Grab:
      return {"grab", "openhand"};
    case Cursor::Grabbing:
      return {"grabbing", "closedhand"};
    case Cursor::NotAllowed:
      return {"not-allowed", "crossed_circle"};
    case Cursor::Wait:
      return {"wait", "watch"};
    case Cursor::Progress:
      return {"progress", "left_ptr_watch"};
    case Cursor::Help:
      return {"help", "question_arrow"};
    case Cursor::ResizeEW:
      return {"ew-resize", "sb_h_double_arrow"};
    case Cursor::ResizeNS:
      return {"ns-resize", "sb_v_double_arrow"};
    case Cursor::ResizeNESW:
      return {"nesw-resize", "fd_double_arrow"};
    case Cursor::ResizeNWSE:
      return {"nwse-resize", "bd_double_arrow"};
    case Cursor::ZoomIn:
      return {"zoom-in", "zoom_in"};
    case Cursor::ZoomOut:
      return {"zoom-out", "zoom_out"};
    case Cursor::Inherit:
    case Cursor::Default:
    default:
      return {"left_ptr", "default"};
    }
  }

  // Shows the cursor for wherever the pointer is: a window-edge resize
  // handle wins, otherwise the Style::cursor of what's under it (or of the
  // view holding capture). Cheap enough to call often: setCursor() does
  // nothing when the cursor is already showing.
  void refreshCursor() {
    if (!cursorTheme_ || !pointer_)
      return;
    uint32_t edge = resizeEdgeAt(pointer_x_, pointer_y_);
    if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
      setCursor(cursorNameForEdge(edge));
      return;
    }
    CursorNames n = xcursorNames(ptrRouter_.cursorAt(
        static_cast<float>(pointer_x_), static_cast<float>(pointer_y_)));
    setCursor(n.primary, n.alt);
  }

  // Swaps the pointer's visible cursor to the named XCursor image (trying
  // `alt` if the theme lacks `name`), skipping the work entirely if it's
  // already showing (motion events fire far more often than the cursor
  // actually needs to change). "none" hides the cursor.
  void setCursor(const char *name, const char *alt = nullptr) {
    if (!cursorTheme_ || !pointer_ || currentCursorName_ == name)
      return;
    if (std::strcmp(name, "none") == 0) {
      currentCursorName_ = name;
      wl_pointer_set_cursor(pointer_, pointerEnterSerial_, nullptr, 0, 0);
      return;
    }
    wl_cursor *cursor = wl_cursor_theme_get_cursor(cursorTheme_, name);
    if (!cursor && alt)
      cursor = wl_cursor_theme_get_cursor(cursorTheme_, alt);
    if (!cursor)
      cursor = wl_cursor_theme_get_cursor(cursorTheme_, "left_ptr");
    if (!cursor)
      cursor = wl_cursor_theme_get_cursor(cursorTheme_, "default");
    if (!cursor || cursor->image_count == 0)
      return;
    wl_cursor_image *image = cursor->images[0];
    wl_buffer *cbuf = wl_cursor_image_get_buffer(image);
    if (!cbuf)
      return;
    currentCursorName_ = name;
    wl_surface_attach(cursorSurface_, cbuf, 0, 0);
    wl_surface_damage_buffer(cursorSurface_, 0, 0, image->width, image->height);
    wl_surface_commit(cursorSurface_);
    wl_pointer_set_cursor(pointer_, pointerEnterSerial_, cursorSurface_,
                          image->hotspot_x, image->hotspot_y);
  }

  // Handles a compositor-driven resize: tears down the old shm buffers,
  // updates width_/height_, re-runs layout against the new size, and
  // allocates+attaches fresh buffers at the new dimensions. Buffers can't
  // be resized in place — wl_shm buffers are fixed-size — so this is a full
  // destroy/recreate rather than a realloc.
  void resize(int newWidth, int newHeight) {
    width_ = newWidth;
    height_ = newHeight;
    if (eglWindow_)
      wl_egl_window_resize(eglWindow_, width_, height_, 0, 0);
    relayout();

    pollAndRelayout();
    hideTooltip();
    if (eglReady_)
      redraw();
  }

  // Creates the EGL context/surface for this window's wl_surface and
  // compiles the two shader programs. Called once, from surfaceConfigure(),
  // the first time the compositor hands us a configure event — mirroring
  // when attachBuffer() used to run.
  // ---- GL context lifecycle ----------------------------------------------
  // Creates the OpenGL ES 3.0 context for eglConfig_.
  void createEglContext() {
    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    eglContext_ =
        eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, ctxAttribs);
    if (eglContext_ == EGL_NO_CONTEXT)
      throw std::runtime_error(
          "eglCreateContext failed: liteui needs an OpenGL ES 3.0 capable "
          "EGL driver");
  }

  // Binds the context to the window surface and checks what we really got.
  void makeContextCurrent() {
    if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_))
      throw std::runtime_error("eglMakeCurrent failed");
    // Belt and braces: some drivers hand back a lower-version context than
    // asked for. Fail here with a clear message rather than later with a
    // missing-entry-point crash inside the Canvas renderer.
    GLint glMajor = 0, glMinor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
    glGetIntegerv(GL_MINOR_VERSION, &glMinor);
    if (glMajor < 3) {
      const char *ver = reinterpret_cast<const char *>(glGetString(GL_VERSION));
      fprintf(stderr, "liteui: got GL \"%s\", need OpenGL ES 3.0+\n",
              ver ? ver : "?");
      throw std::runtime_error("OpenGL ES 3.0 context unavailable");
    }
    eglSwapInterval(eglDisplay_,
                    1); // vsync — avoids tearing during scroll drags
  }

  // Every GL object the window renderer owns: programs, shared buffers and
  // fixed pipeline state. Run once at startup and again after the context
  // is recreated.
  void createGlResources() {
    rectProgram_ = linkProgram(kRectVS, kRectFS);
    rectAPos_ = glGetAttribLocation(rectProgram_, "a_pos");
    rectUPos_ = glGetUniformLocation(rectProgram_, "u_rectPos");
    rectUSize_ = glGetUniformLocation(rectProgram_, "u_rectSize");
    rectUScreen_ = glGetUniformLocation(rectProgram_, "u_screen");
    rectURadius_ = glGetUniformLocation(rectProgram_, "u_radius");
    rectUColor_ = glGetUniformLocation(rectProgram_, "u_color");

    flatProgram_ = linkProgram(kFlatVS, kFlatFS);
    flatAPos_ = glGetAttribLocation(flatProgram_, "a_pos");
    flatUScreen_ = glGetUniformLocation(flatProgram_, "u_screen");
    flatUColor_ = glGetUniformLocation(flatProgram_, "u_color");

    texProgram_ = linkProgram(kTexVS, kTexFS);
    texAPos_ = glGetAttribLocation(texProgram_, "a_pos");
    texUPos_ = glGetUniformLocation(texProgram_, "u_pos");
    texUSize_ = glGetUniformLocation(texProgram_, "u_size");
    texUScreen_ = glGetUniformLocation(texProgram_, "u_screen");
    texUTex_ = glGetUniformLocation(texProgram_, "u_tex");
    texUColor_ = glGetUniformLocation(texProgram_, "u_color");

#if defined(LITEUI_CANVAS_GL)
    canvasProgram_ = linkProgram(kTexVS, kCanvasRgbaFS);
#else
    canvasProgram_ = linkProgram(kTexVS, kCanvasFS);
#endif
    canvasAPos_ = glGetAttribLocation(canvasProgram_, "a_pos");
    canvasUPos_ = glGetUniformLocation(canvasProgram_, "u_pos");
    canvasUSize_ = glGetUniformLocation(canvasProgram_, "u_size");
    canvasUScreen_ = glGetUniformLocation(canvasProgram_, "u_screen");
    canvasUTex_ = glGetUniformLocation(canvasProgram_, "u_tex");

    static const float kUnitQuad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kUnitQuad), kUnitQuad, GL_STATIC_DRAW);
    glGenBuffers(1, &flatVbo_); // contents supplied per-draw in drawLine()

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST); // ClipRect maps straight onto glScissor
#if defined(LITEUI_CANVAS_GL)
    if (!canvasGl_.init())
      throw std::runtime_error("liteui: GL canvas renderer failed to start");
#endif
  }

  // Forgets (does NOT delete) every GL name held by the view tree. Used when
  // the context is gone: all names died with it, and deleting a stale name
  // inside the replacement context could destroy an unrelated new object
  // that happens to reuse the number. Everything is rebuilt lazily: text
  // and canvas textures are re-created because their handle is now 0.
  static void forgetGlHandles(View &v) {
    Computed_forget(v.computed);
    for (auto &c : v.children)
      forgetGlHandles(c);
  }
  static void Computed_forget(const View::Computed &c) {
    c.textTexture = 0;
    c.canvasTexture = 0;
    c.canvasFbo = 0;
    c.canvasMsaaFbo = 0;
    c.canvasMsaaColorRb = 0;
    c.canvasDsRb = 0;
    c.cachedImageTexture = 0;
    c.cachedImagePixels = nullptr; // the cached upload is gone
    c.canvasNeedsRedraw = true;    // contents were GPU-only: repaint them
    c.canvasBuiltForWidth = c.canvasBuiltForHeight = -1.0f;
  }

  // Called when eglSwapBuffers reports EGL_CONTEXT_LOST (GPU reset, driver
  // restart, suspend/resume on some stacks). The window surface survives; the
  // context and everything created in it does not. Builds a fresh context,
  // recreates the window's GL objects, and repaints. Gives up after three
  // consecutive losses so a dead GPU can't spin forever; the window then
  // simply stops updating.
  void recoverFromContextLoss() {
    if (recoveringContext_)
      return;
    if (++contextLossStreak_ > 3) {
      fprintf(stderr, "liteui: GL context keeps getting lost; giving up\n");
      eglReady_ = false;
      return;
    }
    recoveringContext_ = true;
    fprintf(stderr, "liteui: GL context lost, recreating it\n");
    eglReady_ = false;
    eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (eglContext_ != EGL_NO_CONTEXT) {
      eglDestroyContext(eglDisplay_, eglContext_);
      eglContext_ = EGL_NO_CONTEXT;
    }
    if (hasRoot_)
      forgetGlHandles(root_);
    rectProgram_ = flatProgram_ = texProgram_ = canvasProgram_ = 0;
    quadVbo_ = flatVbo_ = 0;
    tooltipTextTexture_ = 0;
    tooltipTextureBuiltFor_.clear();
#if defined(LITEUI_CANVAS_GL)
    canvasGl_.abandon();
#endif
    try {
      createEglContext();
      makeContextCurrent();
      createGlResources();
      eglReady_ = true;
    } catch (const std::exception &e) {
      fprintf(stderr, "liteui: could not recover from GL context loss: %s\n",
              e.what());
    }
    recoveringContext_ = false;
    if (eglReady_)
      redraw(); // repaint immediately with the new context
  }

  void initEgl() {
    // eglGetDisplay() can't reliably tell "this pointer is a wl_display*"
    // apart from other native display types on multi-platform Mesa
    // builds — on some setups it silently falls back to the generic
    // "device" platform, which tries to open a DRM fd (fails with -1)
    // and then falls further back to a broken software/zink path. Asking
    // for EGL_PLATFORM_WAYLAND_KHR explicitly removes the guesswork.
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (eglGetPlatformDisplayEXT) {
      eglDisplay_ =
          eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, display_, nullptr);
    } else {
      eglDisplay_ =
          eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
    }
    if (eglDisplay_ == EGL_NO_DISPLAY)
      throw std::runtime_error("eglGetDisplay failed");
    EGLint major, minor;
    if (!eglInitialize(eglDisplay_, &major, &minor))
      throw std::runtime_error("eglInitialize failed");
    eglBindAPI(EGL_OPENGL_ES_API);

    // The Canvas backend needs OpenGL ES 3.0 (VAOs, multisampled
    // renderbuffers, glBlitFramebuffer, PBO readback, R8 textures). EGL 1.5
    // names the bit EGL_OPENGL_ES3_BIT; older headers only have the KHR
    // spelling, which has the same value.
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

    const EGLint cfgAttribs[] = {EGL_SURFACE_TYPE,
                                 EGL_WINDOW_BIT,
                                 EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES3_BIT,
                                 EGL_RED_SIZE,
                                 8,
                                 EGL_GREEN_SIZE,
                                 8,
                                 EGL_BLUE_SIZE,
                                 8,
                                 EGL_ALPHA_SIZE,
                                 0,
                                 EGL_NONE};
    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay_, cfgAttribs, &eglConfig_, 1,
                         &numConfigs) ||
        numConfigs < 1)
      throw std::runtime_error("eglChooseConfig failed");

    createEglContext();

    eglWindow_ = wl_egl_window_create(surface_, width_, height_);
    if (!eglWindow_)
      throw std::runtime_error("wl_egl_window_create failed");

    eglSurface_ = eglCreateWindowSurface(
        eglDisplay_, eglConfig_,
        reinterpret_cast<EGLNativeWindowType>(eglWindow_), nullptr);
    if (eglSurface_ == EGL_NO_SURFACE)
      throw std::runtime_error("eglCreateWindowSurface failed");

    makeContextCurrent();

    // Tell the compositor this whole surface is opaque, so it can skip
    // blending against the desktop entirely rather than relying solely on
    // the alpha-less EGL config above. INT32_MAX as the region size is the
    // standard "cover the whole surface regardless of actual dimensions"
    // idiom — Wayland clips it to the surface's real bounds internally.
    wl_region *opaque = wl_compositor_create_region(compositor_);
    wl_region_add(opaque, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region(surface_, opaque);
    wl_region_destroy(opaque);

    createGlResources();

    eglReady_ = true;
    redraw();
  }

  // Restricts subsequent draws to `clip`, intersected with the window
  // bounds. GL's scissor origin is bottom-left, so the y range is flipped
  // relative to ClipRect's top-left/y-down convention.
  void applyScissor(const ClipRect &clip) {
    float x0 = std::max(clip.x0, 0.0f), y0 = std::max(clip.y0, 0.0f);
    float x1 = std::min(clip.x1, static_cast<float>(width_));
    float y1 = std::min(clip.y1, static_cast<float>(height_));
    if (x1 < x0)
      x1 = x0;
    if (y1 < y0)
      y1 = y0;
    glScissor(static_cast<int>(x0), static_cast<int>(height_ - y1),
              static_cast<int>(x1 - x0), static_cast<int>(y1 - y0));
  }

  // Draws one filled, optionally rounded-corner rectangle. radius <= 0
  // renders a plain rect (same shader — the SDF collapses correctly).
  void drawRectGL(float x, float y, float w, float h, float radius, Color c,
                  const ClipRect &clip) {
    if (w <= 0 || h <= 0)
      return;
    applyScissor(clip);
    glUseProgram(rectProgram_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(rectAPos_);
    glVertexAttribPointer(rectAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glUniform2f(rectUPos_, x, y);
    glUniform2f(rectUSize_, w, h);
    glUniform2f(rectUScreen_, static_cast<float>(width_),
                static_cast<float>(height_));
    glUniform1f(rectURadius_, std::max(0.0f, radius));
    glUniform4f(rectUColor_, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
                c.a / 255.0f);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // Thick line as an oriented quad — needed (rather than a bbox rect) since
  // drawTitlebar uses this for the X icon's two diagonal strokes.
  void drawLineGL(float x0, float y0, float x1, float y1, Color c,
                  float thickness = 2.0f) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    float nx = len > 0.0001f ? -dy / len : 1.0f;
    float ny = len > 0.0001f ? dx / len : 0.0f;
    float hw = thickness / 2.0f;
    const float verts[8] = {x0 + nx * hw, y0 + ny * hw, x0 - nx * hw,
                            y0 - ny * hw, x1 + nx * hw, y1 + ny * hw,
                            x1 - nx * hw, y1 - ny * hw};
    applyScissor(ClipRect{}); // titlebar icons are never clipped
    glUseProgram(flatProgram_);
    glBindBuffer(GL_ARRAY_BUFFER, flatVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(flatAPos_);
    glVertexAttribPointer(flatAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glUniform2f(flatUScreen_, static_cast<float>(width_),
                static_cast<float>(height_));
    glUniform4f(flatUColor_, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // Fills an axis-aligned rectangle with a solid color by calling setPixel for
  // every point inside it. Same unclipped/clipped overload split as setPixel.
  void fillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    fillRect(x0, y0, w, h, r, g, b, ClipRect{});
  }
  void fillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b,
                const ClipRect &clip) {
    drawRectGL(static_cast<float>(x0), static_cast<float>(y0),
               static_cast<float>(w), static_cast<float>(h), 0.0f, {r, g, b},
               clip);
  }

  // Fills an axis-aligned rectangle with rounded corners, clamping the
  // radius so it can't exceed half the shorter side. Per-pixel distance
  // check against each corner's circle center — fine at this scale, not
  // meant for huge boxes. Same unclipped/clipped overload split as setPixel.
  void fillRoundedRect(int x0, int y0, int w, int h, int radius, uint8_t r,
                       uint8_t g, uint8_t b) {
    fillRoundedRect(x0, y0, w, h, radius, r, g, b, ClipRect{});
  }
  void fillRoundedRect(int x0, int y0, int w, int h, int radius, uint8_t r,
                       uint8_t g, uint8_t b, const ClipRect &clip) {
    radius = std::max(0, std::min({radius, w / 2, h / 2}));
    drawRectGL(static_cast<float>(x0), static_cast<float>(y0),
               static_cast<float>(w), static_cast<float>(h),
               static_cast<float>(radius), {r, g, b}, clip);
  }

  GLuint tooltipTextTexture_ = 0;
  int tooltipTexW_ = 0, tooltipTexH_ = 0;
  std::string tooltipTextureBuiltFor_;

  void ensureTooltipTexture(const std::string &text) {
    if (tooltipTextTexture_ && tooltipTextureBuiltFor_ == text)
      return;
    if (tooltipTextTexture_) {
      glDeleteTextures(1, &tooltipTextTexture_);
      tooltipTextTexture_ = 0;
    }
    TextStyle ts;
    ts.fontSize = tooltipStyle_.fontSize;
    ts.fontFamily = tooltipStyle_.fontFamily;
    ts.wrap = TextWrap::NoWrap;
    liteui_text::Measurement m = liteui_text::measure(
        text, ts, resolveStatic(ts.fontSize), resolveStatic(ts.fontWeight),
        resolveStatic(ts.fontStyle), -1);
    int w = std::max(1, (int)std::ceil(m.width)),
        h = std::max(1, (int)std::ceil(m.height));
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    PangoLayout *layout = liteui_text::makeLayout(
        text, ts, resolveStatic(ts.fontSize), resolveStatic(ts.fontWeight),
        resolveStatic(ts.fontStyle), -1, cr);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    cairo_surface_flush(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    std::vector<unsigned char> alpha((size_t)w * h);
    for (int row = 0; row < h; ++row)
      for (int col = 0; col < w; ++col)
        alpha[row * w + col] = data[row * stride + col * 4 + 3];
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA,
                 GL_UNSIGNED_BYTE, alpha.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    tooltipTextTexture_ = tex;
    tooltipTexW_ = w;
    tooltipTexH_ = h;
    tooltipTextureBuiltFor_ = text;
  }

  void paintTooltip() {
    if (!tooltipVisible_ || !tooltipTarget_ || tooltipTarget_->tooltip.empty())
      return;
    ensureTooltipTexture(tooltipTarget_->tooltip);
    float pad = tooltipStyle_.padding;
    float boxW = tooltipTexW_ + pad * 2, boxH = tooltipTexH_ + pad * 2;
    float maxX = std::max(2.0f, (float)width_ - boxW - 2.0f);
    float maxY = std::max(2.0f, (float)height_ - boxH - 2.0f);
    float bx = std::clamp(tooltipPointerX_ + 12.0f, 2.0f, maxX);
    float by = std::clamp(tooltipPointerY_ + 18.0f, 2.0f, maxY);

    drawRectGL(bx, by, boxW, boxH, 4.0f, tooltipStyle_.background, ClipRect{});

    glUseProgram(texProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tooltipTextTexture_);
    glUniform1i(texUTex_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(texAPos_);
    glVertexAttribPointer(texAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glUniform2f(texUPos_, bx + pad, by + pad);
    glUniform2f(texUSize_, (float)tooltipTexW_, (float)tooltipTexH_);
    glUniform2f(texUScreen_, (float)width_, (float)height_);
    Color c = tooltipStyle_.textColor;
    glUniform4f(texUColor_, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
                c.a / 255.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // Rasterizes v.text under v.textStyle into an alpha-only glyph-coverage
  // surface sized to v's final content box (via Pango/Cairo), uploads it
  // as a GL texture, and caches the result until v.computed.w changes —
  // the only thing that can change how the text wraps without the
  // string/style themselves changing.
  //
  // Uploads as a single-channel GL_ALPHA texture (see below) rather than
  // cairo's native ARGB32 buffer — GL_ALPHA is core GLES2, unlike the
  // BGRA extension and non-tightly-packed row uploads that ARGB32 would
  // otherwise require.
  static void ensureTextTexture(const View &v) {
    const std::string &text = resolveDynamic(v.text, v.computed.resolvedText);
    float fontSize =
        resolveDynamic(v.textStyle.fontSize, v.computed.resolvedFontSize);
    FontWeight fontWeight =
        resolveDynamic(v.textStyle.fontWeight, v.computed.resolvedFontWeight);
    FontStyle fontStyleVal =
        resolveDynamic(v.textStyle.fontStyle, v.computed.resolvedFontStyle);
    if (v.computed.textTexture &&
        v.computed.textTextureBuiltForWidth == v.computed.w &&
        v.computed.textTextureBuiltForText == text &&
        v.computed.textTextureBuiltForFontSize == fontSize &&
        v.computed.textTextureBuiltForFontWeight == fontWeight &&
        v.computed.textTextureBuiltForFontStyle == fontStyleVal)
      return;
    if (v.computed.textTexture) {
      glDeleteTextures(1, &v.computed.textTexture);
      v.computed.textTexture = 0;
    }
    const EdgeInsets &pad =
        resolveDynamic(v.style.padding, v.computed.resolvedPadding);
    int innerW =
        std::max(1, static_cast<int>(v.computed.w - pad.left - pad.right));
    int innerH =
        std::max(1, static_cast<int>(v.computed.h - pad.top - pad.bottom));

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, innerW, innerH);
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(
        cr, 1, 1, 1,
        1); // color is applied later via u_color; only alpha matters
    PangoLayout *layout =
        liteui_text::makeLayout(text, v.textStyle, fontSize, fontWeight,
                                fontStyleVal, static_cast<float>(innerW), cr);
    pango_cairo_update_layout(cr, layout);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    cairo_surface_flush(surf);

    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    // GLES2 core has neither GL_UNPACK_ROW_LENGTH (GLES3+/desktop only)
    // nor a guaranteed GL_BGRA_EXT, so uploading cairo's native ARGB32
    // buffer directly isn't portable here. The fragment shader (kTexFS)
    // only ever reads this texture's alpha channel anyway — color comes
    // from u_color — so there's no need to upload color data at all:
    // extract just the alpha byte of each pixel into a tightly packed
    // buffer and upload that as a single-channel GL_ALPHA texture.
    // Cairo's ARGB32 stores alpha in the high byte of each native-endian
    // 32-bit pixel, which on the little-endian platforms this file
    // targets is byte offset 3.
    std::vector<unsigned char> alpha(static_cast<size_t>(innerW) * innerH);
    for (int row = 0; row < innerH; ++row) {
      const unsigned char *src = data + row * stride;
      unsigned char *dst = alpha.data() + row * innerW;
      for (int col = 0; col < innerW; ++col)
        dst[col] = src[col * 4 + 3];
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Our `alpha` buffer is tightly packed at 1 byte/pixel with no row
    // padding, but GL's default GL_UNPACK_ALIGNMENT is 4 — it would
    // otherwise assume each row starts on a 4-byte boundary and skip
    // "padding" bytes that don't actually exist, shearing the image
    // into diagonal noise whenever innerW isn't a multiple of 4 (which
    // is almost always, for arbitrary text widths).
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, innerW, innerH, 0, GL_ALPHA,
                 GL_UNSIGNED_BYTE, alpha.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT,
                  4); // restore GL's default for other uploads

    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    v.computed.textTexture = tex;
    v.computed.textTexW = innerW;
    v.computed.textTexH = innerH;
    v.computed.textTextureBuiltForWidth = v.computed.w;
    v.computed.textTextureBuiltForText = text;
    v.computed.textTextureBuiltForFontSize = fontSize;
    v.computed.textTextureBuiltForFontWeight = fontWeight;
    v.computed.textTextureBuiltForFontStyle = fontStyleVal;
  }

  void drawTextTexture(const View &v, const ClipRect &clip) {
    ensureTextTexture(v);
    if (!v.computed.textTexture)
      return;
    applyScissor(clip);
    glUseProgram(texProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v.computed.textTexture);
    glUniform1i(texUTex_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(texAPos_);
    glVertexAttribPointer(texAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    const EdgeInsets &pad =
        resolveDynamic(v.style.padding, v.computed.resolvedPadding);
    float x = v.computed.x + pad.left;
    float y = v.computed.y + pad.top;
    glUniform2f(texUPos_, x, y);
    glUniform2f(texUSize_, static_cast<float>(v.computed.textTexW),
                static_cast<float>(v.computed.textTexH));
    glUniform2f(texUScreen_, static_cast<float>(width_),
                static_cast<float>(height_));
    const Color &c =
        resolveDynamic(v.textStyle.color, v.computed.resolvedTextColor);
    glUniform4f(texUColor_, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
                c.a / 255.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

#if !defined(LITEUI_CANVAS_GL)
  // (Re)builds v.computed.canvasSurface — a Cairo ARGB32 image surface
  // sized to v's inner (padding-excluded) content box — whenever that
  // size has changed since it was last built, then (re)invokes v.onPaint
  // into it whenever the size changed OR the app requested a redraw via
  // View::requestCanvasRedraw() (canvasNeedsRedraw starts true, so this
  // always happens at least once). Finally (re)uploads the surface's
  // pixels as a GL texture whenever either of those happened — a plain
  // repaint with neither condition true reuses the existing texture
  // untouched.
  void ensureCanvasSurface(const View &v) {
    const EdgeInsets &pad =
        resolveDynamic(v.style.padding, v.computed.resolvedPadding);
    float innerW = std::max(1.0f, v.computed.w - pad.left - pad.right);
    float innerH = std::max(1.0f, v.computed.h - pad.top - pad.bottom);
    int iw = std::max(1, static_cast<int>(innerW));
    int ih = std::max(1, static_cast<int>(innerH));
    bool resized = !v.computed.canvasSurface ||
                   v.computed.canvasBuiltForWidth != innerW ||
                   v.computed.canvasBuiltForHeight != innerH;
    if (resized) {
      if (v.computed.canvasSurface) {
        cairo_surface_destroy(v.computed.canvasSurface);
        v.computed.canvasSurface = nullptr;
      }
      v.computed.canvasSurface =
          cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
      v.computed.canvasBuiltForWidth = innerW;
      v.computed.canvasBuiltForHeight = innerH;
      v.computed.canvasNeedsRedraw = true;
    }
    if (!v.computed.canvasSurface)
      return;

    bool needUpload = resized;
    if (v.computed.canvasNeedsRedraw && v.onPaint) {
      cairo_t *cr = cairo_create(v.computed.canvasSurface);
      cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_rgba(cr, 0, 0, 0, 0);
      cairo_paint(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
      CanvasContextImpl ctx(cr, static_cast<float>(iw), static_cast<float>(ih),
                            &v.computed);
      v.onPaint(ctx);
      cairo_destroy(cr);
      v.computed.canvasNeedsRedraw = false;
      needUpload = true;
    }
    if (!needUpload && v.computed.canvasTexture)
      return;

    cairo_surface_flush(v.computed.canvasSurface);
    unsigned char *data =
        cairo_image_surface_get_data(v.computed.canvasSurface);
    int stride = cairo_image_surface_get_stride(v.computed.canvasSurface);

    // Cairo's ARGB32 stride is always width*4, so the surface can be
    // uploaded directly. Fall back to a repack only if that ever changes.
    std::vector<unsigned char> repacked;
    const unsigned char *upload = data;
    if (stride != iw * 4) {
      repacked.resize(static_cast<size_t>(iw) * ih * 4);
      for (int row = 0; row < ih; ++row)
        std::memcpy(repacked.data() + static_cast<size_t>(row) * iw * 4,
                    data + static_cast<size_t>(row) * stride,
                    static_cast<size_t>(iw) * 4);
      upload = repacked.data();
    }

    glActiveTexture(GL_TEXTURE0);
    bool created = false;
    if (!v.computed.canvasTexture) {
      GLuint tex = 0;
      glGenTextures(1, &tex);
      v.computed.canvasTexture = tex;
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      created = true;
    } else {
      glBindTexture(GL_TEXTURE_2D, v.computed.canvasTexture);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT,
                  4); // rows are iw*4 bytes, always 4-aligned
    if (created || resized) {
      // (Re)specify storage: new texture, or the canvas changed size.
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, upload);
    } else {
      // Same size: overwrite in place, no reallocation on the driver side.
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, iw, ih, GL_RGBA, GL_UNSIGNED_BYTE,
                      upload);
    }
  }

#else // LITEUI_CANVAS_GL
  // (Re)builds v's GPU target — an RGBA8 texture + FBO(s) sized to v's inner
  // (padding-excluded) content box — whenever that size changed, then
  // re-invokes v.onPaint into it whenever the size changed OR the app
  // requested a redraw via View::requestCanvasRedraw() (canvasNeedsRedraw
  // starts true, so this always happens at least once). A plain repaint with
  // neither condition true reuses the texture untouched: no CPU work, no
  // upload. Drawing happens between canvasGl_.begin()/end(), which save and
  // restore the window renderer's GL state around it.
  void ensureCanvasTargetGl(const View &v) {
    if (!canvasGl_.ready())
      return;
    const EdgeInsets &pad =
        resolveDynamic(v.style.padding, v.computed.resolvedPadding);
    float innerW = std::max(1.0f, v.computed.w - pad.left - pad.right);
    float innerH = std::max(1.0f, v.computed.h - pad.top - pad.bottom);
    int iw = std::max(1, static_cast<int>(innerW));
    int ih = std::max(1, static_cast<int>(innerH));
    bool resized = !v.computed.canvasTexture ||
                   v.computed.canvasBuiltForWidth != innerW ||
                   v.computed.canvasBuiltForHeight != innerH;
    if (resized) {
      if (!liteui_canvas_gl::Renderer::buildTarget(v.computed, iw, ih)) {
        fprintf(stderr, "liteui: could not create a %dx%d canvas target\n", iw,
                ih);
        return; // texture stays 0: nothing is drawn for this node
      }
      v.computed.canvasBuiltForWidth = innerW;
      v.computed.canvasBuiltForHeight = innerH;
      v.computed.canvasNeedsRedraw = true;
    }
    if (!v.computed.canvasTexture)
      return;
    if (v.computed.canvasNeedsRedraw && v.onPaint) {
      canvasGl_.begin(v.computed, iw, ih);
      {
        CanvasContextImpl ctx(canvasGl_, static_cast<float>(iw),
                              static_cast<float>(ih), &v.computed);
        v.onPaint(ctx);
      }
      canvasGl_.end(v.computed);
      v.computed.canvasNeedsRedraw = false;
    }
  }
#endif // LITEUI_CANVAS_GL

  void drawCanvasTexture(const View &v, const ClipRect &clip) {
#if defined(LITEUI_CANVAS_GL)
    ensureCanvasTargetGl(v);
#else
    ensureCanvasSurface(v);
#endif
    if (!v.computed.canvasTexture)
      return;
    applyScissor(clip);
    glUseProgram(canvasProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v.computed.canvasTexture);
    glUniform1i(canvasUTex_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(canvasAPos_);
    glVertexAttribPointer(canvasAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    const EdgeInsets &pad =
        resolveDynamic(v.style.padding, v.computed.resolvedPadding);
    float x = v.computed.x + pad.left;
    float y = v.computed.y + pad.top;
    glUniform2f(canvasUPos_, x, y);
    glUniform2f(canvasUSize_, v.computed.canvasBuiltForWidth,
                v.computed.canvasBuiltForHeight);
    glUniform2f(canvasUScreen_, static_cast<float>(width_),
                static_cast<float>(height_));

    // The texture is premultiplied now, so blend with ONE instead of
    // SRC_ALPHA, then restore the blend mode the rest of the renderer uses.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }

  // showing) as flat filled rectangles — classic fixed track+thumb
  // styling, matching the Windows/GDI renderer's look. `clip` is v's own
  // ancestor-level clip (not narrowed by v's own children), so a
  // scrollbar correctly disappears if v itself has scrolled out of some
  // outer ancestor's viewport, same reasoning as the GDI version.
  void renderScrollbars(const View &v, const ClipRect &clip) {
    // Auto-hide: painted only while hovered, or while this exact view's
    // thumb is mid-drag (so a fast drag that briefly outruns the pointer
    // doesn't make the bar vanish out from under it).
    if (v.style.scrollbarAutoHide && !v.computed.isHovered &&
        !((scrollDrag_.mode == DragMode::VThumb ||
           scrollDrag_.mode == DragMode::HThumb) &&
          scrollDrag_.target == &v))
      return;
    // Per-view override falls back to the window-wide default independently
    // for track and thumb (see Style::scrollTrackColor/scrollThumbColor).
    Color track = resolveDynamic(v.style.scrollTrackColor,
                                 v.computed.resolvedScrollTrackColor)
                      .value_or(scrollTrack_);
    Color thumb = resolveDynamic(v.style.scrollThumbColor,
                                 v.computed.resolvedScrollThumbColor)
                      .value_or(scrollThumb_);
    auto box = [&](const PixRect &r, Color c) {
      drawRectGL(r.x, r.y, r.w, r.h, 0.0f, c, clip);
    };
    if (wantVBar(v)) {
      box(vTrackRect(v), track);
      box(vThumbRect(v), thumb);
    }
    if (wantHBar(v)) {
      box(hTrackRect(v), track);
      box(hThumbRect(v), thumb);
    }
    if (wantVBar(v) && wantHBar(v))
      drawRectGL(v.computed.x + v.computed.w - kScrollbarThickness,
                 v.computed.y + v.computed.h - kScrollbarThickness,
                 kScrollbarThickness, kScrollbarThickness, 0.0f, track, clip);
  }

  // Draws one View (background + border) using its already-computed layout,
  // then recurses into children. Border is drawn as an outer rounded rect in
  // borderColor with an inner rounded rect in backgroundColor inset by
  // borderWidth — simple and correct for a uniform border on all sides.
  // Paints v's own background/border, then recurses into in-flow
  // children only. Any Position::Absolute descendant, at any depth, is
  // skipped here — it's collected separately and painted in a single
  // global-stacking pass afterward, so it can interleave correctly with
  // absolute nodes from entirely different subtrees.
  //
  // `clip` is the accumulated intersection of every scrollable ancestor's
  // viewport, exactly mirroring hitTestFlow()'s clip parameter — v's own
  // background/border is drawn under `clip` (the ancestor-level one, not
  // narrowed by v itself), while children are drawn under a further-
  // narrowed clip if v itself scrolls, which is what actually makes
  // scrolled-out content invisible instead of just mispositioned.
  void renderView(const View &v, ClipRect clip) {
    if (resolveDynamic(v.style.visibility, v.computed.resolvedVisibility) ==
        Visibility::Hidden)
      return; // space already reserved by layout; just don't draw it
    // Same reasoning as paintView (Windows): a text leaf's Style is
    // layout-only, so it must never paint its own opaque background —
    // otherwise white text (or any text) can end up invisible against
    // its own node's default-white box, as happened here.
    if (v.isText) {
      drawTextTexture(v, clip);
      return;
    }
    const Style &s = v.style;
    int x = static_cast<int>(v.computed.x), y = static_cast<int>(v.computed.y);
    int w = static_cast<int>(v.computed.w), h = static_cast<int>(v.computed.h);
    int radius = static_cast<int>(
        resolveDynamic(s.borderRadius, v.computed.resolvedBorderRadius));
    Color bg = (v.computed.isHovered && s.hoverColor)
                   ? *s.hoverColor
                   : resolveDynamic(s.backgroundColor,
                                    v.computed.resolvedBackgroundColor);
    radius = std::max(0, std::min({radius, w / 2, h / 2}));
    float borderWidthVal =
        resolveDynamic(s.borderWidth, v.computed.resolvedBorderWidth);
    if (borderWidthVal > 0) {
      Color bc = resolveDynamic(s.borderColor, v.computed.resolvedBorderColor);
      int bw = static_cast<int>(borderWidthVal);
      drawRectGL(x, y, w, h, radius, bc, clip);
      drawRectGL(x + bw, y + bw, std::max(0, w - 2 * bw),
                 std::max(0, h - 2 * bw), std::max(0, radius - bw), bg, clip);
    } else if (bg.a > 0) {
      drawRectGL(x, y, w, h, radius, bg, clip);
    }
    // A canvas node paints its own Style background/border like any
    // other box (above), then has its cached onPaint texture drawn on
    // top — unlike a text leaf, which skips background/border entirely.
    // Canvas nodes aren't expected to have children, so control falls
    // through to the (harmless, normally no-op) children loop below.
    if (v.isCanvas)
      drawCanvasTexture(v, clip);

    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (const auto &child : v.children)
      if (child.style.position != Position::Absolute &&
          resolveDynamic(child.style.display, child.computed.resolvedDisplay) !=
              Display::None)
        renderView(child, childClip);
    // Scrollbars sit in the gutter layout already reserved outside the
    // children's placement area, so drawing them after children never
    // overlaps content, and keeps them visually on top.
    if (v.scrollsX() || v.scrollsY())
      renderScrollbars(v, clip);
  }

  // Simple stepped line, good enough for axis-aligned/diagonal 18px icons.
  // Draws a crude line between two points by linear interpolation, stepping
  // once per pixel along the longer axis.
  void drawLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g,
                uint8_t b) {
    drawLineGL(static_cast<float>(x0), static_cast<float>(y0),
               static_cast<float>(x1), static_cast<float>(y1), {r, g, b});
  }

  // ---- titlebar button layout ----
  // Plain axis-aligned rectangle used for button hit-testing.
  struct Rect {
    int x, y, w, h;
  };
  // Returns whether point (px, py) falls within rectangle r (using half-open
  // bounds).
  static bool inside(const Rect &r, double px, double py) {
    // Standard axis-aligned bounding box containment test.
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
  }
  // Computes the close button's rectangle: flush against the right edge of the
  // titlebar.
  Rect closeRect() const {
    // x is inset from the right edge by the margin and the button's own width;
    // y centers it vertically in the titlebar.
    return {width_ - kButtonMargin - kButtonSize,
            (kTitlebarHeight - kButtonSize) / 2, kButtonSize, kButtonSize};
  }
  // Computes the maximize button's rectangle, positioned one button-width left
  // of the close button.
  Rect maximizeRect() const {
    // Start from the close button's rectangle as a reference point.
    Rect c = closeRect();
    // Shift left by one margin plus one button width, keeping the same y/size.
    return {c.x - kButtonMargin - kButtonSize, c.y, kButtonSize, kButtonSize};
  }
  // Computes the minimize button's rectangle, positioned one button-width left
  // of the maximize button.
  Rect minimizeRect() const {
    // Start from the maximize button's rectangle as a reference point.
    Rect m = maximizeRect();
    // Shift left by one margin plus one button width, keeping the same y/size.
    return {m.x - kButtonMargin - kButtonSize, m.y, kButtonSize, kButtonSize};
  }

  // Paints the custom titlebar background and its three buttons/icons.
  void drawTitlebar() {
    // Fill the whole titlebar strip with a dark gray background.
    fillRect(0, 0, width_, kTitlebarHeight, 0x2D, 0x2D, 0x2D);

    // Compute all three button rectangles up front for reuse below.
    Rect minr = minimizeRect(), maxr = maximizeRect(), clsr = closeRect();
    // Draw the minimize button's background in a lighter gray.
    fillRect(minr.x, minr.y, minr.w, minr.h, 0x50, 0x50, 0x50);
    // Draw the maximize button's background in the same lighter gray.
    fillRect(maxr.x, maxr.y, maxr.w, maxr.h, 0x50, 0x50, 0x50);
    // Draw the close button's background in red to visually distinguish it.
    fillRect(clsr.x, clsr.y, clsr.w, clsr.h, 0xC0, 0x39, 0x2B);

    // minimize: underscore
    // Draw a single horizontal line near the bottom of the minimize button to
    // form an underscore glyph.
    drawLine(minr.x + 4, minr.y + minr.h - 5, minr.x + minr.w - 4,
             minr.y + minr.h - 5, 0xFF, 0xFF, 0xFF);
    // maximize: square outline
    // Top edge of the square icon.
    drawLine(maxr.x + 4, maxr.y + 4, maxr.x + maxr.w - 4, maxr.y + 4, 0xFF,
             0xFF, 0xFF);
    // Bottom edge of the square icon.
    drawLine(maxr.x + 4, maxr.y + maxr.h - 4, maxr.x + maxr.w - 4,
             maxr.y + maxr.h - 4, 0xFF, 0xFF, 0xFF);
    // Left edge of the square icon.
    drawLine(maxr.x + 4, maxr.y + 4, maxr.x + 4, maxr.y + maxr.h - 4, 0xFF,
             0xFF, 0xFF);
    // Right edge of the square icon.
    drawLine(maxr.x + maxr.w - 4, maxr.y + 4, maxr.x + maxr.w - 4,
             maxr.y + maxr.h - 4, 0xFF, 0xFF, 0xFF);
    // close: X
    // First diagonal stroke of the X, from top-left to bottom-right.
    drawLine(clsr.x + 4, clsr.y + 4, clsr.x + clsr.w - 4, clsr.y + clsr.h - 4,
             0xFF, 0xFF, 0xFF);
    // Second diagonal stroke of the X, from top-right to bottom-left.
    drawLine(clsr.x + clsr.w - 4, clsr.y + 4, clsr.x + 4, clsr.y + clsr.h - 4,
             0xFF, 0xFF, 0xFF);
  }

  // Repaints the entire window content and commits it to the compositor.
  // Draws into whichever of the two buffers (drawBuf_) isn't currently
  // still owned by the compositor — see the "Double buffering" comment
  // block for why that matters. If it's still busy, the draw is skipped
  // entirely (never blocks) and redrawPending_ is set so bufferRelease()
  // retries automatically the moment that buffer frees up — a call to
  // redraw() should therefore be thought of as "request a repaint soon",
  // not "repaint synchronously right now".
  void redraw() {
    if (!eglReady_)
      return;
    glViewport(0, 0, width_, height_);
    applyScissor(ClipRect{});
    glClearColor(windowBg_.r / 255.0f, windowBg_.g / 255.0f,
                 windowBg_.b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (hasRoot_) {
      renderView(root_, ClipRect{});
      std::vector<AbsoluteEntry> absolutes;
      collectAbsolutes(root_, absolutes);
      std::stable_sort(absolutes.begin(), absolutes.end(),
                       [](const AbsoluteEntry &a, const AbsoluteEntry &b) {
                         int za =
                             resolveDynamic(a.view->style.zIndex,
                                            a.view->computed.resolvedZIndex);
                         int zb =
                             resolveDynamic(b.view->style.zIndex,
                                            b.view->computed.resolvedZIndex);
                         if (za != zb)
                           return za < zb;
                         return a.order < b.order;
                       });
      for (const auto &e : absolutes)
        renderView(*e.view, e.clip);
    }
    // Paint the titlebar and its buttons on top of that background.
    if (!hideTitlebar_)
      drawTitlebar();
    paintTooltip();
    if (eglSwapBuffers(eglDisplay_, eglSurface_))
      contextLossStreak_ = 0;
    else if (eglGetError() == EGL_CONTEXT_LOST)
      recoverFromContextLoss();
  }

  // Hit-tests a left-button press against the titlebar buttons, resize
  // edges, and drag region. Resize/move grabs and the chrome buttons
  // (close/maximize/minimize) still act immediately on press, same as
  // before — only content-area widget clicks wait for a matching release
  // (see beginPress/endPress).
  void handlePress(uint32_t serial, MouseButton btn = MouseButton::Left) {
    lastPressSerial_ = serial;
    hideTooltip();
    // Resize/move grabs and the chrome buttons are a left-button-only
    // convention (matching every desktop's own titlebar) — a middle/
    // right click on the resize strip just falls through to whatever's
    // below it instead of starting a grab.
    if (btn == MouseButton::Left) {
      uint32_t edge = resizeEdgeAt(pointer_x_, pointer_y_);
      if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
        if (seat_)
          xdg_toplevel_resize(toplevel_, seat_, serial, edge);
        return;
      }
    }

    // Presses below the titlebar strip first give scrollbars/scrollable
    // content a chance to claim the press (beginScrollPress) — a thumb
    // grab or track click consumes it entirely; anything else falls
    // through to the ordinary pending-click press, resolved later in
    // handleRelease().
    if (pointer_y_ >= titlebarHeight()) {
      float x = static_cast<float>(pointer_x_),
            y = static_cast<float>(pointer_y_);
      // Only the left button interacts with scrollbars — a thumb grab
      // or track-click jump is a left-drag convention.
      if (btn != MouseButton::Left || !beginScrollPress(x, y))
        beginPress(x, y, btn);
      redraw();
      return;
    }

    if (btn != MouseButton::Left)
      return; // titlebar chrome (close/max/min/move) is left-button only

    // If the click landed on the close button...
    if (inside(closeRect(), pointer_x_, pointer_y_)) {
      // ...request the event loop to stop, ending run().
      running_ = false;
      return;
    }
    if (inside(maximizeRect(), pointer_x_, pointer_y_)) {
      requestMaximize();
      redraw();
      return;
    }
    if (inside(minimizeRect(), pointer_x_, pointer_y_)) {
      requestMinimize();
      return;
    }

    // Any other click in the titlebar (empty space) starts an interactive move,
    // if we have a seat to drive it.
    if (seat_)
      xdg_toplevel_move(toplevel_, seat_, serial);
  }

  // Resolves a pending widget click (or in-progress scroll drag) on button
  // release. Chrome buttons and resize/move grabs don't need this — they
  // already acted on press — so this only matters for content-area
  // interactions below the titlebar.
  void handleRelease(MouseButton btn = MouseButton::Left) {
    if (pointer_y_ >= titlebarHeight()) {
      bool changed = (btn == MouseButton::Left)
                         ? endScrollPress(static_cast<float>(pointer_x_),
                                          static_cast<float>(pointer_y_))
                         : endPress(static_cast<float>(pointer_x_),
                                    static_cast<float>(pointer_y_), btn);
      if (changed)
        redraw();
    }
    // Whatever the release didn't resolve — it landed in the titlebar, or a
    // content pan (which never reaches endPress) swallowed it — can't
    // complete: deliver onPointerCancel. A no-op after
    // a normal release, which has already cleared everything.
    cancelPress(btn);
    if (btn == MouseButton::Left)
      scrollDrag_ = {}; // scroll-drag tracking only ever exists for Left
  }

// Ends the Windows/Linux member block.
#endif
};

// Out-of-line constructor definition; inline because this is a single-header
// library.
inline LiteUIImpl::LiteUIImpl(const std::string &title, int w, int h,
                              bool hideTitlebar)
    : width_(w > 0 ? w : 800), height_(h > 0 ? h : 600) {
  startMaximized_ = (w <= 0 || h <= 0);
  hideTitlebar_ = hideTitlebar;
  activeInstance_ = this;
  ptrRouter_.poll = [this] { return pollAndRelayout(); };
  ptrRouter_.focus = [this](View *v) { setFocus(v); };
// Windows-specific construction path.
#if defined(_WIN32)
  // Handle to the current executable module, needed to register a window class.
  HINSTANCE hInst = GetModuleHandleW(nullptr);
  // Name used to identify our window class to Windows.
  const wchar_t *className = L"LiteUIClass";

  // Descriptor for the window class we're about to register.
  WNDCLASSW wc = {};
  // Point the class at our static message-handling function.
  wc.lpfnWndProc = WndProc;
  // Associate the class with this module.
  wc.hInstance = hInst;
  // Give the class the name declared above.
  wc.lpszClassName = className;
  // Use the standard arrow cursor while hovering the window.
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // Use the default window background brush/color.
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  // Register the class with the OS so CreateWindowExW can use it.
  RegisterClassW(&wc);

  // Convert the UTF-8 title into the wide string Win32 expects.
  std::wstring wtitle = toWide(title);

  DWORD style = WS_OVERLAPPEDWINDOW;
  int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
  if (hideTitlebar_) {
    // WS_POPUP = no caption, no border. SYSMENU/MINIMIZEBOX keep the
    // taskbar button's right-click menu and click-to-minimize working.
    style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    // CW_USEDEFAULT only means "let Windows pick" for overlapped windows,
    // so center the window in the work area ourselves.
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    posX = wa.left + ((wa.right - wa.left) - width_) / 2;
    posY = wa.top + ((wa.bottom - wa.top) - height_) / 2;
  }
  hwnd_ = CreateWindowExW(0, className, wtitle.c_str(), style, posX, posY,
                          width_, height_, nullptr, nullptr, hInst, this);
  // If creation failed, surface it as an exception rather than continuing with
  // a null handle.
  if (!hwnd_) {
    throw std::runtime_error("CreateWindowExW failed");
  }

  // One factory per LiteUI instance is simplest here; a real app with many
  // windows would normally share a single process-wide factory instead.
  if (FAILED(
          D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_)))
    throw std::runtime_error("D2D1CreateFactory failed");
  ensureRenderTarget();

  // Make the window visible. If no explicit size was given, start it
  // maximized (title bar/borders intact, user can un-maximize/drag/resize
  // normally) instead of the platform default show. WM_SIZE fires as a
  // result and updates width_/height_ + relayout automatically via the
  // existing handler.
  ShowWindow(hwnd_, startMaximized_ ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
  // Force an initial paint of the window.
  UpdateWindow(hwnd_);

// Linux/Wayland-specific construction path.
#else
  // Connect to the compositor via the default Wayland socket.
  display_ = wl_display_connect(nullptr);
  // Fail loudly if no compositor is reachable.
  if (!display_)
    throw std::runtime_error("Failed to connect to Wayland display");

  // Ask the display for its registry, the object through which globals are
  // advertised.
  wl_registry *registry = wl_display_get_registry(display_);
  // Register our global-announcement callbacks on it.
  wl_registry_add_listener(registry, &registryListener, this);
  // Round-trip blocks until the server has answered every request sent
  // so far — the only way to get bind() results synchronously, since
  // globals normally arrive as async events.
  // Block until the compositor has processed everything so far, guaranteeing
  // all globals are bound by now.
  wl_display_roundtrip(display_);

  // Make sure the globals this window absolutely needs were actually
  // advertised.
  if (!compositor_ || !wm_base_ || !shm_)
    throw std::runtime_error("Missing required Wayland globals");

  // Load a cursor theme so resize/arrow cursors can be shown over CSD
  // window edges — nothing does this automatically the way server-side
  // decorations would. nullptr picks the user's configured/default theme;
  // 24 is a conventional base cursor size in pixels. Non-fatal if it fails:
  // we just silently keep whatever cursor the compositor already set.
  cursorTheme_ = wl_cursor_theme_load(nullptr, 24, shm_);
  if (cursorTheme_)
    cursorSurface_ = wl_compositor_create_surface(compositor_);

  // Create the raw drawable surface from the compositor.
  surface_ = wl_compositor_create_surface(compositor_);
  // Wrap that surface as an xdg_surface so it can become a desktop window.
  xdg_surf_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
  // Register the configure/ack handler on the xdg_surface.
  xdg_surface_add_listener(xdg_surf_, &surfListener, this);

  // Promote the xdg_surface to a toplevel (a normal, movable/resizable
  // top-level window).
  toplevel_ = xdg_surface_get_toplevel(xdg_surf_);
  // Register the configure/close handlers on the toplevel.
  xdg_toplevel_add_listener(toplevel_, &toplevelListener, this);
  // Set the window's title as shown in taskbars/window switchers.
  xdg_toplevel_set_title(toplevel_, title.c_str());

  // If no explicit size was given, ask the compositor to start us
  // maximized (our own CSD titlebar stays intact, unlike fullscreen).
  // The compositor's first configure event reports the real work-area
  // size via toplevelConfigure -> pendingWidth_/pendingHeight_, which
  // surfaceConfigure() then applies via resize().
  if (startMaximized_) {
    xdg_toplevel_set_maximized(toplevel_);
    maximized_ = true; // keep our own tracking flag in sync, so the
                       // titlebar's maximize button correctly offers
                       // "unmaximize" as its next click
  }

  // Commit the surface state now, which triggers the compositor's first
  // configure event.
  wl_surface_commit(surface_); // triggers the first configure event
// Ends the Windows/Linux construction branch.
#endif
}

// Out-of-line destructor definition.
inline LiteUIImpl::~LiteUIImpl() {
  if (activeInstance_ == this)
    activeInstance_ = nullptr;
  if (hasRoot_)
    root_.freeTextResources(); // must run before releasing d2dFactory_ / before
                               // eglMakeCurrent(NO_CONTEXT) below

// Windows-specific teardown path.
#if defined(_WIN32)
  // Release the Direct3D 11 / Direct2D device stack — order matters: the
  // contexts, swap chain and devices must go before the factory that
  // created them.
  releaseDeviceResources();
  if (d2dFactory_)
    d2dFactory_->Release();
  // Destroy the native window if it was successfully created.
  if (hwnd_)
    DestroyWindow(hwnd_);
// Linux/Wayland-specific teardown path.
#else
  if (eglDisplay_ != EGL_NO_DISPLAY) {
#if defined(LITEUI_CANVAS_GL)
    canvasGl_.destroy(); // needs the context still current
#endif
    eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (rectProgram_)
      glDeleteProgram(rectProgram_);
    if (flatProgram_)
      glDeleteProgram(flatProgram_);
    if (texProgram_)
      glDeleteProgram(texProgram_);
    if (canvasProgram_)
      glDeleteProgram(canvasProgram_);
    if (tooltipTextTexture_)
      glDeleteTextures(1, &tooltipTextTexture_);
    if (quadVbo_)
      glDeleteBuffers(1, &quadVbo_);
    if (flatVbo_)
      glDeleteBuffers(1, &flatVbo_);
    if (eglSurface_ != EGL_NO_SURFACE)
      eglDestroySurface(eglDisplay_, eglSurface_);
    if (eglContext_ != EGL_NO_CONTEXT)
      eglDestroyContext(eglDisplay_, eglContext_);
    eglTerminate(eglDisplay_);
  }
  if (eglWindow_)
    wl_egl_window_destroy(eglWindow_);
  // Pens and touch.
  for (auto &t : tabletTools_)
    if (t->tool)
      zwp_tablet_tool_v2_destroy(t->tool);
  tabletTools_.clear();
  if (tabletSeat_)
    zwp_tablet_seat_v2_destroy(tabletSeat_);
  if (tabletManager_)
    zwp_tablet_manager_v2_destroy(tabletManager_);
  if (touch_)
    wl_touch_destroy(touch_);
  if (toolCursorSurface_)
    wl_surface_destroy(toolCursorSurface_);
  // Destroy the cursor surface and theme if they were created.
  if (cursorSurface_)
    wl_surface_destroy(cursorSurface_);
  if (cursorTheme_)
    wl_cursor_theme_destroy(cursorTheme_);
  // Destroy the decoration-mode object if one was requested.
  if (toplevel_decoration_)
    zxdg_toplevel_decoration_v1_destroy(toplevel_decoration_);
  if (keyboard_)
    wl_keyboard_destroy(keyboard_);
  if (xkbState_)
    xkb_state_unref(xkbState_);
  if (xkbKeymap_)
    xkb_keymap_unref(xkbKeymap_);
  if (xkbContext_)
    xkb_context_unref(xkbContext_);
  if (repeatTimerFd_ >= 0)
    close(repeatTimerFd_);
  if (wakeFd_ >= 0)
    close(wakeFd_);
  // Release the pointer device if one was obtained.
  if (pointer_)
    wl_pointer_destroy(pointer_);
  // Release the seat object if one was bound.
  if (seat_)
    wl_seat_destroy(seat_);
  // Destroy the toplevel object if one was created.
  if (toplevel_)
    xdg_toplevel_destroy(toplevel_);
  // Destroy the xdg_surface wrapper if one was created.
  if (xdg_surf_)
    xdg_surface_destroy(xdg_surf_);
  // Destroy the raw surface if one was created.
  if (surface_)
    wl_surface_destroy(surface_);
  // Disconnect from the compositor if a connection was established.
  if (display_)
    wl_display_disconnect(display_);
// Ends the Windows/Linux teardown branch.
#endif
}

View View::toView(TextInput ti) {
  auto state = ti.state;
  state->text = ti.text;
  state->cursor = state->text.size();

  TextStyle ts;
  ts.fontSize = ti.fontSize;
  ts.fontWeight = ti.fontWeight;
  ts.fontFamily = ti.fontFamily;

  Color textColor = ti.textColor;
  Color placeholderColor = ti.placeholderColor;
  Color caretColor = ti.caretColor;
  Color borderColor = ti.borderColor;
  Color focusedBorderColor = ti.focusedBorderColor;
  float leftPad = ti.leftPadding;
  std::string placeholder = ti.placeholder;
  auto onChange = ti.onChange;
  auto onSubmit = ti.onSubmit;

  View v;
  v.style = std::move(ti.style);
  v.isCanvas = true;
  v.focusable = true;

  // Border color reflects focus automatically — overrides whatever the
  // caller set on style.borderColor.
  v.style.borderColor = [state, borderColor, focusedBorderColor] {
    return state->focused ? focusedBorderColor : borderColor;
  };

  v.canvasDirtySource = [state] {
    bool d = state->dirty;
    state->dirty = false;
    return d;
  };

  v.wantsFocus = [state]() -> bool {
    if (!state->requestFocus)
      return false;
    state->requestFocus = false;
    return true;
  };

  v.onPaint = [state, ts, textColor, placeholderColor, caretColor, leftPad,
               placeholder](CanvasContext &ctx) {
    float availW = std::max(0.0f, ctx.width() - leftPad * 2.0f);

    // Keep the caret in view: scroll right if it's past the visible edge,
    // scroll left if it's before the start of the visible window.
    liteui_text::Measurement cursorM = liteui_text::measure(
        state->text.substr(0, state->cursor), ts, resolveStatic(ts.fontSize),
        resolveStatic(ts.fontWeight), resolveStatic(ts.fontStyle), -1);
    float cursorX = cursorM.width;
    if (cursorX - state->scrollOffset > availW)
      state->scrollOffset = cursorX - availW;
    if (cursorX - state->scrollOffset < 0.0f)
      state->scrollOffset = cursorX;

    // Never scroll past the end of the text (e.g. after deleting chars).
    liteui_text::Measurement fullM = liteui_text::measure(
        state->text, ts, resolveStatic(ts.fontSize),
        resolveStatic(ts.fontWeight), resolveStatic(ts.fontStyle), -1);
    float maxScroll = std::max(0.0f, fullM.width - availW);
    state->scrollOffset = std::clamp(state->scrollOffset, 0.0f, maxScroll);

    // Clip to the field's own bounds so scrolled-off text/caret don't
    // paint outside the box.
    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, ctx.width(), ctx.height());
    ctx.clip();

    ctx.setFont(ts.fontFamily, resolveStatic(ts.fontSize),
                resolveStatic(ts.fontWeight), resolveStatic(ts.fontStyle));
    ctx.setTextBaseline(TextBaseline::Middle);
    ctx.setTextAlign(TextAlign::Start);

    float textX = leftPad - state->scrollOffset;
    if (state->text.empty() && !state->focused) {
      ctx.setFillColor(placeholderColor);
      ctx.fillText(placeholder, leftPad, ctx.height() / 2.0f);
    } else {
      ctx.setFillColor(textColor);
      ctx.fillText(state->text, textX, ctx.height() / 2.0f);
    }

    if (state->focused && state->blinkOn) {
      float cx = textX + cursorX;
      ctx.setFillColor(caretColor);
      ctx.fillRect(cx, 5.0f, 1.5f, std::max(0.0f, ctx.height() - 10.0f));
    }

    ctx.restore();
  };

  v.onPointerDown = [state, ts, leftPad](PointerEvent &e) {
    if (e.button != MouseButton::Left)
      return; // only the primary button places the caret
    state->cursor = liteui_text::caretIndexForX(
        state->text, ts, resolveStatic(ts.fontSize),
        resolveStatic(ts.fontWeight), resolveStatic(ts.fontStyle),
        e.x - leftPad + state->scrollOffset);
    state->blinkOn = true;
    state->dirty = true;
    e.stopPropagation(); // the field owns the press; don't also click a parent
  };

  v.onFocus = [state] {
    state->focused = true;
    state->blinkOn = true;
    state->dirty = true;
    if (state->blinkTimerHandle < 0 && LiteUIImpl::current()) {
      state->blinkTimerHandle =
          LiteUIImpl::current()->addInterval(530, [state] {
            state->blinkOn = !state->blinkOn;
            state->dirty = true;
          });
    }
  };
  v.onBlur = [state] {
    state->focused = false;
    state->dirty = true;
    if (state->blinkTimerHandle >= 0 && LiteUIImpl::current()) {
      LiteUIImpl::current()->removeInterval(state->blinkTimerHandle);
      state->blinkTimerHandle = -1;
    }
  };

  v.onKeyDown = [state, onChange, onSubmit](KeyEvent e) {
    switch (e.key) {
    case Key::Left:
      if (state->cursor > 0)
        state->cursor--;
      break;
    case Key::Right:
      if (state->cursor < state->text.size())
        state->cursor++;
      break;
    case Key::Home:
      state->cursor = 0;
      break;
    case Key::End:
      state->cursor = state->text.size();
      break;
    case Key::Backspace:
      if (state->cursor > 0) {
        state->text.erase(state->cursor - 1, 1);
        state->cursor--;
        if (onChange)
          onChange(state->text);
      }
      break;
    case Key::Delete:
      if (state->cursor < state->text.size()) {
        state->text.erase(state->cursor, 1);
        if (onChange)
          onChange(state->text);
      }
      break;
    case Key::Enter:
      if (onSubmit)
        onSubmit(state->text);
      return; // don't touch blink/dirty for a submit
    case Key::V: {
      if (!e.mods.ctrl)
        return;
      std::string clip = liteui_clipboard::getTextWithFallback();
      // single-line field: strip line breaks, and keep ASCII-only to match
      // this widget's byte-offset cursor (see note below)
      clip.erase(std::remove_if(clip.begin(), clip.end(),
                                [](unsigned char c) {
                                  return c == '\n' || c == '\r' || c >= 0x80;
                                }),
                 clip.end());
      if (clip.empty())
        return;
      state->text.insert(state->cursor, clip);
      state->cursor += clip.size();
      if (onChange)
        onChange(state->text);
      break;
    }
    default:
      return;
    }
    state->blinkOn = true;
    state->dirty = true;
  };

  v.onTextInput = [state, onChange](uint32_t cp) {
    if (cp >= 0x20 && cp < 128) {
      state->text.insert(state->cursor, 1, static_cast<char>(cp));
      state->cursor++;
      state->blinkOn = true;
      state->dirty = true;
      if (onChange)
        onChange(state->text);
    }
  };

  return v;
}

View View::toView(Image img) {
  auto state = std::make_shared<ImageState>();

  // A dynamic source is polled every dispatch cycle (see
  // canvasDirtySource below); a fixed value or path/memoryData is
  // resolved exactly once, right here, matching the old one-shot-decode
  // behavior.
  std::function<std::shared_ptr<CanvasImage>()> sourceFn;
  std::function<std::shared_ptr<ImageBuffer>()> hdrFn;
  std::string err;
  std::shared_ptr<ImageBuffer> hdrFixed;
  if (auto *hf = std::get_if<std::function<std::shared_ptr<ImageBuffer>()>>(
          &img.hdrSource))
    hdrFn = *hf;
  else if (auto *hp = std::get_if<std::shared_ptr<ImageBuffer>>(&img.hdrSource))
    hdrFixed = *hp;

  // High-precision source: resolved to an RGBA8 display copy; the rest of
  // this function (sizing, fit, paint, bitmap cache) is unchanged.
  const bool hdrMode = static_cast<bool>(hdrFn) || hdrFixed;
  std::function<DisplaySettings()> settingsFn;
  if (auto *sf =
          std::get_if<std::function<DisplaySettings()>>(&img.displaySettings))
    settingsFn = *sf;
  if (hdrMode) {
    state->hdr = hdrFn ? hdrFn() : hdrFixed;
    if (settingsFn)
      state->settings = settingsFn();
    else if (auto *fs = std::get_if<DisplaySettings>(&img.displaySettings))
      state->settings = *fs;
    if (state->hdr) {
      if (auto disp = liteui_image::toCanvasImage(*state->hdr, state->settings))
        state->pixels = std::make_shared<CanvasImage>(std::move(*disp));
      else
        err = "liteui: could not convert the high-precision image for display";
    }
  } else if (auto *fn =
                 std::get_if<std::function<std::shared_ptr<CanvasImage>()>>(
                     &img.source)) {
    sourceFn = *fn;
    state->pixels = sourceFn ? sourceFn() : nullptr;
  } else if (auto *fixed =
                 std::get_if<std::shared_ptr<CanvasImage>>(&img.source);
             fixed && *fixed) {
    state->pixels = *fixed;
  } else if (!img.path.empty()) {
    if (auto decoded = liteui_image::decodeFile(img.path, &err))
      state->pixels = std::make_shared<CanvasImage>(std::move(*decoded));
  } else if (!img.memoryData.empty()) {
    if (auto decoded = liteui_image::decodeMemory(img.memoryData.data(),
                                                  img.memoryData.size(), &err))
      state->pixels = std::make_shared<CanvasImage>(std::move(*decoded));
  }

  auto onLoad = img.onLoad;
  auto onError = img.onError;
  if (state->pixels && onLoad)
    onLoad(state->pixels->width, state->pixels->height);
  else if (!state->pixels && onError)
    onError(err.empty() ? "liteui: no image source given" : err);

  View v;
  v.style = std::move(img.style);
  v.isCanvas = true;

  // Fit-size defaulting: for a STATIC source this is a one-time pixel
  // size, same as before. For a DYNAMIC source, install a
  // std::function<Size()> instead — checkForUpdates() already knows how
  // to re-poll a dynamic style.width/height on every View (see its
  // generic per-field handling), so swapping to a differently-sized
  // image reflows automatically with no new machinery.
  bool widthIsFit = std::get_if<Size>(&v.style.width) &&
                    std::get<Size>(v.style.width).kind == Size::Kind::Fit;
  bool heightIsFit = std::get_if<Size>(&v.style.height) &&
                     std::get<Size>(v.style.height).kind == Size::Kind::Fit;
  if (sourceFn || hdrFn) {
    if (widthIsFit)
      v.style.width = std::function<Size()>([state] {
        return state->pixels
                   ? Size::pixel(static_cast<float>(state->pixels->width))
                   : Size::fit();
      });
    if (heightIsFit)
      v.style.height = std::function<Size()>([state] {
        return state->pixels
                   ? Size::pixel(static_cast<float>(state->pixels->height))
                   : Size::fit();
      });
  } else if (state->pixels) {
    if (widthIsFit)
      v.style.width = Size::pixel(static_cast<float>(state->pixels->width));
    if (heightIsFit)
      v.style.height = Size::pixel(static_cast<float>(state->pixels->height));
  }

  ObjectFit fit = img.fit;
  Color bg = img.backgroundColor;

  // Polled every dispatch cycle exactly like TextInput's blink timer
  // (see toView(TextInput)) — re-invokes sourceFn, and if it handed back
  // a different shared_ptr than last time, swaps it in, re-fires
  // onLoad/onError, and marks this canvas dirty. A no-op (one call, one
  // pointer compare) for a static source, since sourceFn is empty there.
  v.canvasDirtySource = [state, sourceFn, hdrFn, hdrMode, settingsFn, onLoad,
                         onError] {
    if (hdrMode && (hdrFn || settingsFn)) {
      std::shared_ptr<ImageBuffer> next = hdrFn ? hdrFn() : state->hdr;
      const DisplaySettings cur = settingsFn ? settingsFn() : state->settings;
      const bool bufferChanged = next != state->hdr;
      if (bufferChanged || !(cur == state->settings)) {
        state->hdr = next;
        state->settings = cur;
        std::shared_ptr<CanvasImage> disp;
        if (next)
          if (auto d = liteui_image::toCanvasImage(*next, cur))
            disp = std::make_shared<CanvasImage>(std::move(*d));
        // The new copy is allocated while the old one is still alive; the
        // old one is parked until the next paint (see ImageState::retired).
        if (state->pixels)
          state->retired.push_back(std::move(state->pixels));
        state->pixels = std::move(disp);
        state->dirty = true;
        if (state->pixels && onLoad && bufferChanged)
          onLoad(state->pixels->width, state->pixels->height);
        else if (!state->pixels && onError)
          onError(next ? "liteui: could not convert the high-precision image "
                         "for display"
                       : "liteui: dynamic image source returned null");
      }
    }
    if (sourceFn) {
      std::shared_ptr<CanvasImage> next = sourceFn();
      if (next != state->pixels) {
        state->pixels = next;
        state->dirty = true;
        if (state->pixels && onLoad)
          onLoad(state->pixels->width, state->pixels->height);
        else if (!state->pixels && onError)
          onError("liteui: dynamic image source returned null");
      }
    }
    bool d = state->dirty;
    state->dirty = false;
    return d;
  };

  v.onPaint = [state, fit, bg](CanvasContext &ctx) {
    if (bg.a > 0) {
      ctx.setFillColor(bg);
      ctx.fillRect(0, 0, ctx.width(), ctx.height());
    }
    if (!state->pixels)
      return;
    const CanvasImage &im = *state->pixels;
    if (im.width <= 0 || im.height <= 0)
      return;
    float cw = ctx.width(), ch = ctx.height();
    float iw = static_cast<float>(im.width), ih = static_cast<float>(im.height);
    float dw = cw, dh = ch, dx = 0, dy = 0;
    switch (fit) {
    case ObjectFit::Fill:
      break;
    case ObjectFit::None:
      dw = iw;
      dh = ih;
      dx = (cw - dw) / 2.0f;
      dy = (ch - dh) / 2.0f;
      break;
    case ObjectFit::Contain:
    case ObjectFit::ScaleDown: {
      float scale = std::min(cw / iw, ch / ih);
      if (fit == ObjectFit::ScaleDown)
        scale = std::min(scale, 1.0f);
      dw = iw * scale;
      dh = ih * scale;
      dx = (cw - dw) / 2.0f;
      dy = (ch - dh) / 2.0f;
      break;
    }
    case ObjectFit::Cover: {
      float scale = std::max(cw / iw, ch / ih);
      dw = iw * scale;
      dh = ih * scale;
      dx = (cw - dw) / 2.0f;
      dy = (ch - dh) / 2.0f;
      break;
    }
    }
    ctx.drawImage(im, dx, dy, dw, dh);
    state->retired.clear(); // the backend cache now refers to `im`
  };

  return v;
}

View View::toView(Svg s) {
  std::string err;
  auto doc = std::make_shared<std::optional<liteui_svg::Document>>();
  *doc = s.path.empty() ? liteui_svg::parseString(s.source)
                        : liteui_svg::parseFile(s.path, &err);

  if (doc->has_value() && s.onLoad)
    s.onLoad();
  else if (!doc->has_value() && s.onError)
    s.onError(err.empty() ? "liteui: no SVG source given or failed to parse"
                          : err);

  View v;
  v.style = std::move(s.style);
  v.isCanvas = true;

  if (doc->has_value()) {
    if (std::get_if<Size>(&v.style.width) &&
        std::get<Size>(v.style.width).kind == Size::Kind::Fit)
      v.style.width = Size::pixel((*doc)->width);
    if (std::get_if<Size>(&v.style.height) &&
        std::get<Size>(v.style.height).kind == Size::Kind::Fit)
      v.style.height = Size::pixel((*doc)->height);
  }

  ObjectFit fit = s.fit;
  Color bg = s.backgroundColor;

  v.onPaint = [doc, fit, bg](CanvasContext &ctx) {
    if (bg.a > 0) {
      ctx.setFillColor(bg);
      ctx.fillRect(0, 0, ctx.width(), ctx.height());
    }
    if (!doc->has_value())
      return;
    const liteui_svg::Document &d = **doc;
    if (d.width <= 0 || d.height <= 0)
      return;
    float cw = ctx.width(), ch = ctx.height();
    float sx = cw / d.width, sy = ch / d.height, dx = 0, dy = 0;
    switch (fit) {
    case ObjectFit::Fill:
      break;
    case ObjectFit::None:
      sx = sy = 1.0f;
      dx = (cw - d.width) / 2.0f;
      dy = (ch - d.height) / 2.0f;
      break;
    case ObjectFit::Contain:
    case ObjectFit::ScaleDown: {
      float scale = std::min(sx, sy);
      if (fit == ObjectFit::ScaleDown)
        scale = std::min(scale, 1.0f);
      sx = sy = scale;
      dx = (cw - d.width * scale) / 2.0f;
      dy = (ch - d.height * scale) / 2.0f;
      break;
    }
    case ObjectFit::Cover: {
      float scale = std::max(sx, sy);
      sx = sy = scale;
      dx = (cw - d.width * scale) / 2.0f;
      dy = (ch - d.height * scale) / 2.0f;
      break;
    }
    }
    ctx.save();
    ctx.translate(dx, dy);
    ctx.scale(sx, sy);
    for (const auto &shape : d.shapes) {
      ctx.save();
      ctx.transformBy(shape.transform.a, shape.transform.b, shape.transform.c,
                      shape.transform.d, shape.transform.e, shape.transform.f);
      ctx.beginPath();
      for (const auto &op : shape.ops) {
        switch (op.kind) {
        case liteui_svg::PathOp::Kind::Move:
          ctx.moveTo(op.x, op.y);
          break;
        case liteui_svg::PathOp::Kind::Line:
          ctx.lineTo(op.x, op.y);
          break;
        case liteui_svg::PathOp::Kind::Cubic:
          ctx.bezierCurveTo(op.c1x, op.c1y, op.c2x, op.c2y, op.x, op.y);
          break;
        case liteui_svg::PathOp::Kind::Quad:
          ctx.quadraticCurveTo(op.qx, op.qy, op.x, op.y);
          break;
        case liteui_svg::PathOp::Kind::Close:
          ctx.closePath();
          break;
        }
      }
      if (shape.hasFill) {
        ctx.setFillColor(shape.fill);
        ctx.fill(shape.evenOdd ? FillRule::EvenOdd : FillRule::NonZero);
      }
      if (shape.hasStroke) {
        ctx.setStrokeColor(shape.stroke);
        ctx.setLineWidth(shape.strokeWidth);
        ctx.setLineCap(shape.lineCap == 1   ? LineCap::Round
                       : shape.lineCap == 2 ? LineCap::Square
                                            : LineCap::Butt);
        ctx.setLineJoin(shape.lineJoin == 1   ? LineJoin::Round
                        : shape.lineJoin == 2 ? LineJoin::Bevel
                                              : LineJoin::Miter);
        ctx.setMiterLimit(shape.miterLimit);
        ctx.setLineDash(shape.dashArray);
        ctx.setLineDashOffset(shape.dashOffset);
        ctx.stroke();
      }
      ctx.restore();
    }
    ctx.restore();
  };
  return v;
}

inline void LiteUIImpl::requestRepaint() {
  pollAndRelayout();
#if defined(_WIN32)
  if (hwnd_)
    InvalidateRect(hwnd_, nullptr, FALSE);
#else
  if (eglReady_)
    redraw();
#endif
}

#if defined(_WIN32)
inline int LiteUIImpl::addInterval(int ms, std::function<void()> fn) {
  UINT_PTR id =
      SetTimer(hwnd_, timers_.size() + 1, static_cast<UINT>(ms), nullptr);
  timers_[id] = std::move(fn);
  return static_cast<int>(id);
}
inline void LiteUIImpl::removeInterval(int handle) {
  KillTimer(hwnd_, static_cast<UINT_PTR>(handle));
  timers_.erase(static_cast<UINT_PTR>(handle));
}
inline void LiteUIImpl::wake() {
  if (!hwnd_)
    return;
  // PostMessage is documented as callable from any thread. Only post if none
  // is already waiting; WndProc clears the flag when it picks it up.
  if (!wakePending_.exchange(true) && !PostMessageW(hwnd_, kWakeMsg, 0, 0))
    wakePending_.store(false); // queue full: let the next wake() retry
}
#else
inline int LiteUIImpl::addInterval(int ms, std::function<void()> fn) {
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  itimerspec spec{};
  spec.it_value.tv_sec = ms / 1000;
  spec.it_value.tv_nsec = (ms % 1000) * 1000000L;
  spec.it_interval = spec.it_value; // repeating
  timerfd_settime(fd, 0, &spec, nullptr);
  int handle = nextIntervalHandle_++;
  intervalHandleToFd_[handle] = fd;
  intervalTimers_.push_back({fd, std::move(fn)});
  return handle;
}
inline void LiteUIImpl::removeInterval(int handle) {
  auto it = intervalHandleToFd_.find(handle);
  if (it == intervalHandleToFd_.end())
    return;
  int fd = it->second;
  intervalTimers_.erase(
      std::remove_if(intervalTimers_.begin(), intervalTimers_.end(),
                     [fd](const IntervalTimer &t) { return t.fd == fd; }),
      intervalTimers_.end());
  close(fd);
  intervalHandleToFd_.erase(it);
}
inline void LiteUIImpl::wake() {
  if (wakeFd_ < 0)
    return;
  const uint64_t one = 1;
  // Nonblocking eventfd: this can only fail if the 64-bit counter is full,
  // in which case it is already readable and a wake is already pending.
  if (write(wakeFd_, &one, sizeof one) < 0) {
  }
}
#endif

inline void LiteUIImpl::setRoot(View view) {
  // Every one of these is a raw View* into root_'s subtree. root_ is
  // about to be destroyed (freeTextResources() below, then replaced) —
  // leaving any of these pointing at it would dangle the moment the old
  // tree's destructor runs, and the next dispatched event (a keypress
  // via focusedView_, a scroll via scrollDrag_.target, a tooltip poll
  // via tooltipTarget_, ...) would then use-after-free.
  focusedView_ = nullptr;
  ptrRouter_.invalidate();
  scrollDrag_ = ScrollDrag{};
  hideTooltip(); // clears tooltipTarget_ and its timer
  if (hasRoot_)
    root_.freeTextResources();
  root_ = std::move(view);
  hasRoot_ = true;
  ptrRouter_.root = &root_;
  // First pass: runs every onLayout callback (e.g. a track capturing its
  // own width into a plain variable) so any positionSource/valueSource
  // that reads such app-side state sees a real value, not whatever it
  // was default-initialized to.
  relayout();
  // Now poll dynamic sources — textSource/valueSource/positionSource/etc.
  // — so a Text with only `source` set isn't blank, and anything whose
  // source depends on state the pass above just populated computes
  // correctly instead of against stale zeros.
  checkForUpdates(root_);
  // Second pass: applies whatever checkForUpdates just wrote (e.g. the
  // now-correct style.left from positionSource) to the actual layout
  // before the first paint.
  relayout();
#if defined(_WIN32)
  if (hwnd_)
    InvalidateRect(hwnd_, nullptr, FALSE);
#else
  if (eglReady_)
    redraw();
#endif
}

// Out-of-line definition of the blocking event loop.
inline void LiteUIImpl::run() {
// Windows-specific message loop.
#if defined(_WIN32)
  // Storage for each retrieved message.
  MSG msg;
  // GetMessage blocks until a message arrives and returns 0 on WM_QUIT, ending
  // the loop.
  while (GetMessage(&msg, nullptr, 0, 0)) {
    // Translate virtual-key messages into character messages (needed for text
    // input).
    TranslateMessage(&msg);
    // Send the message on to WndProc for handling.
    DispatchMessage(&msg);
  }
// Linux/Wayland-specific event loop.
#else
  while (running_) {
    // Apply the newest pointer position (one drag/hover update + at most one
    // redraw) before blocking. Must happen here, outside the
    // prepare_read/read_events window below — see flushPendingMotion.
    flushPendingMotion();
    flushPendingPen(); // parked pen/touch motion, with all its samples
    while (wl_display_prepare_read(display_) != 0)
      wl_display_dispatch_pending(display_);
    // Events dispatched by the loop above may have recorded fresh motion.
    // Give up the read intent and go around again to flush it, rather than
    // sleeping in poll() with an unapplied pointer position.
    if (motionPending_ || penTouchPending_) {
      wl_display_cancel_read(display_);
      continue;
    }
    wl_display_flush(display_);

    std::vector<pollfd> fds;
    fds.push_back({wl_display_get_fd(display_), POLLIN, 0});
    size_t repeatIdx = fds.size();
    if (repeatTimerFd_ >= 0)
      fds.push_back({repeatTimerFd_, POLLIN, 0});
    size_t wakeIdx = fds.size();
    if (wakeFd_ >= 0)
      fds.push_back({wakeFd_, POLLIN, 0});
    size_t intervalStart = fds.size();
    for (auto &t : intervalTimers_)
      fds.push_back({t.fd, POLLIN, 0});

    int n = poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
    if (n < 0) {
      wl_display_cancel_read(display_);
      break;
    }
    if (fds[0].revents & POLLIN)
      wl_display_read_events(display_);
    else
      wl_display_cancel_read(display_);
    wl_display_dispatch_pending(display_);

    if (repeatTimerFd_ >= 0 && fds[repeatIdx].revents & POLLIN)
      fireRepeatIfDue();

    for (size_t i = 0; i < intervalTimers_.size(); ++i) {
      if (fds[intervalStart + i].revents & POLLIN) {
        uint64_t exp;
        {
          ssize_t n = read(intervalTimers_[i].fd, &exp, sizeof(exp));
          (void)n; // timerfd drain; value unused
        }
        intervalTimers_[i].fn();
        requestRepaint();
      }
    }

    // wake() from another thread. Handled after the interval timers because
    // the handler is free to add/remove intervals, which would shift the
    // indices the loop above relies on.
    if (wakeFd_ >= 0 && (fds[wakeIdx].revents & POLLIN)) {
      uint64_t n;
      if (read(wakeFd_, &n, sizeof n) > 0) { // drain: N wakes -> one callback
        if (wakeHandler_)
          wakeHandler_();
        requestRepaint();
      }
    }
  }
#endif
}

float CanvasContext::width() const {
  return static_cast<const CanvasContextImpl *>(this)->width();
}
float CanvasContext::height() const {
  return static_cast<const CanvasContextImpl *>(this)->height();
}
void CanvasContext::save() { static_cast<CanvasContextImpl *>(this)->save(); }
void CanvasContext::restore() {
  static_cast<CanvasContextImpl *>(this)->restore();
}
void CanvasContext::translate(float x, float y) {
  static_cast<CanvasContextImpl *>(this)->translate(x, y);
}
void CanvasContext::rotate(float radians) {
  static_cast<CanvasContextImpl *>(this)->rotate(radians);
}
void CanvasContext::scale(float sx, float sy) {
  static_cast<CanvasContextImpl *>(this)->scale(sx, sy);
}
void CanvasContext::transformBy(float a, float b, float c, float d, float e,
                                float f) {
  static_cast<CanvasContextImpl *>(this)->transformBy(a, b, c, d, e, f);
}
void CanvasContext::setTransform(float a, float b, float c, float d, float e,
                                 float f) {
  static_cast<CanvasContextImpl *>(this)->setTransform(a, b, c, d, e, f);
}
void CanvasContext::resetTransform() {
  static_cast<CanvasContextImpl *>(this)->resetTransform();
}
void CanvasContext::setFillColor(Color c) {
  static_cast<CanvasContextImpl *>(this)->setFillColor(c);
}
void CanvasContext::setFillGradient(const CanvasGradient &g) {
  static_cast<CanvasContextImpl *>(this)->setFillGradient(g);
}
void CanvasContext::setStrokeColor(Color c) {
  static_cast<CanvasContextImpl *>(this)->setStrokeColor(c);
}
void CanvasContext::setStrokeGradient(const CanvasGradient &g) {
  static_cast<CanvasContextImpl *>(this)->setStrokeGradient(g);
}
void CanvasContext::setLineWidth(float w) {
  static_cast<CanvasContextImpl *>(this)->setLineWidth(w);
}
void CanvasContext::setLineCap(LineCap c) {
  static_cast<CanvasContextImpl *>(this)->setLineCap(c);
}
void CanvasContext::setLineJoin(LineJoin j) {
  static_cast<CanvasContextImpl *>(this)->setLineJoin(j);
}
void CanvasContext::setMiterLimit(float m) {
  static_cast<CanvasContextImpl *>(this)->setMiterLimit(m);
}
void CanvasContext::setLineDash(const std::vector<float> &dashes) {
  static_cast<CanvasContextImpl *>(this)->setLineDash(dashes);
}
void CanvasContext::setLineDashOffset(float offset) {
  static_cast<CanvasContextImpl *>(this)->setLineDashOffset(offset);
}
void CanvasContext::setGlobalAlpha(float a) {
  static_cast<CanvasContextImpl *>(this)->setGlobalAlpha(a);
}
void CanvasContext::setGlobalCompositeOperation(CompositeOp op) {
  static_cast<CanvasContextImpl *>(this)->setGlobalCompositeOperation(op);
}
void CanvasContext::setShadow(Color color, float blurPx, float offsetX,
                              float offsetY) {
  static_cast<CanvasContextImpl *>(this)->setShadow(color, blurPx, offsetX,
                                                    offsetY);
}
void CanvasContext::setFont(const std::string &family, float sizePx,
                            FontWeight weight, FontStyle style) {
  static_cast<CanvasContextImpl *>(this)->setFont(family, sizePx, weight,
                                                  style);
}
void CanvasContext::setTextAlign(TextAlign a) {
  static_cast<CanvasContextImpl *>(this)->setTextAlign(a);
}
void CanvasContext::setTextBaseline(TextBaseline b) {
  static_cast<CanvasContextImpl *>(this)->setTextBaseline(b);
}
void CanvasContext::beginPath() {
  static_cast<CanvasContextImpl *>(this)->beginPath();
}
void CanvasContext::closePath() {
  static_cast<CanvasContextImpl *>(this)->closePath();
}
void CanvasContext::moveTo(float x, float y) {
  static_cast<CanvasContextImpl *>(this)->moveTo(x, y);
}
void CanvasContext::lineTo(float x, float y) {
  static_cast<CanvasContextImpl *>(this)->lineTo(x, y);
}
void CanvasContext::quadraticCurveTo(float cpx, float cpy, float x, float y) {
  static_cast<CanvasContextImpl *>(this)->quadraticCurveTo(cpx, cpy, x, y);
}
void CanvasContext::bezierCurveTo(float c1x, float c1y, float c2x, float c2y,
                                  float x, float y) {
  static_cast<CanvasContextImpl *>(this)->bezierCurveTo(c1x, c1y, c2x, c2y, x,
                                                        y);
}
void CanvasContext::arc(float cx, float cy, float r, float startAngle,
                        float endAngle, bool ccw) {
  static_cast<CanvasContextImpl *>(this)->arc(cx, cy, r, startAngle, endAngle,
                                              ccw);
}
void CanvasContext::arcTo(float x1, float y1, float x2, float y2,
                          float radius) {
  static_cast<CanvasContextImpl *>(this)->arcTo(x1, y1, x2, y2, radius);
}
void CanvasContext::ellipse(float cx, float cy, float rx, float ry,
                            float rotation, float startAngle, float endAngle,
                            bool ccw) {
  static_cast<CanvasContextImpl *>(this)->ellipse(cx, cy, rx, ry, rotation,
                                                  startAngle, endAngle, ccw);
}
void CanvasContext::rect(float x, float y, float w, float h) {
  static_cast<CanvasContextImpl *>(this)->rect(x, y, w, h);
}
void CanvasContext::roundRect(float x, float y, float w, float h,
                              float radius) {
  static_cast<CanvasContextImpl *>(this)->roundRect(x, y, w, h, radius);
}
void CanvasContext::fill(FillRule rule) {
  static_cast<CanvasContextImpl *>(this)->fill(rule);
}
void CanvasContext::stroke() {
  static_cast<CanvasContextImpl *>(this)->stroke();
}
void CanvasContext::clip(FillRule rule) {
  static_cast<CanvasContextImpl *>(this)->clip(rule);
}
void CanvasContext::fillRect(float x, float y, float w, float h) {
  static_cast<CanvasContextImpl *>(this)->fillRect(x, y, w, h);
}
void CanvasContext::strokeRect(float x, float y, float w, float h) {
  static_cast<CanvasContextImpl *>(this)->strokeRect(x, y, w, h);
}
void CanvasContext::clearRect(float x, float y, float w, float h) {
  static_cast<CanvasContextImpl *>(this)->clearRect(x, y, w, h);
}
CanvasContext::TextMetricsResult
CanvasContext::measureText(const std::string &text) {
  return static_cast<CanvasContextImpl *>(this)->measureText(text);
}
void CanvasContext::fillText(const std::string &text, float x, float y,
                             float maxWidth) {
  static_cast<CanvasContextImpl *>(this)->fillText(text, x, y, maxWidth);
}
void CanvasContext::strokeText(const std::string &text, float x, float y,
                               float maxWidth) {
  static_cast<CanvasContextImpl *>(this)->strokeText(text, x, y, maxWidth);
}
void CanvasContext::drawImage(const CanvasImage &img, float dx, float dy) {
  static_cast<CanvasContextImpl *>(this)->drawImage(img, dx, dy);
}
void CanvasContext::drawImage(const CanvasImage &img, float dx, float dy,
                              float dw, float dh) {
  static_cast<CanvasContextImpl *>(this)->drawImage(img, dx, dy, dw, dh);
}
void CanvasContext::drawImage(const CanvasImage &img, float sx, float sy,
                              float sw, float sh, float dx, float dy, float dw,
                              float dh) {
  static_cast<CanvasContextImpl *>(this)->drawImage(img, sx, sy, sw, sh, dx, dy,
                                                    dw, dh);
}
std::optional<CanvasImage> CanvasContext::getImageData(float x, float y,
                                                       float w, float h) {
  return static_cast<CanvasContextImpl *>(this)->getImageData(x, y, w, h);
}
void CanvasContext::putImageData(const CanvasImage &img, float dx, float dy) {
  static_cast<CanvasContextImpl *>(this)->putImageData(img, dx, dy);
}

// ---- public LiteUI handle -> platform implementation ----
static LiteUI *liteui_currentFacade_ = nullptr;
LiteUI::LiteUI(const std::string &title, int width, int height,
               bool hideTitlebar)
    : p_(new LiteUIImpl(title, width, height, hideTitlebar)) {
  liteui_currentFacade_ = this;
}
LiteUI::~LiteUI() {
  if (liteui_currentFacade_ == this)
    liteui_currentFacade_ = nullptr;
  delete p_;
}
void LiteUI::run() { p_->run(); }
void LiteUI::setRoot(View view) { p_->setRoot(std::move(view)); }
void LiteUI::requestClose() { p_->requestClose(); }
void LiteUI::requestMinimize() { p_->requestMinimize(); }
void LiteUI::requestMaximize() { p_->requestMaximize(); }
void LiteUI::requestMove() { p_->requestMove(); }
void LiteUI::setOnCloseRequest(std::function<void()> fn) {
  p_->setOnCloseRequest(std::move(fn));
}
void LiteUI::forceClose() { p_->forceClose(); }
KeyModifiers LiteUI::modifiers() const { return p_->modifiers(); }
int LiteUI::addInterval(int ms, std::function<void()> fn) {
  return p_->addInterval(ms, std::move(fn));
}
void LiteUI::removeInterval(int handle) { p_->removeInterval(handle); }
void LiteUI::setTooltipStyle(TooltipStyle s) {
  p_->setTooltipStyle(std::move(s));
}
void LiteUI::setWindowBackground(Color c) { p_->setWindowBackground(c); }
void LiteUI::setScrollbarColors(Color track, Color thumb) {
  p_->setScrollbarColors(track, thumb);
}
void LiteUI::addShortcut(KeyModifiers mods, Key key, std::function<void()> fn) {
  p_->addShortcut(mods, key, std::move(fn));
}
void LiteUI::requestRepaint() { p_->requestRepaint(); }
LiteUI *LiteUI::current() { return liteui_currentFacade_; }
void LiteUI::wake() { p_->wake(); }
void LiteUI::setWakeHandler(std::function<void()> fn) {
  p_->setWakeHandler(std::move(fn));
}

#endif // LITEUI_IMPLEMENTATION