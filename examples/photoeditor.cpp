// Simple Photo Editor.

#include "liteui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

// ---------------- Layout constants ----------------
constexpr float kTitlebarH = 36.0f;
constexpr float kStatusBarH = 26.0f;
constexpr float kActivityBarW = 48.0f;
constexpr float kDividerW = 4.0f;
constexpr float kMinSidebarW = 180.0f;
constexpr float kMaxSidebarW = 420.0f;

// Zoom range/step.
constexpr float kZoomMin = 0.1f;
constexpr float kZoomMax = 8.0f;
constexpr float kZoomStep = 1.25f;

// Crop tool. Sizes marked "screen px" are constant on screen regardless of
// zoom (the overlay divides by state.zoom when drawing/hit-testing).
constexpr float kCropMinPx = 16.0f;   // smallest selection edge, image px
constexpr float kCropHandlePx = 8.0f; // drawn handle size, screen px
constexpr float kCropHitPx = 10.0f;   // handle grab tolerance, screen px

// Brightness range (flat offset added to each R/G/B channel).
constexpr int kBrightnessMin = -100;
constexpr int kBrightnessMax = 100;

// Exposure range (percentage; scales each R/G/B channel multiplicatively,
// applied before brightness's flat offset — see kAdjustmentSpecs below).
// Interactive-preview sizing (see EditorState::previewBase): the preview's
// long side is at most kPreviewMaxEdge px, and images whose long side is
// within kPreviewMaxEdge * kPreviewMinRatio skip the preview entirely.
constexpr int kPreviewMaxEdge = 1024;
constexpr float kPreviewMinRatio = 1.5f;

constexpr int kExposureMin = -100;
constexpr int kExposureMax = 100;

// Document size before any image has been opened, so there's a visible
// page to scroll/zoom around from the start.
constexpr float kPlaceholderW = 800.0f;
constexpr float kPlaceholderH = 600.0f;

// ---------------- Palette ----------------
constexpr Color kColorTransparent{0, 0, 0, 0};
constexpr Color kColorWhite{255, 255, 255, 255};

// Chrome backgrounds (titlebar, sidebar, activity bar, root, etc.).
constexpr Color kColorRootBg{235, 235, 235, 255};
constexpr Color kColorPanelBg{245, 245, 245, 255}; // titlebar / sidebar
constexpr Color kColorActivityBarBg{225, 225, 225,
                                    255};            // also win-btn hover,
                                                     // inactive activity btn
constexpr Color kColorMenuHover{224, 224, 224, 255}; // menu btn hover/active
constexpr Color kColorActivityHover{210, 210, 210, 255}; // also divider bg,
                                                         // scrollbar track
constexpr Color kColorScrollThumb{130, 130, 135, 255};

// Text.
constexpr Color kColorTextDark{40, 40, 40, 255};  // titles, menu labels
constexpr Color kColorTextMid{50, 50, 50, 255};   // win-btn glyphs, info pane
constexpr Color kColorTextIcon{60, 60, 60, 255};  // activity bar icons
constexpr Color kColorTextMuted{90, 90, 90, 255}; // sidebar section title
constexpr Color kColorTextFaint{140, 140, 140,
                                255}; // "coming soon" / "no image" placeholders

// Dropdown menu.
constexpr Color kColorDropdownBorder{190, 190, 190, 255};
constexpr Color kColorDropdownRowHover{232, 240, 250, 255};

// Window controls / accents.
constexpr Color kColorCloseHover{232, 17, 35, 255}; // close button hover (red)
constexpr Color kColorDividerHover{100, 140, 220, 255}; // sidebar divider drag

// Canvas / viewport.
constexpr Color kColorCanvasPlaceholderBg{250, 250, 250, 255}; // no image open
constexpr Color kColorViewportBg{200, 200, 200, 255};
constexpr Color kColorCanvasBorder{150, 150, 150, 255};

// Status bar (blue).
constexpr Color kColorStatusBarBg{0, 122, 204, 255};
constexpr Color kColorStatusHover{30, 140, 220, 255};

// A sidebar tab in the activity bar.
struct Activity {
  int id;
  std::string title;
  std::string icon; // single glyph
};

// ---------------- Adjustments ----------------
// Every slider in the "Adjust" pane is one entry in kAdjustmentSpecs below.
// Adding a new one (Contrast, Saturation, ...) means adding one entry to
// that table — AdjustmentSet, the sidebar loop, and the pixel pipeline all
// pick it up automatically; nothing else needs to change.

enum class AdjId : int { Exposure, Brightness, Count };

// One slider's identity, range, and per-channel transform. `apply` takes a
// single channel's 0..255 value (not the whole image) and returns the
// transformed value, unclamped and unrounded — buildLut below rounds and
// clamps after each spec while composing them into one lookup table. This means
// every current adjustment must be pointwise per-channel: it can't be written
// this way if it needs neighbor-pixel access (a future Clarity/sharpen) or a
// different color space (Saturation/Vibrance in HSL) — those would need
// their own image-wide pass in AdjustmentSet::applyTo instead of a table
// entry here.
struct AdjustmentSpec {
  AdjId id;
  std::string label;
  float min, max;
  std::function<float(float channelValue, float value)> apply;
};

// Declaration order here is both the sidebar's display order and the
// pipeline's processing order — each spec's `apply` runs on the previous
// one's output. Exposure before Brightness mirrors Lightroom's own Basic
// panel ordering, and keeps exposure's multiplicative scale from being
// thrown off by an offset that was meant to be applied after it.
static const std::array<AdjustmentSpec, static_cast<size_t>(AdjId::Count)>
    kAdjustmentSpecs = {{
        {AdjId::Exposure, "Exposure", static_cast<float>(kExposureMin),
         static_cast<float>(kExposureMax),
         [](float x, float value) { return x * (1.0f + value / 100.0f); }},
        {AdjId::Brightness, "Brightness", static_cast<float>(kBrightnessMin),
         static_cast<float>(kBrightnessMax),
         [](float x, float value) { return x + std::round(value); }},
    }};

// A crop region in *original*-image pixel coordinates. A default-constructed
// CropRect (w == h == 0) means "no crop — use the whole image".
struct CropRect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  bool operator==(const CropRect &) const = default;
};

// Owns the live value of every slider, the committed crop, their undo/redo
// history, and the logic to render them onto a copy of the original image.
// A slider drags continuously (many `set()` calls per second while held)
// with no drag-end event from LiteUI to mark "the gesture is over" — so
// `set` coalesces repeated calls for the *same* slider into one history
// entry instead of growing it every frame; touching a different slider (or
// an undo/redo, or a crop) starts a fresh entry. Dragging Exposure from -20
// to +40 is one undo step, not hundreds. A crop is always exactly one step.
class AdjustmentSet {
public:
  float get(AdjId id) const { return values_[static_cast<size_t>(id)]; }
  const CropRect &crop() const { return crop_; }

  void set(AdjId id, float value) {
    values_[static_cast<size_t>(id)] = value;
    if (id == lastEdited_ && historyIndex_ + 1 == history_.size()) {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
    } else {
      history_.resize(historyIndex_ + 1); // discard any redo tail
      history_.push_back(snapshot());
      ++historyIndex_;
      lastEdited_ = id;
    }
  }

  // Commits a new crop (in original-image coordinates; an empty CropRect
  // removes the crop). Always its own undo step.
  void setCrop(const CropRect &c) {
    if (c == crop_)
      return;
    crop_ = c;
    history_.resize(historyIndex_ + 1); // discard any redo tail
    history_.push_back(snapshot());
    ++historyIndex_;
    lastEdited_ = AdjId::Count; // never coalesce a slider drag into a crop
  }

  // Fresh baseline for a newly opened image: every slider back to 0, no
  // crop, no history to undo into.
  void reset() {
    values_.fill(0.0f);
    crop_ = CropRect{};
    history_ = {snapshot()};
    historyIndex_ = 0;
    lastEdited_ = AdjId::Count; // sentinel: no in-progress edit
  }

  bool canUndo() const { return historyIndex_ > 0; }
  bool canRedo() const { return historyIndex_ + 1 < history_.size(); }

  void undo() {
    if (!canUndo())
      return;
    restore(history_[--historyIndex_]);
    lastEdited_ = AdjId::Count; // next slider touch starts a new entry
  }
  void redo() {
    if (!canRedo())
      return;
    restore(history_[++historyIndex_]);
    lastEdited_ = AdjId::Count;
  }

  // Composes every non-zero adjustment's pointwise transform into a single
  // 256-entry lookup table, applied identically to R/G/B. Building the
  // table is O(256) regardless of image size — the expensive part of a
  // slider drag is applying it, and that's now one array lookup per
  // channel instead of a float multiply/add + lround + clamp per spec per
  // channel.
  std::array<uint8_t, 256> buildLut() const {
    std::array<uint8_t, 256> lut{};
    for (int i = 0; i < 256; ++i) {
      float x = static_cast<float>(i);
      for (const auto &spec : kAdjustmentSpecs) {
        float v = values_[static_cast<size_t>(spec.id)];
        if (v != 0.0f)
          // Round + clamp after EACH spec, not just once at the end, so
          // the result is bit-identical to running the specs as separate
          // per-pixel passes that each stored an 8-bit result (which is
          // what this pipeline did before it used a LUT).
          x = static_cast<float>(std::clamp(
              static_cast<int>(std::lround(spec.apply(x, v))), 0, 255));
      }
      lut[i] = static_cast<uint8_t>(x);
    }
    return lut;
  }

  // Non-destructive: always starts from `original`'s untouched pixels, so
  // re-running after any value change never compounds onto a prior result.
  std::shared_ptr<CanvasImage> applyTo(const CanvasImage &original) const {
    auto img = std::make_shared<CanvasImage>(original);
    const auto lut = buildLut();
    // Raw pointers rather than vector/array operator[] in the hot loop: in
    // an unoptimized (Debug) build every checked operator[] is a real
    // function call, which made this loop ~20x slower than Release.
    const uint8_t *lutp = lut.data();
    uint8_t *p = img->pixels.data();
    const size_t n = img->pixels.size();
    for (size_t i = 0; i + 3 < n; i += 4) {
      p[i] = lutp[p[i]];
      p[i + 1] = lutp[p[i + 1]];
      p[i + 2] = lutp[p[i + 2]];
    }
    return img;
  }

private:
  // Everything one undo step restores: slider values + committed crop.
  struct Snapshot {
    std::array<float, static_cast<size_t>(AdjId::Count)> values{};
    CropRect crop;
  };
  Snapshot snapshot() const { return {values_, crop_}; }
  void restore(const Snapshot &s) {
    values_ = s.values;
    crop_ = s.crop;
  }

  std::array<float, static_cast<size_t>(AdjId::Count)> values_{};
  CropRect crop_;
  std::vector<Snapshot> history_{Snapshot{}};
  size_t historyIndex_ = 0;
  AdjId lastEdited_ = AdjId::Count;
};

// ---------------- Crop tool state ----------------

// Selection-drag "handle" bitmask: which edges of the selection a drag is
// moving. Two bits (e.g. L|T) = a corner; one bit = an edge; kEdgeMove =
// the whole rectangle.
constexpr int kEdgeL = 1, kEdgeR = 2, kEdgeT = 4, kEdgeB = 8;
constexpr int kEdgeMove = 16;

// Aspect-ratio presets in the Crop pane. ratio (width / height): 0 = free,
// negative = "same as the image being cropped".
struct CropAspect {
  const char *label;
  float ratio;
};
static const std::array<CropAspect, 6> kCropAspects = {{
    {"Free", 0.0f},
    {"Original", -1.0f},
    {"1:1", 1.0f},
    {"4:3", 4.0f / 3.0f},
    {"3:2", 3.0f / 2.0f},
    {"16:9", 16.0f / 9.0f},
}};

// The live (not yet applied) selection rectangle and the drag currently
// editing it. Coordinates are in pixels of the image being shown
// (EditorState::image), not the original.
struct CropUi {
  float l = 0, t = 0, r = 0, b = 0;
  size_t aspectIdx = 0; // index into kCropAspects

  enum class Drag { None, New, Move, Resize } drag = Drag::None;
  int mask = 0;                // Resize: which edges (kEdge* bits)
  float ax = 0, ay = 0;        // New: the fixed anchor corner
  float grabX = 0, grabY = 0;  // Move: pointer offset from selection's l/t
};

struct EditorState {
  std::shared_ptr<CanvasImage> original; // untouched decoded pixels; null
                                         // until something is opened
  std::shared_ptr<CanvasImage> source;   // original with the committed crop
                                         // applied (== original if none) —
                                         // what adjustments are run on
  CropRect sourceCrop;                   // the crop `source` was built with
  std::shared_ptr<CanvasImage> image;    // source + adjustments applied —
                                         // this is what's drawn and exported
  std::string imagePath;                 // full path of the opened file
  float zoom = 1.0f;
  AdjustmentSet adjust; // current slider values + their undo/redo history
  bool dirty = true;

  // Set by a slider drag (many events per second while held). The actual
  // pixel recompute is deferred until canvasDirtySource is polled — which
  // happens at most once per rendered frame — instead of running once per
  // drag event, so a flurry of mouse-move events between two frames only
  // costs one recompute, not one per event.
  bool adjustDirty = false;

  // Interactive-preview path for large images. While a slider is being
  // dragged, adjustments are applied to `previewBase` (`source`
  // box-downsampled to roughly kPreviewMaxEdge on its long side) instead
  // of the full-resolution original, and the result (`previewImage`) is
  // drawn stretched to the document's full size. One full-resolution
  // recompute runs when the drag ends (View::onDragEnd). `previewBase` is
  // null for images already small enough that a preview wouldn't be
  // cheaper, in which case dragging just uses the full-resolution path.
  std::shared_ptr<CanvasImage> previewBase;
  std::shared_ptr<CanvasImage> previewImage;
  bool dragging = false;

  CropUi crop; // live crop selection; only shown/editable while the Crop pane
               // (activity 2) is open

  int activeActivity = 0; // -1 = sidebar closed
  float sidebarWidth = 220.0f;
  int openMenu = -1;      // -1 = no dropdown open, else its menu id
  bool maximized = false; // mirrors our own toggle (no getter in LiteUI)

  float docWidth() const {
    return image ? static_cast<float>(image->width) : kPlaceholderW;
  }
  float docHeight() const {
    return image ? static_cast<float>(image->height) : kPlaceholderH;
  }
  void markDirty() { dirty = true; }
  void markAdjustDirty() { adjustDirty = true; }
};

static EditorState state;

static std::string baseName(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

static void setZoom(float z) {
  state.zoom = std::clamp(z, kZoomMin, kZoomMax);
  state.markDirty();
}

static void syncSource(bool force);

// Rebuilds `state.image` (what's drawn and exported) from the untouched
// `state.source` pixels (the original, cropped if a crop is committed)
// using the current adjustment values. Called after every change to
// `state.adjust` — a slider drag, undo, redo, a crop, or opening a new
// image.
static void updateImageFromAdjustments() {
  if (!state.original)
    return;
  syncSource(false); // picks up a crop change from apply/undo/redo
  state.image = state.adjust.applyTo(*state.source);
  state.previewImage.reset(); // superseded by the full-resolution result
  state.markDirty();
}

// Box-downsamples `src` by the smallest integer factor that brings its
// long side to at most kPreviewMaxEdge. Returns null when `src` is already
// within kPreviewMaxEdge * kPreviewMinRatio — below that, a preview
// wouldn't be meaningfully cheaper than the full image, so the drag path
// just uses the full-resolution one. Runs once per opened image.
static std::shared_ptr<CanvasImage> makePreviewBase(const CanvasImage &src) {
  int longEdge = std::max(src.width, src.height);
  if (longEdge <= static_cast<int>(kPreviewMaxEdge * kPreviewMinRatio))
    return nullptr;
  int k = (longEdge + kPreviewMaxEdge - 1) / kPreviewMaxEdge; // ceil
  int w = src.width / k, h = src.height / k;
  if (w <= 0 || h <= 0)
    return nullptr;
  auto out = std::make_shared<CanvasImage>(w, h);
  const int area = k * k;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      int sum[4] = {0, 0, 0, 0};
      for (int dy = 0; dy < k; ++dy) {
        const uint8_t *row =
            &src.pixels[(static_cast<size_t>(y * k + dy) * src.width + x * k) *
                        4];
        for (int dx = 0; dx < k; ++dx)
          for (int c = 0; c < 4; ++c)
            sum[c] += row[dx * 4 + c];
      }
      uint8_t *o = &out->pixels[(static_cast<size_t>(y) * w + x) * 4];
      for (int c = 0; c < 4; ++c)
        o[c] = static_cast<uint8_t>((sum[c] + area / 2) / area);
    }
  return out;
}

// Like updateImageFromAdjustments(), but applied to the small preview
// instead of the full-resolution original. Only touches state.previewImage;
// state.image (and so the document size, export, and info labels) is left
// alone until the drag ends.
static void updatePreviewFromAdjustments() {
  if (!state.previewBase)
    return;
  state.previewImage = state.adjust.applyTo(*state.previewBase);
  state.markDirty();
}

// ---------------- Crop ----------------

// Copies the `r` region of `src` into a new image (clamped to src's bounds).
static std::shared_ptr<CanvasImage> cropImage(const CanvasImage &src,
                                              const CropRect &r) {
  const int x0 = std::clamp(r.x, 0, src.width);
  const int y0 = std::clamp(r.y, 0, src.height);
  const int x1 = std::clamp(r.x + r.w, x0, src.width);
  const int y1 = std::clamp(r.y + r.h, y0, src.height);
  const int w = x1 - x0, h = y1 - y0;
  auto out = std::make_shared<CanvasImage>(w, h);
  for (int y = 0; y < h; ++y) {
    const uint8_t *from =
        src.pixels.data() + (static_cast<size_t>(y0 + y) * src.width + x0) * 4;
    std::copy_n(from, static_cast<size_t>(w) * 4,
                out->pixels.data() + static_cast<size_t>(y) * w * 4);
  }
  return out;
}

static bool cropToolActive() {
  return state.activeActivity == 2 && state.image != nullptr;
}

// Width/height ratio the selection is locked to, or 0 for free-form.
static float cropAspect() {
  const float ratio = kCropAspects[state.crop.aspectIdx].ratio;
  if (ratio >= 0.0f)
    return ratio;
  return (state.source && state.source->height > 0)
             ? static_cast<float>(state.source->width) /
                   static_cast<float>(state.source->height)
             : 0.0f;
}

// Shrinks the selection (about its center) to the locked aspect ratio, so
// it always stays inside where it already was.
static void cropFitToAspect() {
  const float a = cropAspect();
  if (a <= 0.0f)
    return;
  auto &c = state.crop;
  float w = c.r - c.l, h = c.b - c.t;
  if (w <= 0.0f || h <= 0.0f)
    return;
  const float cx = (c.l + c.r) / 2.0f, cy = (c.t + c.b) / 2.0f;
  if (w / h > a)
    w = h * a;
  else
    h = w / a;
  c.l = cx - w / 2.0f;
  c.r = cx + w / 2.0f;
  c.t = cy - h / 2.0f;
  c.b = cy + h / 2.0f;
}

// Selection = the whole image being cropped (then fitted to the aspect lock).
static void resetCropSelection() {
  auto &c = state.crop;
  c.l = 0.0f;
  c.t = 0.0f;
  c.r = state.source ? static_cast<float>(state.source->width) : 0.0f;
  c.b = state.source ? static_cast<float>(state.source->height) : 0.0f;
  c.drag = CropUi::Drag::None;
  cropFitToAspect();
}

static void setCropAspect(size_t idx) {
  state.crop.aspectIdx = idx;
  cropFitToAspect();
  state.markDirty();
}

// Rebuilds `state.source` (and the interactive-preview base derived from
// it) when the committed crop no longer matches the one it was built with —
// after applying/removing a crop, an undo/redo across one, or (force) when a
// new image is opened. Also re-seeds the selection, since the image the
// selection is measured against just changed size.
static void syncSource(bool force) {
  if (!state.original)
    return;
  const CropRect crop = state.adjust.crop();
  if (!force && state.source && crop == state.sourceCrop)
    return;
  state.sourceCrop = crop;
  state.source = crop.empty() ? state.original
                              : cropImage(*state.original, crop);
  state.previewBase = makePreviewBase(*state.source);
  state.previewImage.reset();
  resetCropSelection();
}

// Which part of the selection is under (px, py) (image coordinates)?
// Returns kEdge* bits for a handle, kEdgeMove for the interior, 0 for none.
static int cropHitTest(float px, float py) {
  const auto &c = state.crop;
  const float tol = kCropHitPx / state.zoom;
  const bool inX = px >= c.l - tol && px <= c.r + tol;
  const bool inY = py >= c.t - tol && py <= c.b + tol;
  int m = 0;
  if (inY) {
    if (std::abs(px - c.l) <= tol)
      m |= kEdgeL;
    else if (std::abs(px - c.r) <= tol)
      m |= kEdgeR;
  }
  if (inX) {
    if (std::abs(py - c.t) <= tol)
      m |= kEdgeT;
    else if (std::abs(py - c.b) <= tol)
      m |= kEdgeB;
  }
  if (m)
    return m;
  if (px > c.l && px < c.r && py > c.t && py < c.b)
    return kEdgeMove;
  return 0;
}

// Sets the selection to the rectangle spanned by a fixed corner (ax, ay)
// and the pointer, honoring the aspect lock and the image bounds. Used for
// both "drag out a new selection" and "drag a corner handle".
static void cropDragCorner(float ax, float ay, float px, float py) {
  auto &c = state.crop;
  const float W = state.docWidth(), H = state.docHeight();
  const float a = cropAspect();
  const float sx = px >= ax ? 1.0f : -1.0f;
  const float sy = py >= ay ? 1.0f : -1.0f;
  float w = std::abs(px - ax), h = std::abs(py - ay);
  if (a > 0.0f) {
    w = std::max(w, h * a); // grow to whichever dimension asks for more
    h = w / a;
    const float maxW = sx > 0.0f ? W - ax : ax;
    const float maxH = sy > 0.0f ? H - ay : ay;
    if (w > maxW) {
      w = maxW;
      h = w / a;
    }
    if (h > maxH) {
      h = maxH;
      w = h * a;
    }
  }
  const float x2 = ax + sx * w, y2 = ay + sy * h;
  c.l = std::min(ax, x2);
  c.r = std::max(ax, x2);
  c.t = std::min(ay, y2);
  c.b = std::max(ay, y2);
}

// Drags one edge or corner of the existing selection to the pointer.
static void cropResize(int mask, float px, float py) {
  auto &c = state.crop;
  const float W = state.docWidth(), H = state.docHeight();
  const float minSz = std::min({kCropMinPx, W, H});
  const bool horiz = (mask & (kEdgeL | kEdgeR)) != 0;
  const bool vert = (mask & (kEdgeT | kEdgeB)) != 0;

  if (horiz && vert) { // corner: the opposite corner stays put
    cropDragCorner((mask & kEdgeL) ? c.r : c.l, (mask & kEdgeT) ? c.b : c.t,
                   px, py);
    return;
  }

  const float a = cropAspect();
  if (a <= 0.0f) { // free-form edge
    if (mask & kEdgeL)
      c.l = std::clamp(px, 0.0f, c.r - minSz);
    if (mask & kEdgeR)
      c.r = std::clamp(px, c.l + minSz, W);
    if (mask & kEdgeT)
      c.t = std::clamp(py, 0.0f, c.b - minSz);
    if (mask & kEdgeB)
      c.b = std::clamp(py, c.t + minSz, H);
    return;
  }

  // Aspect-locked edge: the dragged edge sets the primary dimension, and
  // the other dimension follows, centered on where the selection was.
  if (horiz) {
    const float cy = (c.t + c.b) / 2.0f;
    const bool left = (mask & kEdgeL) != 0;
    const float room = left ? c.r : W - c.l;
    const float roomByH = 2.0f * std::min(cy, H - cy) * a;
    float w = left ? c.r - px : px - c.l;
    w = std::clamp(w, minSz, std::max(minSz, std::min(room, roomByH)));
    const float h = w / a;
    if (left)
      c.l = c.r - w;
    else
      c.r = c.l + w;
    c.t = cy - h / 2.0f;
    c.b = cy + h / 2.0f;
  } else {
    const float cx = (c.l + c.r) / 2.0f;
    const bool top = (mask & kEdgeT) != 0;
    const float room = top ? c.b : H - c.t;
    const float roomByW = 2.0f * std::min(cx, W - cx) / a;
    float h = top ? c.b - py : py - c.t;
    h = std::clamp(h, minSz, std::max(minSz, std::min(room, roomByW)));
    const float w = h * a;
    if (top)
      c.t = c.b - h;
    else
      c.b = c.t + h;
    c.l = cx - w / 2.0f;
    c.r = cx + w / 2.0f;
  }
}

// Canvas mouse handlers. (lx, ly) are canvas-local screen px; dividing by
// the zoom gives image pixels.
static void cropPress(float lx, float ly) {
  state.openMenu = -1; // the canvas now swallows presses that used to fall
                       // through to the workspace's "close menu" handler
  if (!cropToolActive())
    return;
  auto &c = state.crop;
  const float px = std::clamp(lx / state.zoom, 0.0f, state.docWidth());
  const float py = std::clamp(ly / state.zoom, 0.0f, state.docHeight());
  const int hit = cropHitTest(px, py);
  if (hit == kEdgeMove) {
    c.drag = CropUi::Drag::Move;
    c.grabX = px - c.l;
    c.grabY = py - c.t;
  } else if (hit) {
    c.drag = CropUi::Drag::Resize;
    c.mask = hit;
  } else {
    // Empty area: start a new selection here. The old one stays until the
    // pointer actually moves, so a plain click doesn't wipe it out.
    c.drag = CropUi::Drag::New;
    c.ax = px;
    c.ay = py;
  }
}

static void cropDrag(float lx, float ly) {
  auto &c = state.crop;
  if (!cropToolActive() || c.drag == CropUi::Drag::None)
    return;
  const float W = state.docWidth(), H = state.docHeight();
  const float px = std::clamp(lx / state.zoom, 0.0f, W);
  const float py = std::clamp(ly / state.zoom, 0.0f, H);
  switch (c.drag) {
  case CropUi::Drag::New:
    cropDragCorner(c.ax, c.ay, px, py);
    break;
  case CropUi::Drag::Resize:
    cropResize(c.mask, px, py);
    break;
  case CropUi::Drag::Move: {
    const float w = c.r - c.l, h = c.b - c.t;
    c.l = std::clamp(px - c.grabX, 0.0f, W - w);
    c.t = std::clamp(py - c.grabY, 0.0f, H - h);
    c.r = c.l + w;
    c.b = c.t + h;
    break;
  }
  case CropUi::Drag::None:
    break;
  }
  state.markDirty();
}

// Commits the current selection as the crop (one undo step).
static void applyCrop() {
  if (!state.source)
    return;
  const auto &c = state.crop;
  const int sw = state.source->width, sh = state.source->height;
  const int x0 = std::clamp(static_cast<int>(std::lround(c.l)), 0, sw);
  const int y0 = std::clamp(static_cast<int>(std::lround(c.t)), 0, sh);
  const int x1 = std::clamp(static_cast<int>(std::lround(c.r)), x0, sw);
  const int y1 = std::clamp(static_cast<int>(std::lround(c.b)), y0, sh);
  const int w = x1 - x0, h = y1 - y0;
  if (w < 1 || h < 1)
    return;
  if (w == sw && h == sh)
    return; // selection covers everything: nothing to crop
  // The selection is relative to the already-cropped `source`; the stored
  // crop is relative to the untouched original, so add the existing offset.
  const CropRect base = state.adjust.crop(); // 0,0 when there's no crop yet
  state.adjust.setCrop(CropRect{base.x + x0, base.y + y0, w, h});
  updateImageFromAdjustments(); // syncSource() rebuilds source + selection
}

// Drops the committed crop, restoring the full original (one undo step).
static void removeCrop() {
  if (state.adjust.crop().empty())
    return;
  state.adjust.setCrop(CropRect{});
  updateImageFromAdjustments();
}

static void undo() {
  if (!state.adjust.canUndo())
    return;
  state.adjust.undo();
  updateImageFromAdjustments();
}

static void redo() {
  if (!state.adjust.canRedo())
    return;
  state.adjust.redo();
  updateImageFromAdjustments();
}

static void openImage() {
  auto path = openFilePicker(
      "Open Image", {{"Images", "*.png;*.jpg;*.jpeg"}, {"All Files", "*.*"}});
  if (!path)
    return; // user cancelled

  std::string err;
  auto decoded = liteui_image::decodeFile(*path, &err);
  if (!decoded)
    return; // TODO: surface `err` to the user

  state.original = std::make_shared<CanvasImage>(std::move(*decoded));
  state.imagePath = *path;
  state.zoom = 1.0f;    // start fresh at 100% on every newly opened image
  state.adjust.reset(); // and with no adjustment or crop applied yet
  state.dragging = false;
  syncSource(true); // source = original; builds previewBase, resets selection
  updateImageFromAdjustments(); // builds state.image from state.source
}

// ---------------- PNG export ----------------
// liteui.hpp only ships image *decoding* (see liteui_image::decodeFile
// above) — no encoder — so exporting is a small amount of platform code
// here, using the same libraries liteui.hpp already links in for us
// (libpng on Linux via CMakeLists' PkgConfig::PNG, WIC on Windows).

#if defined(_WIN32)

static bool encodePngFile(const std::string &path, const CanvasImage &img,
                          std::string *errorOut) {
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
    return false;
  }

  IWICStream *stream = nullptr;
  IWICBitmapEncoder *encoder = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  bool ok = false;
  do {
    if (FAILED(factory->CreateStream(&stream)))
      break;
    if (FAILED(stream->InitializeFromFilename(toWide(path).c_str(),
                                              GENERIC_WRITE)))
      break;
    if (FAILED(
            factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
      break;
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
      break;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)))
      break;
    if (FAILED(frame->Initialize(nullptr)))
      break;
    if (FAILED(frame->SetSize(static_cast<UINT>(img.width),
                              static_cast<UINT>(img.height))))
      break;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt)))
      break;
    if (FAILED(frame->WritePixels(static_cast<UINT>(img.height),
                                  static_cast<UINT>(img.width) * 4,
                                  static_cast<UINT>(img.pixels.size()),
                                  const_cast<BYTE *>(img.pixels.data()))))
      break;
    if (FAILED(frame->Commit()))
      break;
    ok = SUCCEEDED(encoder->Commit());
  } while (false);

  if (frame)
    frame->Release();
  if (encoder)
    encoder->Release();
  if (stream)
    stream->Release();
  factory->Release();
  if (weInitialized)
    CoUninitialize();

  if (!ok && errorOut)
    *errorOut = "WIC: PNG encode failed";
  return ok;
}

#else // Linux

static bool encodePngFile(const std::string &path, const CanvasImage &img,
                          std::string *errorOut) {
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp) {
    if (errorOut)
      *errorOut = "failed to open file for writing";
    return false;
  }

  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info) {
    if (png)
      png_destroy_write_struct(&png, nullptr);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng: failed to create write struct";
    return false;
  }

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng: error during write";
    return false;
  }

  png_init_io(png, fp);
  png_set_IHDR(png, info, static_cast<png_uint_32>(img.width),
               static_cast<png_uint_32>(img.height), 8, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  std::vector<png_bytep> rows(img.height);
  for (int y = 0; y < img.height; ++y)
    rows[y] = const_cast<png_bytep>(img.pixels.data() +
                                    static_cast<size_t>(y) * img.width * 4);
  png_write_image(png, rows.data());
  png_write_end(png, nullptr);

  png_destroy_write_struct(&png, &info);
  fclose(fp);
  return true;
}

#endif

static void exportImage() {
  if (!state.image)
    return; // nothing open to export

  auto path = saveFilePicker("Export as PNG", "export.png",
                             {{"PNG Image", "*.png"}}, "png");
  if (!path)
    return; // user cancelled

  std::string err;
  if (!encodePngFile(*path, *state.image, &err)) {
    // TODO: surface `err` to the user
  }
}

static void toggleSidebar() {
  state.activeActivity = (state.activeActivity == -1) ? 0 : -1;
  state.markDirty(); // the crop overlay shows only while the Crop pane is open
}

// ---------------- Small reusable pieces ----------------

// A titlebar menu button ("File", "View", ...) that opens a dropdown of
// {label, action} items below it when clicked. Only one menu is open at
// a time (EditorState::openMenu); picking an item runs its action and
// closes the menu.
static View
menuButton(const std::string &label, int id,
           std::vector<std::pair<std::string, std::function<void()>>> items) {
  View btn;
  btn.style.height = Size::full();
  btn.style.padding = EdgeInsets{0, 12, 0, 12};
  btn.style.justifyContent = Justify::Center;
  btn.style.alignItems = Align::Center;
  btn.style.hoverColor = kColorMenuHover;
  btn.style.backgroundColor = std::function<Color()>([id] {
    return state.openMenu == id ? kColorMenuHover : kColorTransparent;
  });
  btn.onClick = [id] { state.openMenu = (state.openMenu == id) ? -1 : id; };

  Text label_;
  label_.label = label;
  label_.fontSize = 13.0f;
  label_.color = kColorTextDark;
  btn.addChild(std::move(label_));

  View dropdown;
  dropdown.style.position = Position::Absolute;
  dropdown.style.top = kTitlebarH;
  dropdown.style.left = 0.0f;
  dropdown.style.width = Size::pixel(210);
  dropdown.style.direction = FlexDirection::Column;
  dropdown.style.backgroundColor = kColorWhite;
  dropdown.style.borderWidth = 1.0f;
  dropdown.style.borderColor = kColorDropdownBorder;
  dropdown.style.borderRadius = 4.0f;
  dropdown.style.padding = EdgeInsets::all(4.0f);
  dropdown.style.zIndex = 100;
  dropdown.style.display = std::function<Display()>(
      [id] { return state.openMenu == id ? Display::Flex : Display::None; });

  for (auto &item : items) {
    View row;
    row.style.height = Size::pixel(30);
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding = EdgeInsets{0, 10, 0, 10};
    row.style.borderRadius = 3.0f;
    row.style.hoverColor = kColorDropdownRowHover;
    std::function<void()> action = item.second;
    row.onClick = [action] {
      action();
      state.openMenu = -1;
    };

    Text t;
    t.label = item.first;
    t.fontSize = 13.0f;
    t.color = kColorTextDark;
    row.addChild(std::move(t));
    dropdown.addChild(std::move(row));
  }

  btn.addChild(std::move(dropdown));
  return btn;
}

// One of the three window-control buttons (minimize/maximize/close).
static View winButton(const std::string &glyph, Color hoverBg,
                      std::function<void()> onClick) {
  View v;
  v.style.width = Size::pixel(46);
  v.style.height = Size::full();
  v.style.justifyContent = Justify::Center;
  v.style.alignItems = Align::Center;
  v.style.hoverColor = hoverBg;
  v.onClick = std::move(onClick);

  Text t;
  t.label = glyph;
  t.fontSize = 13.0f;
  t.color = kColorTextMid;
  v.addChild(std::move(t));
  return v;
}

// A small square titlebar icon button (undo/redo, etc). `enabled` is
// optional: pass it to dim the glyph when the action isn't currently
// available — the click itself is always safe to wire up as a no-op in
// that case (undo()/redo() already check before doing anything).
static View titlebarIconBtn(const std::string &glyph,
                            std::function<void()> onClick,
                            std::function<bool()> enabled = {}) {
  View v;
  v.style.width = Size::pixel(28);
  v.style.height = Size::pixel(28);
  v.style.borderRadius = 4.0f;
  v.style.justifyContent = Justify::Center;
  v.style.alignItems = Align::Center;
  v.style.hoverColor = kColorMenuHover;
  v.onClick = std::move(onClick);

  Text t;
  t.label = glyph;
  t.fontSize = 14.0f;
  t.color = enabled ? Dynamic<Color>(std::function<Color()>([enabled] {
    return enabled() ? kColorTextDark : kColorTextFaint;
  }))
                    : Dynamic<Color>(kColorTextDark);
  v.addChild(std::move(t));
  return v;
}

// A full-width sidebar button. `selected` (optional) draws it highlighted —
// used for the aspect-ratio presets, which behave like radio buttons.
// `primary` gives it the accent fill for the pane's main action.
static View sidebarButton(const std::string &label,
                          std::function<void()> onClick,
                          std::function<bool()> selected = {},
                          bool primary = false) {
  View b;
  b.style.width = Size::full();
  b.style.height = Size::pixel(28);
  b.style.flexShrink = 0;
  b.style.borderRadius = 4.0f;
  b.style.alignItems = Align::Center;
  b.style.padding = EdgeInsets{0, 10, 0, 10};
  b.style.hoverColor = primary ? kColorStatusHover : kColorActivityHover;
  b.style.backgroundColor = std::function<Color()>([selected, primary]() -> Color {
    if (primary)
      return kColorStatusBarBg;
    return (selected && selected()) ? kColorDropdownRowHover
                                    : kColorActivityBarBg;
  });
  b.style.borderWidth = std::function<float()>(
      [selected] { return (selected && selected()) ? 1.0f : 0.0f; });
  b.style.borderColor = kColorDividerHover;
  b.onClick = std::move(onClick);

  Text t;
  t.label = label;
  t.fontSize = 13.0f;
  t.color = primary ? kColorWhite : kColorTextDark;
  b.addChild(std::move(t));
  return b;
}

// A single Lightroom-style slider row for one AdjustmentSpec: a label with
// the live value, then a draggable track. Reads and writes go through
// `state.adjust` (AdjustmentSet::get/set) rather than binding a raw
// float& the way a standalone slider widget would — that's what lets
// AdjustmentSet coalesce a whole drag into one undo step instead of one
// per frame. One call to this builds an entire slider; adding a new
// adjustment to kAdjustmentSpecs needs no new UI code at all.
static View buildAdjustmentSlider(const AdjustmentSpec &spec) {
  constexpr float kTrackW = 150.0f;
  constexpr float kTrackH = 4.0f;
  constexpr float kThumb = 16.0f;
  constexpr float kMaxThumbX = kTrackW - kThumb;

  const AdjId id = spec.id;
  const std::string name = spec.label;
  const float lo = spec.min;
  const float hi = spec.max;

  // Same bipolar-fill treatment as LiteUI's slider example: a range that
  // straddles zero (like ours, -100..100) fills outward from the center
  // instead of always from the left edge.
  const bool isBipolar = (lo < 0.0f && hi > 0.0f);
  const float anchor = isBipolar ? 0.0f : lo;

  auto thumbXFor = [lo, hi, kMaxThumbX](float v) {
    return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) * kMaxThumbX;
  };

  View group;
  group.style.direction = FlexDirection::Column;
  group.style.gap = 6.0f;

  Text rowLabel;
  rowLabel.label = std::function<std::string()>([id, name] {
    int v = static_cast<int>(std::lround(state.adjust.get(id)));
    return name + ": " + std::to_string(v);
  });
  rowLabel.fontSize = 13.0f;
  rowLabel.color = kColorTextMid;
  group.addChild(std::move(rowLabel));

  View track;
  track.style.width = Size::pixel(kTrackW);
  track.style.height = Size::pixel(kTrackH);
  track.style.backgroundColor = kColorActivityHover;
  track.style.borderRadius = kTrackH / 2.0f;

  // Filled portion, absolutely positioned so it can grow independently of
  // the thumb. Spans from the anchor's thumb-center position to the
  // current value's.
  View fill;
  fill.style.position = Position::Absolute;
  fill.style.top = 0.0f;
  fill.style.height = Size::pixel(kTrackH);
  fill.style.left = std::function<float()>([id, thumbXFor, anchor, isBipolar] {
    float anchorX =
        isBipolar ? thumbXFor(anchor) + kThumb / 2.0f : thumbXFor(anchor);
    float valueCenter = thumbXFor(state.adjust.get(id)) + kThumb / 2.0f;
    return std::min(anchorX, valueCenter);
  });
  fill.style.width = std::function<Size()>([id, thumbXFor, anchor, isBipolar] {
    float anchorX =
        isBipolar ? thumbXFor(anchor) + kThumb / 2.0f : thumbXFor(anchor);
    float valueCenter = thumbXFor(state.adjust.get(id)) + kThumb / 2.0f;
    return Size::pixel(std::abs(valueCenter - anchorX));
  });
  fill.style.backgroundColor = kColorDividerHover;
  fill.style.borderRadius = kTrackH / 2.0f;
  track.addChild(std::move(fill));

  // Shared between onPressAt and onDragTo across one drag gesture, same
  // pattern as the standalone slider widget this is adapted from.
  auto grabX = std::make_shared<float>(0.0f);

  View thumb;
  thumb.style.position = Position::Absolute;
  thumb.style.left = std::function<float()>(
      [id, thumbXFor] { return thumbXFor(state.adjust.get(id)); });
  thumb.style.top = -(kThumb - kTrackH) / 2.0f; // vertically centered
  thumb.style.width = Size::pixel(kThumb);
  thumb.style.height = Size::pixel(kThumb);
  thumb.style.backgroundColor = kColorWhite;
  thumb.style.borderWidth = 2.0f;
  thumb.style.borderColor = kColorDividerHover;
  thumb.style.borderRadius = kThumb / 2.0f;

  thumb.onPressAt = [grabX](float lx, float) { *grabX = lx; };
  thumb.onDragTo = [grabX, id, thumbXFor, lo, hi, kMaxThumbX](float lx, float) {
    if (!state.original)
      return; // no image loaded yet — nothing to adjust
    float newThumbX = std::clamp(
        thumbXFor(state.adjust.get(id)) + (lx - *grabX), 0.0f, kMaxThumbX);
    state.adjust.set(id, lo + (newThumbX / kMaxThumbX) * (hi - lo));
    state.dragging = true; // switches recompute+paint to the preview path
    // Cheap: just records the new value and asks for a recompute before
    // the next frame paints (see canvasDirtySource) — doesn't itself touch
    // a single pixel, so it stays fast no matter how often the drag fires.
    state.markAdjustDirty();
  };
  // Drag finished: leave preview mode and request one full-resolution
  // recompute (deferred to the next canvasDirtySource poll like any other
  // adjustment change — endPress polls right after this fires, so it lands
  // in the same frame the mouse is released).
  thumb.onDragEnd = [] {
    if (!state.dragging)
      return;
    state.dragging = false;
    state.markAdjustDirty();
  };
  track.addChild(std::move(thumb));

  group.addChild(std::move(track));
  return group;
}

// Draws the crop selection over the image: dimmed surround, rule-of-thirds
// guides, border, and eight drag handles. Expects the CTM to already be
// scaled by state.zoom (so everything here is in image pixels), and divides
// line widths / handle sizes by the zoom to keep them constant on screen.
static void paintCropOverlay(CanvasContext &ctx) {
  const auto &c = state.crop;
  const float W = state.docWidth(), H = state.docHeight();
  const float px = 1.0f / state.zoom; // one screen pixel, in image units
  const float sw = c.r - c.l, sh = c.b - c.t;

  ctx.setFillColor(Color{0, 0, 0, 120});
  ctx.fillRect(0, 0, W, c.t);                 // above
  ctx.fillRect(0, c.b, W, H - c.b);           // below
  ctx.fillRect(0, c.t, c.l, sh);              // left
  ctx.fillRect(c.r, c.t, W - c.r, sh);        // right

  ctx.setLineWidth(px);
  ctx.setStrokeColor(Color{255, 255, 255, 110});
  ctx.beginPath();
  for (int i = 1; i <= 2; ++i) {
    const float x = c.l + sw * static_cast<float>(i) / 3.0f;
    const float y = c.t + sh * static_cast<float>(i) / 3.0f;
    ctx.moveTo(x, c.t);
    ctx.lineTo(x, c.b);
    ctx.moveTo(c.l, y);
    ctx.lineTo(c.r, y);
  }
  ctx.stroke();

  ctx.setStrokeColor(kColorWhite);
  ctx.setLineWidth(px * 1.5f);
  ctx.strokeRect(c.l, c.t, sw, sh);

  const float hs = kCropHandlePx * px;
  const bool midX = sw > hs * 4.0f, midY = sh > hs * 4.0f;
  const float xs[3] = {c.l, (c.l + c.r) / 2.0f, c.r};
  const float ys[3] = {c.t, (c.t + c.b) / 2.0f, c.b};
  ctx.setFillColor(kColorWhite);
  ctx.setStrokeColor(Color{60, 60, 60, 255});
  ctx.setLineWidth(px);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      if (i == 1 && j == 1)
        continue; // center: no handle
      if ((i == 1 && !midX) || (j == 1 && !midY))
        continue; // edge midpoints crowd together on tiny selections
      ctx.fillRect(xs[i] - hs / 2.0f, ys[j] - hs / 2.0f, hs, hs);
      ctx.strokeRect(xs[i] - hs / 2.0f, ys[j] - hs / 2.0f, hs, hs);
    }
}

static void paintCanvas(CanvasContext &ctx) {
  LITEUI_PROF_START(profAppPaint);
  ctx.setFillColor(state.image ? kColorWhite : kColorCanvasPlaceholderBg);
  ctx.fillRect(0, 0, ctx.width(), ctx.height());

  if (!state.image || state.image->width <= 0 || state.image->height <= 0)
    return;

  // Drawn at the document's natural pixel size; the canvas itself is
  // already sized to docWidth*zoom / docHeight*zoom (see canvas.style
  // below), so a plain ctx.scale(zoom, zoom) here is all that's needed.
  ctx.save();
  ctx.scale(state.zoom, state.zoom);
  if (state.dragging && state.previewImage) {
    // Mid-drag preview: smaller image stretched to the full document size
    // (bilinear), so layout/zoom are unaffected by its lower resolution.
    ctx.drawImage(*state.previewImage, 0, 0, state.docWidth(),
                  state.docHeight());
  } else {
    ctx.drawImage(*state.image, 0, 0);
  }
  if (cropToolActive())
    paintCropOverlay(ctx);
  ctx.restore();
  LITEUI_PROF_ADD(appPaint, profAppPaint);
}

int main() {
  LiteUI ui("Photo Editor", 1100, 700, /*hideTitlebar=*/true);

  const std::vector<Activity> activities = {
      {0, "Info", "I"},
      {1, "Adjust", "A"},
      {2, "Crop", "C"},
  };

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = kColorRootBg;

  // Titlebar: title, File/View menus, window controls.

  View titlebar;
  titlebar.style.direction = FlexDirection::Row;
  titlebar.style.width = Size::full();
  titlebar.style.height = Size::pixel(kTitlebarH);
  titlebar.style.flexShrink = 0;
  titlebar.style.alignItems = Align::Center;
  titlebar.style.backgroundColor = kColorPanelBg;
  titlebar.style.borderWidth = 0.0f;
  // Empty space in the titlebar (i.e. not over a button) drags the
  // window, and also closes any open dropdown menu.
  titlebar.onPressAt = [&](float, float) {
    state.openMenu = -1;
    ui.requestMove();
  };

  Text appTitle;
  appTitle.label = std::string("Photo Editor");
  appTitle.fontSize = 13.0f;
  appTitle.fontWeight = FontWeight::SemiBold;
  appTitle.color = kColorTextDark;
  appTitle.style.margin = EdgeInsets{0, 16, 0, 14};
  titlebar.addChild(std::move(appTitle));

  titlebar.addChild(menuButton("File", 0,
                               {
                                   {"Open Image...", openImage},
                                   {"Export as PNG...", exportImage},
                                   {"Exit", [&] { ui.requestClose(); }},
                               }));
  titlebar.addChild(
      menuButton("View", 1,
                 {
                     {"Zoom In", [] { setZoom(state.zoom * kZoomStep); }},
                     {"Zoom Out", [] { setZoom(state.zoom / kZoomStep); }},
                     {"Actual Size (100%)", [] { setZoom(1.0f); }},
                     {"Toggle Sidebar", toggleSidebar},
                 }));

  View undoRedoGap;
  undoRedoGap.style.width = Size::pixel(8);
  titlebar.addChild(std::move(undoRedoGap));

  titlebar.addChild(
      titlebarIconBtn("\u21B6", undo, [] { return state.adjust.canUndo(); }));
  titlebar.addChild(
      titlebarIconBtn("\u21B7", redo, [] { return state.adjust.canRedo(); }));

  View titlebarSpacer;
  titlebarSpacer.style.flexGrow = 1;
  titlebarSpacer.style.height = Size::full();
  titlebar.addChild(std::move(titlebarSpacer));

  titlebar.addChild(
      winButton("\u2500", kColorActivityBarBg, [&] { ui.requestMinimize(); }));
  titlebar.addChild(winButton("\u25A1", kColorActivityBarBg, [&] {
    ui.requestMaximize();
    state.maximized = !state.maximized;
  }));
  titlebar.addChild(
      winButton("\u00D7", kColorCloseHover, [&] { ui.requestClose(); }));

  root.addChild(std::move(titlebar));

  // Workspace: activity bar, sidebar, divider, canvas viewport.

  View workspace;
  workspace.style.direction = FlexDirection::Row;
  workspace.style.width = Size::full();
  workspace.style.flexGrow = 1;
  // Clicking anywhere in the workspace that isn't itself a button
  // (e.g. the canvas, or blank sidebar space) closes an open menu too.
  workspace.onPressAt = [](float, float) { state.openMenu = -1; };

  // ---- Activity bar ----
  View activityBar;
  activityBar.style.width = Size::pixel(kActivityBarW);
  activityBar.style.height = Size::full();
  activityBar.style.flexShrink = 0;
  activityBar.style.direction = FlexDirection::Column;
  activityBar.style.alignItems = Align::Center;
  activityBar.style.backgroundColor = kColorActivityBarBg;
  activityBar.style.padding = EdgeInsets::all(6.0f);
  activityBar.style.gap = 10.0f;

  for (const auto &item : activities) {
    View button;
    button.style.width = Size::pixel(32);
    button.style.height = Size::pixel(32);
    button.style.borderRadius = 6.0f;
    button.style.justifyContent = Justify::Center;
    button.style.alignItems = Align::Center;
    button.style.hoverColor = kColorActivityHover;

    int id = item.id;
    button.style.backgroundColor = std::function<Color()>([id] {
      return state.activeActivity == id ? kColorWhite : kColorActivityBarBg;
    });
    button.onClick = [id] {
      state.activeActivity = (state.activeActivity == id) ? -1 : id;
      state.markDirty(); // show/hide the crop overlay
    };

    Text icon;
    icon.label = item.icon;
    icon.fontSize = 15.0f;
    icon.fontWeight = FontWeight::SemiBold;
    icon.color = kColorTextIcon;
    button.addChild(std::move(icon));

    activityBar.addChild(std::move(button));
  }

  workspace.addChild(std::move(activityBar));

  // ---- Sidebar (width driven by activeActivity: 0 when closed) ----
  View sidebar;
  sidebar.style.height = Size::full();
  sidebar.style.flexShrink = 0;
  sidebar.style.direction = FlexDirection::Column;
  sidebar.style.backgroundColor = kColorPanelBg;
  sidebar.style.padding = EdgeInsets::all(12.0f);
  sidebar.style.overflowX = Overflow::Hidden;
  sidebar.style.overflowY = Overflow::Auto;
  sidebar.style.width = std::function<Size()>([] {
    return Size::pixel(state.activeActivity != -1 ? state.sidebarWidth : 0.0f);
  });

  Text sidebarTitle;
  sidebarTitle.label =
      std::function<std::string()>([activities]() -> std::string {
        for (const auto &a : activities)
          if (a.id == state.activeActivity)
            return a.title;
        return "";
      });
  sidebarTitle.fontSize = 12.0f;
  sidebarTitle.fontWeight = FontWeight::SemiBold;
  sidebarTitle.color = kColorTextMuted;
  sidebarTitle.style.margin = EdgeInsets{0, 0, 10, 0};
  sidebar.addChild(std::move(sidebarTitle));

  // -- "Info" pane: real data about the open image.
  View infoPane;
  infoPane.style.direction = FlexDirection::Column;
  infoPane.style.width = Size::full();
  infoPane.style.gap = 6.0f;
  infoPane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 0 ? Display::Flex : Display::None; });

  Text infoName;
  infoName.label = std::function<std::string()>([] {
    return state.image ? ("File: " + baseName(state.imagePath))
                       : std::string("No image open");
  });
  infoName.fontSize = 13.0f;
  infoName.color = kColorTextMid;
  infoPane.addChild(std::move(infoName));

  Text infoDims;
  infoDims.label = std::function<std::string()>([] {
    if (!state.image)
      return std::string();
    return "Size: " + std::to_string(state.image->width) + " x " +
           std::to_string(state.image->height) + " px";
  });
  infoDims.fontSize = 13.0f;
  infoDims.color = kColorTextMid;
  infoPane.addChild(std::move(infoDims));

  Text infoZoom;
  infoZoom.label = std::function<std::string()>([] {
    return "Zoom: " +
           std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
  });
  infoZoom.fontSize = 13.0f;
  infoZoom.color = kColorTextMid;
  infoPane.addChild(std::move(infoZoom));

  sidebar.addChild(std::move(infoPane));

  // -- "Adjust" pane: one slider row per entry in kAdjustmentSpecs. Adding
  // a new adjustment later is a one-line addition to that table — this
  // loop, AdjustmentSet::applyTo, and buildAdjustmentSlider all need no
  // changes.
  View adjustPane;
  adjustPane.style.width = Size::full();
  adjustPane.style.direction = FlexDirection::Column;
  adjustPane.style.gap = 16.0f;
  adjustPane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 1 ? Display::Flex : Display::None; });

  for (const auto &spec : kAdjustmentSpecs)
    adjustPane.addChild(buildAdjustmentSlider(spec));

  sidebar.addChild(std::move(adjustPane));

  // -- "Crop" pane: aspect-ratio presets, a live readout, and the
  // Apply/Reset/Remove buttons. The selection itself is drawn and dragged
  // directly on the canvas (paintCropOverlay / cropPress / cropDrag).
  View cropPane;
  cropPane.style.width = Size::full();
  cropPane.style.direction = FlexDirection::Column;
  cropPane.style.gap = 6.0f;
  cropPane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 2 ? Display::Flex : Display::None; });

  Text cropHint;
  cropHint.label = std::function<std::string()>([]() -> std::string {
    return state.image
               ? "Drag on the image to choose an area. Drag the handles to "
                 "resize, or the inside to move."
               : "Open an image to crop it.";
  });
  cropHint.fontSize = 12.0f;
  cropHint.color = kColorTextMuted;
  cropHint.style.margin = EdgeInsets{0, 0, 6, 0};
  cropPane.addChild(std::move(cropHint));

  Text cropAspectTitle;
  cropAspectTitle.label = std::string("Aspect ratio");
  cropAspectTitle.fontSize = 12.0f;
  cropAspectTitle.fontWeight = FontWeight::SemiBold;
  cropAspectTitle.color = kColorTextMuted;
  cropPane.addChild(std::move(cropAspectTitle));

  for (size_t i = 0; i < kCropAspects.size(); ++i)
    cropPane.addChild(sidebarButton(
        kCropAspects[i].label, [i] { setCropAspect(i); },
        [i] { return state.crop.aspectIdx == i; }));

  Text cropInfo;
  cropInfo.label = std::function<std::string()>([]() -> std::string {
    if (!state.image)
      return std::string();
    const int w = static_cast<int>(std::lround(state.crop.r - state.crop.l));
    const int h = static_cast<int>(std::lround(state.crop.b - state.crop.t));
    return "Selection: " + std::to_string(w) + " x " + std::to_string(h) +
           " px";
  });
  cropInfo.fontSize = 13.0f;
  cropInfo.color = kColorTextMid;
  cropInfo.style.margin = EdgeInsets{8, 0, 4, 0};
  cropPane.addChild(std::move(cropInfo));

  cropPane.addChild(sidebarButton("Apply Crop", applyCrop, {}, true));
  cropPane.addChild(sidebarButton("Reset Selection", [] {
    resetCropSelection();
    state.markDirty();
  }));
  cropPane.addChild(sidebarButton("Remove Crop", removeCrop));

  sidebar.addChild(std::move(cropPane));

  workspace.addChild(std::move(sidebar));

  // ---- Vertical divider — drag to resize the sidebar ----
  View vDivider;
  vDivider.style.height = Size::full();
  vDivider.style.flexShrink = 0;
  vDivider.style.backgroundColor = kColorActivityHover;
  vDivider.style.hoverColor = kColorDividerHover;
  vDivider.style.width = std::function<Size()>([] {
    return Size::pixel(state.activeActivity != -1 ? kDividerW : 0.0f);
  });

  auto updateSidebarWidth = [](float localX) {
    float next = state.sidebarWidth + localX - kDividerW / 2.0f;
    state.sidebarWidth = std::clamp(next, kMinSidebarW, kMaxSidebarW);
  };
  vDivider.onPressAt = [updateSidebarWidth](float localX, float) {
    updateSidebarWidth(localX);
  };
  vDivider.onDragTo = [updateSidebarWidth](float localX, float) {
    updateSidebarWidth(localX);
  };

  workspace.addChild(std::move(vDivider));

  // ---- Scrollable canvas viewport ----
  View viewport;
  viewport.style.height = Size::full();
  viewport.style.flexGrow = 1;
  viewport.style.backgroundColor = kColorViewportBg;
  viewport.style.overflowX = Overflow::Auto;
  viewport.style.overflowY = Overflow::Auto;
  viewport.style.justifyContent = Justify::Center;
  viewport.style.alignItems = Align::Center;
  viewport.style.padding = EdgeInsets::all(24);
  viewport.style.wheelScrollEnabled = false;
  viewport.style.scrollThumbColor = kColorScrollThumb;
  viewport.style.scrollTrackColor = kColorActivityHover;
  viewport.style.scrollbarAutoHide = true;
  // Only occupies the slot in `workspace` while an image is open; the
  // "no image" placeholder below is workspace's other child for that
  // same slot and shows exactly when this one doesn't.
  viewport.style.display = std::function<Display()>(
      [] { return state.image ? Display::Flex : Display::None; });

  View canvas;
  canvas.isCanvas = true;
  canvas.style.width = std::function<Size()>(
      [] { return Size::pixel(state.docWidth() * state.zoom); });
  canvas.style.height = std::function<Size()>(
      [] { return Size::pixel(state.docHeight() * state.zoom); });
  canvas.style.borderWidth = 1.0f;
  canvas.style.borderColor = kColorCanvasBorder;
  canvas.style.backgroundColor = kColorWhite;

  canvas.onPaint = [](CanvasContext &ctx) { paintCanvas(ctx); };
  canvas.canvasDirtySource = [] {
    // Polled once per frame (see liteui.hpp's checkForUpdates) — the right
    // place to do the deferred, expensive part of a slider drag exactly
    // once per frame, no matter how many onDragTo events fired since the
    // last poll.
    if (state.adjustDirty) {
      state.adjustDirty = false;
      LITEUI_PROF_START(profRecompute);
      if (state.dragging && state.previewBase)
        updatePreviewFromAdjustments(); // cheap: small image, mid-drag
      else
        updateImageFromAdjustments(); // full-res: rebuilds state.image
      LITEUI_PROF_ADD(recompute, profRecompute);
    }
    bool d = state.dirty;
    state.dirty = false;
    return d;
  };
  // Crop-selection dragging (no-ops unless the Crop pane is open).
  canvas.onPressAt = [](float lx, float ly) { cropPress(lx, ly); };
  canvas.onDragTo = [](float lx, float ly) { cropDrag(lx, ly); };
  canvas.onDragEnd = [] { state.crop.drag = CropUi::Drag::None; };
  canvas.onScrollUp = [] { setZoom(state.zoom * kZoomStep); };
  canvas.onScrollDown = [] { setZoom(state.zoom / kZoomStep); };

  viewport.addChild(std::move(canvas));

  workspace.addChild(std::move(viewport));

  View noImagePlaceholder;
  noImagePlaceholder.style.height = Size::full();
  noImagePlaceholder.style.flexGrow = 1;
  noImagePlaceholder.style.backgroundColor = kColorViewportBg;
  noImagePlaceholder.style.justifyContent = Justify::Center;
  noImagePlaceholder.style.alignItems = Align::Center;
  noImagePlaceholder.style.display = std::function<Display()>(
      [] { return state.image ? Display::None : Display::Flex; });

  Text noImageText;
  noImageText.label = std::string("No image selected");
  noImageText.fontSize = 14.0f;
  noImageText.color = kColorTextFaint;
  noImagePlaceholder.addChild(std::move(noImageText));

  workspace.addChild(std::move(noImagePlaceholder));

  root.addChild(std::move(workspace));

  // Status bar: image info on the left, zoom controls on the right.

  View statusBar;
  statusBar.style.width = Size::full();
  statusBar.style.height = Size::pixel(kStatusBarH);
  statusBar.style.flexShrink = 0;
  statusBar.style.direction = FlexDirection::Row;
  statusBar.style.alignItems = Align::Center;
  statusBar.style.justifyContent = Justify::SpaceBetween;
  statusBar.style.backgroundColor = kColorStatusBarBg;
  statusBar.style.padding = EdgeInsets{0, 10, 0, 12};

  Text statusLeft;
  statusLeft.label = std::function<std::string()>([] {
    if (!state.image)
      return std::string("No image open");
    return baseName(state.imagePath) + "  —  " +
           std::to_string(state.image->width) + " x " +
           std::to_string(state.image->height) + " px";
  });
  statusLeft.fontSize = 12.0f;
  statusLeft.color = kColorWhite;
  statusBar.addChild(std::move(statusLeft));

  // Right side of the status bar: zoom controls.
  auto statusGlyphBtn = [](const std::string &label,
                           std::function<void()> onClick) {
    View v;
    v.style.width = Size::pixel(20);
    v.style.height = Size::pixel(20);
    v.style.borderRadius = 3.0f;
    v.style.justifyContent = Justify::Center;
    v.style.alignItems = Align::Center;
    v.style.hoverColor = kColorStatusHover;
    v.onClick = std::move(onClick);
    Text t;
    t.label = label;
    t.fontSize = 12.0f;
    t.color = kColorWhite;
    v.addChild(std::move(t));
    return v;
  };

  View zoomControls;
  zoomControls.style.direction = FlexDirection::Row;
  zoomControls.style.alignItems = Align::Center;
  zoomControls.style.gap = 4.0f;

  zoomControls.addChild(
      statusGlyphBtn("-", [] { setZoom(state.zoom / kZoomStep); }));

  Text zoomLabel;
  zoomLabel.label = [] {
    return std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
  };
  zoomLabel.fontSize = 12.0f;
  zoomLabel.color = kColorWhite;
  zoomLabel.style.margin = EdgeInsets{0, 4, 0, 4};
  zoomControls.addChild(std::move(zoomLabel));

  zoomControls.addChild(
      statusGlyphBtn("+", [] { setZoom(state.zoom * kZoomStep); }));

  View zoomResetSpacer;
  zoomResetSpacer.style.width = Size::pixel(6);
  zoomControls.addChild(std::move(zoomResetSpacer));

  View zoomResetBtn;
  zoomResetBtn.style.justifyContent = Justify::Center;
  zoomResetBtn.style.alignItems = Align::Center;
  zoomResetBtn.style.hoverColor = kColorStatusHover;
  zoomResetBtn.style.borderRadius = 3.0f;
  zoomResetBtn.style.padding = EdgeInsets{2, 6, 2, 6};
  zoomResetBtn.onClick = [] { setZoom(1.0f); };
  {
    Text t;
    t.label = std::string("100%");
    t.fontSize = 12.0f;
    t.color = kColorWhite;
    zoomResetBtn.addChild(std::move(t));
  }
  zoomControls.addChild(std::move(zoomResetBtn));

  statusBar.addChild(std::move(zoomControls));

  root.addChild(std::move(statusBar));

  ui.setRoot(std::move(root));
  ui.run();
  return 0;
}