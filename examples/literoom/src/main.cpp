// Simple Photo Editor.

#include "liteui.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// Engine headers: pure logic with no UI, each covered by its own unit
// test in tests/ (see CMakeLists.txt). Grouped by folder, lowest layer first.

// core: shared vocabulary
#include "core/pixel_math.hpp"
#include "core/geometry.hpp"
#include "core/adjustments.hpp"
#include "core/raster_ops.hpp"

// color
#include "color/color_mix.hpp"
#include "color/color_grade.hpp"
#include "color/color_pass.hpp"
#include "color/curves.hpp"

// detail effects
#include "detail/detail_layer.hpp"
#include "detail/neighbor_fx.hpp"

// masks and object selection
#include "mask/mask.hpp"
#include "mask/seg_basic.hpp"
#include "mask/sam_segmenter.hpp"

// saving and files
#include "io/file_utils.hpp"
#include "io/minijson.hpp"
#include "io/exif.hpp"
#include "io/image_meta.hpp"
#include "io/image_codec.hpp"

// edit state and rendering
#include "edit/adjustment_set.hpp"
#include "render/render.hpp"
#include "render/histogram.hpp"

// background work
#include "workers/latest_worker.hpp"
#include "workers/thumb_worker.hpp"

constexpr float kSamSoftness = 2.0f; // logit -> coverage steepness (edge softness)

// ---------------- Layout constants ----------------
constexpr float kTitlebarH = 36.0f;
constexpr float kStatusBarH = 26.0f;
constexpr float kActivityBarW = 48.0f;
constexpr float kDividerW = 4.0f;
constexpr float kMinSidebarW = 180.0f;
constexpr float kMaxSidebarW = 420.0f;

// Right-hand panel of the Masks pane (the selected mask's controls).
constexpr float kMinMaskPanelW = 300.0f;
constexpr float kMaxMaskPanelW = 480.0f;
constexpr float kMaskPanelPad = 12.0f; // panel padding, each side
constexpr float kMaskTrackW = 240.0f;  // slider track length in that panel
                                       // (kMinMaskPanelW - 2 * pad - scrollbar
                                       // must stay >= this)

// Zoom range/step.
constexpr float kZoomMin = 0.1f;
constexpr float kZoomMax = 8.0f;
constexpr float kZoomStep = 1.25f;

// Crop tool. Sizes marked "screen px" are constant on screen regardless of
// zoom (the overlay divides by state.zoom when drawing/hit-testing).
constexpr float kCropMinPx = 16.0f;   // smallest selection edge, image px
constexpr float kCropHandlePx = 8.0f; // drawn handle size, screen px
constexpr float kCropHitPx = 10.0f;   // handle grab tolerance, screen px

// Color Mixer tab width (the engine constants live in color_mix.hpp).
constexpr float kMixTabW = 46.0f; // Hue/Sat/Lum tab width (3 + gaps = 150)

// Histogram canvas height (the counting constants live in histogram.hpp).
constexpr float kHistHeightPx = 80.0f;

// Clipping warnings: corner triangles on the histogram (the canvas overlay's
// constants live in histogram.hpp too).
constexpr float kHistTriPx = 9.0f;     // triangle leg length
constexpr float kHistTriHitPx = 18.0f; // click zone in each top corner

// Tone curve editor (canvas is square, kCurveSizePx on a side; the plot area
// is inset by kCurvePadPx so the end points aren't clipped).
constexpr float kCurveSizePx = 180.0f;
constexpr float kCurvePadPx = 8.0f;
constexpr float kCurveHitPx = 10.0f;  // point grab tolerance, screen px
constexpr float kCurvePointPx = 8.0f; // drawn point size, screen px
constexpr float kCurveTabW =
    42.0f; // channel tab width (4 tabs + gaps = canvas width)

// Document size before any image has been opened, so there's a visible
// page to scroll/zoom around from the start.
constexpr float kPlaceholderW = 800.0f;
constexpr float kPlaceholderH = 600.0f;

// Activity id of the History panel (see the activities list in main).
constexpr int kHistoryActivity = 9;

// Masks, UI side: tint-bitmap sizes and overlay handle sizes. The engine
// constants live next to their algorithms: mask_geom.hpp (radii), mask_brush.hpp,
// mask_range.hpp, mask_object.hpp, seg_basic.hpp and mask.hpp (list limits).
constexpr int kCompTintEdge = 512;    // long side of a combined mask's tint
constexpr float kMaskHandlePx = 9.0f; // overlay handle size, screen px
constexpr float kMaskHitPx = 12.0f;   // overlay handle grab tolerance, px
constexpr int kBrushTintEdge = 512;   // long side of the brush tint bitmap
constexpr int kLumTintEdge = 512;     // long side of the on-canvas tint bitmap

// ---------------- Palette (dark, Lightroom-like) ----------------
// Base swatches.
constexpr Color kPanelBg{68, 68, 68};
constexpr Color kHeaderBg{40, 40, 40};
constexpr Color kTitle{176, 176, 176};
constexpr Color kLabel{160, 160, 160};
constexpr Color kValue{232, 232, 232};
constexpr Color kDivider{46, 46, 46};
constexpr Color kTrackFill{32, 32, 32};
constexpr Color kTrackEdge{104, 104, 104};
constexpr Color kThumbFill{196, 196, 196};
constexpr Color kThumbEdge{28, 28, 28};
constexpr Color kIconRing{205, 205, 205};
constexpr Color kIconActiveBg{92, 92, 92};
constexpr Color kTransparent{0, 0, 0, 0};

// Semantic names used throughout the UI, mapped onto the swatches above.
constexpr Color kColorTransparent = kTransparent;
constexpr Color kColorWhite{255, 255, 255, 255}; // real white (image, glyphs)

// Chrome backgrounds.
constexpr Color kColorRootBg = kHeaderBg;
constexpr Color kColorPanelBg = kPanelBg;             // sidebar
constexpr Color kColorTitlebarBg = kHeaderBg;         // titlebar
constexpr Color kColorActivityBarBg = kHeaderBg;      // activity bar, idle buttons
constexpr Color kColorMenuHover{62, 62, 62, 255};     // menu / win-btn hover
constexpr Color kColorActivityHover{84, 84, 84, 255}; // button / tab hover
constexpr Color kColorActivityActive = kIconActiveBg; // selected activity btn
constexpr Color kColorDivider = kDivider;             // divider bg
constexpr Color kColorTrack = kTrackFill;             // slider + scroll track
constexpr Color kColorScrollThumb = kTrackEdge;

// Text.
constexpr Color kColorTextDark = kValue;             // titles, menu labels
constexpr Color kColorTextMid = kTitle;              // info pane, win-btn glyphs
constexpr Color kColorTextIcon = kIconRing;          // activity bar icons
constexpr Color kColorTextMuted = kLabel;            // section titles
constexpr Color kColorTextFaint{120, 120, 120, 255}; // placeholders

// Dropdown menu.
constexpr Color kColorDropdownBg = kHeaderBg;
constexpr Color kColorDropdownBorder = kThumbEdge;
constexpr Color kColorDropdownRowHover = kIconActiveBg; // also "selected"

// Window controls / accents.
constexpr Color kColorCloseHover{232, 17, 35, 255};
constexpr Color kColorDividerHover{100, 140, 220, 255}; // accent (drag, fills)
constexpr Color kColorAccent{0, 122, 204, 255};         // primary button
constexpr Color kColorAccentHover{30, 140, 220, 255};

// Canvas / viewport.
constexpr Color kColorCanvasPlaceholderBg{52, 52, 52, 255};
constexpr Color kColorViewportBg{30, 30, 30, 255};
constexpr Color kColorCanvasBorder = kTrackEdge;
constexpr Color kColorEmptyCorner{50, 50, 50, 255}; // behind transparent
                                                    // corners of a
                                                    // straightened image

// Status bar.
constexpr Color kColorStatusBarBg = kHeaderBg;
constexpr Color kColorStatusHover = kPanelBg;
constexpr Color kColorStatusError{255, 214, 102, 255};

// A sidebar tab in the activity bar.
struct Activity
{
  int id;
  std::string title;
  std::string icon; // single glyph
};

// Display color of each Color Mixer band (UI only; the math is in
// color_mix.hpp).
static Color mixBandColor(size_t band)
{
  static const Color colors[kMixBandCount] = {
      {230, 60, 60, 255}, {240, 140, 50, 255}, {225, 195, 40, 255}, {70, 180, 80, 255}, {50, 185, 190, 255}, {60, 110, 230, 255}, {140, 80, 210, 255}, {215, 70, 170, 255}};
  return colors[band];
}

// BEGIN renderworker
// ---------------- Background render worker ----------------
// Runs the full-resolution pixel pass off the UI thread so releasing a slider
// on a 24-megapixel photo doesn't freeze the window.
//
// Thread model: everything public here is called from the UI thread only. The
// one worker thread (started lazily on the first submit, so small images
// never spawn it) sleeps until a job is waiting, renders it, and parks the
// result in a slot. It touches nothing else in the program: no `state`, no
// LiteUI calls. Its only inputs are an immutable source image and a
// RenderParams copy; its only output is the parked result.
//
// Stale work is handled with a generation number. Every submit() and
// invalidate() bumps it; a result is only ever accepted if its generation
// still equals the newest one, and the running render polls the same number
// (see renderWith) so it abandons itself within a few milliseconds of being
// superseded. Only the newest request can win, so jobs are never queued up:
// a newer submit simply replaces one still waiting.
// The thread, the generation protocol and the parking slot live in
// latest_worker.hpp (LatestWorker, tested on its own); what is specific to
// rendering is just the job below.
struct RenderJob
{
  std::shared_ptr<const CanvasImage> src;
  RenderParams params;
};
struct RenderResult
{
  std::shared_ptr<CanvasImage> image;
};

// Worker thread only.
static std::optional<RenderResult> runRenderJob(RenderJob &&job, WorkerCancel c)
{
  auto img = renderWith(*job.src, job.params, c.latest, c.gen);
  if (!img)
    return std::nullopt; // abandoned part-way: superseded
  return RenderResult{std::move(img)};
}

// `src` in a submitted RenderJob must never be modified afterwards (the
// editor's source images are replaced, never edited in place).
using RenderWorker = LatestWorker<RenderJob, RenderResult>;
// END renderworker

static RenderWorker renderWorker{runRenderJob};

// ---------------- Crop tool state ----------------

// Selection-drag "handle" bitmask: which edges of the selection a drag is
// moving. Two bits (e.g. L|T) = a corner; one bit = an edge; kEdgeMove =
// the whole rectangle.
constexpr int kEdgeL = 1, kEdgeR = 2, kEdgeT = 4, kEdgeB = 8;
constexpr int kEdgeMove = 16;

// Aspect-ratio presets in the Crop pane. ratio (width / height): 0 = free,
// negative = "same as the image being cropped".
struct CropAspect
{
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
struct CropUi
{
  float l = 0, t = 0, r = 0, b = 0;
  size_t aspectIdx = 0; // index into kCropAspects

  enum class Drag
  {
    None,
    New,
    Move,
    Resize
  } drag = Drag::None;
  int mask = 0;               // Resize: which edges (kEdge* bits)
  float ax = 0, ay = 0;       // New: the fixed anchor corner
  float grabX = 0, grabY = 0; // Move: pointer offset from selection's l/t
};

// Mask-overlay drag in progress (see maskPress / maskDrag).
struct MaskUi
{
  enum class Drag
  {
    None,
    Move,
    ResizeX,
    ResizeY,
    LinEnd,
    LinStart,
    Paint // a brush stroke is being painted
  } drag = Drag::None;
  float grabX = 0, grabY = 0; // Move: pointer offset from the mask's center

  // Brush tool. These settings apply to the NEXT stroke; every stroke keeps
  // the values it was painted with. Size is a radius, as a fraction of the
  // SHORTER side of the picture on screen.
  float brushSize = kBrushDefaultSize;
  float brushFeather = 0.5f;
  float brushDensity = 1.0f;
  float brushFlow = 1.0f;
  bool brushErase = false;
  bool brushTint = true;      // paint the selected brush's coverage in red
  float lastX = 0, lastY = 0; // Paint: last recorded point, document px
  float curX = 0, curY = 0;   // Paint: pointer, document px (cursor ring)

  // Object tool.
  bool objBg = false; // plain clicks add BACKGROUND points (touchpad-friendly
                      // alternative to right-click)
  struct ObjPending
  {
    int mask = -1, part = -1;
    std::vector<ObjectPrompt> prompts; // original-picture fractions
  } objPending;

  // The red coverage bitmap of the selected brush mask, rebuilt only when the
  // strokes (or the picture's size) change.
  std::shared_ptr<CanvasImage> tint;
  BrushShape tintShape;
  bool tintInvert = false;

  // The red selection bitmap of the selected luminosity mask. It is built
  // from the picture as it stands just BEFORE that mask (see
  // luminosityTintImage), so it is rebuilt when the shape, the mask's place in
  // the list, or the pixels change. The pixels are keyed by the identity of
  // the images on screen; weak pointers, so no picture is kept alive and no
  // address can be mistaken for a newer image that reuses it.
  std::shared_ptr<CanvasImage> lumTint;
  LuminosityShape lumShape;
  bool lumInvert = false;
  int lumSel = -1;
  std::weak_ptr<CanvasImage> lumKeyImage, lumKeyPreview;
  // Small stand-in for state.source the tint is computed on.
  std::shared_ptr<CanvasImage> lumBase;
  std::weak_ptr<CanvasImage> lumBaseSrc;

  // Which part of the selected mask the canvas and the shape sliders edit.
  // Read it through selPart(), which keeps it in range.
  int part = 0;

  // The red bitmap of a selected mask with SEVERAL parts: the whole
  // combination, evaluated like the renderer does (see compositeTintImage).
  std::shared_ptr<CanvasImage> compTint;
  std::vector<MaskPart> compParts; // in-view form, the cache key
  bool compInvert = false;
  bool compUsesPix = false;
  int compSel = -1, compW = 0, compH = 0;
  std::weak_ptr<CanvasImage> compKeyImage, compKeyPreview;
  uint64_t encodedKey = 0; // imageSerial the segmenter has already analyzed
};

// ---------------- Filmstrip and Before / After state ----------------
// One picture in the open folder, as the filmstrip shows it.
struct FilmEntry
{
  std::string path;                         // full path (UTF-8)
  std::string name;                         // file name only
  std::shared_ptr<const CanvasImage> thumb; // null until made
  bool failed = false;                      // couldn't be decoded: plain box
  bool edited = false;                      // has saved edits (a sidecar file)
};

// The pictures of the folder the open picture lives in (JPEG / PNG, natural
// name order), plus the strip's scroll position. UI thread only.
struct FilmStrip
{
  std::vector<FilmEntry> files;
  std::unordered_map<std::string, size_t> byName; // lower-case name -> index
  std::string folder;
  int current = -1;          // index of the picture that is open
  int target = -1;           // the one last asked for (== current when idle)
  float scrollX = 0.0f;      // pixels scrolled
  float viewW = 0.0f;        // strip width at the last paint
  bool visible = true;       // View > Toggle Filmstrip
  bool dirty = true;         // repaint wanted
  uint64_t thumbVersion = 0; // adjust.version() when the open picture's
                             // thumbnail was last (re)made
};

// What the canvas shows: the edited picture, the same picture with every
// edit switched off (crop and rotation stay), or both side by side.
enum class CompareMode
{
  Off,
  Before,
  Split
};

struct EditorState
{
  std::shared_ptr<CanvasImage> original; // untouched decoded pixels; null
                                         // until something is opened
  std::shared_ptr<CanvasImage> oriented; // original rotated/flipped per
                                         // adjust.orient() (== original if
                                         // none); crops are measured on this
  Orient orientedFor;                    // the orientation `oriented` has
  std::shared_ptr<CanvasImage>
      orientedPreview;                  // `oriented` downsampled for
                                        // live angle drags (null if small)
  std::shared_ptr<CanvasImage> rotated; // oriented, straightened by the
                                        // angle (== oriented at 0 degrees);
                                        // crops are measured on this
  float rotatedAngle = 0.0f;            // the angle `rotated` was built with
  bool rotatedExpand = false;           // ...and its corner mode
  std::shared_ptr<CanvasImage> source;  // rotated with the committed crop
                                        // applied (== rotated if none) —
                                        // what adjustments are run on
  CropRect sourceCrop;                  // the crop `source` was built with
  std::shared_ptr<CanvasImage> image;   // source + adjustments applied —
                                        // this is what's drawn and exported
  std::string imagePath;                // full path of the opened file
  float zoom = 1.0f;
  AdjustmentSet adjust;      // current slider values + their undo/redo history
  uint64_t sidecarSaved = 0; // adjust.version() at the last sidecar save / load

  // Export pane settings.
  bool exportJpeg = true;   // the format the Export dialog offers first
  int jpegQuality = 92;     // 1..100
  bool keepMetadata = true; // carry the photo's EXIF + ICC profile into a JPEG

  FilmStrip film;                         // the open folder's pictures (bottom strip)
  CompareMode compare = CompareMode::Off; // Before / After view
  float splitPos = 0.5f;                  // Split view: line position, 0..1 of width
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

  // Masks pane (activity 7). The masks themselves live in `adjust` (so they
  // are undoable); this is only which one is selected and how it is shown.
  int maskSel = -1;          // index into adjust.masks(); read it through selMask()
  bool maskShowTint = false; // paint the selected mask's weight in red
  MaskUi mask;

  int activeActivity = 0; // -1 = sidebar closed
  float sidebarWidth = 220.0f;
  float maskPanelW = 300.0f; // right-hand mask panel (Masks pane only)
  int maskTab = 0;           // its tab: 0 Effect, 1 Shape, 2 Parts
  int maskFx = 0;            // Effect tab: 0 Basic, 1 Mix, 2 Grade, 3 Curve
  int openMenu = -1;         // -1 = no dropdown open, else its menu id
  bool maximized = false;    // mirrors our own toggle (no getter in LiteUI)

  // One-line message shown in the status bar after the image info: open and
  // export failures, and export success. Cleared by the next open, export or
  // edit (see clearStatus's callers).
  std::string statusMsg;
  bool statusIsError = false;

  // While an angle drag shows its low-res preview, the picture's size is
  // changing every frame but `image` (full-res) isn't rebuilt until the drag
  // ends — so the document size comes from these overrides instead.
  float previewDocW = 0.0f, previewDocH = 0.0f;
  bool dragIsAngle = false; // the drag in progress is the angle slider

  uint64_t imageSerial = 0; // bumps on every opened file (keys segmenter caches)

  // True while `previewImage` stands in for `image`: during a drag, and after
  // it until the background full-resolution render lands (renderWorker).
  bool showPreview() const
  {
    return previewImage && (dragging || renderWorker.outstanding());
  }

  bool previewDocActive() const
  {
    return dragging && previewImage && previewDocW > 0.0f;
  }
  float docWidth() const
  {
    if (previewDocActive())
      return previewDocW;
    return image ? static_cast<float>(image->width) : kPlaceholderW;
  }
  float docHeight() const
  {
    if (previewDocActive())
      return previewDocH;
    return image ? static_cast<float>(image->height) : kPlaceholderH;
  }
  void markDirty() { dirty = true; }
  void markAdjustDirty() { adjustDirty = true; }

  // Histogram of what is currently shown (the mid-drag preview while a
  // slider is held, else the full-resolution result). Recomputed lazily by
  // the histogram canvas's dirty poll when `histStale` is set; `histVersion`
  // bumps on every recompute so that canvas knows to repaint.
  std::array<uint32_t, 256> histR{}, histG{}, histB{}, histL{};
  bool histStale = true;
  uint32_t histVersion = 0;

  // Clipping warnings. The flags say which overlays are on; histClipLo/Hi
  // are the channels (bit 0/1/2 = R/G/B) that have any clipped pixel in the
  // shown picture (drives the triangle colors). clipPerChannel colors the
  // overlay by clipped channel (Shift+J) instead of classic red/blue.
  // clipOverlay is rebuilt by computeHistogram and is null while both
  // overlays are off.
  bool clipShadowsOn = false, clipHighlightsOn = false;
  uint8_t histClipLo = 0, histClipHi = 0;
  bool clipPerChannel = false;
  std::shared_ptr<CanvasImage> clipOverlay;

  // RGB readout under the cursor (status bar). hoverValid is false while
  // the pointer isn't over a picture pixel.
  bool hoverValid = false;
  uint8_t hoverR = 0, hoverG = 0, hoverB = 0;
};

static EditorState state;

// Shows `msg` in the status bar. Kept to one short line: newlines (library
// error strings sometimes have them) become spaces, and long text is cut on
// a UTF-8 boundary with an ellipsis.
static void setStatus(std::string msg, bool isError)
{
  constexpr size_t kMaxLen = 110;
  for (char &c : msg)
    if (c == '\n' || c == '\r')
      c = ' ';
  if (msg.size() > kMaxLen)
  {
    size_t cut = kMaxLen - 3;
    while (cut > 0 && (static_cast<unsigned char>(msg[cut]) & 0xC0) == 0x80)
      --cut; // don't split a multi-byte character
    msg.resize(cut);
    msg += "...";
  }
  state.statusMsg = std::move(msg);
  state.statusIsError = isError;
}
static void clearStatus()
{
  state.statusMsg.clear();
  state.statusIsError = false;
}

static void setZoom(float z)
{
  state.zoom = std::clamp(z, kZoomMin, kZoomMax);
  state.markDirty();
}

static void syncSource(bool force);

static void updatePreviewFromAdjustments();

// Defined after GeometryWorker (needs its type); forward-declared here
// because updateImageFromAdjustments (right below) is written once, in its
// natural place, even though it now has to decide between the synchronous
// pipeline and GeometryWorker before either exists yet in the file.
static bool geometryDirty();
static void submitGeometryJob();

// ---- Handing the worker's result back to the UI thread ----
// The event loop sleeps until the OS hands it an event, and a worker finishing
// is not one — so the worker announces itself: RenderWorker::notify_ (wired
// in main() to LiteUI::wake, which is safe from any thread) wakes the loop,
// which polls the canvases, and collectFullRender() below picks the result up
// from there. Nothing runs while no render is outstanding, so an idle editor
// costs nothing.

static void adoptFullRender(RenderWorker::Result &&r)
{
  state.image = std::move(r.image);
  state.previewImage.reset(); // the real thing replaces the stand-in
  state.histStale = true;
  state.markDirty();
}

// Non-blocking; polled once per frame from flushPendingAdjust.
static void collectFullRender()
{
  if (auto r = renderWorker.collect())
    adoptFullRender(std::move(*r));
}

// Blocking: makes `state.image` current. For the few places that need the
// real pixels *now* (export).
static void finishFullRender()
{
  if (!renderWorker.outstanding())
    return;
  adoptFullRender(renderWorker.waitForLatest());
}

// A drag starts (or continues) on the preview path. A full-resolution render
// still in flight would be for values the drag is about to change, so drop it.
static void enterDrag(bool isAngle)
{
  if (!state.dragging)
    renderWorker.invalidate();
  state.dragging = true; // switches recompute+paint to the preview path
  state.dragIsAngle = isAngle;
}

// Rebuilds `state.image` (what's drawn and exported) from the untouched
// `state.source` pixels (the original, cropped if a crop is committed)
// using the current adjustment values. Called after every change to
// `state.adjust` — a slider drag, undo, redo, a crop, or opening a new
// image.
//
// Two paths:
//  * Deferred (large images, adjustment-only changes): render the cheap
//    preview now so the picture updates immediately, and let the worker
//    produce the full-resolution `image` in the background. paintCanvas keeps
//    showing the preview (EditorState::showPreview) until it arrives.
//  * Synchronous (everything else): small images, where the pass is faster
//    than the hand-off, and any change that alters the picture's geometry
//    (crop, rotate, straighten, a newly opened file), because then
//    `source`, `image`, the document size and the crop selection must all
//    change together.
static void updateImageFromAdjustments()
{
  if (!state.original)
    return;
  clearStatus();             // the image is about to change: an old message is stale
  renderWorker.invalidate(); // whatever was in flight is now out of date

  // Orient/straighten/crop are comparatively expensive at full resolution —
  // the bilinear straighten especially — so on a large photo they run on
  // geometryWorker instead of blocking here. geometryDirty() folds in
  // "a job is already running" (see its definition): either way, submit()
  // replaces whatever's in flight, so this is safe to call every time.
  if (isLargeImage(*state.original) && geometryDirty())
  {
    submitGeometryJob();
    return; // keep showing the current picture until the bundle lands
  }

  const std::shared_ptr<CanvasImage> sourceBefore = state.source;
  syncSource(false); // picks up a crop change from apply/undo/redo
  const bool sourceChanged = state.source != sourceBefore;
  const bool canDefer = state.previewBase && !sourceChanged && state.image &&
                        state.image->width == state.source->width &&
                        state.image->height == state.source->height;
  if (canDefer)
  {
    updatePreviewFromAdjustments(); // cheap stand-in, shown immediately
    renderWorker.submit(RenderJob{state.source, state.adjust.buildRenderParams()});
    return;
  }
  state.image = renderAdjusted(state.adjust, *state.source);
  state.previewImage.reset(); // superseded by the full-resolution result
  state.previewDocW = state.previewDocH = 0.0f;
  state.histStale = true;
  state.markDirty();
}

// Like updateImageFromAdjustments(), but applied to the small preview
// instead of the full-resolution original. Only touches state.previewImage;
// state.image (and so the document size, export, and info labels) is left
// alone until the drag ends.
static void updatePreviewFromAdjustments()
{
  if (!state.previewBase)
    return;
  state.previewImage = renderAdjusted(state.adjust, *state.previewBase);
  state.histStale = true;
  state.markDirty();
}

// ---------------- Crop ----------------

static bool cropToolActive()
{
  return state.activeActivity == 2 && state.image != nullptr;
}

// Width/height ratio the selection is locked to, or 0 for free-form.
static float cropAspect()
{
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
static void cropFitToAspect()
{
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
static void resetCropSelection()
{
  auto &c = state.crop;
  c.l = 0.0f;
  c.t = 0.0f;
  c.r = state.source ? static_cast<float>(state.source->width) : 0.0f;
  c.b = state.source ? static_cast<float>(state.source->height) : 0.0f;
  c.drag = CropUi::Drag::None;
  cropFitToAspect();
}

static void setCropAspect(size_t idx)
{
  state.crop.aspectIdx = idx;
  cropFitToAspect();
  state.markDirty();
}

// Rebuilds `state.oriented` / `state.source` (and the interactive-preview
// base derived from it) when the committed crop or orientation no longer
// matches what they were built with — after applying/removing a crop, a
// rotate/flip/straighten, an undo/redo across any of them, or (force) when a
// new image is opened. Also re-seeds the selection, since the image the
// selection is measured against just changed size.
static void syncSource(bool force)
{
  if (!state.original)
    return;
  const CropRect crop = state.adjust.crop();
  const Orient orient = state.adjust.orient();
  const float angle = state.adjust.angle();
  const bool expand = state.adjust.expand();

  const bool orientChanged =
      force || !state.oriented || orient != state.orientedFor;
  // The corner mode only matters while there's an angle to straighten.
  const bool rotateChanged = orientChanged || !state.rotated ||
                             angle != state.rotatedAngle ||
                             (angle != 0.0f && expand != state.rotatedExpand);
  if (!force && state.source && !rotateChanged && crop == state.sourceCrop)
    return;

  if (orientChanged)
  {
    state.oriented = orientImage(state.original, orient);
    state.orientedFor = orient;
    state.orientedPreview = makePreviewBase(*state.oriented);
  }
  if (rotateChanged)
  {
    state.rotated = angle == 0.0f
                        ? state.oriented
                        : rotateArbitrary(*state.oriented, angle, expand);
    state.rotatedAngle = angle;
    state.rotatedExpand = expand;
  }
  state.sourceCrop = crop;
  state.source = crop.empty() ? state.rotated : cropImage(*state.rotated, crop);
  state.previewBase = makePreviewBase(*state.source);
  state.previewImage.reset();
  resetCropSelection();
}

// BEGIN geometryworker
// ---------------- Background geometry worker ----------------
// Runs the same pipeline syncSource() + renderAdjusted run
// synchronously (orient -> straighten -> crop -> render) off the UI thread
// for a large photo, so clicking Rotate or Straighten's nudge buttons on a
// 24-megapixel image doesn't freeze the window the way releasing a slider
// used to before RenderWorker.
//
// Same thread model and cancellation contract as RenderWorker (one lazily
// started thread; a generation number supersedes stale work; the running
// job polls it and abandons within a few scanlines of being superseded).
// The one real difference: RenderWorker only re-renders pixels for a source
// that hasn't changed, so it can start from `state.source`. A geometry job
// might be changing the source itself, so it always rebuilds the WHOLE
// pipeline from `original` — even a job that only changed the crop redoes
// an unchanged straighten. That wastes some work when angle is non-zero and
// only the crop is edited afterwards, but keeps a job a pure function of
// its five inputs, with nothing that can drift out of sync — and the extra
// work only costs latency on a background thread, never a UI freeze.
// The thread and the generation protocol live in latest_worker.hpp; this is
// the job. `original` must never be modified afterwards (the editor's images
// are replaced, never edited in place - the same rule RenderJob::src relies on).

// Exactly the geometry-derived EditorState fields, adopted as one atomic swap
// on the UI thread (see adoptGeometryBundle) so the picture, its document
// size, and the crop selection always change together.
struct GeometryBundle
{
  std::shared_ptr<CanvasImage> oriented;
  Orient orientedFor;
  std::shared_ptr<CanvasImage> orientedPreview;
  std::shared_ptr<CanvasImage> rotated;
  float rotatedAngle = 0.0f;
  bool rotatedExpand = false;
  std::shared_ptr<CanvasImage> source;
  CropRect sourceCrop;
  std::shared_ptr<CanvasImage> previewBase;
  std::shared_ptr<CanvasImage> image;
};
struct GeometryResult
{
  GeometryBundle bundle;
};
struct GeometryJob
{
  // Non-const, unlike RenderJob::src: orientImage (identity case) and the rest
  // of this pipeline are written in terms of the same shared_ptr<CanvasImage>
  // type `state.original` already is, so this just carries that type through -
  // never a license to mutate it.
  std::shared_ptr<CanvasImage> original;
  Orient orient;
  float angle = 0.0f;
  bool expand = false;
  CropRect crop;
  RenderParams renderParams;
};

// The pipeline itself (worker thread only). Checked for cancellation between
// every stage, and by row/block inside the two per-pixel passes
// (rotateArbitrary, renderWith) - so a job superseded mid-straighten or
// mid-render abandons within a few milliseconds rather than finishing
// unwanted work.
static std::optional<GeometryResult> runGeometryJob(GeometryJob &&job,
                                                    WorkerCancel c)
{
  if (c.cancelled())
    return std::nullopt;
  GeometryBundle b;
  b.oriented = orientImage(job.original, job.orient); // identity: no copy
  b.orientedFor = job.orient;
  if (c.cancelled())
    return std::nullopt;
  b.orientedPreview = makePreviewBase(*b.oriented);
  if (c.cancelled())
    return std::nullopt;
  if (job.angle == 0.0f)
  {
    b.rotated = b.oriented;
  }
  else
  {
    b.rotated = rotateArbitrary(*b.oriented, job.angle, job.expand, c.latest,
                                c.gen);
    if (!b.rotated)
      return std::nullopt; // abandoned mid-straighten
  }
  b.rotatedAngle = job.angle;
  b.rotatedExpand = job.expand;
  if (c.cancelled())
    return std::nullopt;
  b.sourceCrop = job.crop;
  b.source = job.crop.empty() ? b.rotated : cropImage(*b.rotated, job.crop);
  if (c.cancelled())
    return std::nullopt;
  b.previewBase = makePreviewBase(*b.source);
  b.image = renderWith(*b.source, job.renderParams, c.latest, c.gen);
  if (!b.image)
    return std::nullopt; // abandoned mid-render
  return GeometryResult{std::move(b)};
}

using GeometryWorker = LatestWorker<GeometryJob, GeometryResult>;
// END geometryworker

static GeometryWorker geometryWorker{runGeometryJob};

// True when state.adjust's committed orientation/angle/expand/crop hasn't
// been rendered into state.oriented/rotated/source yet — mirrors syncSource's
// own dirty checks, but as a read-only predicate (no rebuilding). Folds in
// "a geometry job is already running" too: that can only be true right after
// this function itself returned true and a submit followed, so the two are
// never actually independent — but checking both directly, rather than
// relying on that invariant, means a bug elsewhere fails safe (one wasted
// resubmit) instead of silently letting a stale in-flight job get adopted.
static bool geometryDirty()
{
  if (geometryWorker.outstanding())
    return true;
  if (!state.oriented || !state.rotated)
    return true;
  const Orient orient = state.adjust.orient();
  const float angle = state.adjust.angle();
  const bool expand = state.adjust.expand();
  if (orient != state.orientedFor)
    return true;
  if (angle != state.rotatedAngle)
    return true;
  if (angle != 0.0f && expand != state.rotatedExpand)
    return true;
  return state.adjust.crop() != state.sourceCrop;
}

static void submitGeometryJob()
{
  geometryWorker.submit(GeometryJob{
      state.original, state.adjust.orient(), state.adjust.angle(),
      state.adjust.expand(), state.adjust.crop(),
      state.adjust.buildRenderParams()});
  setStatus("Applying changes...", /*isError=*/false);
}

// Swaps in every field of a finished geometry rebuild together, so the
// picture, its document size, and the crop selection never show a
// combination that belonged to two different requests.
static void adoptGeometryBundle(GeometryWorker::Result &&r)
{
  GeometryBundle &b = r.bundle;
  state.oriented = std::move(b.oriented);
  state.orientedFor = b.orientedFor;
  state.orientedPreview = std::move(b.orientedPreview);
  state.rotated = std::move(b.rotated);
  state.rotatedAngle = b.rotatedAngle;
  state.rotatedExpand = b.rotatedExpand;
  state.source = std::move(b.source);
  state.sourceCrop = b.sourceCrop;
  state.previewBase = std::move(b.previewBase);
  state.image = std::move(b.image);
  state.previewImage.reset(); // the real thing replaces any stand-in
  state.previewDocW = state.previewDocH = 0.0f;
  resetCropSelection(); // the crop UI must match the new source's dimensions
  state.histStale = true;
  clearStatus(); // the "Applying changes..." message is no longer true
  state.markDirty();
}

// Non-blocking; polled once per frame from flushPendingAdjust, alongside
// collectFullRender.
static void collectGeometryJob()
{
  if (auto r = geometryWorker.collect())
    adoptGeometryBundle(std::move(*r));
}

// Blocking: makes every field a geometry rebuild touches current. For the
// one place that needs the real pixels *now* (export).
static void finishGeometryJob()
{
  if (!geometryWorker.outstanding())
    return;
  adoptGeometryBundle(geometryWorker.waitForLatest());
}

// True while a crop/rotate/flip/straighten rebuild is still running. Until
// it lands, state.rotated / state.source / the crop selection describe the
// PREVIOUS geometry while state.adjust already holds the new one, so any
// edit that measures against them (a second rotate remapping the crop rect,
// Apply Crop reading the selection) would compute from stale sizes. Those
// edits are ignored while this is true; their buttons are dimmed to match.
static bool geometryBusy() { return geometryWorker.outstanding(); }

// Which part of the selection is under (px, py) (image coordinates)?
// Returns kEdge* bits for a handle, kEdgeMove for the interior, 0 for none.
static int cropHitTest(float px, float py)
{
  const auto &c = state.crop;
  const float tol = kCropHitPx / state.zoom;
  const bool inX = px >= c.l - tol && px <= c.r + tol;
  const bool inY = py >= c.t - tol && py <= c.b + tol;
  int m = 0;
  if (inY)
  {
    if (std::abs(px - c.l) <= tol)
      m |= kEdgeL;
    else if (std::abs(px - c.r) <= tol)
      m |= kEdgeR;
  }
  if (inX)
  {
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
static void cropDragCorner(float ax, float ay, float px, float py)
{
  auto &c = state.crop;
  const float W = state.docWidth(), H = state.docHeight();
  const float a = cropAspect();
  const float sx = px >= ax ? 1.0f : -1.0f;
  const float sy = py >= ay ? 1.0f : -1.0f;
  float w = std::abs(px - ax), h = std::abs(py - ay);
  if (a > 0.0f)
  {
    w = std::max(w, h * a); // grow to whichever dimension asks for more
    h = w / a;
    const float maxW = sx > 0.0f ? W - ax : ax;
    const float maxH = sy > 0.0f ? H - ay : ay;
    if (w > maxW)
    {
      w = maxW;
      h = w / a;
    }
    if (h > maxH)
    {
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
static void cropResize(int mask, float px, float py)
{
  auto &c = state.crop;
  const float W = state.docWidth(), H = state.docHeight();
  const float minSz = std::min({kCropMinPx, W, H});
  const bool horiz = (mask & (kEdgeL | kEdgeR)) != 0;
  const bool vert = (mask & (kEdgeT | kEdgeB)) != 0;

  if (horiz && vert)
  { // corner: the opposite corner stays put
    cropDragCorner((mask & kEdgeL) ? c.r : c.l, (mask & kEdgeT) ? c.b : c.t, px,
                   py);
    return;
  }

  const float a = cropAspect();
  if (a <= 0.0f)
  { // free-form edge
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
  if (horiz)
  {
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
  }
  else
  {
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
static void cropPress(float lx, float ly)
{
  state.openMenu = -1; // the canvas now swallows presses that used to fall
                       // through to the workspace's "close menu" handler
  if (!cropToolActive())
    return;
  auto &c = state.crop;
  const float px = std::clamp(lx / state.zoom, 0.0f, state.docWidth());
  const float py = std::clamp(ly / state.zoom, 0.0f, state.docHeight());
  const int hit = cropHitTest(px, py);
  if (hit == kEdgeMove)
  {
    c.drag = CropUi::Drag::Move;
    c.grabX = px - c.l;
    c.grabY = py - c.t;
  }
  else if (hit)
  {
    c.drag = CropUi::Drag::Resize;
    c.mask = hit;
  }
  else
  {
    // Empty area: start a new selection here. The old one stays until the
    // pointer actually moves, so a plain click doesn't wipe it out.
    c.drag = CropUi::Drag::New;
    c.ax = px;
    c.ay = py;
  }
}

static void cropDrag(float lx, float ly)
{
  auto &c = state.crop;
  if (!cropToolActive() || c.drag == CropUi::Drag::None)
    return;
  const float W = state.docWidth(), H = state.docHeight();
  const float px = std::clamp(lx / state.zoom, 0.0f, W);
  const float py = std::clamp(ly / state.zoom, 0.0f, H);
  switch (c.drag)
  {
  case CropUi::Drag::New:
    cropDragCorner(c.ax, c.ay, px, py);
    break;
  case CropUi::Drag::Resize:
    cropResize(c.mask, px, py);
    break;
  case CropUi::Drag::Move:
  {
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
static void applyCrop()
{
  if (!state.source || geometryBusy())
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
static void removeCrop()
{
  if (geometryBusy() || state.adjust.crop().empty())
    return;
  state.adjust.setCrop(CropRect{});
  updateImageFromAdjustments();
}

// ---------------- Rotate / flip ----------------

// One undo step. `state.rotated` is what crops are measured against, so its
// size is the (W, H) geoStep needs.
static void applyGeoOp(GeoOp op)
{
  if (!state.original || !state.rotated || geometryBusy())
    return;
  Orient o = state.adjust.orient();
  float angle = state.adjust.angle();
  CropRect c = state.adjust.crop();
  int W = state.rotated->width, H = state.rotated->height;
  geoStep(o, angle, c, W, H, op);
  state.adjust.setGeometry(o, angle, c);
  updateImageFromAdjustments(); // syncSource() rebuilds oriented/rotated/source
}

// Undoes every 90-degree turn and flip in one step (the crop follows,
// staying on the same part of the picture; the straighten angle is kept,
// still tilting the picture the same way relative to its own edges).
static void resetOrientation()
{
  if (!state.original || !state.rotated || geometryBusy() ||
      state.adjust.orient().identity())
    return;
  Orient o = state.adjust.orient();
  float angle = state.adjust.angle();
  CropRect c = state.adjust.crop();
  int W = state.rotated->width, H = state.rotated->height;
  if (o.flip)
    geoStep(o, angle, c, W, H, GeoOp::FlipH); // leaves a pure rotation
  while (o.rot != 0)
    geoStep(o, angle, c, W, H, GeoOp::RotateCCW);
  state.adjust.setGeometry(o, angle, c);
  updateImageFromAdjustments();
}

// ---------------- Straighten (arbitrary-angle rotation) ----------------

// Slider drag: one undo step per drag. Only records the value — the
// resample is deferred to canvasDirtySource (see there), once per frame.
static void setAngleFromSlider(float deg)
{
  state.adjust.setAngle(snapAngle(deg), /*coalesce=*/true);
}

// A discrete change (nudge button / reset): its own undo step, resampled at
// full resolution on the next frame.
static void setAngleDiscrete(float deg)
{
  if (!state.original)
    return;
  state.adjust.setAngle(snapAngle(deg), /*coalesce=*/false);
  state.markAdjustDirty();
}
static void nudgeAngle(float delta)
{
  setAngleDiscrete(state.adjust.angle() + delta);
}

static void setCornerMode(bool expand)
{
  if (!state.original)
    return;
  state.adjust.setExpand(expand);
  state.markAdjustDirty();
}

// Interactive path for an angle drag on a large image: turns the small
// `orientedPreview` instead of the full-resolution picture, drawn stretched
// to the size the full-resolution result will have (previewDoc*). The
// full-resolution turn runs once, when the drag ends (View::onDragEnd).
static void updateAnglePreview()
{
  if (!state.orientedPreview || !state.oriented)
    return;
  const float a = state.adjust.angle();
  const bool ex = state.adjust.expand();
  auto turned = rotateArbitrary(*state.orientedPreview, a, ex);
  state.previewImage = renderAdjusted(state.adjust, *turned);
  state.histStale = true;
  const Dims d =
      rotatedDims(state.oriented->width, state.oriented->height, a, ex);
  state.previewDocW = static_cast<float>(d.w);
  state.previewDocH = static_cast<float>(d.h);
  state.markDirty();
}

static void undo()
{
  if (!state.adjust.canUndo())
    return;
  state.adjust.undo();
  updateImageFromAdjustments();
}

static void redo()
{
  if (!state.adjust.canRedo())
    return;
  state.adjust.redo();
  updateImageFromAdjustments();
}

// ---------------- Opening a file (background decode) ----------------
// Decoding a big PNG/JPEG takes long enough to freeze the window, so it runs
// on its own thread. Same contract as RenderWorker / GeometryWorker: submit()
// supersedes any earlier request, the finished result is parked until the UI
// thread collect()s it, and notify_ wakes the sleeping event loop. Until the
// result lands the previously open image stays fully usable; nothing about
// the editor state changes until adoptDecoded() swaps the new file in.
//
// A decode already running can't be interrupted (the image libraries have no
// cancel hook), so opening a second file queues behind it and the first
// file's result is simply dropped when it finishes.
struct DecodeJob
{
  std::string path;
};
struct DecodeResult
{
  std::string path;
  std::optional<CanvasImage> image; // empty on failure
  std::string error;                // library message when image is empty
};

// Worker thread only. Always returns a value (a failure is an empty `image`
// plus `error`): a decode can't be cancelled part-way.
static std::optional<DecodeResult> runDecodeJob(DecodeJob &&job, WorkerCancel)
{
  DecodeResult r;
  r.path = job.path;
  r.image = liteui_image::decodeFile(job.path, &r.error);

  if (r.image)
  { // a camera's rotation (EXIF) is applied here, once: from now on the
    // picture is upright everywhere (editor, masks, saved edits, export)
    const Orient o = exifOrientationOf(job.path);
    if (!o.identity())
    {
      auto turned = orientImage(
          std::make_shared<CanvasImage>(std::move(*r.image)), o);
      r.image.emplace(std::move(*turned));
    }
  }
  return r;
}

// LatestWorker plus the UI-thread bookkeeping of which file is being decoded.
class DecodeWorker : public LatestWorker<DecodeJob, DecodeResult>
{
public:
  DecodeWorker() : LatestWorker<DecodeJob, DecodeResult>(runDecodeJob) {}

  void submit(std::string path)
  {
    pendingPath_ = path;
    LatestWorker<DecodeJob, DecodeResult>::submit(DecodeJob{std::move(path)});
  }

  // The file the newest request is decoding (UI thread; valid while
  // outstanding()).
  const std::string &pendingPath() const { return pendingPath_; }

private:
  std::string pendingPath_; // UI thread only
};

static DecodeWorker decodeWorker;

// ---------------- Object segmentation (background) ----------------
struct SegmentRequest
{
  std::shared_ptr<const CanvasImage> image; // the untouched original
  uint64_t imageKey = 0;                    // state.imageSerial
  std::vector<ObjectPrompt> prompts;        // original-picture fractions
  int maskIdx = -1, part = -1;              // where the result goes
  bool prefetch = false;                    // encode only (warm the model), no result map
};

// ---- Stand-in segmenter: see seg_basic.hpp ----

// Which engine is active, shown in the Object panel (written by the worker,
// read by the UI thread's label poll).
static std::mutex gSegInfoMutex;
static std::string gSegInfo = "Engine: loads on first use";
static void setSegInfo(std::string s)
{
  std::lock_guard<std::mutex> lk(gSegInfoMutex);
  gSegInfo = std::move(s);
}
static std::string getSegInfo()
{
  std::lock_guard<std::mutex> lk(gSegInfoMutex);
  return gSegInfo;
}

// Models: $LITEROOM_MODELS, else a "models" folder next to the executable.
static std::string samModelDir()
{
  if (const char *e = std::getenv("LITEROOM_MODELS"); e && *e)
    return e;
#if defined(_WIN32)
  wchar_t wbuf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
  if (n > 0 && n < MAX_PATH)
  {
    std::wstring w(wbuf, n);
    const size_t slash = w.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
      w.resize(slash);
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                        nullptr, nullptr);
    std::string s(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
    if (len > 0)
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr,
                          nullptr);
    return s + "\\models";
  }
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0)
  {
    std::string p(buf, static_cast<size_t>(n));
    const size_t slash = p.find_last_of('/');
    if (slash != std::string::npos)
      return p.substr(0, slash) + "/models";
  }
#endif
  return "models";
}

// Worker thread only. Prefetch (no prompts): load + encode, return null with
// no error. Falls back to the color-grow stand-in when SAM is unavailable.
static std::shared_ptr<const ObjectMap>
segmentObject(const CanvasImage &img, uint64_t key,
              const std::vector<ObjectPrompt> &prompts, std::string *err,
              const std::atomic<uint64_t> *latest, uint64_t gen)
{
  static sam::Segmenter seg;
  static int backend = 0; // 0 untried, 1 SAM, -1 fallback
  static uint64_t encodedKey = 0;
  if (backend == 0)
  {
    const std::string dir = samModelDir();
    std::string e;
    if (seg.load(dir + "/mobile_sam.encoder.onnx",
                 dir + "/mobile_sam.decoder.onnx", &e))
    {
      backend = 1;
      setSegInfo("Engine: MobileSAM");
    }
    else
    {
      backend = -1;
      setSegInfo("Engine: basic color grow (" + e + "; looked in " + dir + ")");
    }
  }
  if (backend < 0)
    return prompts.empty()
               ? nullptr
               : segmentObjectWand(img, key, prompts, err, latest, gen);

  if (encodedKey != key)
  {
    if (!seg.encode(img.pixels.data(), img.width, img.height, err))
      return nullptr;
    encodedKey = key;
  }
  if (prompts.empty() ||
      (latest && latest->load(std::memory_order_relaxed) != gen))
    return nullptr; // prefetch, or superseded while encoding

  std::vector<sam::Point> pts;
  pts.reserve(prompts.size());
  for (const ObjectPrompt &p : prompts)
    pts.push_back({p.x, p.y, p.fg});
  std::vector<float> logits;
  int w = 0, h = 0;
  if (!seg.decode(pts, &logits, &w, &h, err))
    return nullptr;
  auto m = std::make_shared<ObjectMap>();
  m->w = w;
  m->h = h;
  m->id = nextObjectMapId();
  m->a.resize(logits.size());
  for (size_t i = 0; i < logits.size(); ++i) // soft edge: sigmoid of the logit
    m->a[i] = static_cast<uint8_t>(std::lround(
        255.0f / (1.0f + std::exp(-logits[i] * kSamSoftness))));
  return m;
}

struct SegmentResult
{
  SegmentRequest req;                   // image dropped; prompts + target kept
  std::shared_ptr<const ObjectMap> map; // null on failure
  std::string error;
};

// Worker thread only. Always returns a value (null `map` + `error` on failure).
static std::optional<SegmentResult> runSegmentJob(SegmentRequest &&req,
                                                  WorkerCancel c)
{
  SegmentResult r;
  r.map = segmentObject(*req.image, req.imageKey, req.prompts, &r.error,
                        c.latest, c.gen);
  r.req = std::move(req);
  r.req.image.reset();
  return r;
}

// Same protocol as RenderWorker / DecodeWorker (see latest_worker.hpp).
using SegmentWorker = LatestWorker<SegmentRequest, SegmentResult>;

static SegmentWorker segmentWorker{runSegmentJob};

// UI thread: commits a finished segmentation. Prompts and map land in ONE
// undo step. A result for a mask/part that is gone (deleted, undone away) is
// dropped.
static void adoptSegment(SegmentWorker::Result &&r)
{

  if (r.req.prefetch)
  {
    if (!r.error.empty())
      setStatus("Object selection unavailable: " + r.error, true);
    else
    {
      state.mask.encodedKey = r.req.imageKey;
      clearStatus();
    }
    state.markDirty();
    return;
  }
  state.mask.objPending = {};
  state.markDirty();
  if (!r.map)
  {
    setStatus("Object selection failed" +
                  (r.error.empty() ? std::string() : ": " + r.error),
              true);
    return;
  }
  MaskSet ms = state.adjust.masks();
  if (r.req.maskIdx < 0 || r.req.maskIdx >= static_cast<int>(ms.size()))
    return;
  Mask &m = ms[static_cast<size_t>(r.req.maskIdx)];
  if (r.req.part < 0 || r.req.part >= static_cast<int>(m.parts.size()))
    return;
  auto *o =
      std::get_if<ObjectShape>(&m.parts[static_cast<size_t>(r.req.part)].shape);
  if (!o)
    return;
  o->map = r.map;
  o->prompts = std::move(r.req.prompts);
  state.mask.encodedKey = r.req.imageKey;
  state.adjust.setMasks(ms, -1);
  clearStatus();
  state.markAdjustDirty(); // re-render through the normal (drag-safe) path
}

// Polled once per frame from flushPendingAdjust.
static void collectSegmentJob()
{
  if (auto r = segmentWorker.collect())
    adoptSegment(std::move(*r));
}

// Swaps a finished decode into the editor (UI thread only).
static void filmMarkEdited(const std::string &path); // filmstrip (below)

// ---------------- Saved edits: the sidecar file ----------------
// photo.jpg's edits live in photo.jpg.literoom.json beside it. They are
// written (temp file, then rename, so a crash can't leave half a file)
//   - after any change, once no mouse drag is in progress (autosaveSidecar,
//     polled every frame from flushPendingAdjust),
//   - when another picture replaces this one, and when the app closes;
// and read back when the picture is opened. A missing file is normal; a
// damaged one, one from a newer version, or one written for a different-sized
// picture is ignored (the editor starts clean) and the status bar says so.
// Opening a picture and touching nothing writes nothing. The undo history is
// not saved.

static std::filesystem::path sidecarPath(const std::string &imagePath)
{
  return fsPath(sidecarFileOf(imagePath));
}

// Writes the sidecar for the open picture. On failure (read-only folder...)
// the status bar says so; the change still counts as "seen" so a failing
// disk isn't retried every frame, only on the next edit.
static void saveSidecar()
{
  if (!state.original || state.imagePath.empty())
    return;
  state.sidecarSaved = state.adjust.version();

  mj::Json img = mj::Json::object();
  img.set("name", mj::Json::str(baseName(state.imagePath)));
  img.set("width", mj::Json::num(state.original->width));
  img.set("height", mj::Json::num(state.original->height));
  mj::Json root = mj::Json::object();
  root.set("app", mj::Json::str("literoom"));
  root.set("version", mj::Json::num(kSidecarVersion));
  root.set("image", std::move(img));
  root.set("edits", state.adjust.toJson());
  const std::string text = root.dump();

  const std::filesystem::path path = sidecarPath(state.imagePath);
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    f.flush();
    if (!f)
    {
      f.close();
      std::filesystem::remove(tmp, ec);
      setStatus("Couldn't save edits next to " + baseName(state.imagePath),
                true);
      return;
    }
  }
  std::filesystem::rename(tmp, path, ec); // replaces an existing file
  if (ec)
  {
    std::error_code ignore;
    std::filesystem::remove(tmp, ignore);
    setStatus("Couldn't save edits: " + ec.message(), true);
  }
  else
    filmMarkEdited(state.imagePath); // the strip's "has edits" dot
}

// Saves now if anything changed since the last save / load.
static void flushSidecar()
{
  if (state.original && state.adjust.version() != state.sidecarSaved)
    saveSidecar();
}

// Per frame: saves a finished change, but not while the mouse is still
// dragging a slider or a mask (that would write on every frame).
static void autosaveSidecar()
{
  if (!state.original || state.adjust.version() == state.sidecarSaved)
    return;
  if (state.dragging || state.mask.drag != MaskUi::Drag::None)
    return;
  saveSidecar();
}

// Applies the picture's saved edits, if it has any. Call right after
// adjust.reset() + setOriginalSize() and BEFORE syncSource(), so the saved
// crop / rotation is what gets built.
static void loadSidecar()
{
  state.sidecarSaved = state.adjust.version(); // pristine unless loaded below
  std::ifstream f(sidecarPath(state.imagePath),
                  std::ios::binary | std::ios::ate);
  if (!f)
    return; // no saved edits: the normal case
  const std::streamoff n = f.tellg();
  if (n <= 0 || n > (64 << 20))
  {
    setStatus("Ignored saved edits: unexpected file size", true);
    return;
  }
  std::string text(static_cast<size_t>(n), '\0');
  f.seekg(0);
  f.read(text.data(), n);
  if (!f)
  {
    setStatus("Ignored saved edits: couldn't read the file", true);
    return;
  }
  mj::Json root;
  std::string err;
  if (!mj::Json::parse(text, &root, &err))
  {
    setStatus("Ignored saved edits: " + err, true);
    return;
  }
  const mj::Json *ver = root.find("version");
  if (!ver || !ver->isNumber() || ver->asNum(0) > kSidecarVersion)
  {
    setStatus("Ignored saved edits: made by a newer Literoom", true);
    return;
  }
  const mj::Json *img = root.find("image");
  if (!img || !img->isObject() ||
      img->find("width") == nullptr || img->find("height") == nullptr ||
      img->find("width")->asNum(-1) != state.original->width ||
      img->find("height")->asNum(-1) != state.original->height)
  {
    setStatus("Ignored saved edits: the picture's size has changed", true);
    return;
  }
  const mj::Json *edits = root.find("edits");
  if (!edits || !state.adjust.loadJson(*edits))
  {
    setStatus("Ignored saved edits: unreadable", true);
    return;
  }
  state.sidecarSaved = state.adjust.version();
  setStatus("Restored saved edits", false);
}

// BEGIN filmstrip
// ---------------- Filmstrip: the folder's pictures ----------------
// A strip under the canvas with a thumbnail of every JPEG / PNG in the open
// picture's folder. Click one (or press Left / Right) to open it; a dot marks
// pictures that have saved edits. It is one canvas that paints only the cells
// in view, so a folder of thousands costs nothing extra. Thumbnails are made
// by a background thread, nearest-to-the-open-picture first, and the open
// picture's thumbnail is refreshed (edits included) when you leave it.
constexpr float kFilmH = 104.0f;
constexpr float kFilmCellW = 108.0f;
constexpr size_t kFilmMaxFiles = 5000; // a bigger folder shows its first 5000

// makeThumb / ThumbWorker / kFilmThumbW,H live in thumb_worker.hpp.
static ThumbWorker thumbWorker{
    [](const std::string &path)
    { return exifOrientationOf(path); }};

// Lists the JPEG / PNG files of `folder` into a new strip (not yet shown).
static FilmStrip filmListFolder(const std::string &folder)
{
  FilmStrip f;
  f.folder = folder;
  const std::filesystem::path base = fsPath(folder);
  for (auto &name : listImageFiles(folder, kFilmMaxFiles))
  {
    FilmEntry e;
    e.path = u8ToString(base / fsPath(name));
    e.name = name;
    f.byName[filmLower(name)] = f.files.size();
    f.files.push_back(std::move(e));
  }
  return f;
}

static void filmClampScroll()
{
  FilmStrip &f = state.film;
  const float total = static_cast<float>(f.files.size()) * kFilmCellW;
  f.scrollX = std::clamp(f.scrollX, 0.0f, std::max(0.0f, total - f.viewW));
}

static void filmEnsureVisible(int i)
{
  FilmStrip &f = state.film;
  if (i < 0)
    return;
  const float x0 = static_cast<float>(i) * kFilmCellW, x1 = x0 + kFilmCellW;
  const float w = f.viewW > 0.0f ? f.viewW : 800.0f; // before the first paint
  if (x0 < f.scrollX)
    f.scrollX = x0;
  else if (x1 > f.scrollX + w)
    f.scrollX = x1 - w;
  filmClampScroll();
  f.dirty = true;
}

// Points the thumbnail worker at the pictures that still lack one, nearest
// to the target first.
static void filmQueueThumbs()
{
  const FilmStrip &f = state.film;
  const int n = static_cast<int>(f.files.size());
  std::vector<std::string> todo;
  auto want = [&](int i)
  {
    const FilmEntry &e = f.files[static_cast<size_t>(i)];
    if (!e.thumb && !e.failed)
      todo.push_back(e.path);
  };
  const int mid = std::clamp(f.target >= 0 ? f.target : f.current, 0,
                             std::max(0, n - 1));
  if (n > 0)
  {
    want(mid);
    for (int d = 1; mid - d >= 0 || mid + d < n; ++d)
    {
      if (mid + d < n)
        want(mid + d);
      if (mid - d >= 0)
        want(mid - d);
    }
  }
  thumbWorker.setQueue(std::move(todo));
}

// Makes the folder `folder` the filmstrip's. False (nothing changed) if it
// holds no JPEG / PNG pictures.
static bool filmAdoptFolder(const std::string &folder)
{
  FilmStrip next = filmListFolder(folder);
  if (next.files.empty())
    return false;
  next.visible = state.film.visible;
  state.film = std::move(next);
  state.film.dirty = true;
  return true;
}

// The open picture changed (or was just opened): follow it. Re-lists the
// folder if it is a different one, selects the picture, scrolls to it and
// re-aims the thumbnail worker. Call after loadSidecar().
static void filmSync(const std::string &imagePath)
{
  FilmStrip &f = state.film;
  const std::filesystem::path p = fsPath(imagePath);
  std::string folder = u8ToString(p.parent_path());
  if (folder.empty())
    folder = ".";
  bool same = false;
  if (!f.folder.empty())
  {
    std::error_code ec;
    same = std::filesystem::equivalent(fsPath(f.folder), fsPath(folder), ec) &&
           !ec;
  }
  if (!same)
    filmAdoptFolder(folder);
  FilmStrip &g = state.film;
  const auto it = g.byName.find(filmLower(u8ToString(p.filename())));
  g.current = g.target = it == g.byName.end() ? -1 : static_cast<int>(it->second);
  g.thumbVersion = state.adjust.version();
  filmEnsureVisible(g.current);
  filmQueueThumbs();
  g.dirty = true;
}

// The open picture is about to be replaced: if it was edited, make its
// thumbnail show the edits.
static void filmLeave()
{
  FilmStrip &f = state.film;
  if (f.current < 0 || static_cast<size_t>(f.current) >= f.files.size() ||
      !state.image || state.adjust.version() == f.thumbVersion)
    return;
  FilmEntry &e = f.files[static_cast<size_t>(f.current)];
  if (auto t = makeThumb(*state.image))
  {
    e.thumb = std::move(t);
    e.failed = false;
  }
  e.edited = true;
  f.dirty = true;
}

// Called when a sidecar file was written for `path`.
static void filmMarkEdited(const std::string &path)
{
  FilmStrip &f = state.film;
  const auto it = f.byName.find(filmLower(baseName(path)));
  if (it == f.byName.end())
    return;
  FilmEntry &e = f.files[it->second];
  if (!e.edited)
  {
    e.edited = true;
    f.dirty = true;
  }
}

// UI thread: takes in the thumbnails the worker has made.
static void collectThumbJobs()
{
  for (auto &r : thumbWorker.collect())
  {
    FilmStrip &f = state.film;
    const auto it = f.byName.find(filmLower(baseName(r.path)));
    if (it == f.byName.end() || f.files[it->second].path != r.path)
      continue; // from a folder we have left
    FilmEntry &e = f.files[it->second];
    if (r.thumb)
    {
      // The open picture's own thumbnail already shows its edits: keep that.
      if (!(static_cast<int>(it->second) == f.current &&
            state.adjust.version() != f.thumbVersion))
        e.thumb = std::move(r.thumb);
    }
    else
      e.failed = true;
    e.edited = e.edited || r.edited;
    f.dirty = true;
  }
}

// Opens the picture at index `i` (same background decode as File > Open).
static void filmOpenIndex(int i)
{
  FilmStrip &f = state.film;
  if (i < 0 || static_cast<size_t>(i) >= f.files.size() || i == f.target ||
      state.dragging)
    return;
  f.target = i;
  f.dirty = true;
  filmEnsureVisible(i);
  setStatus("Opening " + f.files[static_cast<size_t>(i)].name + "...", false);
  decodeWorker.submit(f.files[static_cast<size_t>(i)].path);
  filmQueueThumbs();
}

// Left / Right: the previous / next picture (relative to the one asked for
// last, so quick repeats keep moving).
static void filmStep(int d)
{
  const FilmStrip &f = state.film;
  if (f.files.empty())
    return;
  const int base = f.target >= 0 ? f.target : f.current;
  filmOpenIndex(std::clamp(base + d, 0, static_cast<int>(f.files.size()) - 1));
}

static void filmScrollBy(float dx)
{
  state.film.scrollX += dx;
  filmClampScroll();
  state.film.dirty = true;
}

static void filmClick(float localX)
{
  const FilmStrip &f = state.film;
  const int i = static_cast<int>(std::floor((localX + f.scrollX) / kFilmCellW));
  filmOpenIndex(i);
}

static void toggleFilmstrip()
{
  state.film.visible = !state.film.visible;
  state.film.dirty = true;
}

static bool filmstripShown()
{
  return state.film.visible && !state.film.files.empty();
}

// File > Open Folder: lists the folder and opens its first picture.
static void openFolder()
{
  auto dir = openFolderPicker("Open Folder");
  if (!dir)
    return; // user cancelled
  if (!filmAdoptFolder(*dir))
  {
    setStatus("No JPEG or PNG pictures in " + baseName(*dir), true);
    return;
  }
  filmQueueThumbs();
  filmOpenIndex(0);
}

static void paintFilmstrip(CanvasContext &ctx)
{
  FilmStrip &f = state.film;
  const float W = ctx.width(), H = ctx.height();
  f.viewW = W;
  filmClampScroll();
  ctx.setFillColor(kColorRootBg);
  ctx.fillRect(0, 0, W, H);
  if (f.files.empty())
    return;

  const int n = static_cast<int>(f.files.size());
  const int first = std::max(0, static_cast<int>(f.scrollX / kFilmCellW));
  const int last =
      std::min(n - 1, static_cast<int>((f.scrollX + W) / kFilmCellW));
  ctx.setFont("", 11.0f);
  ctx.setTextAlign(TextAlign::Center);
  ctx.setTextBaseline(TextBaseline::Top);
  for (int i = first; i <= last; ++i)
  {
    const FilmEntry &e = f.files[static_cast<size_t>(i)];
    const float cx = static_cast<float>(i) * kFilmCellW - f.scrollX;
    const bool selected = i == f.target;
    if (selected)
    {
      ctx.setFillColor(kColorActivityActive);
      ctx.fillRect(cx + 2, 2, kFilmCellW - 4, H - 4);
    }
    const float bx = cx + (kFilmCellW - kFilmThumbW) / 2.0f, by = 6.0f;
    if (e.thumb)
    {
      const float x = bx + (kFilmThumbW - e.thumb->width) / 2.0f;
      const float y = by + (kFilmThumbH - e.thumb->height) / 2.0f;
      ctx.drawImage(*e.thumb, std::floor(x), std::floor(y));
    }
    else
    {
      ctx.setFillColor(kColorTrack);
      ctx.fillRect(bx, by, kFilmThumbW, kFilmThumbH);
    }
    if (selected)
    {
      ctx.setStrokeColor(kColorAccent);
      ctx.setLineWidth(2.0f);
      ctx.strokeRect(cx + 2, 2, kFilmCellW - 4, H - 4);
    }
    if (e.edited)
    { // a dot in the thumbnail's bottom-right corner
      ctx.setFillColor(kColorAccent);
      ctx.beginPath();
      ctx.roundRect(bx + kFilmThumbW - 12, by + kFilmThumbH - 12, 8, 8, 4);
      ctx.fill();
    }
    std::string label = e.name;
    constexpr size_t kMaxChars = 15;
    if (label.size() > kMaxChars)
    {
      size_t cut = kMaxChars - 1; // never split a UTF-8 character
      while (cut > 0 && (static_cast<unsigned char>(label[cut]) & 0xC0) == 0x80)
        --cut;
      label = label.substr(0, cut) + "\u2026";
    }
    ctx.setFillColor(selected ? kColorTextDark : kColorTextMuted);
    ctx.fillText(label, cx + kFilmCellW / 2.0f, by + kFilmThumbH + 6.0f);
  }
}
// END filmstrip

static void adoptDecoded(DecodeWorker::Result &&r)
{
  if (!r.image)
  {
    // Leave whatever image is already open untouched and say what happened.
    setStatus("Couldn't open " + baseName(r.path) + ": " +
                  (r.error.empty() ? std::string("unsupported or damaged image")
                                   : r.error),
              true);
    state.film.target = state.film.current; // stay on the picture still open
    state.film.dirty = true;
    state.markDirty();
    return;
  }
  clearStatus();
  flushSidecar(); // the picture being replaced keeps its latest edits
  filmLeave();    // ...and its filmstrip thumbnail shows them

  // Anything still in flight belongs to the previous picture; its result
  // must never be adopted over the new one.
  renderWorker.invalidate();
  geometryWorker.invalidate();
  segmentWorker.invalidate();
  state.mask.objPending = {};
  ++state.imageSerial;

  state.original = std::make_shared<CanvasImage>(std::move(*r.image));
  state.imagePath = std::move(r.path);
  state.zoom = 1.0f;    // start fresh at 100% on every newly opened image
  state.adjust.reset(); // and with no adjustment, crop or rotation applied yet
  state.adjust.setOriginalSize(state.original->width, state.original->height);
  state.maskSel = -1;
  state.mask.part = 0;
  loadSidecar();             // this picture's saved edits, if any (before syncSource: crop / rotation)
  filmSync(state.imagePath); // the strip follows: folder, selection, thumbnails
  state.dragging = false;
  state.dragIsAngle = false;
  state.image.reset();          // the old file's pixels: force the synchronous path
  syncSource(true);             // source = original; builds previewBase, resets selection
  updateImageFromAdjustments(); // builds state.image from state.source
}

// Non-blocking; polled once per frame from flushPendingAdjust.
static void collectDecodeJob()
{
  if (auto r = decodeWorker.collect())
    adoptDecoded(std::move(*r));
}

static void openImage()
{
  auto path = openFilePicker(
      "Open Image", {{"Images", "*.png;*.jpg;*.jpeg"}, {"All Files", "*.*"}});
  if (!path)
    return; // user cancelled
  setStatus("Opening " + baseName(*path) + "...", /*isError=*/false);
  decodeWorker.submit(*path);
}

// Encodes `img` as a JPEG at `quality` (1..100). With `keepMetadata`, the
// EXIF block and ICC profile of the photo at `sourcePath` ride along
// (orientation reset to "upright", because the pixels already are; picture
// size updated; see embedSourceMetadata).
static bool saveJpegFile(const std::string &path, const CanvasImage &img,
                         int quality, bool keepMetadata,
                         const std::string &sourcePath, std::string *errorOut)
{
  std::vector<uint8_t> jpeg;
  if (!encodeJpegMemory(img, std::clamp(quality, 1, 100), &jpeg, errorOut))
    return false;
  if (keepMetadata)
    embedSourceMetadata(&jpeg, sourcePath, img.width, img.height);
  return writeFileAtomic(path, jpeg, errorOut);
}

// Exports the picture as you see it (every edit, crop and rotation applied).
// The format follows the file name the user picks (.jpg / .jpeg / .png); the
// Export pane's format choice only decides what the dialog offers first.

static void exportImage()
{
  if (!state.image)
    return;            // nothing open to export
  finishGeometryJob(); // never export the stale (pre-rebuild) picture
  finishFullRender();  // never export the stale frame of a pending render

  std::string stem = baseName(state.imagePath);
  if (const auto dot = stem.find_last_of('.');
      dot != std::string::npos && dot > 0)
    stem.resize(dot);
  if (stem.empty())
    stem = "export";

  const bool wantJpeg = state.exportJpeg;
  auto path = wantJpeg
                  ? saveFilePicker("Export as JPEG", stem + "-edit.jpg",
                                   {{"JPEG Image", "*.jpg;*.jpeg"},
                                    {"PNG Image", "*.png"}},
                                   "jpg")
                  : saveFilePicker("Export as PNG", stem + "-edit.png",
                                   {{"PNG Image", "*.png"},
                                    {"JPEG Image", "*.jpg;*.jpeg"}},
                                   "png");
  if (!path)
    return; // user cancelled

  std::string out = *path;
  bool jpeg = isJpegPath(out);
  if (!jpeg && fileExtLower(out) != "png")
  { // no usable extension: use the pane's format and name the file for it
    jpeg = wantJpeg;
    out += jpeg ? ".jpg" : ".png";
  }

  std::string err;
  const bool ok =
      jpeg ? saveJpegFile(out, *state.image, state.jpegQuality,
                          state.keepMetadata, state.imagePath, &err)
           : encodePngFile(out, *state.image, &err);
  if (ok)
    setStatus("Exported " + baseName(out), false);
  else
    setStatus("Export failed: " +
                  (err.empty() ? "couldn't write " + baseName(out) : err),
              true);
}

static void toggleSidebar()
{
  state.activeActivity = (state.activeActivity == -1) ? 0 : -1;
  state.markDirty(); // the crop overlay shows only while the Crop pane is open
}

// ---------------- Small reusable pieces ----------------

// One row of a dropdown: label, action, and an optional shortcut hint shown
// right-aligned (display only — the real bindings are in main()).
struct MenuItem
{
  std::string label;
  std::function<void()> action;
  std::string shortcut = {}; // optional
};

// A titlebar menu button ("File", "View", ...) that opens a dropdown of
// MenuItems below it when clicked. Only one menu is open at a time
// (EditorState::openMenu); picking an item runs its action and closes the
// menu.
static View menuButton(const std::string &label, int id,
                       std::vector<MenuItem> items)
{
  View btn;
  btn.style.height = Size::full();
  btn.style.padding = EdgeInsets{0, 12, 0, 12};
  btn.style.justifyContent = Justify::Center;
  btn.style.alignItems = Align::Center;
  btn.style.hoverColor = kColorMenuHover;
  btn.style.backgroundColor = std::function<Color()>([id]
                                                     { return state.openMenu == id ? kColorMenuHover : kColorTransparent; });
  btn.onClick = [id]
  { state.openMenu = (state.openMenu == id) ? -1 : id; };

  Text label_;
  label_.label = label;
  label_.fontSize = 13.0f;
  label_.color = kColorTextDark;
  btn.addChild(std::move(label_));

  View dropdown;
  dropdown.style.position = Position::Absolute;
  dropdown.style.top = kTitlebarH;
  dropdown.style.left = 0.0f;
  dropdown.style.width = Size::pixel(250);
  dropdown.style.direction = FlexDirection::Column;
  dropdown.style.backgroundColor = kColorDropdownBg;
  dropdown.style.borderWidth = 1.0f;
  dropdown.style.borderColor = kColorDropdownBorder;
  dropdown.style.borderRadius = 4.0f;
  dropdown.style.padding = EdgeInsets::all(4.0f);
  dropdown.style.zIndex = 100;
  dropdown.style.display = std::function<Display()>(
      [id]
      { return state.openMenu == id ? Display::Flex : Display::None; });

  for (auto &item : items)
  {
    View row;
    row.style.height = Size::pixel(30);
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.justifyContent = Justify::SpaceBetween;
    row.style.padding = EdgeInsets{0, 10, 0, 10};
    row.style.borderRadius = 3.0f;
    row.style.hoverColor = kColorDropdownRowHover;
    std::function<void()> action = item.action;
    row.onClick = [action]
    {
      action();
      state.openMenu = -1;
    };

    Text t;
    t.label = item.label;
    t.fontSize = 13.0f;
    t.color = kColorTextDark;
    row.addChild(std::move(t));
    if (!item.shortcut.empty())
    {
      Text hint;
      hint.label = item.shortcut;
      hint.fontSize = 12.0f;
      hint.color = kColorTextFaint;
      row.addChild(std::move(hint));
    }
    dropdown.addChild(std::move(row));
  }

  btn.addChild(std::move(dropdown));
  return btn;
}

// One of the three window-control buttons (minimize/maximize/close).
static View winButton(const std::string &glyph, Color hoverBg,
                      std::function<void()> onClick)
{
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
                            std::function<bool()> enabled = {})
{
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
  t.color = enabled ? Dynamic<Color>(std::function<Color()>([enabled]
                                                            { return enabled() ? kColorTextDark : kColorTextFaint; }))
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
                          bool primary = false,
                          std::function<bool()> enabled = {})
{
  View b;
  b.style.width = Size::full();
  b.style.height = Size::pixel(28);
  b.style.flexShrink = 0;
  b.style.borderRadius = 4.0f;
  b.style.alignItems = Align::Center;
  b.style.padding = EdgeInsets{0, 10, 0, 10};
  b.style.hoverColor = primary ? kColorAccentHover : kColorActivityHover;
  b.style.backgroundColor =
      std::function<Color()>([selected, primary, enabled]() -> Color
                             {
        if (enabled && !enabled())
          return kColorActivityBarBg; // dimmed: no primary fill while busy
        if (primary)
          return kColorAccent;
        return (selected && selected()) ? kColorDropdownRowHover
                                        : kColorActivityBarBg; });
  b.style.borderWidth = std::function<float()>(
      [selected]
      { return (selected && selected()) ? 1.0f : 0.0f; });
  b.style.borderColor = kColorDividerHover;
  // A disabled button ignores clicks (the action functions also re-check, so
  // shortcuts are covered) and shows muted text.
  b.onClick = [onClick = std::move(onClick), enabled]
  {
    if (!enabled || enabled())
      onClick();
  };

  Text t;
  t.label = label;
  t.fontSize = 13.0f;
  const Color liveColor = primary ? kColorWhite : kColorTextDark;
  if (enabled)
    t.color = std::function<Color()>([enabled, liveColor]() -> Color
                                     { return enabled() ? liveColor : kColorTextMuted; });
  else
    t.color = liveColor;
  b.addChild(std::move(t));
  return b;
}

// What a slider row needs to know: its label, range, how to read/write the
// value, and how to print it. Shared by the Adjust sliders (via
// buildAdjustmentSlider) and the Straighten angle slider.
struct SliderSpec
{
  std::string name;
  float lo = 0.0f, hi = 1.0f;
  std::function<float()> get;
  std::function<void(float)> set;           // called with each new value while dragging
  std::function<std::string(float)> format; // value -> text after "name: "
  bool isAngle = false;                     // drag drives the angle preview instead of the
                                            // pixel-adjustment preview (see canvasDirtySource)
  bool affectsPixels = true;                // false: the value only changes a tool setting
                                            // (brush size...), so a drag needs no re-render
  bool hasAccent = false;                   // tint the fill + thumb border (Color Mixer bands)
  Color accent{};
  float trackW = 150.0f;       // track length in px
  std::function<void()> onEnd; // optional: fired when a drag on it ends
};

// A single Lightroom-style slider row: a label with the live value, then a
// draggable track. Reads and writes go through the spec's get/set rather
// than binding a raw float& the way a standalone slider widget would —
// that's what lets AdjustmentSet coalesce a whole drag into one undo step
// instead of one per frame.
static View buildSliderRow(const SliderSpec &spec)
{
  const float kTrackW = spec.trackW;
  constexpr float kTrackH = 4.0f;
  constexpr float kThumb = 16.0f;
  const float kMaxThumbX = kTrackW - kThumb;

  const SliderSpec s = spec;
  const float lo = s.lo;
  const float hi = s.hi;

  // Same bipolar-fill treatment as LiteUI's slider example: a range that
  // straddles zero (like ours, -100..100) fills outward from the center
  // instead of always from the left edge.
  const bool isBipolar = (lo < 0.0f && hi > 0.0f);
  const float anchor = isBipolar ? 0.0f : lo;

  auto thumbXFor = [lo, hi, kMaxThumbX](float v)
  {
    return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) * kMaxThumbX;
  };

  View group;
  group.style.direction = FlexDirection::Column;
  group.style.gap = 6.0f;

  Text rowLabel;
  rowLabel.label = std::function<std::string()>(
      [s]
      { return s.name + ": " + s.format(s.get()); });
  rowLabel.fontSize = 13.0f;
  rowLabel.color = kColorTextMid;
  group.addChild(std::move(rowLabel));

  View track;
  track.style.width = Size::pixel(kTrackW);
  track.style.height = Size::pixel(kTrackH);
  track.style.backgroundColor = kTrackFill;
  track.style.borderWidth = 1.0f;
  track.style.borderColor = kTrackEdge;
  track.style.borderRadius = kTrackH / 2.0f;

  // Filled portion, absolutely positioned so it can grow independently of
  // the thumb. Spans from the anchor's thumb-center position to the
  // current value's.
  View fill;
  fill.style.position = Position::Absolute;
  fill.style.top = 0.0f;
  fill.style.height = Size::pixel(kTrackH);
  fill.style.left = std::function<float()>([s, thumbXFor, anchor, isBipolar]
                                           {
    float anchorX =
        isBipolar ? thumbXFor(anchor) + kThumb / 2.0f : thumbXFor(anchor);
    float valueCenter = thumbXFor(s.get()) + kThumb / 2.0f;
    return std::min(anchorX, valueCenter); });
  fill.style.width = std::function<Size()>([s, thumbXFor, anchor, isBipolar]
                                           {
    float anchorX =
        isBipolar ? thumbXFor(anchor) + kThumb / 2.0f : thumbXFor(anchor);
    float valueCenter = thumbXFor(s.get()) + kThumb / 2.0f;
    return Size::pixel(std::abs(valueCenter - anchorX)); });
  fill.style.backgroundColor = s.hasAccent ? s.accent : kTrackEdge;
  fill.style.borderRadius = kTrackH / 2.0f;
  track.addChild(std::move(fill));

  // Shared between onPressAt and onDragTo across one drag gesture, same
  // pattern as the standalone slider widget this is adapted from.
  auto grabX = std::make_shared<float>(0.0f);

  View thumb;
  thumb.style.position = Position::Absolute;
  thumb.style.left =
      std::function<float()>([s, thumbXFor]
                             { return thumbXFor(s.get()); });
  thumb.style.top = -(kThumb - kTrackH) / 2.0f; // vertically centered
  thumb.style.width = Size::pixel(kThumb);
  thumb.style.height = Size::pixel(kThumb);
  thumb.style.backgroundColor = kThumbFill;
  thumb.style.borderWidth = 2.0f;
  thumb.style.borderColor = s.hasAccent ? s.accent : kThumbEdge;
  thumb.style.borderRadius = kThumb / 2.0f;

  thumb.onPressAt = [grabX](float lx, float)
  { *grabX = lx; };
  thumb.onDragTo = [grabX, s, thumbXFor, lo, hi, kMaxThumbX](float lx, float)
  {
    if (!state.original)
      return; // no image loaded yet — nothing to adjust
    float newThumbX =
        std::clamp(thumbXFor(s.get()) + (lx - *grabX), 0.0f, kMaxThumbX);
    s.set(lo + (newThumbX / kMaxThumbX) * (hi - lo));
    if (!s.affectsPixels)
    {
      state.markDirty(); // just repaint (an overlay may show the setting)
      return;
    }
    enterDrag(s.isAngle); // switches recompute+paint to the preview path
    // Cheap: just records the new value and asks for a recompute before
    // the next frame paints (see canvasDirtySource) — doesn't itself touch
    // a single pixel, so it stays fast no matter how often the drag fires.
    state.markAdjustDirty();
  };
  // Drag finished: leave preview mode and request one full-resolution
  // recompute (deferred to the next canvasDirtySource poll like any other
  // adjustment change — endPress polls right after this fires, so it lands
  // in the same frame the mouse is released).
  thumb.onDragEnd = [s]
  {
    if (s.onEnd)
      s.onEnd();
    if (!state.dragging)
      return;
    state.dragging = false;
    state.dragIsAngle = false;
    state.markAdjustDirty();
  };
  track.addChild(std::move(thumb));

  group.addChild(std::move(track));
  return group;
}

// One Adjust-pane slider for one AdjustmentSpec: adding a new adjustment to
// kAdjustmentSpecs needs no new UI code at all.
static View buildAdjustmentSlider(const AdjustmentSpec &spec)
{
  const AdjId id = spec.id;
  SliderSpec s;
  s.name = spec.label;
  s.lo = spec.min;
  s.hi = spec.max;
  s.get = [id]
  { return state.adjust.get(id); };
  s.set = [id](float v)
  { state.adjust.set(id, v); };
  s.format = [](float v)
  {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  return buildSliderRow(s);
}

// ---------------- Color Mixer UI ----------------
static MixChan gMixChan = MixChan::Hue; // which of Hue / Sat / Lum is shown

// One Color Mixer slider (a band's Hue, Saturation or Luminance), tinted in
// the band's own color. Same undo/preview behavior as every Adjust slider.
static View buildMixSlider(MixChan chan, size_t band)
{
  SliderSpec s;
  s.name = mixBandName(band);
  s.lo = static_cast<float>(kMixMin);
  s.hi = static_cast<float>(kMixMax);
  s.get = [chan, band]
  { return state.adjust.mix(chan, band); };
  s.set = [chan, band](float v)
  { state.adjust.setMix(chan, band, v); };
  s.format = [](float v)
  {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  s.hasAccent = true;
  s.accent = mixBandColor(band);
  return buildSliderRow(s);
}

// Zeroes all 24 mixer sliders (one undo step).
static void resetColorMix()
{
  state.adjust.resetMix();
  state.markAdjustDirty();
}

// ---------------- Color Grading UI ----------------
constexpr float kGradePaneW =
    250.0f; // sidebar width the wheels are laid out for
constexpr float kGradePi = 3.14159265358979f;
constexpr int kGradeMainWheel = 130;   // Midtones wheel in 3-way mode
constexpr int kGradeSmallWheel = 84;   // Shadows / Highlights in 3-way mode
constexpr int kGradeSingleWheel = 170; // wheel in the single-range modes
constexpr float kGradeTabW = 40.0f;

static int gGradeMode = 0;   // 0 = 3-way, 1..4 = Shadows..Global
static int gGradeActive = 1; // wheel shown in the H/S/L readout (Midtones)

static const char *gradeRangeName(int r)
{
  static const char *const n[kGradeRangeCount] = {"Shadows", "Midtones",
                                                  "Highlights", "Global"};
  return n[r];
}

// Wheel brightness is cosmetic (Lightroom dims the shadow wheel).
constexpr float kGradeWheelBrightness[kGradeRangeCount] = {0.62f, 1.0f, 0.80f,
                                                           1.0f};

static Color gradeHsv(float h, float s, float v, uint8_t a = 255)
{
  float c[3];
  gradeHueToRgb(h, c);
  auto to8 = [&](float f)
  {
    const float o = (f * s + (1.0f - s)) * v * 255.0f; // blend toward white
    return static_cast<uint8_t>(std::lround(std::clamp(o, 0.0f, 255.0f)));
  };
  return Color{to8(c[0]), to8(c[1]), to8(c[2]), a};
}

static size_t gradeHash(float v) { return std::hash<float>{}(v); }
static size_t gradeMix(size_t a, size_t b)
{
  return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
}

// Pre-renders the hue/saturation disc once per (diameter, brightness):
// angle = hue (0 at 3 o'clock, counter-clockwise), radius = saturation,
// white in the centre. Cached in a map so the CanvasImage address stays
// stable (liteui caches the converted surface per image).
static const CanvasImage &gradeWheelImage(int d, float brightness)
{
  static std::map<std::pair<int, int>, CanvasImage> cache;
  const auto key =
      std::make_pair(d, static_cast<int>(std::lround(brightness * 100)));
  const auto it = cache.find(key);
  if (it != cache.end())
    return it->second;

  CanvasImage img(d, d);
  const float R = d / 2.0f;
  for (int y = 0; y < d; ++y)
  {
    for (int x = 0; x < d; ++x)
    {
      const float dx = x + 0.5f - R, dy = y + 0.5f - R;
      const float dist = std::sqrt(dx * dx + dy * dy);
      const float alpha = std::clamp(R - dist + 0.5f, 0.0f, 1.0f); // AA rim
      if (alpha <= 0.0f)
        continue;
      const float hue = std::atan2(-dy, dx) * 180.0f / kGradePi;
      const Color c = gradeHsv(hue, std::min(1.0f, dist / R), brightness);
      const size_t i = (static_cast<size_t>(y) * d + x) * 4;
      img.pixels[i + 0] = c.r;
      img.pixels[i + 1] = c.g;
      img.pixels[i + 2] = c.b;
      img.pixels[i + 3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
    }
  }
  return cache.emplace(key, std::move(img)).first->second;
}

// Applies `fn` to a copy of the current grade and commits it (key: see
// AdjustmentSet::setGrade).
static void gradeEdit(const std::function<void(ColorGrade &)> &fn, int key)
{
  ColorGrade g = state.adjust.grade();
  fn(g);
  state.adjust.setGrade(g, key);
}

// Leaves the low-res drag preview and asks for one full-resolution render.
static void gradeEndDrag()
{
  state.adjust.endGradeEdit();
  if (state.dragging)
  {
    state.dragging = false;
    state.dragIsAngle = false;
    state.markAdjustDirty();
  }
}

// A slider bound to one field (hue / sat / lum) of one wheel.
static View buildGradeWheelSlider(const std::string &name, int range,
                                  float GradeWheel::*field, float lo, float hi,
                                  int keyBase, float trackW, bool isSigned)
{
  SliderSpec s;
  s.name = name;
  s.lo = lo;
  s.hi = hi;
  s.trackW = trackW;
  s.get = [range, field]
  { return state.adjust.grade().wheel[range].*field; };
  s.set = [range, field, keyBase](float v)
  {
    gradeEdit([&](ColorGrade &g)
              { g.wheel[range].*field = std::round(v); },
              keyBase + range);
    gGradeActive = range;
  };
  s.format = [isSigned](float v)
  {
    const int i = static_cast<int>(std::lround(v));
    return isSigned && i > 0 ? "+" + std::to_string(i) : std::to_string(i);
  };
  s.onEnd = []
  { state.adjust.endGradeEdit(); };
  return buildSliderRow(s);
}

// One hue/saturation wheel: click or drag sets hue (angle) + saturation
// (radius); right-click resets both.
static View buildGradeWheel(int range, int diameter, float pad)
{
  const float size = static_cast<float>(diameter) + 2.0f * pad;
  View c;
  c.isCanvas = true;
  c.style.width = Size::pixel(size);
  c.style.height = Size::pixel(size);
  c.style.flexShrink = 0;

  auto apply = [range, diameter, size](float lx, float ly, bool drag)
  {
    if (!state.original)
      return; // no image loaded yet — nothing to grade
    const float R = diameter / 2.0f, cx = size / 2.0f, cy = size / 2.0f;
    const float dx = lx - cx, dy = ly - cy;
    const float dist = std::sqrt(dx * dx + dy * dy);
    gradeEdit(
        [&](ColorGrade &g)
        {
          GradeWheel &w = g.wheel[range];
          w.sat = std::round(std::clamp(dist / R, 0.0f, 1.0f) * 100.0f);
          if (dist > 2.0f)
          { // keep the old hue while sitting on the centre
            float hue = std::atan2(-dy, dx) * 180.0f / kGradePi;
            if (hue < 0.0f)
              hue += 360.0f;
            w.hue = std::fmod(std::round(hue), 360.0f);
          }
        },
        range);
    gGradeActive = range;
    if (drag)
      enterDrag(false); // switches recompute+paint to the preview path
    state.markAdjustDirty();
  };
  c.onPressAt = [apply](float lx, float ly)
  { apply(lx, ly, false); };
  c.onDragTo = [apply](float lx, float ly)
  { apply(lx, ly, true); };
  c.onDragEnd = []
  { gradeEndDrag(); };
  c.onRightClick = [range] { // reset hue + sat, keep luminance
    if (!state.original)
      return;
    gradeEdit(
        [&](ColorGrade &g)
        {
          g.wheel[range].hue = 0.0f;
          g.wheel[range].sat = 0.0f;
        },
        -1);
    gGradeActive = range;
    state.markAdjustDirty();
  };

  c.onPaint = [range, diameter](CanvasContext &ctx)
  {
    const GradeWheel &w = state.adjust.grade().wheel[range];
    const float cx = ctx.width() / 2, cy = ctx.height() / 2;
    const float R = diameter / 2.0f;

    ctx.setGlobalAlpha(w.visible ? 1.0f : 0.3f);
    ctx.drawImage(gradeWheelImage(diameter, kGradeWheelBrightness[range]),
                  cx - R, cy - R);
    ctx.setGlobalAlpha(1.0f);

    const float ang = -w.hue * kGradePi / 180.0f;
    const float ca = std::cos(ang), sa = std::sin(ang);
    const float dist = w.sat / 100.0f * R;
    const float px = cx + ca * dist, py = cy + sa * dist;

    // spoke from the centre to the puck
    ctx.setStrokeColor(gradeHsv(w.hue, 0.30f, 1.0f, 200));
    ctx.setLineWidth(2.0f);
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.lineTo(px, py);
    ctx.stroke();

    // hue dot outside the rim (grey while saturation is 0)
    const float k = std::clamp(w.sat / 100.0f * 3.0f, 0.0f, 1.0f);
    const Color full = gradeHsv(w.hue, 1.0f, 1.0f);
    const Color grey{70, 70, 70};
    auto lerp8 = [k](uint8_t a, uint8_t b)
    {
      return static_cast<uint8_t>(std::lround(a + (b - a) * k));
    };
    ctx.setFillColor(Color{lerp8(grey.r, full.r), lerp8(grey.g, full.g),
                           lerp8(grey.b, full.b)});
    ctx.beginPath();
    ctx.arc(cx + ca * (R + 11), cy + sa * (R + 11), 5.0f, 0, 2 * kGradePi);
    ctx.fill();

    // puck: dark ring with a faint light halo so it reads on any colour
    ctx.setStrokeColor(Color{255, 255, 255, 90});
    ctx.setLineWidth(1.0f);
    ctx.beginPath();
    ctx.arc(px, py, 9.0f, 0, 2 * kGradePi);
    ctx.stroke();
    ctx.setStrokeColor(Color{50, 50, 50});
    ctx.setLineWidth(2.0f);
    ctx.beginPath();
    ctx.arc(px, py, 7.5f, 0, 2 * kGradePi);
    ctx.stroke();
  };

  // Repaint only when this wheel changed (drag, undo/redo, reset, new image).
  auto last = std::make_shared<size_t>(~size_t{0});
  c.canvasDirtySource = [range, last]() -> bool
  {
    const GradeWheel &w = state.adjust.grade().wheel[range];
    size_t h = gradeHash(w.hue);
    h = gradeMix(h, gradeHash(w.sat));
    h = gradeMix(h, w.visible ? 1u : 0u);
    if (h == *last)
      return false;
    *last = h;
    return true;
  };
  return c;
}

// Eye icon: toggles that wheel's preview (one undo step).
static View buildGradeEye(int range)
{
  View c;
  c.isCanvas = true;
  c.style.width = Size::pixel(22);
  c.style.height = Size::pixel(14);
  c.style.flexShrink = 0;
  c.onClick = [range]
  {
    if (!state.original)
      return;
    gradeEdit(
        [&](ColorGrade &g)
        {
          g.wheel[range].visible = !g.wheel[range].visible;
        },
        -1);
    state.markAdjustDirty();
  };
  c.onPaint = [range](CanvasContext &ctx)
  {
    const bool on = state.adjust.grade().wheel[range].visible;
    const float cx = ctx.width() / 2, cy = ctx.height() / 2;
    const Color col = on ? Color{190, 190, 190} : Color{118, 118, 118};
    ctx.setStrokeColor(col);
    ctx.setFillColor(col);
    ctx.setLineWidth(1.4f);
    ctx.beginPath();
    ctx.moveTo(cx - 8, cy);
    ctx.quadraticCurveTo(cx, cy - 9, cx + 8, cy);
    ctx.quadraticCurveTo(cx, cy + 9, cx - 8, cy);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(cx, cy, 2.2f, 0, 2 * kGradePi);
    ctx.fill();
    if (!on)
    {
      ctx.beginPath();
      ctx.moveTo(cx - 8, cy + 6);
      ctx.lineTo(cx + 8, cy - 6);
      ctx.stroke();
    }
  };
  auto last = std::make_shared<int>(-1);
  c.canvasDirtySource = [range, last]() -> bool
  {
    const int now = state.adjust.grade().wheel[range].visible ? 1 : 0;
    if (now == *last)
      return false;
    *last = now;
    return true;
  };
  return c;
}

// wheel + eye row + luminance slider, as one column
static View buildGradeWheelBlock(int range, int diameter, float pad)
{
  const float size = static_cast<float>(diameter) + 2.0f * pad;
  View col;
  col.style.direction = FlexDirection::Column;
  col.style.alignItems = Align::Center;
  col.style.gap = 2.0f;
  col.addChild(buildGradeWheel(range, diameter, pad));

  View eyeRow;
  eyeRow.style.direction = FlexDirection::Row;
  eyeRow.style.justifyContent = Justify::End;
  eyeRow.style.width = Size::pixel(static_cast<float>(diameter));
  eyeRow.addChild(buildGradeEye(range));
  col.addChild(std::move(eyeRow));

  col.addChild(buildGradeWheelSlider("Lum", range, &GradeWheel::lum, -100, 100,
                                     4, std::min(150.0f, size - 8.0f), true));
  return col;
}

// 3-way mode: big Midtones wheel on top, Shadows + Highlights below.
static View buildGradeThreeWay()
{
  View col;
  col.style.direction = FlexDirection::Column;
  col.style.alignItems = Align::Center;
  col.style.gap = 6.0f;
  col.addChild(buildGradeWheelBlock(static_cast<int>(GradeRange::Midtones),
                                    kGradeMainWheel, 20.0f));

  View row;
  row.style.direction = FlexDirection::Row;
  row.style.justifyContent = Justify::Center;
  row.style.gap = 2.0f;
  auto labeled = [](int range)
  {
    View v;
    v.style.direction = FlexDirection::Column;
    v.style.alignItems = Align::Center;
    Text t;
    t.label = gradeRangeName(range);
    t.fontSize = 12.0f;
    t.color = kColorTextMuted;
    v.addChild(std::move(t));
    v.addChild(buildGradeWheelBlock(range, kGradeSmallWheel, 14.0f));
    return v;
  };
  row.addChild(labeled(static_cast<int>(GradeRange::Shadows)));
  row.addChild(labeled(static_cast<int>(GradeRange::Highlights)));
  col.addChild(std::move(row));
  return col;
}

// Single-range mode: one large wheel + Hue / Saturation / Luminance sliders.
static View buildGradeSingle(int range)
{
  View col;
  col.style.direction = FlexDirection::Column;
  col.style.alignItems = Align::Center;
  col.style.gap = 8.0f;
  col.addChild(buildGradeWheel(range, kGradeSingleWheel, 22.0f));
  col.addChild(buildGradeWheelSlider("Hue", range, &GradeWheel::hue, 0, 359, 8,
                                     150.0f, false));
  col.addChild(buildGradeWheelSlider("Saturation", range, &GradeWheel::sat, 0,
                                     100, 12, 150.0f, false));
  col.addChild(buildGradeWheelSlider("Luminance", range, &GradeWheel::lum, -100,
                                     100, 4, 150.0f, true));
  return col;
}

// Resets every wheel, Blending and Balance (one undo step).
static void resetColorGrade()
{
  state.adjust.setGrade(ColorGrade{}, -1);
  state.markAdjustDirty();
}

// Draws the crop selection over the image: dimmed surround, rule-of-thirds
// guides, border, and eight drag handles. Expects the CTM to already be
// scaled by state.zoom (so everything here is in image pixels), and divides
// line widths / handle sizes by the zoom to keep them constant on screen.
// BEGIN maskui
// ---------------- Masks: selection, editing, overlay ----------------
// The Masks pane (activity 7) edits `state.adjust.masks()`. Sliders go through
// buildSliderRow like everywhere else; the shape can also be dragged on the
// canvas (press inside = move, press a square handle = resize).
constexpr int kMaskCtlMove = 90; // undo-coalescing control ids (see maskEdit)
constexpr int kMaskCtlResizeX = 91;
constexpr int kMaskCtlResizeY = 92;
constexpr int kMaskCtlLinEnd = 93;
constexpr int kMaskCtlLinStart = 94;
constexpr int kMaskCtlBrush = 95; // one stroke = one undo step
constexpr int kMaskCtlLumLo = 96;
constexpr int kMaskCtlLumHi = 97;
constexpr int kMaskCtlRange = 84;
constexpr int kMaskCtlFeather = 1;
constexpr int kMaskCtlAngle = 2;
constexpr int kMaskCtlWidth = 3;
constexpr int kMaskCtlHeight = 4;
constexpr int kMaskCtlLength = 5;
constexpr int kMaskCtlAdj = 10; // + AdjId
constexpr int kMaskCtlName = 86;

static bool maskToolActive()
{
  return state.activeActivity == 7 && state.image != nullptr;
}

// Width of one of `n` equal chips / tabs sharing a row of the mask panel.
static float maskChipW(int n)
{
  const float inner =
      state.maskPanelW - 2.0f * kMaskPanelPad - kScrollbarThickness;
  return std::max(20.0f, (inner - 4.0f * static_cast<float>(n - 1)) /
                             static_cast<float>(n));
}

// The selected mask's index, or -1. Clamped here instead of at every place
// that can shrink the list (undo, redo, open, delete).
static int selMask()
{
  const int n = static_cast<int>(state.adjust.masks().size());
  if (state.maskSel >= n)
    state.maskSel = n - 1;
  if (state.maskSel < -1)
    state.maskSel = -1;
  return state.maskSel;
}

static const Mask *selectedMask()
{
  const int i = selMask();
  return i < 0 ? nullptr : &state.adjust.masks()[static_cast<size_t>(i)];
}

// The mask's radial shape as it sits in the picture being shown (see the
// "masks" block: masks are STORED against the original picture). Everything
// on the canvas — outlines, hit tests, drags — works on this form.
// The selected mask's part that the canvas and the shape sliders work on. A
// mask can hold several parts (see MaskComposite); like selMask() this is
// clamped on read, so undo / delete / open never need to fix it up.
static int selPart()
{
  const Mask *m = selectedMask();
  if (!m || m->parts.empty())
    return 0;
  const int n = static_cast<int>(m->parts.size());
  if (state.mask.part >= n)
    state.mask.part = n - 1;
  if (state.mask.part < 0)
    state.mask.part = 0;
  return state.mask.part;
}
static const MaskPart *selectedPart()
{
  const Mask *m = selectedMask();
  if (!m || m->parts.empty())
    return nullptr;
  return &m->parts[static_cast<size_t>(selPart())];
}
static MaskType selectedPartType()
{
  const MaskPart *p = selectedPart();
  return p ? p->type() : MaskType::Count;
}
static bool selectedIsBrush()
{ // the SELECTED PART is a brush
  return selectedPartType() == MaskType::Brush;
}
// "Show mask tint": a brush that stands alone has no outline, so it keeps its
// own switch (on by default); everything else shares the common one.
static bool &tintFlag()
{
  const Mask *m = selectedMask();
  if (m && m->parts.size() == 1 && m->parts[0].type() == MaskType::Brush)
    return state.mask.brushTint;
  return state.maskShowTint;
}

// A part's shape as it sits in the picture being shown (see the "masks" block:
// masks are STORED against the original picture). Everything on the canvas —
// outlines, hit tests, drags — works on this form.
template <class Shape>
static std::optional<Shape> shapeInView(const MaskPart &p, const GeoFrame &f)
{
  if (const auto *r = std::get_if<Shape>(&p.shape))
    return r->toSource(f);
  return std::nullopt;
}
static std::optional<RadialShape> radialInView(const MaskPart &p,
                                               const GeoFrame &f)
{
  return shapeInView<RadialShape>(p, f);
}
static std::optional<LinearShape> linearInView(const MaskPart &p,
                                               const GeoFrame &f)
{
  return shapeInView<LinearShape>(p, f);
}
static std::optional<BrushShape> brushInView(const MaskPart &p,
                                             const GeoFrame &f)
{
  return shapeInView<BrushShape>(p, f);
}

// Edits part `part` of a mask on its in-view form and stores the result back
// (only the fields `fn` changed; see RadialShape::applySourceEdit).
template <class Shape>
static void editShapeInView(Mask &m, size_t part, const GeoFrame &f,
                            const std::function<void(Shape &)> &fn)
{
  if (part >= m.parts.size())
    return;
  auto *stored = std::get_if<Shape>(&m.parts[part].shape);
  if (!stored)
    return;
  const Shape before = stored->toSource(f);
  Shape after = before;
  fn(after);
  stored->applySourceEdit(before, after, f);
}
static void editRadialInView(Mask &m, size_t part, const GeoFrame &f,
                             const std::function<void(RadialShape &)> &fn)
{
  editShapeInView<RadialShape>(m, part, f, fn);
}
static void editLinearInView(Mask &m, size_t part, const GeoFrame &f,
                             const std::function<void(LinearShape &)> &fn)
{
  editShapeInView<LinearShape>(m, part, f, fn);
}

// Edits mask `idx` in a copy of the list and commits it. `control` >= 0 names
// the control being dragged: repeated edits of the same control of the same
// mask (and, for the selected mask, the same part) fold into one undo step
// (see AdjustmentSet::setMasks). -1 = discrete.
// Does NOT recompute the picture — slider / canvas drags do that through the
// usual markAdjustDirty path; discrete actions call maskCommitDiscrete.
static void maskEdit(int idx, const std::function<void(Mask &)> &fn,
                     int control)
{
  MaskSet ms = state.adjust.masks();
  if (idx < 0 || idx >= static_cast<int>(ms.size()))
    return;
  const int part = idx == selMask() ? selPart() : 0;
  fn(ms[static_cast<size_t>(idx)]);
  state.adjust.setMasks(
      ms, control < 0
              ? -1
              : (idx * static_cast<int>(kMaxMaskParts) + part) * 100 + control);
}

// A one-off change to the selected mask (toggle, reset): its own undo step and
// an immediate recompute.
static void maskEditDiscrete(const std::function<void(Mask &)> &fn)
{
  const int i = selMask();
  if (i < 0)
    return;
  maskEdit(i, fn, -1);
  updateImageFromAdjustments();
  state.markDirty();
}

// ---- Mask name field ----
// The TextInput keeps its text in a shared state; we hold a pointer to it so
// the field can follow the selected mask (selection change, undo / redo).
static std::shared_ptr<TextInputState> gMaskNameState;

// True while the name field has keyboard focus: the global shortcuts that
// would also fire on plain keys (Y, J, Left, Right, Delete...) must stand
// down, or typing a name would toggle views and Delete would delete the mask.
static bool typingInTextField()
{
  return gMaskNameState && gMaskNameState->focused;
}

// Makes the field show the selected mask's name. Cheap; polled from the
// field's display lambda, so it also catches undo / redo.
static void syncMaskNameField()
{
  if (!gMaskNameState)
    return;
  const Mask *m = selectedMask();
  const std::string want = m ? m->name : std::string();
  TextInputState &t = *gMaskNameState;
  if (t.text == want)
    return;
  t.text = want;
  t.cursor = want.size();
  t.scrollOffset = 0.0f;
  t.dirty = true;
}

static void renameSelectedMask(const std::string &typed)
{
  const std::string s = sanitizeMaskName(typed);
  if (gMaskNameState && gMaskNameState->text != s)
  { // the cap (or a stripped character) changed what was typed
    gMaskNameState->text = s;
    gMaskNameState->cursor = std::min(gMaskNameState->cursor, s.size());
    gMaskNameState->dirty = true;
  }
  // Typing folds into one undo step (until Enter / another edit).
  maskEdit(selMask(), [&](Mask &m)
           { m.name = s; }, kMaskCtlName);
  state.markDirty();
}

static void setSelectedObjectMap(std::shared_ptr<const ObjectMap> m)
{
  const size_t part = static_cast<size_t>(selPart());
  maskEditDiscrete([&](Mask &mk)
                   {
    if (part < mk.parts.size())
      if (auto *o = std::get_if<ObjectShape>(&mk.parts[part].shape))
        o->map = m; });
  state.maskShowTint = true;
  state.markDirty();
}

// Step-1 stand-in for a segmenter: pixels brighter than the picture's mean.
static std::shared_ptr<const ObjectMap> makeTestObjectMap()
{
  if (!state.original)
    return nullptr;
  const CanvasImage &im = *state.original;
  const int k = std::max(1, (std::max(im.width, im.height) +
                             kObjectMapEdge - 1) /
                                kObjectMapEdge);
  const int w = std::max(1, im.width / k), h = std::max(1, im.height / k);
  std::vector<int> luma(static_cast<size_t>(w) * h);
  double sum = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      const uint8_t *p =
          &im.pixels[(static_cast<size_t>(y * k) * im.width + x * k) * 4];
      const int l = (54 * p[0] + 183 * p[1] + 19 * p[2] + 128) >> 8;
      luma[static_cast<size_t>(y) * w + x] = l;
      sum += l;
    }
  const double mean = sum / static_cast<double>(luma.size());
  auto m = std::make_shared<ObjectMap>();
  m->w = w;
  m->h = h;
  m->id = nextObjectMapId();
  m->a.resize(luma.size());
  for (size_t i = 0; i < luma.size(); ++i)
    m->a[i] = luma[i] > mean ? 255 : 0;
  return m;
}

static bool selectedIsObject()
{
  return selectedPartType() == MaskType::Object;
}

// The prompts a new click builds on: the ones still being segmented for this
// part, else the committed ones. Original-picture fractions.
static std::vector<ObjectPrompt> objectCurrentPrompts(int mask, int part)
{
  const MaskUi::ObjPending &p = state.mask.objPending;
  if (p.mask == mask && p.part == part)
    return p.prompts;
  const MaskSet &ms = state.adjust.masks();
  if (mask < 0 || mask >= static_cast<int>(ms.size()))
    return {};
  const Mask &m = ms[static_cast<size_t>(mask)];
  if (part < 0 || part >= static_cast<int>(m.parts.size()))
    return {};
  if (const auto *o = std::get_if<ObjectShape>(&m.parts[static_cast<size_t>(part)].shape))
    return o->prompts;
  return {};
}

static void clearSelectedObject()
{
  segmentWorker.invalidate();
  state.mask.objPending = {};
  const size_t part = static_cast<size_t>(selPart());
  maskEditDiscrete([part](Mask &m)
                   {
    if (part < m.parts.size())
      if (auto *o = std::get_if<ObjectShape>(&m.parts[part].shape)) {
        o->map.reset();
        o->prompts.clear();
      } });
}

static void objectPrefetch()
{
  if (!state.original || segmentWorker.outstanding() ||
      state.mask.encodedKey == state.imageSerial)
    return;
  SegmentRequest req;
  req.image = state.original;
  req.imageKey = state.imageSerial;
  req.prefetch = true;
  segmentWorker.submit(std::move(req));
  setStatus("Preparing object selection...", false);
}

// A click on the picture with an object part selected. Left click adds a
// point (foreground, or background if "Click adds Background" is on);
// right click adds the opposite kind; clicking an existing dot removes it.
static void objectPress(float lx, float ly, bool rightClick)
{
  state.openMenu = -1;
  if (!maskToolActive() || !selectedIsObject() || !state.original)
    return;
  const int mask = selMask(), part = selPart();
  const GeoFrame frame = state.adjust.geoFrame();
  const float W = state.docWidth(), H = state.docHeight();
  const float px = lx / state.zoom, py = ly / state.zoom;
  std::vector<ObjectPrompt> prompts = objectCurrentPrompts(mask, part);

  // Existing dot under the pointer -> remove it.
  const float tol = kMaskHitPx / state.zoom;
  bool removed = false;
  for (size_t k = 0; k < prompts.size(); ++k)
  {
    double sx, sy;
    frame.toSrc(prompts[k].x * frame.origW, prompts[k].y * frame.origH, sx,
                sy);
    if (std::hypot(sx / frame.srcW * W - px, sy / frame.srcH * H - py) <= tol)
    {
      prompts.erase(prompts.begin() + static_cast<std::ptrdiff_t>(k));
      removed = true;
      break;
    }
  }
  if (!removed)
  {
    double ox, oy; // view -> original
    frame.toOrig(px / W * frame.srcW, py / H * frame.srcH, ox, oy);
    if (ox < 0 || oy < 0 || ox >= frame.origW || oy >= frame.origH)
      return; // outside the picture (straighten's empty corners)
    ObjectPrompt p;
    p.x = static_cast<float>(ox / frame.origW);
    p.y = static_cast<float>(oy / frame.origH);
    p.fg = (rightClick != state.mask.objBg) ? false : true;
    // left + objBg=false -> fg; left + objBg=true -> bg;
    // right + objBg=false -> bg; right + objBg=true -> fg
    prompts.push_back(p);
  }
  if (prompts.empty())
  { // last dot removed: nothing selected any more
    clearSelectedObject();
    return;
  }
  SegmentRequest req;
  req.image = state.original;
  req.imageKey = state.imageSerial;
  req.prompts = prompts;
  req.maskIdx = mask;
  req.part = part;
  state.mask.objPending = {mask, part, std::move(prompts)};
  segmentWorker.submit(std::move(req));
  setStatus(state.mask.encodedKey == state.imageSerial
                ? "Selecting object..."
                : "Analyzing image (first selection takes a moment)...",
            false);
  state.markDirty();
}

// The starting shape of each kind of mask (nullopt for an unknown kind).
static std::optional<MaskShape> newMaskShape(MaskType t)
{
  switch (t)
  {
  case MaskType::Radial:
    // Centered on the picture as it is shown now.
    return RadialShape::fromSource(RadialShape{}, state.adjust.geoFrame());
  case MaskType::Linear:
    return LinearShape::fromSource(LinearShape{}, state.adjust.geoFrame());
  case MaskType::Brush:
    return BrushShape{}; // empty: the first drag on the image paints
  case MaskType::Luminosity:
    return LuminosityShape{}; // not spatial: nothing to place
  case MaskType::Object:
    return ObjectShape{};
  case MaskType::ColorRange:
    return ColorRangeShape{}; // nothing selected until a color is clicked
  default:
    return std::nullopt;
  }
}

static void objectPrefetch();

static void addMask(MaskType t)
{
  if (!state.original)
    return;
  MaskSet ms = state.adjust.masks();
  if (ms.size() >= kMaxMasks)
  {
    setStatus("Mask limit reached (" + std::to_string(kMaxMasks) + ")", true);
    return;
  }
  const auto shape = newMaskShape(t);
  if (!shape)
    return;
  Mask m;
  m.parts.push_back(MaskPart{*shape, MaskOp::Add});
  ms.push_back(std::move(m));
  state.adjust.setMasks(ms, -1);
  state.maskSel = static_cast<int>(ms.size()) - 1;
  state.mask.part = 0;
  // A new mask has no sliders moved yet, so no pixels change: overlay only.
  state.markDirty();

  if (t == MaskType::Object)
    objectPrefetch();
}

static void deleteSelectedMask()
{
  const int i = selMask();
  if (i < 0)
    return;
  MaskSet ms = state.adjust.masks();
  ms.erase(ms.begin() + i);
  state.adjust.setMasks(ms, -1);
  state.maskSel = std::min(i, static_cast<int>(ms.size()) - 1);
  state.mask.part = 0;
  updateImageFromAdjustments();
  state.markDirty();
}

// ---- Mask list operations: duplicate, reorder, copy / paste effects ----
// A copy of the selected mask (shapes and effects) right after it, selected.
static void duplicateSelectedMask()
{
  const int i = selMask();
  if (i < 0)
    return;
  MaskSet ms = state.adjust.masks();
  if (ms.size() >= kMaxMasks)
  {
    setStatus("Mask limit reached (" + std::to_string(kMaxMasks) + ")", true);
    return;
  }
  Mask copy = ms[static_cast<size_t>(i)];
  if (!maskNameBlank(copy.name))
    copy.name = sanitizeMaskName(copy.name + " copy");
  ms.insert(ms.begin() + i + 1, copy);
  state.adjust.setMasks(ms, -1);
  state.maskSel = i + 1;
  state.mask.part = 0;
  updateImageFromAdjustments();
  state.markDirty();
}

// Masks apply in list order (later ones on top, and a luminosity range sees
// the result of the masks before it), so order matters. dir = -1 up, +1 down.
static void moveSelectedMask(int dir)
{
  const int i = selMask();
  const int j = i + dir;
  MaskSet ms = state.adjust.masks();
  if (i < 0 || j < 0 || j >= static_cast<int>(ms.size()))
    return;
  std::swap(ms[static_cast<size_t>(i)], ms[static_cast<size_t>(j)]);
  state.adjust.setMasks(ms, -1);
  state.maskSel = j;
  updateImageFromAdjustments();
  state.markDirty();
}

// What Copy / Paste Effects moves between masks: everything but the shape.
struct MaskEffects
{
  float amount = 100.0f;
  AdjValues adj{};
  MixValues mix{};
  ColorGrade grade;
  CurveSet curves = identityCurves();
};
static std::optional<MaskEffects> gMaskEffectsClip;

static void copyMaskEffects()
{
  const Mask *m = selectedMask();
  if (!m)
    return;
  gMaskEffectsClip = MaskEffects{m->amount, m->adj, m->mix, m->grade, m->curves};
  setStatus("Mask effects copied", false);
}

static void pasteMaskEffects()
{
  if (!gMaskEffectsClip)
  {
    setStatus("Copy a mask's effects first", true);
    return;
  }
  const MaskEffects fx = *gMaskEffectsClip;
  maskEditDiscrete([&](Mask &m)
                   {
    m.amount = fx.amount;
    m.adj = fx.adj;
    m.mix = fx.mix;
    m.grade = fx.grade;
    m.curves = fx.curves; });
}

// ---- Parts: combining shapes inside the selected mask ----
// Adds a part of kind `t` (joined with Add; change it with setPartOp) and
// selects it. The combined result is what matters from here on, so the tint
// is switched on.
static void addPart(MaskType t)
{
  const int i = selMask();
  if (i < 0)
    return;
  const auto shape = newMaskShape(t);
  if (!shape)
    return;
  MaskSet ms = state.adjust.masks();
  Mask &m = ms[static_cast<size_t>(i)];
  if (m.parts.size() >= kMaxMaskParts)
  {
    setStatus("Part limit reached (" + std::to_string(kMaxMaskParts) + ")",
              true);
    return;
  }
  m.parts.push_back(MaskPart{*shape, MaskOp::Add});
  state.adjust.setMasks(ms, -1);
  state.mask.part = static_cast<int>(m.parts.size()) - 1;
  state.maskShowTint = true;
  updateImageFromAdjustments();
  state.markDirty();

  if (t == MaskType::Object)
    objectPrefetch();
}

// How the selected part joins the ones before it (the first part has none).
static void setPartOp(MaskOp op)
{
  const size_t j = static_cast<size_t>(selPart());
  if (j == 0)
    return;
  maskEditDiscrete([&](Mask &m)
                   {
    if (j < m.parts.size())
      m.parts[j].op = op; });
}

static void removeSelectedPart()
{
  const Mask *sm = selectedMask();
  if (!sm || sm->parts.size() <= 1)
    return;
  const size_t j = static_cast<size_t>(selPart());
  maskEditDiscrete([&](Mask &m)
                   {
    if (m.parts.size() > 1 && j < m.parts.size())
      m.parts.erase(m.parts.begin() + static_cast<std::ptrdiff_t>(j)); });
  state.mask.part = static_cast<int>(j) - 1; // the one before; selPart clamps
  state.markDirty();
}

// A drag on the canvas or a mask slider ended: leave the low-res preview and
// ask for one full-resolution render (same job as gradeEndDrag).
static void maskEndDrag()
{
  state.mask.drag = MaskUi::Drag::None;
  state.adjust.endMaskEdit();
  if (state.dragging)
  {
    state.dragging = false;
    state.dragIsAngle = false;
    state.markAdjustDirty();
  }
}

// ---- Radial mask geometry on the canvas (document pixels) ----
struct MaskGeom
{
  float cx, cy; // center
  float rx, ry; // half-axes
  float ang;    // radians, clockwise
  float c, s;   // cos, sin of ang
  float S;      // the shorter document side (radius unit)
};

static MaskGeom radialGeom(const RadialShape &r)
{
  MaskGeom g{};
  const float W = state.docWidth(), H = state.docHeight();
  g.S = std::min(W, H);
  g.cx = r.cx * W;
  g.cy = r.cy * H;
  g.rx = r.rx * g.S;
  g.ry = r.ry * g.S;
  g.ang = r.angle * (3.14159265358979f / 180.0f);
  g.c = std::cos(g.ang);
  g.s = std::sin(g.ang);
  return g;
}

enum
{
  kMaskHitNone = 0,
  kMaskHitBody,
  kMaskHitX,
  kMaskHitY,
  kMaskHitLinEnd,
  kMaskHitLinStart
};

// What is under (px, py)? Handles (only for the selected mask) win over the
// body. The four handles sit on the rotated axes at the ellipse outline.
static int maskHit(const RadialShape &r, float px, float py, bool handles)
{
  const MaskGeom g = radialGeom(r);
  const float tol = kMaskHitPx / state.zoom;
  if (handles)
  {
    const float hx[2] = {g.cx + g.rx * g.c, g.cx - g.rx * g.c};
    const float hy[2] = {g.cy + g.rx * g.s, g.cy - g.rx * g.s};
    const float vx[2] = {g.cx - g.ry * g.s, g.cx + g.ry * g.s};
    const float vy[2] = {g.cy + g.ry * g.c, g.cy - g.ry * g.c};
    for (int k = 0; k < 2; ++k)
    {
      if (std::hypot(px - hx[k], py - hy[k]) <= tol)
        return kMaskHitX;
      if (std::hypot(px - vx[k], py - vy[k]) <= tol)
        return kMaskHitY;
    }
  }
  const float dx = px - g.cx, dy = py - g.cy;
  const float u = (dx * g.c + dy * g.s) / g.rx;
  const float v = (-dx * g.s + dy * g.c) / g.ry;
  return (u * u + v * v <= 1.0f) ? kMaskHitBody : kMaskHitNone;
}

// ---- Linear mask geometry on the canvas (document pixels) ----
struct LinGeom
{
  float cx, cy; // center
  float ux, uy; // unit direction of fade-out
  float len;    // half length of the ramp
  float ang;    // radians
  float S;      // the shorter document side
  // The weight-1 line's and the weight-0 line's axis points.
  float sx() const { return cx - ux * len; }
  float sy() const { return cy - uy * len; }
  float ex() const { return cx + ux * len; }
  float ey() const { return cy + uy * len; }
};

static LinGeom linearGeom(const LinearShape &l)
{
  LinGeom g{};
  const float W = state.docWidth(), H = state.docHeight();
  g.S = std::min(W, H);
  g.cx = l.cx * W;
  g.cy = l.cy * H;
  g.len = std::max(l.len, kMaskMinRadius) * g.S;
  g.ang = l.angle * (3.14159265358979f / 180.0f);
  g.ux = std::cos(g.ang);
  g.uy = std::sin(g.ang);
  return g;
}

// Handles (selected mask only) sit where the axis crosses the two lines; the
// body is the band between the lines.
static int linearHit(const LinearShape &l, float px, float py, bool handles)
{
  const LinGeom g = linearGeom(l);
  const float tol = kMaskHitPx / state.zoom;
  if (handles)
  {
    if (std::hypot(px - g.ex(), py - g.ey()) <= tol)
      return kMaskHitLinEnd;
    if (std::hypot(px - g.sx(), py - g.sy()) <= tol)
      return kMaskHitLinStart;
  }
  const float s = (px - g.cx) * g.ux + (py - g.cy) * g.uy;
  return std::fabs(s) <= g.len ? kMaskHitBody : kMaskHitNone;
}

// ---- Brush painting ----
// With a brush part selected, every press on the image starts a stroke and the
// drag extends it; the whole stroke is one undo step (control kMaskCtlBrush,
// closed by maskEndDrag). Points are recorded in document pixels of the
// picture on screen and stored through the GeoFrame.

// A brush mask has no body to grab; its "pin" (where its first stroke began)
// is what an unselected brush is picked by.
static bool brushPinHit(const BrushShape &b, float px, float py)
{
  if (b.strokes.empty() || b.strokes.front().pts.empty())
    return false;
  const BrushPt &p = b.strokes.front().pts.front();
  const float tol = kMaskHitPx / state.zoom;
  return std::hypot(px - p.x * state.docWidth(), py - p.y * state.docHeight()) <=
         tol;
}

static void brushBegin(float px, float py)
{
  const int i = selMask();
  if (i < 0)
    return;
  const size_t part = static_cast<size_t>(selPart());
  const float W = state.docWidth(), H = state.docHeight();
  const GeoFrame frame = state.adjust.geoFrame();
  const MaskUi &u = state.mask;
  BrushStroke st;
  st.erase = u.brushErase;
  st.size = BrushShape::sizeFromSource(u.brushSize, frame);
  st.feather = u.brushFeather;
  st.density = u.brushDensity;
  st.flow = u.brushFlow;
  st.pts.push_back(BrushShape::pointFromSource(BrushPt{px / W, py / H}, frame));
  bool full = false;
  maskEdit(
      i,
      [&](Mask &m)
      {
        auto *b = part < m.parts.size()
                      ? std::get_if<BrushShape>(&m.parts[part].shape)
                      : nullptr;
        if (!b)
          return;
        if (b->strokes.size() >= kBrushMaxStrokes)
        {
          full = true;
          return;
        }
        b->strokes.push_back(std::move(st));
      },
      kMaskCtlBrush);
  if (full)
  {
    setStatus("Brush stroke limit reached (" + std::to_string(kBrushMaxStrokes) +
                  ")",
              true);
    return;
  }
  state.mask.drag = MaskUi::Drag::Paint;
  state.mask.lastX = state.mask.curX = px;
  state.mask.lastY = state.mask.curY = py;
  enterDrag(false); // recompute on the cheap preview path while held
  state.markAdjustDirty();
  state.markDirty();
}

static void brushDragTo(float px, float py)
{
  const int i = selMask();
  if (i < 0)
    return;
  MaskUi &u = state.mask;
  const size_t part = static_cast<size_t>(selPart());
  u.curX = px;
  u.curY = py;
  const float W = state.docWidth(), H = state.docHeight();
  const float rad = u.brushSize * std::min(W, H);
  // Skip points closer than a fraction of the radius: they add nothing to the
  // rendered stroke and would only bloat it (and every undo snapshot).
  const float gap = std::max(rad * kBrushPointGap, 0.5f / state.zoom);
  if (std::hypot(px - u.lastX, py - u.lastY) >= gap)
  {
    const GeoFrame frame = state.adjust.geoFrame();
    const BrushPt pt =
        BrushShape::pointFromSource(BrushPt{px / W, py / H}, frame);
    maskEdit(
        i,
        [&](Mask &m)
        {
          auto *b = part < m.parts.size()
                        ? std::get_if<BrushShape>(&m.parts[part].shape)
                        : nullptr;
          if (!b || b->strokes.empty())
            return;
          auto &pts = b->strokes.back().pts;
          if (pts.size() < kBrushMaxPoints)
            pts.push_back(pt);
        },
        kMaskCtlBrush);
    u.lastX = px;
    u.lastY = py;
    enterDrag(false);
    state.markAdjustDirty();
  }
  state.markDirty(); // the cursor ring follows the pointer
}

static std::shared_ptr<CanvasImage> lumTintBase(); // defined with the luminosity overlay below

// Color of the picture at (fx, fy) (fractions of the picture on screen) as
// it stands just BEFORE mask `sel`: the same pixels ColorRangeEval sees.
static std::optional<std::array<float, 3>> samplePreMaskColor(int sel,
                                                              float fx, float fy)
{
  const MaskSet &ms = state.adjust.masks();
  const auto base = lumTintBase();
  if (!base || sel < 0 || sel >= static_cast<int>(ms.size()))
    return std::nullopt;
  RenderParams rp = state.adjust.buildRenderParams();
  rp.masks = buildMaskPasses(MaskSet(ms.begin(), ms.begin() + sel),
                             state.adjust.geoFrame());
  const auto pre = renderWith(*base, rp);
  const int x = std::clamp(static_cast<int>(fx * pre->width), 0, pre->width - 1);
  const int y = std::clamp(static_cast<int>(fy * pre->height), 0, pre->height - 1);
  // Average a 5x5 patch so one noisy pixel doesn't pick the color.
  float sum[3] = {0, 0, 0};
  int n = 0;
  for (int dy = -2; dy <= 2; ++dy)
    for (int dx = -2; dx <= 2; ++dx)
    {
      const int xx = std::clamp(x + dx, 0, pre->width - 1);
      const int yy = std::clamp(y + dy, 0, pre->height - 1);
      const uint8_t *p =
          &pre->pixels[(static_cast<size_t>(yy) * pre->width + xx) * 4];
      for (int c = 0; c < 3; ++c)
        sum[c] += p[c];
      ++n;
    }
  return std::array<float, 3>{sum[0] / (255.0f * n), sum[1] / (255.0f * n),
                              sum[2] / (255.0f * n)};
}

static void colorRangePress(float px, float py)
{
  const float W = state.docWidth(), H = state.docHeight();
  if (W <= 0 || H <= 0)
    return;
  const auto c = samplePreMaskColor(selMask(), px / W, py / H);
  if (!c)
    return;
  const size_t part = static_cast<size_t>(selPart());
  maskEditDiscrete([&](Mask &m)
                   {
    if (part < m.parts.size())
      if (auto *s = std::get_if<ColorRangeShape>(&m.parts[part].shape))
      {
        s->set = true;
        s->r = (*c)[0];
        s->g = (*c)[1];
        s->b = (*c)[2];
      } });
}

static void maskPress(float lx, float ly)
{
  if (!maskToolActive())
    return;
  state.mask.drag = MaskUi::Drag::None;
  const MaskSet &ms = state.adjust.masks();
  const GeoFrame frame = state.adjust.geoFrame();
  const float px = lx / state.zoom, py = ly / state.zoom;
  const int sel = selMask();
  const int selP = selPart();
  const int nMasks = static_cast<int>(ms.size());
  const auto partCount = [&](int i)
  {
    return static_cast<int>(ms[static_cast<size_t>(i)].parts.size());
  };
  const auto partAt = [&](int i, int j) -> const MaskPart &
  {
    return ms[static_cast<size_t>(i)].parts[static_cast<size_t>(j)];
  };
  // With a brush part selected every press paints (pick another part from
  // the list, or its pin if it is a brush).
  if (sel >= 0 && partCount(sel) > 0 &&
      std::holds_alternative<BrushShape>(partAt(sel, selP).shape))
  {
    bool picked = false;
    for (int i = nMasks - 1; i >= 0 && !picked; --i)
      for (int j = partCount(i) - 1; j >= 0 && !picked; --j)
      {
        if (i == sel && j == selP)
          continue;
        if (const auto bv = brushInView(partAt(i, j), frame))
          if (brushPinHit(*bv, px, py))
          {
            state.maskSel = i;
            state.mask.part = j;
            state.markDirty();
            picked = true;
          }
      }
    if (!picked)
      brushBegin(px, py);
    return;
  }

  if (sel >= 0 && partCount(sel) > 0 &&
      std::holds_alternative<ObjectShape>(partAt(sel, selP).shape))
  {
    objectPress(lx, ly, false); // every press is a prompt, like a brush
    return;
  }
  // Color Range: every press samples a new color (like a brush, no handles).
  if (sel >= 0 && partCount(sel) > 0 &&
      std::holds_alternative<ColorRangeShape>(partAt(sel, selP).shape))
  {
    colorRangePress(px, py);
    return;
  }
  // Tries part `j` of mask `i`; on a hit selects it and starts the matching
  // drag.
  auto tryPart = [&](int i, int j, bool isSelected) -> bool
  {
    const MaskPart &m = partAt(i, j);
    int hit = kMaskHitNone;
    float shapeCx = 0, shapeCy = 0; // center, fraction of the picture
    if (const auto rv = radialInView(m, frame))
    {
      hit = maskHit(*rv, px, py, isSelected);
      shapeCx = rv->cx;
      shapeCy = rv->cy;
    }
    else if (const auto lv = linearInView(m, frame))
    {
      hit = linearHit(*lv, px, py, isSelected);
      shapeCx = lv->cx;
      shapeCy = lv->cy;
    }
    else if (const auto bv = brushInView(m, frame))
    {
      // Nothing to grab on a brush: its pin only selects it.
      if (!brushPinHit(*bv, px, py))
        return false;
      state.maskSel = i;
      state.mask.part = j;
      state.markDirty();
      return true;
    }
    if (hit == kMaskHitNone)
      return false;
    state.maskSel = i;
    state.mask.part = j;
    if (hit == kMaskHitX)
    {
      state.mask.drag = MaskUi::Drag::ResizeX;
    }
    else if (hit == kMaskHitY)
    {
      state.mask.drag = MaskUi::Drag::ResizeY;
    }
    else if (hit == kMaskHitLinEnd)
    {
      state.mask.drag = MaskUi::Drag::LinEnd;
    }
    else if (hit == kMaskHitLinStart)
    {
      state.mask.drag = MaskUi::Drag::LinStart;
    }
    else
    {
      state.mask.drag = MaskUi::Drag::Move;
      state.mask.grabX = px - shapeCx * state.docWidth();
      state.mask.grabY = py - shapeCy * state.docHeight();
    }
    state.markDirty();
    return true;
  };
  if (sel >= 0 && partCount(sel) > 0 && tryPart(sel, selP, true))
    return;
  // The selected mask's other parts next (last part first), so a click on one
  // of them picks that part rather than a mask underneath.
  if (sel >= 0)
    for (int j = partCount(sel) - 1; j >= 0; --j)
      if (j != selP && tryPart(sel, j, false))
        return;
  // Otherwise clicking inside another mask selects it (topmost first).
  for (int i = nMasks - 1; i >= 0; --i)
  {
    if (i == sel)
      continue;
    for (int j = partCount(i) - 1; j >= 0; --j)
      if (tryPart(i, j, false))
        return;
  }
}

static void maskDrag(float lx, float ly)
{
  if (!maskToolActive() || state.mask.drag == MaskUi::Drag::None)
    return;
  const int i = selMask();
  if (i < 0)
    return;
  const float W = state.docWidth(), H = state.docHeight();
  const float px = lx / state.zoom, py = ly / state.zoom;
  const MaskUi::Drag d = state.mask.drag;
  if (d == MaskUi::Drag::Paint)
  {
    brushDragTo(px, py);
    return;
  }
  const float grabX = state.mask.grabX, grabY = state.mask.grabY;
  const GeoFrame frame = state.adjust.geoFrame();
  const size_t part = static_cast<size_t>(selPart());
  maskEdit(
      i,
      [&](Mask &m)
      {
        editRadialInView(m, part, frame, [&](RadialShape &r)
                         {
          if (d == MaskUi::Drag::Move) {
            r.cx = std::clamp((px - grabX) / W, 0.0f, 1.0f);
            r.cy = std::clamp((py - grabY) / H, 0.0f, 1.0f);
          } else {
            // Distance of the pointer from the center along the rotated axis.
            const MaskGeom g = radialGeom(r);
            const float dx = px - g.cx, dy = py - g.cy;
            if (d == MaskUi::Drag::ResizeX)
              r.rx = std::clamp(std::fabs(dx * g.c + dy * g.s) / g.S,
                                kMaskMinRadius, kMaskMaxRadius);
            else
              r.ry = std::clamp(std::fabs(-dx * g.s + dy * g.c) / g.S,
                                kMaskMinRadius, kMaskMaxRadius);
          } });
        editLinearInView(m, part, frame, [&](LinearShape &l)
                         {
          if (d == MaskUi::Drag::Move) {
            l.cx = std::clamp((px - grabX) / W, 0.0f, 1.0f);
            l.cy = std::clamp((py - grabY) / H, 0.0f, 1.0f);
          } else {
            // Dragging a line's handle sets the half length AND turns the
            // axis so the handle stays under the pointer (the start handle
            // is on the opposite side of the center, so its pull is negated).
            const LinGeom g = linearGeom(l);
            const float sign = d == MaskUi::Drag::LinEnd ? 1.0f : -1.0f;
            const float dx = (px - g.cx) * sign, dy = (py - g.cy) * sign;
            const float dist = std::hypot(dx, dy);
            l.len = std::clamp(dist / g.S, kMaskMinRadius, kMaskMaxRadius);
            if (dist > 1e-3f)
              l.angle = std::atan2(dy, dx) * (180.0f / 3.14159265358979f);
          } });
      },
      d == MaskUi::Drag::Move      ? kMaskCtlMove
      : d == MaskUi::Drag::ResizeX ? kMaskCtlResizeX
      : d == MaskUi::Drag::ResizeY ? kMaskCtlResizeY
      : d == MaskUi::Drag::LinEnd  ? kMaskCtlLinEnd
                                   : kMaskCtlLinStart);
  enterDrag(false); // recompute on the cheap preview path while held
  state.markAdjustDirty();
  state.markDirty(); // the overlay follows the pointer
}

// Red tint showing the mask's weight (1 = solid, 0 = clear) over the picture.
static void paintRadialTint(CanvasContext &ctx, const RadialShape &r,
                            bool invert)
{
  const MaskGeom g = radialGeom(r);
  const float W = state.docWidth(), H = state.docHeight();
  const float inner = r.innerFrac();
  const Color on{255, 40, 40, 120}, off{255, 40, 40, 0};
  ctx.save();
  ctx.beginPath();
  ctx.rect(0, 0, W, H);
  ctx.clip();
  // Unit-circle gradient under the ellipse's own transform.
  ctx.translate(g.cx, g.cy);
  ctx.rotate(g.ang);
  ctx.scale(g.rx, g.ry);
  CanvasGradient grad = CanvasGradient::radial(0, 0, 0, 0, 0, 1);
  grad.addColorStop(0.0f, invert ? off : on);
  grad.addColorStop(inner, invert ? off : on);
  grad.addColorStop(1.0f, invert ? on : off);
  ctx.setFillGradient(grad);
  const float R = (W + H) / std::min(g.rx, g.ry) + 2.0f; // covers the document
  ctx.fillRect(-R, -R, 2.0f * R, 2.0f * R);
  ctx.restore();
}

// Outline of one radial mask; the selected one also gets its feather ring
// and handles.
static void paintRadialOutline(CanvasContext &ctx, const RadialShape &r,
                               bool selected)
{
  const MaskGeom g = radialGeom(r);
  const float px = 1.0f / state.zoom; // one screen pixel, in image units
  constexpr float kTwoPi = 6.28318530717959f;

  ctx.beginPath();
  ctx.ellipse(g.cx, g.cy, g.rx, g.ry, g.ang, 0.0f, kTwoPi);
  ctx.setStrokeColor(Color{0, 0, 0, selected ? uint8_t{150} : uint8_t{90}});
  ctx.setLineWidth(px * 3.0f);
  ctx.stroke();
  ctx.setStrokeColor(
      Color{255, 255, 255, selected ? uint8_t{255} : uint8_t{140}});
  ctx.setLineWidth(px * (selected ? 1.5f : 1.0f));
  ctx.stroke();
  if (!selected)
    return;

  // Where the feather begins (skipped when it would sit on the outline or
  // collapse into the center).
  const float inner = r.innerFrac();
  if (r.feather > 0.02f && inner > 0.02f)
  {
    ctx.beginPath();
    ctx.ellipse(g.cx, g.cy, g.rx * inner, g.ry * inner, g.ang, 0.0f, kTwoPi);
    ctx.setLineDash({4.0f * px, 4.0f * px});
    ctx.setStrokeColor(Color{255, 255, 255, 170});
    ctx.setLineWidth(px);
    ctx.stroke();
    ctx.setLineDash({});
  }

  // Four square handles on the axes, and a ring at the center.
  const float hs = kMaskHandlePx * px;
  const float hx[4] = {g.cx + g.rx * g.c, g.cx - g.rx * g.c, g.cx - g.ry * g.s,
                       g.cx + g.ry * g.s};
  const float hy[4] = {g.cy + g.rx * g.s, g.cy - g.rx * g.s, g.cy + g.ry * g.c,
                       g.cy - g.ry * g.c};
  ctx.setFillColor(kColorWhite);
  ctx.setStrokeColor(Color{60, 60, 60, 255});
  ctx.setLineWidth(px);
  for (int k = 0; k < 4; ++k)
  {
    ctx.fillRect(hx[k] - hs / 2.0f, hy[k] - hs / 2.0f, hs, hs);
    ctx.strokeRect(hx[k] - hs / 2.0f, hy[k] - hs / 2.0f, hs, hs);
  }
  ctx.beginPath();
  ctx.arc(g.cx, g.cy, hs * 0.55f, 0.0f, kTwoPi);
  ctx.setFillColor(Color{255, 255, 255, 60});
  ctx.fill();
  ctx.setStrokeColor(kColorWhite);
  ctx.setLineWidth(px * 1.5f);
  ctx.stroke();
}

// Red tint of a linear mask: a gradient along the axis from the weight-1 line
// to the weight-0 line, with stops sampled from the same smoothstep the
// renderer uses (the gradient itself is piecewise linear).
static void paintLinearTint(CanvasContext &ctx, const LinearShape &l,
                            bool invert)
{
  const LinGeom g = linearGeom(l);
  const float W = state.docWidth(), H = state.docHeight();
  const auto alphaAt = [invert](float t) -> Color
  {
    float w = 1.0f - t * t * (3.0f - 2.0f * t);
    if (invert)
      w = 1.0f - w;
    return Color{255, 40, 40, static_cast<uint8_t>(std::lround(120.0f * w))};
  };
  CanvasGradient grad = CanvasGradient::linear(g.sx(), g.sy(), g.ex(), g.ey());
  constexpr int kStops = 8;
  for (int k = 0; k <= kStops; ++k)
  {
    const float t = static_cast<float>(k) / kStops;
    grad.addColorStop(t, alphaAt(t));
  }
  ctx.save();
  ctx.setFillGradient(grad);
  ctx.fillRect(0, 0, W, H); // the gradient pads beyond both lines
  ctx.restore();
}

// Outline of one linear mask: the two lines of the ramp; the selected one
// also gets the center line, the axis with an arrowhead, and its handles.
static void paintLinearOutline(CanvasContext &ctx, const LinearShape &l,
                               bool selected)
{
  const LinGeom g = linearGeom(l);
  const float px = 1.0f / state.zoom; // one screen pixel, in image units
  constexpr float kTwoPi = 6.28318530717959f;
  const float W = state.docWidth(), H = state.docHeight();
  const float R = W + H;             // long enough to cross the document
  const float vx = -g.uy, vy = g.ux; // along the lines
  const auto line = [&](float ax, float ay)
  {
    ctx.beginPath();
    ctx.moveTo(ax - vx * R, ay - vy * R);
    ctx.lineTo(ax + vx * R, ay + vy * R);
  };
  const auto strokeTwice = [&](float wOuter, float wInner, uint8_t a1,
                               uint8_t a2)
  {
    ctx.setStrokeColor(Color{0, 0, 0, a1});
    ctx.setLineWidth(px * wOuter);
    ctx.stroke();
    ctx.setStrokeColor(Color{255, 255, 255, a2});
    ctx.setLineWidth(px * wInner);
    ctx.stroke();
  };

  // The lines are clipped to the picture; handles are drawn outside the clip.
  ctx.save();
  ctx.beginPath();
  ctx.rect(0, 0, W, H);
  ctx.clip();
  const uint8_t a1 = selected ? uint8_t{150} : uint8_t{90};
  const uint8_t a2 = selected ? uint8_t{255} : uint8_t{140};
  line(g.sx(), g.sy());
  strokeTwice(3.0f, selected ? 1.5f : 1.0f, a1, a2);
  line(g.ex(), g.ey());
  strokeTwice(3.0f, selected ? 1.5f : 1.0f, a1, a2);
  if (selected)
  {
    line(g.cx, g.cy); // the 50% line
    ctx.setLineDash({4.0f * px, 4.0f * px});
    ctx.setStrokeColor(Color{255, 255, 255, 170});
    ctx.setLineWidth(px);
    ctx.stroke();
    ctx.setLineDash({});
  }
  ctx.restore();
  if (!selected)
    return;

  // The axis between the handles, with an arrowhead showing the fade-out.
  const float hs = kMaskHandlePx * px;
  ctx.beginPath();
  ctx.moveTo(g.sx(), g.sy());
  ctx.lineTo(g.ex(), g.ey());
  ctx.setStrokeColor(Color{255, 255, 255, 200});
  ctx.setLineWidth(px);
  ctx.stroke();
  ctx.beginPath();
  ctx.moveTo(g.ex() + g.ux * hs * 1.6f, g.ey() + g.uy * hs * 1.6f);
  ctx.lineTo(g.ex() + g.ux * hs * 0.7f + vx * hs * 0.7f,
             g.ey() + g.uy * hs * 0.7f + vy * hs * 0.7f);
  ctx.lineTo(g.ex() + g.ux * hs * 0.7f - vx * hs * 0.7f,
             g.ey() + g.uy * hs * 0.7f - vy * hs * 0.7f);
  ctx.closePath();
  ctx.setFillColor(kColorWhite);
  ctx.fill();

  // Square handles on the two lines, and a ring at the center.
  const float hx[2] = {g.sx(), g.ex()}, hy[2] = {g.sy(), g.ey()};
  ctx.setFillColor(kColorWhite);
  ctx.setStrokeColor(Color{60, 60, 60, 255});
  ctx.setLineWidth(px);
  for (int k = 0; k < 2; ++k)
  {
    ctx.fillRect(hx[k] - hs / 2.0f, hy[k] - hs / 2.0f, hs, hs);
    ctx.strokeRect(hx[k] - hs / 2.0f, hy[k] - hs / 2.0f, hs, hs);
  }
  ctx.beginPath();
  ctx.arc(g.cx, g.cy, hs * 0.55f, 0.0f, kTwoPi);
  ctx.setFillColor(Color{255, 255, 255, 60});
  ctx.fill();
  ctx.setStrokeColor(kColorWhite);
  ctx.setLineWidth(px * 1.5f);
  ctx.stroke();
}

// ---- Brush overlay ----
// The red coverage bitmap: the brush rasterized at a small size through the
// SAME BrushShape::prepare the renderer uses, so what is tinted is exactly what
// gets adjusted. Cached until the strokes or the picture's size change (the
// shape is compared, which is cheap next to rasterizing it).
static std::shared_ptr<CanvasImage> brushTintImage(const BrushShape &b,
                                                   bool invert)
{
  const float W = state.docWidth(), H = state.docHeight();
  const float sc =
      std::min(1.0f, static_cast<float>(kBrushTintEdge) / std::max(W, H));
  const int tw = std::max(1, static_cast<int>(std::lround(W * sc)));
  const int th = std::max(1, static_cast<int>(std::lround(H * sc)));
  MaskUi &u = state.mask;
  if (u.tint && u.tint->width == tw && u.tint->height == th &&
      u.tintInvert == invert && u.tintShape == b)
    return u.tint;
  const BrushEval ev = b.prepare(tw, th);
  auto img = std::make_shared<CanvasImage>(tw, th);
  std::vector<float> wb(static_cast<size_t>(tw));
  for (int y = 0; y < th; ++y)
  {
    ev.rowWeights(y, 0, tw, wb.data());
    uint8_t *row = img->pixels.data() +
                   static_cast<size_t>(y) * static_cast<size_t>(tw) * 4;
    for (int x = 0; x < tw; ++x)
    {
      float w = wb[static_cast<size_t>(x)];
      if (invert)
        w = 1.0f - w;
      uint8_t *p = row + static_cast<size_t>(x) * 4;
      p[0] = 255;
      p[1] = 40;
      p[2] = 40;
      p[3] = static_cast<uint8_t>(std::lround(120.0f * w));
    }
  }
  u.tint = img;
  u.tintShape = b;
  u.tintInvert = invert;
  return img;
}

static void paintBrushTint(CanvasContext &ctx, const BrushShape &b,
                           bool invert)
{
  if (b.strokes.empty() && !invert)
    return; // nothing painted yet
  const auto img = brushTintImage(b, invert);
  ctx.drawImage(*img, 0, 0, state.docWidth(), state.docHeight());
}

// The pin marks where a brush mask's first stroke began (all a brush has
// for an "outline"); the selected one is drawn solid.
static void paintBrushPin(CanvasContext &ctx, const BrushShape &b,
                          bool selected)
{
  if (b.strokes.empty() || b.strokes.front().pts.empty())
    return;
  const BrushPt &p = b.strokes.front().pts.front();
  const float px = 1.0f / state.zoom; // one screen pixel, in image units
  constexpr float kTwoPi = 6.28318530717959f;
  const float cx = p.x * state.docWidth(), cy = p.y * state.docHeight();
  const float r = kMaskHandlePx * px * 0.55f;
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0.0f, kTwoPi);
  ctx.setFillColor(Color{255, 255, 255, selected ? uint8_t{200} : uint8_t{90}});
  ctx.fill();
  ctx.setStrokeColor(Color{0, 0, 0, selected ? uint8_t{200} : uint8_t{120}});
  ctx.setLineWidth(px * 1.5f);
  ctx.stroke();
}

// The brush's footprint at the pointer while painting: the rim, and where the
// feather begins.
static void paintBrushCursor(CanvasContext &ctx)
{
  const MaskUi &u = state.mask;
  const float px = 1.0f / state.zoom;
  constexpr float kTwoPi = 6.28318530717959f;
  const float r =
      u.brushSize * std::min(state.docWidth(), state.docHeight());
  ctx.beginPath();
  ctx.arc(u.curX, u.curY, r, 0.0f, kTwoPi);
  ctx.setStrokeColor(Color{0, 0, 0, 150});
  ctx.setLineWidth(px * 3.0f);
  ctx.stroke();
  ctx.setStrokeColor(kColorWhite);
  ctx.setLineWidth(px * 1.5f);
  ctx.stroke();
  const float inner = std::clamp(1.0f - u.brushFeather, 0.0f, 0.99f);
  if (u.brushFeather > 0.02f && inner > 0.02f)
  {
    ctx.beginPath();
    ctx.arc(u.curX, u.curY, r * inner, 0.0f, kTwoPi);
    ctx.setLineDash({4.0f * px, 4.0f * px});
    ctx.setStrokeColor(Color{255, 255, 255, 170});
    ctx.setLineWidth(px);
    ctx.stroke();
    ctx.setLineDash({});
  }
}

// ---- Luminosity overlay ----
static bool sameOwner(const std::weak_ptr<CanvasImage> &a,
                      const std::shared_ptr<CanvasImage> &b)
{
  return !a.owner_before(b) && !b.owner_before(a);
}

// state.source box-downsampled by an integer factor to at most
// kLumTintEdge on its long side (the source itself when already that small).
static std::shared_ptr<CanvasImage> lumTintBase()
{
  const std::shared_ptr<CanvasImage> src = state.source;
  if (!src || src->width <= 0 || src->height <= 0)
    return nullptr;
  MaskUi &u = state.mask;
  if (u.lumBase && sameOwner(u.lumBaseSrc, src))
    return u.lumBase;
  const int longEdge = std::max(src->width, src->height);
  const int k = std::max(1, (longEdge + kLumTintEdge - 1) / kLumTintEdge);
  std::shared_ptr<CanvasImage> base = src;
  if (k > 1)
  {
    const int w = src->width / k, h = src->height / k;
    if (w <= 0 || h <= 0)
      return nullptr;
    auto out = std::make_shared<CanvasImage>(w, h);
    const int area = k * k;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        int sum[4] = {0, 0, 0, 0};
        for (int dy = 0; dy < k; ++dy)
        {
          const uint8_t *row =
              &src->pixels[(static_cast<size_t>(y * k + dy) * src->width +
                            static_cast<size_t>(x * k)) *
                           4];
          for (int dx = 0; dx < k; ++dx)
            for (int c = 0; c < 4; ++c)
              sum[c] += row[dx * 4 + c];
        }
        uint8_t *o = &out->pixels[(static_cast<size_t>(y) * w + x) * 4];
        for (int c = 0; c < 4; ++c)
          o[c] = static_cast<uint8_t>((sum[c] + area / 2) / area);
      }
    base = std::move(out);
  }
  u.lumBase = base;
  u.lumBaseSrc = src;
  return base;
}

// What the mask selects, as a red bitmap. The weights come from the SAME
// LuminosityShape::prepare the renderer uses, run over the small picture as
// it stands just before this mask: the global adjustments plus the masks
// listed above it, but not this mask's own edits (which cannot change what it
// selects). Cached until the shape, the picture's pixels or the mask's place in
// the list change.
static std::shared_ptr<CanvasImage>
luminosityTintImage(const LuminosityShape &s, bool invert, int sel)
{
  MaskUi &u = state.mask;
  if (u.lumTint && u.lumSel == sel && u.lumInvert == invert &&
      u.lumShape == s && sameOwner(u.lumKeyImage, state.image) &&
      sameOwner(u.lumKeyPreview, state.previewImage))
    return u.lumTint;
  const MaskSet &ms = state.adjust.masks();
  const auto base = lumTintBase();
  if (!base || sel < 0 || sel >= static_cast<int>(ms.size()))
    return nullptr;
  RenderParams rp = state.adjust.buildRenderParams();
  rp.masks = buildMaskPasses(MaskSet(ms.begin(), ms.begin() + sel),
                             state.adjust.geoFrame());
  const auto pre = renderWith(*base, rp);
  const int tw = pre->width, th = pre->height;
  const LuminosityEval ev = s.prepare(tw, th);
  auto img = std::make_shared<CanvasImage>(tw, th);
  std::vector<float> wb(static_cast<size_t>(tw));
  for (int y = 0; y < th; ++y)
  {
    const size_t off = static_cast<size_t>(y) * static_cast<size_t>(tw) * 4;
    ev.rowWeights(y, 0, tw, wb.data(), pre->pixels.data() + off);
    uint8_t *row = img->pixels.data() + off;
    for (int x = 0; x < tw; ++x)
    {
      float w = wb[static_cast<size_t>(x)];
      if (invert)
        w = 1.0f - w;
      uint8_t *p = row + static_cast<size_t>(x) * 4;
      p[0] = 255;
      p[1] = 40;
      p[2] = 40;
      p[3] = static_cast<uint8_t>(std::lround(120.0f * w));
    }
  }
  u.lumTint = img;
  u.lumShape = s;
  u.lumInvert = invert;
  u.lumSel = sel;
  u.lumKeyImage = state.image;
  u.lumKeyPreview = state.previewImage;
  return img;
}

static void paintLuminosityTint(CanvasContext &ctx, const LuminosityShape &s,
                                bool invert, int sel)
{
  if (const auto img = luminosityTintImage(s, invert, sel))
    ctx.drawImage(*img, 0, 0, state.docWidth(), state.docHeight());
}

// ---- Combined mask overlay ----
// The red bitmap of a selected mask with SEVERAL parts: the whole combination,
// evaluated by the same MaskComposite the renderer uses, on a small picture.
// If any part is a luminosity range the picture is the one just BEFORE this
// mask (see luminosityTintImage); otherwise the weights need no pixels and
// the bitmap is sized from the document. Cached on the parts in their in-view
// form, so a rotate / crop / edit all rebuild it.
static std::shared_ptr<CanvasImage> compositeTintImage(const Mask &m,
                                                       int sel)
{
  MaskUi &u = state.mask;
  const GeoFrame frame = state.adjust.geoFrame();
  bool usesPix = false;
  std::vector<MaskPart> view;
  view.reserve(m.parts.size());
  for (const MaskPart &p : m.parts)
  {
    if (maskTypeUsesPixels(p.type()))
      usesPix = true;
    view.push_back(MaskPart{maskShapeToSource(p.shape, frame), p.op});
  }
  int tw = 0, th = 0;
  if (!usesPix)
  {
    const float W = state.docWidth(), H = state.docHeight();
    const float sc = std::min(1.0f, static_cast<float>(kCompTintEdge) /
                                        std::max(W, H));
    tw = std::max(1, static_cast<int>(std::lround(W * sc)));
    th = std::max(1, static_cast<int>(std::lround(H * sc)));
  }
  if (u.compTint && u.compSel == sel && u.compInvert == m.invert &&
      u.compUsesPix == usesPix && u.compParts == view &&
      (usesPix ? (sameOwner(u.compKeyImage, state.image) &&
                  sameOwner(u.compKeyPreview, state.previewImage))
               : (u.compW == tw && u.compH == th)))
    return u.compTint;

  std::shared_ptr<CanvasImage> pre;
  if (usesPix)
  {
    const MaskSet &ms = state.adjust.masks();
    const auto base = lumTintBase();
    if (!base || sel < 0 || sel >= static_cast<int>(ms.size()))
      return nullptr;
    RenderParams rp = state.adjust.buildRenderParams();
    rp.masks = buildMaskPasses(MaskSet(ms.begin(), ms.begin() + sel), frame);
    pre = renderWith(*base, rp);
    tw = pre->width;
    th = pre->height;
  }
  std::vector<MaskPassPart> pp;
  pp.reserve(view.size());
  for (const MaskPart &p : view)
    pp.push_back(MaskPassPart{p.shape, p.op});
  const MaskComposite comp = prepareComposite(pp, tw, th);
  auto img = std::make_shared<CanvasImage>(tw, th);
  std::vector<float> wb(static_cast<size_t>(tw)), wb2(static_cast<size_t>(tw));
  for (int y = 0; y < th; ++y)
  {
    const size_t off = static_cast<size_t>(y) * static_cast<size_t>(tw) * 4;
    comp.rowWeights(y, 0, tw, wb.data(),
                    pre ? pre->pixels.data() + off : nullptr, wb2.data());
    uint8_t *row = img->pixels.data() + off;
    for (int x = 0; x < tw; ++x)
    {
      float w = wb[static_cast<size_t>(x)];
      if (m.invert)
        w = 1.0f - w;
      uint8_t *p = row + static_cast<size_t>(x) * 4;
      p[0] = 255;
      p[1] = 40;
      p[2] = 40;
      p[3] = static_cast<uint8_t>(std::lround(120.0f * w));
    }
  }
  u.compTint = img;
  u.compParts = std::move(view);
  u.compInvert = m.invert;
  u.compUsesPix = usesPix;
  u.compSel = sel;
  u.compW = tw;
  u.compH = th;
  u.compKeyImage = state.image;
  u.compKeyPreview = state.previewImage;
  return img;
}

static void paintCompositeTint(CanvasContext &ctx, const Mask &m, int sel)
{
  if (const auto img = compositeTintImage(m, sel))
    ctx.drawImage(*img, 0, 0, state.docWidth(), state.docHeight());
}

static void paintSelectedObjectPrompts(CanvasContext &ctx)
{
  const MaskPart *part = selectedPart();
  if (!part || !std::holds_alternative<ObjectShape>(part->shape))
    return;
  const auto &stored = std::get<ObjectShape>(part->shape);
  const auto &pend = state.mask.objPending;
  const std::vector<ObjectPrompt> &src =
      (pend.mask == selMask() && pend.part == selPart()) ? pend.prompts
                                                         : stored.prompts;
  const GeoFrame f = state.adjust.geoFrame();
  const float W = state.docWidth(), H = state.docHeight();
  const float px = 1.0f / state.zoom;
  for (const ObjectPrompt &p : src)
  {
    double sx, sy;
    f.toSrc(p.x * f.origW, p.y * f.origH, sx, sy);
    const float cx = static_cast<float>(sx / f.srcW) * W;
    const float cy = static_cast<float>(sy / f.srcH) * H;
    ctx.beginPath();
    ctx.arc(cx, cy, 5.0f * px, 0.0f, 6.28318530717959f);
    ctx.setFillColor(p.fg ? Color{60, 220, 90, 255} : Color{80, 150, 255, 255});
    ctx.fill();
    ctx.setStrokeColor(kColorWhite);
    ctx.setLineWidth(1.5f * px);
    ctx.stroke();
  }
}

// A part's outline / handles / pin (nothing for a luminosity range).
static void paintPartOutline(CanvasContext &ctx, const MaskPart &p,
                             const GeoFrame &frame, bool selected)
{
  if (const auto r = radialInView(p, frame))
    paintRadialOutline(ctx, *r, selected);
  else if (const auto l = linearInView(p, frame))
    paintLinearOutline(ctx, *l, selected);
  else if (const auto b = brushInView(p, frame))
    paintBrushPin(ctx, *b, selected);
}

static void paintMaskOverlay(CanvasContext &ctx)
{
  const MaskSet &ms = state.adjust.masks();
  const int sel = selMask();
  const int selP = selPart();
  const GeoFrame frame = state.adjust.geoFrame();
  // Unselected masks first (faint), the selected one on top.
  for (int i = 0; i < static_cast<int>(ms.size()); ++i)
  {
    if (i == sel)
      continue;
    for (const MaskPart &p : ms[static_cast<size_t>(i)].parts)
      paintPartOutline(ctx, p, frame, false);
  }
  if (sel < 0)
    return;
  const Mask &m = ms[static_cast<size_t>(sel)];
  if (m.parts.empty())
    return;
  if (m.parts.size() > 1)
  {
    // Several parts: tint the combination, then the parts' own outlines
    // (the selected part's with its handles).
    if (state.maskShowTint)
      paintCompositeTint(ctx, m, sel);
    for (int j = 0; j < static_cast<int>(m.parts.size()); ++j)
      if (j != selP)
        paintPartOutline(ctx, m.parts[static_cast<size_t>(j)], frame, false);
    paintPartOutline(ctx, m.parts[static_cast<size_t>(selP)], frame, true);
    paintSelectedObjectPrompts(ctx);
    if (state.mask.drag == MaskUi::Drag::Paint && selectedIsBrush())
      paintBrushCursor(ctx);
    return;
  }
  const MaskPart &p = m.parts[0];
  if (const auto r = radialInView(p, frame))
  {
    if (state.maskShowTint)
      paintRadialTint(ctx, *r, m.invert);
    paintRadialOutline(ctx, *r, true);
  }
  else if (const auto l = linearInView(p, frame))
  {
    if (state.maskShowTint)
      paintLinearTint(ctx, *l, m.invert);
    paintLinearOutline(ctx, *l, true);
  }
  else if (const auto b = brushInView(p, frame))
  {
    if (state.mask.brushTint)
      paintBrushTint(ctx, *b, m.invert);
    paintBrushPin(ctx, *b, true);
    if (state.mask.drag == MaskUi::Drag::Paint)
      paintBrushCursor(ctx);
  }
  else if (const auto lm = shapeInView<LuminosityShape>(p, frame))
  {
    // No outline or handles: the tint is the whole overlay.
    if (state.maskShowTint)
      paintLuminosityTint(ctx, *lm, m.invert, sel);
  }
  else if (std::holds_alternative<ObjectShape>(p.shape))
  {
    if (state.maskShowTint)
      paintCompositeTint(ctx, m, sel);
    paintSelectedObjectPrompts(ctx);
  }
  else if (std::holds_alternative<ColorRangeShape>(p.shape))
  {
    // Not spatial: the tint (built from the picture before this mask) is
    // the whole overlay.
    if (state.maskShowTint)
      paintCompositeTint(ctx, m, sel);
  }
}

// ---- Masks pane widgets ----

// What a mask is called in the list: its first part's kind, then the next
// one, then how many more ("Radial + Brush +1").
static std::string maskLabel(const Mask &m)
{
  if (m.parts.empty())
    return "Empty";
  if (!maskNameBlank(m.name))
    return m.name;
  std::string s = maskTypeName(m.parts[0].type());
  if (m.parts.size() >= 2)
    s += std::string(" + ") + maskTypeName(m.parts[1].type());
  if (m.parts.size() > 2)
    s += " +" + std::to_string(m.parts.size() - 2);
  return s;
}

// One row of the mask list: "1  Radial", highlighted when selected. There are
// kMaxMasks of these, shown only for the masks that exist (the view tree is
// built once, so the list is a fixed set of slots).
static View buildMaskRow(size_t i)
{
  View b;
  b.style.width = Size::full();
  b.style.height = Size::pixel(28);
  b.style.flexShrink = 0;
  b.style.borderRadius = 4.0f;
  b.style.alignItems = Align::Center;
  b.style.padding = EdgeInsets{0, 10, 0, 10};
  b.style.hoverColor = kColorActivityHover;
  b.style.display = std::function<Display()>([i]
                                             { return i < state.adjust.masks().size() ? Display::Flex : Display::None; });
  const auto isSel = [i]
  { return selMask() == static_cast<int>(i); };
  b.style.backgroundColor = std::function<Color()>([isSel]() -> Color
                                                   { return isSel() ? kColorDropdownRowHover : kColorActivityBarBg; });
  b.style.borderWidth =
      std::function<float()>([isSel]
                             { return isSel() ? 1.0f : 0.0f; });
  b.style.borderColor = kColorDividerHover;
  b.onClick = [i]
  {
    if (state.maskSel != static_cast<int>(i))
      state.mask.part = 0; // a different mask: start on its first part
    state.maskSel = static_cast<int>(i);
    state.markDirty(); // move the overlay to it
  };

  Text t;
  t.label = std::function<std::string()>([i]() -> std::string
                                         {
    const MaskSet &ms = state.adjust.masks();
    if (i >= ms.size())
      return std::string();
    const Mask &m = ms[i];
    std::string s = std::to_string(i + 1) + "   " + maskLabel(m);
    if (m.invert)
      s += "  (inverted)";
    if (!m.enabled)
      s += "  (off)";
    return s; });
  t.fontSize = 13.0f;
  t.color = kColorTextDark;
  b.addChild(std::move(t));
  return b;
}

// ---------------- History panel ----------------
// Row r shows step (n - 1 - r): newest at the top, the untouched picture at
// the bottom. There are kMaxUndoSteps + 1 rows, shown only for steps that
// exist (the view tree is built once, like the mask list).
static size_t historyStepForRow(size_t r)
{
  const size_t n = state.adjust.historySize();
  return r < n ? n - 1 - r : n; // n = "no such step"
}

static void jumpToHistory(size_t step)
{
  if (state.dragging || step >= state.adjust.historySize() ||
      step == state.adjust.historyIndex())
    return;
  state.adjust.goToHistory(step);
  updateImageFromAdjustments(); // same follow-up as undo() / redo()
}

static View buildHistoryRow(size_t r)
{
  View b;
  b.style.width = Size::full();
  b.style.height = Size::pixel(26);
  b.style.flexShrink = 0;
  b.style.borderRadius = 4.0f;
  b.style.alignItems = Align::Center;
  b.style.padding = EdgeInsets{0, 10, 0, 10};
  b.style.overflowX = Overflow::Hidden;
  b.style.hoverColor = kColorActivityHover;
  b.style.display = std::function<Display()>([r]
                                             { return state.activeActivity == kHistoryActivity &&
                                                              r < state.adjust.historySize()
                                                          ? Display::Flex
                                                          : Display::None; });
  const auto isCur = [r]
  { return historyStepForRow(r) == state.adjust.historyIndex(); };
  b.style.backgroundColor = std::function<Color()>([isCur]() -> Color
                                                   { return isCur() ? kColorDropdownRowHover : kColorActivityBarBg; });
  b.style.borderWidth =
      std::function<float()>([isCur]
                             { return isCur() ? 1.0f : 0.0f; });
  b.style.borderColor = kColorDividerHover;
  b.onClick = [r]
  { jumpToHistory(historyStepForRow(r)); };

  Text t;
  t.label = std::function<std::string()>([r]() -> std::string
                                         {
    // Skip the diffing entirely while the pane is closed.
    if (state.activeActivity != kHistoryActivity)
      return std::string();
    const size_t s = historyStepForRow(r);
    return s < state.adjust.historySize() ? state.adjust.historyLabel(s)
                                          : std::string(); });
  t.fontSize = 13.0f;
  t.wrap = TextWrap::NoWrap;
  // Steps ahead of the current one (what Redo would bring back) are dimmed.
  t.color = std::function<Color()>([r]() -> Color
                                   { return historyStepForRow(r) > state.adjust.historyIndex()
                                                ? kColorTextMuted
                                                : kColorTextDark; });
  b.addChild(std::move(t));
  return b;
}

// One row of the selected mask's PART list: "1   Radial", "2   Subtract:
// Brush". Shown only while the mask has more than one part (a one-part mask
// looks exactly as it always did).
static View buildPartRow(size_t j)
{
  View b;
  b.style.width = Size::full();
  b.style.height = Size::pixel(26);
  b.style.flexShrink = 0;
  b.style.borderRadius = 4.0f;
  b.style.alignItems = Align::Center;
  b.style.padding = EdgeInsets{0, 10, 0, 18};
  b.style.hoverColor = kColorActivityHover;
  b.style.display = std::function<Display()>([j]
                                             {
    const Mask *m = selectedMask();
    return m && m->parts.size() > 1 && j < m->parts.size() ? Display::Flex
                                                           : Display::None; });
  const auto isSel = [j]
  {
    return selMask() >= 0 && static_cast<size_t>(selPart()) == j;
  };
  b.style.backgroundColor = std::function<Color()>([isSel]() -> Color
                                                   { return isSel() ? kColorDropdownRowHover : kColorActivityBarBg; });
  b.style.borderWidth =
      std::function<float()>([isSel]
                             { return isSel() ? 1.0f : 0.0f; });
  b.style.borderColor = kColorDividerHover;
  b.onClick = [j]
  {
    state.mask.part = static_cast<int>(j);
    state.markDirty(); // move the handles to it
  };

  Text t;
  t.label = std::function<std::string()>([j]() -> std::string
                                         {
    const Mask *m = selectedMask();
    if (!m || j >= m->parts.size())
      return std::string();
    std::string s = std::to_string(j + 1) + "   ";
    if (j > 0)
      s += std::string(maskOpName(m->parts[j].op)) + ": ";
    s += maskTypeName(m->parts[j].type());
    return s; });
  t.fontSize = 12.0f;
  t.color = kColorTextDark;
  b.addChild(std::move(t));
  return b;
}

// A shape slider for the selected part. `get` / `set` see the shape
// itself; `control` names the undo-coalescing control.
template <class Shape>
static SliderSpec maskShapeSlider(const char *name, float lo, float hi,
                                  std::function<float(const Shape &)> get,
                                  std::function<void(Shape &, float)> set,
                                  int control)
{
  SliderSpec s;
  s.name = name;
  s.lo = lo;
  s.hi = hi;
  s.trackW = kMaskTrackW;
  // The sliders show and edit the shape as it sits in the picture on screen.
  s.get = [get]
  {
    const MaskPart *p = selectedPart();
    const auto r =
        p ? shapeInView<Shape>(*p, state.adjust.geoFrame()) : std::nullopt;
    return r ? get(*r) : 0.0f;
  };
  s.set = [set, control](float v)
  {
    const GeoFrame frame = state.adjust.geoFrame();
    const size_t part = static_cast<size_t>(selPart());
    maskEdit(selMask(), [&](Mask &m)
             { editShapeInView<Shape>(m, part, frame,
                                      [&](Shape &r)
                                      { set(r, v); }); }, control);
  };
  s.format = [](float v)
  {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  s.onEnd = []
  { state.adjust.endMaskEdit(); };
  return s;
}
static SliderSpec
maskRadialSlider(const char *name, float lo, float hi,
                 std::function<float(const RadialShape &)> get,
                 std::function<void(RadialShape &, float)> set, int control)
{
  return maskShapeSlider<RadialShape>(name, lo, hi, std::move(get),
                                      std::move(set), control);
}
static SliderSpec
maskLinearSlider(const char *name, float lo, float hi,
                 std::function<float(const LinearShape &)> get,
                 std::function<void(LinearShape &, float)> set, int control)
{
  return maskShapeSlider<LinearShape>(name, lo, hi, std::move(get),
                                      std::move(set), control);
}

// The mask's own copy of one Adjust slider (Exposure, Contrast, ...): one per
// kAdjustmentSpecs entry, so a new global adjustment appears here too.
static View buildMaskAdjustmentSlider(const AdjustmentSpec &spec)
{
  const AdjId id = spec.id;
  SliderSpec s;
  s.name = spec.label;
  s.lo = spec.min;
  s.hi = spec.max;
  s.trackW = kMaskTrackW;
  s.get = [id]
  {
    const Mask *m = selectedMask();
    return m ? m->adj[static_cast<size_t>(id)] : 0.0f;
  };
  s.set = [id](float v)
  {
    maskEdit(selMask(), [&](Mask &m)
             { m.adj[static_cast<size_t>(id)] = std::round(v); }, kMaskCtlAdj + static_cast<int>(id));
  };
  s.format = [](float v)
  {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  s.onEnd = []
  { state.adjust.endMaskEdit(); };
  return buildSliderRow(s);
}
// END maskui

static void paintCropOverlay(CanvasContext &ctx)
{
  const auto &c = state.crop;
  const float W = state.docWidth(), H = state.docHeight();
  const float px = 1.0f / state.zoom; // one screen pixel, in image units
  const float sw = c.r - c.l, sh = c.b - c.t;

  ctx.setFillColor(Color{0, 0, 0, 120});
  ctx.fillRect(0, 0, W, c.t);          // above
  ctx.fillRect(0, c.b, W, H - c.b);    // below
  ctx.fillRect(0, c.t, c.l, sh);       // left
  ctx.fillRect(c.r, c.t, W - c.r, sh); // right

  ctx.setLineWidth(px);
  ctx.setStrokeColor(Color{255, 255, 255, 110});
  ctx.beginPath();
  for (int i = 1; i <= 2; ++i)
  {
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
    for (int j = 0; j < 3; ++j)
    {
      if (i == 1 && j == 1)
        continue; // center: no handle
      if ((i == 1 && !midX) || (j == 1 && !midY))
        continue; // edge midpoints crowd together on tiny selections
      ctx.fillRect(xs[i] - hs / 2.0f, ys[j] - hs / 2.0f, hs, hs);
      ctx.strokeRect(xs[i] - hs / 2.0f, ys[j] - hs / 2.0f, hs, hs);
    }
}

// BEGIN compare
// ---------------- Before / After ----------------
// "Before" is state.source: the picture with its crop and rotation but none
// of the slider, curve, color or mask edits. Keeping the geometry means the
// canvas doesn't change size, so the comparison is pixel for pixel.
static void toggleBefore()
{
  if (!state.image)
    return;
  state.compare = state.compare == CompareMode::Before ? CompareMode::Off
                                                       : CompareMode::Before;
  state.markDirty();
}
static void toggleSplit()
{
  if (!state.image)
    return;
  state.compare = state.compare == CompareMode::Split ? CompareMode::Off
                                                      : CompareMode::Split;
  state.markDirty();
}
// Dragging in the canvas moves the split line.
static void splitFromX(float localX)
{
  const float w = state.docWidth() * state.zoom;
  if (w <= 0.0f)
    return;
  state.splitPos = std::clamp(localX / w, 0.02f, 0.98f);
  state.markDirty();
}

// A small dark pill with `text`, its left (or, if alignRight, right) edge at x.
static void paintCompareLabel(CanvasContext &ctx, const std::string &text,
                              float x, float y, bool alignRight)
{
  ctx.setFont("", 12.0f, FontWeight::SemiBold);
  const float w = ctx.measureText(text).width + 16.0f, h = 22.0f;
  const float left = alignRight ? x - w : x;
  ctx.setFillColor(Color{0, 0, 0, 160});
  ctx.beginPath();
  ctx.roundRect(left, y, w, h, 4.0f);
  ctx.fill();
  ctx.setFillColor(kColorWhite);
  ctx.setTextAlign(TextAlign::Start);
  ctx.setTextBaseline(TextBaseline::Middle);
  ctx.fillText(text, left + 8.0f, y + h / 2.0f);
}

// Screen-space decoration over the canvas: labels and the split line.
static void paintCompareOverlay(CanvasContext &ctx)
{
  const float W = state.docWidth() * state.zoom;
  const float H = state.docHeight() * state.zoom;
  if (state.compare == CompareMode::Before)
  {
    paintCompareLabel(ctx, "Before", 10.0f, 10.0f, false);
    return;
  }
  const float x = std::floor(W * state.splitPos);
  ctx.setStrokeColor(kColorWhite);
  ctx.setLineWidth(2.0f);
  ctx.beginPath();
  ctx.moveTo(x, 0);
  ctx.lineTo(x, H);
  ctx.stroke();
  paintCompareLabel(ctx, "Before", 10.0f, 10.0f, false);
  paintCompareLabel(ctx, "After", W - 10.0f, 10.0f, true);
}
// END compare

// Red/blue clipping warning over the edited picture. The overlay image is
// in document space (stretched to docW x docH like the preview), so it
// lines up at any zoom. computeHistogram builds it.
static void computeHistogram();
static void paintClipOverlay(CanvasContext &ctx, float docW, float docH)
{
  if (!state.clipShadowsOn && !state.clipHighlightsOn)
    return;
  if (state.histStale)
    computeHistogram(); // also rebuilds the overlay
  if (state.clipOverlay)
    ctx.drawImage(*state.clipOverlay, 0, 0, docW, docH);
}

static void paintCanvas(CanvasContext &ctx)
{
  LITEUI_PROF_START(profAppPaint);
  ctx.setFillColor(state.image ? kColorWhite : kColorCanvasPlaceholderBg);
  ctx.fillRect(0, 0, ctx.width(), ctx.height());

  if (!state.image || state.image->width <= 0 || state.image->height <= 0)
    return;

  // Drawn at the document's natural pixel size; the canvas itself is
  // already sized to docWidth*zoom / docHeight*zoom (see canvas.style
  // below), so a plain ctx.scale(zoom, zoom) here is all that's needed.
  // A straightened image with "Keep full canvas" has transparent corners;
  // a neutral backdrop keeps them from reading as part of a white picture.
  if (state.adjust.expand() && state.adjust.angle() != 0.0f)
  {
    ctx.setFillColor(kColorEmptyCorner);
    ctx.fillRect(0, 0, ctx.width(), ctx.height());
  }

  ctx.save();
  ctx.scale(state.zoom, state.zoom);
  const float docW = state.docWidth(), docH = state.docHeight();
  // The picture as edited.
  const auto drawAfter = [&]()
  {
    if (state.showPreview())
    {
      // Mid-drag (or awaiting the background full-res render) preview: smaller
      // image stretched to the full document size
      // (bilinear), so layout/zoom are unaffected by its lower resolution.
      ctx.drawImage(*state.previewImage, 0, 0, docW, docH);
    }
    else
    {
      ctx.drawImage(*state.image, 0, 0);
    }
  };
  // The same picture with every edit off (crop and rotation kept).
  const auto drawBefore = [&]()
  {
    if (state.source)
      ctx.drawImage(*state.source, 0, 0, docW, docH);
    else
      drawAfter();
  };
  if (state.compare == CompareMode::Before)
  {
    drawBefore();
  }
  else if (state.compare == CompareMode::Split)
  {
    const float x = docW * state.splitPos;
    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, x, docH);
    ctx.clip();
    drawBefore();
    ctx.restore();
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, 0, docW - x, docH);
    ctx.clip();
    drawAfter();
    ctx.restore();
  }
  else
  {
    drawAfter();
    paintClipOverlay(ctx, docW, docH);
    if (cropToolActive())
      paintCropOverlay(ctx);
    else if (maskToolActive())
      paintMaskOverlay(ctx);
  }
  ctx.restore();
  if (state.compare != CompareMode::Off)
    paintCompareOverlay(ctx); // labels and the split line (screen space)
  LITEUI_PROF_ADD(appPaint, profAppPaint);
}

// ---------------- RGB readout under the cursor ----------------
// Both return true only when the readout changed, so LiteUI repaints only
// then (see View::onHoverAt).
static bool hoverClear()
{
  const bool was = state.hoverValid;
  state.hoverValid = false;
  return was;
}

// Samples the picture the canvas is showing: the original in Before (or the
// left half of Split), else the edited image / mid-drag preview.
static bool hoverSample(float lx, float ly)
{
  if (state.dragging)
    return false; // a drag is running: leave the readout alone
  const float docW = state.docWidth(), docH = state.docHeight();
  if (!state.image || docW <= 0.0f || docH <= 0.0f || state.zoom <= 0.0f)
    return hoverClear();
  const float dx = lx / state.zoom, dy = ly / state.zoom;
  if (dx < 0.0f || dy < 0.0f || dx >= docW || dy >= docH)
    return hoverClear();
  const bool before =
      state.compare == CompareMode::Before ||
      (state.compare == CompareMode::Split && dx < docW * state.splitPos);
  const CanvasImage *img =
      (before && state.source)
          ? state.source.get()
          : (state.showPreview() ? state.previewImage.get() : state.image.get());
  if (!img || img->width <= 0 || img->height <= 0)
    return hoverClear();
  const int ix = std::clamp(static_cast<int>(dx / docW * img->width), 0, img->width - 1);
  const int iy = std::clamp(static_cast<int>(dy / docH * img->height), 0, img->height - 1);
  const uint8_t *px =
      img->pixels.data() + (static_cast<size_t>(iy) * img->width + ix) * 4;
  if (px[3] == 0)
    return hoverClear(); // straighten's empty corner
  const bool changed = !state.hoverValid || state.hoverR != px[0] ||
                       state.hoverG != px[1] || state.hoverB != px[2];
  state.hoverValid = true;
  state.hoverR = px[0];
  state.hoverG = px[1];
  state.hoverB = px[2];
  return changed;
}

// Runs the deferred, expensive part of a slider drag (rebuilding the shown
// image) exactly once per pending change. Idempotent: the main canvas and the
// histogram canvas both call it from their dirty polls, and whichever is
// polled first (the sidebar, so usually the histogram) does the work — so the
// histogram never lags a frame behind the picture.
static void flushPendingAdjust()
{
  collectFullRender();  // a finished background render, if any
  collectGeometryJob(); // a finished background geometry rebuild, if any
  collectDecodeJob();   // a finished background file decode, if any
  collectSegmentJob();
  autosaveSidecar(); // a finished change, once the mouse is released
  if (!state.adjustDirty)
    return;
  state.adjustDirty = false;
  LITEUI_PROF_START(profRecompute);
  if (state.dragging && state.dragIsAngle && state.orientedPreview)
    updateAnglePreview(); // cheap: turns the small preview, mid-drag
  else if (state.dragging && !state.dragIsAngle && state.previewBase)
    updatePreviewFromAdjustments(); // cheap: small image, mid-drag
  else
    updateImageFromAdjustments(); // full-res: rebuilds state.image
  LITEUI_PROF_ADD(recompute, profRecompute);
}

// Brings the shown image (and, if stale, the histogram) up to date. Several
// canvases poll this; only the first poll of a frame does any work.
static void refreshHistogramIfStale();

// Rebuilds the R/G/B/luma histograms and the clipping overlay from the image
// currently on screen (the mid-drag preview, or the full-resolution result).
// The counting and the overlay are histogram.hpp; this only picks the image
// and stores the results where the histogram and canvas painters read them.
static void computeHistogram()
{
  const CanvasImage *img =
      state.showPreview() ? state.previewImage.get() : state.image.get();
  const histogram::Counts c = histogram::compute(img);
  state.histR = c.r;
  state.histG = c.g;
  state.histB = c.b;
  state.histL = c.l;
  state.histClipLo = c.clipLo;
  state.histClipHi = c.clipHi;
  state.clipOverlay = histogram::clipOverlay(
      img, {state.clipShadowsOn, state.clipHighlightsOn, state.clipPerChannel});
  state.histStale = false;
  ++state.histVersion;
}

// Width of the histogram canvas as last painted (for corner hit-testing).
static float gHistCanvasW = 0.0f;

// Luma as gray bars, R/G/B as thin colored outlines on top. Bar heights use
// sqrt scaling and are normalized to the tallest bin among 1..254, so a pile
// of clipped pixels in bin 0/255 doesn't flatten the rest of the graph.
static void paintHistogram(CanvasContext &ctx)
{
  const float W = ctx.width(), H = ctx.height();
  gHistCanvasW = W;
  ctx.setFillColor(Color{34, 34, 38, 255});
  ctx.fillRect(0, 0, W, H);

  uint32_t peak = 1;
  for (int i = 1; i < 255; ++i)
    peak = std::max(
        {peak, state.histL[i], state.histR[i], state.histG[i], state.histB[i]});
  const float invPeak = 1.0f / std::sqrt(static_cast<float>(peak));
  const float binW = W / 256.0f;
  auto heightOf = [&](uint32_t v)
  {
    return std::min(1.0f, std::sqrt(static_cast<float>(v)) * invPeak) *
           (H - 2.0f);
  };

  ctx.setFillColor(Color{170, 170, 175, 255});
  for (int i = 0; i < 256; ++i)
  {
    const float h = heightOf(state.histL[i]);
    if (h > 0.0f)
      ctx.fillRect(static_cast<float>(i) * binW, H - h, binW + 0.5f, h);
  }

  auto outline = [&](const std::array<uint32_t, 256> &bins, Color c)
  {
    ctx.setStrokeColor(c);
    ctx.setLineWidth(1.0f);
    ctx.beginPath();
    for (int i = 0; i < 256; ++i)
    {
      const float x = (static_cast<float>(i) + 0.5f) * binW;
      const float y = H - heightOf(bins[i]);
      if (i == 0)
        ctx.moveTo(x, y);
      else
        ctx.lineTo(x, y);
    }
    ctx.stroke();
  };
  outline(state.histR, Color{235, 70, 70, 200});
  outline(state.histG, Color{70, 200, 90, 200});
  outline(state.histB, Color{80, 130, 255, 200});

  // Clipping triangles. Dark: nothing clipped. Otherwise colored by which
  // channels clip (white = all three). White outline: that overlay is on.
  auto triangle = [&](bool left, uint8_t mask, bool overlayOn)
  {
    const float s = kHistTriPx, m = 3.0f;
    const Color c = mask ? histogram::clipChannelColor(mask, false) : Color{90, 90, 95, 255};
    const float x0 = left ? m : W - m;
    const float dx = left ? s : -s;
    auto path = [&]
    {
      ctx.beginPath();
      ctx.moveTo(x0, m);
      ctx.lineTo(x0 + dx, m);
      ctx.lineTo(x0, m + s);
      ctx.closePath();
    };
    path();
    ctx.setFillColor(c);
    ctx.fill();
    if (overlayOn)
    {
      path();
      ctx.setStrokeColor(kColorWhite);
      ctx.setLineWidth(1.0f);
      ctx.stroke();
    }
  };
  triangle(true, state.histClipLo, state.clipShadowsOn);
  triangle(false, state.histClipHi, state.clipHighlightsOn);
}

static void refreshHistogramIfStale()
{
  flushPendingAdjust();
  if (state.histStale)
    computeHistogram();
}

// Turns the shadow / highlight overlays on or off. Recomputing right away
// rebuilds the overlay, bumps histVersion (so the histogram repaints its
// triangles) and the canvas is told to repaint.
static void setClipping(bool shadows, bool highlights)
{
  state.clipShadowsOn = shadows;
  state.clipHighlightsOn = highlights;
  computeHistogram();
  state.markDirty();
}
// J: both on, or both off if either is currently on.
static void toggleClipping()
{
  if (!state.image)
    return;
  const bool on = !(state.clipShadowsOn || state.clipHighlightsOn);
  setClipping(on, on);
}

// Shift+J: switch between classic red/blue and per-channel colors. If no
// overlay is on yet, turn both on so the key always shows something.
static void toggleChannelClipping()
{
  if (!state.image)
    return;
  state.clipPerChannel = !state.clipPerChannel;
  if (!state.clipShadowsOn && !state.clipHighlightsOn)
    state.clipShadowsOn = state.clipHighlightsOn = true;
  computeHistogram(); // rebuild the overlay with the new coloring
  state.markDirty();
}

// ---------------- Tone curve editor (canvas side) ----------------
// Mapping between canvas-local screen px and curve units (0..255, y up).
static float curvePlotPx() { return kCurveSizePx - 2.0f * kCurvePadPx; }

static CurvePoint curveFromLocal(float lx, float ly)
{
  const float plot = curvePlotPx();
  return {
      std::clamp((lx - kCurvePadPx) / plot * 255.0f, 0.0f, 255.0f),
      std::clamp(255.0f - (ly - kCurvePadPx) / plot * 255.0f, 0.0f, 255.0f)};
}

static void curveToLocal(float x, float y, float &lx, float &ly)
{
  const float plot = curvePlotPx();
  lx = kCurvePadPx + x / 255.0f * plot;
  ly = kCurvePadPx + (255.0f - y) / 255.0f * plot;
}

// Index of the point within kCurveHitPx of (lx, ly), nearest wins; -1 if none.
static int curveHit(const CurvePoints &pts, float lx, float ly)
{
  int hit = -1;
  float best = kCurveHitPx * kCurveHitPx;
  for (size_t i = 0; i < pts.size(); ++i)
  {
    float px, py;
    curveToLocal(pts[i].x, pts[i].y, px, py);
    const float d2 = (px - lx) * (px - lx) + (py - ly) * (py - ly);
    if (d2 <= best)
    {
      best = d2;
      hit = static_cast<int>(i);
    }
  }
  return hit;
}

static int gCurveActive = -1;                             // point being dragged, or -1
static CurveChannel gCurveChannel = CurveChannel::Master; // curve being edited
// Set by UI-only changes (channel switch, selecting a point) that don't
// change the picture, so the curve canvas still repaints; consumed by its
// dirty poll.
static bool gCurveUiDirty = false;

static const CurvePoints &curveActiveCurve()
{
  return state.adjust.curve(gCurveChannel);
}
// (point - mouse) in curve units at press, so a grabbed point doesn't jump
// to the cursor's exact position.
static float gCurveGrabDx = 0.0f, gCurveGrabDy = 0.0f;

static void curvePress(float lx, float ly)
{
  if (!state.original)
    return;
  CurvePoints pts = curveActiveCurve();
  int hit = curveHit(pts, lx, ly);
  gCurveGrabDx = gCurveGrabDy = 0.0f;
  if (hit >= 0)
  {
    const CurvePoint m = curveFromLocal(lx, ly);
    gCurveGrabDx = pts[hit].x - m.x;
    gCurveGrabDy = pts[hit].y - m.y;
  }
  else
  {
    if (pts.size() >= kCurveMaxPoints)
      return;
    const CurvePoint m = curveFromLocal(lx, ly);
    hit = curveInsert(pts, m.x, m.y);
    if (hit < 0)
      return;
    // coalesce = true: the drag that follows folds into this same undo step.
    state.adjust.setCurve(gCurveChannel, pts, true);
    // One full-resolution recompute for the click itself; the preview path
    // only starts once the point actually moves (see curveDrag), so a plain
    // click can never leave the view stuck on the low-res preview.
    state.markAdjustDirty();
  }
  gCurveActive = hit;
  gCurveUiDirty = true; // show the selected point
}

static void curveDrag(float lx, float ly)
{
  if (gCurveActive < 0 || !state.original)
    return;
  CurvePoints pts = curveActiveCurve();
  if (gCurveActive >= static_cast<int>(pts.size()))
    return;
  const CurvePoint m = curveFromLocal(lx, ly);
  curveMovePoint(pts, gCurveActive, m.x + gCurveGrabDx, m.y + gCurveGrabDy);
  if (pts == curveActiveCurve())
    return;
  state.adjust.setCurve(gCurveChannel, pts, true);
  enterDrag(false); // switches recompute+paint to the preview path
  state.markAdjustDirty();
}

static void curveEnd()
{
  gCurveActive = -1;
  gCurveUiDirty = true;
  state.adjust.endCurveEdit();
  if (state.dragging)
  { // leave preview mode: one full-resolution recompute
    state.dragging = false;
    state.dragIsAngle = false;
    state.markAdjustDirty();
  }
}

static void curveRightPress(float lx, float ly)
{
  if (!state.original)
    return;
  CurvePoints pts = curveActiveCurve();
  if (curveRemove(pts, curveHit(pts, lx, ly)))
  {
    state.adjust.setCurve(gCurveChannel, pts, false);
    state.markAdjustDirty();
  }
}

static void setCurveChannel(CurveChannel ch)
{
  gCurveActive = -1;
  gCurveChannel = ch;
  gCurveUiDirty = true;
}

// Resets the curve being edited (one undo step).
static void resetCurve()
{
  gCurveActive = -1;
  state.adjust.setCurve(gCurveChannel, identityCurve(), false);
  gCurveUiDirty = true;
  state.markAdjustDirty();
}

// Resets all four curves (one undo step).
static void resetAllCurves()
{
  gCurveActive = -1;
  state.adjust.resetAllCurves();
  gCurveUiDirty = true;
  state.markAdjustDirty();
}

static const char *curveChannelName(CurveChannel ch)
{
  switch (ch)
  {
  case CurveChannel::Red:
    return "Red";
  case CurveChannel::Green:
    return "Green";
  case CurveChannel::Blue:
    return "Blue";
  default:
    return "RGB";
  }
}

// Line/tint color for a curve: white for the master, channel color otherwise.
static Color curveChannelColor(CurveChannel ch, uint8_t alpha)
{
  switch (ch)
  {
  case CurveChannel::Red:
    return Color{235, 70, 70, alpha};
  case CurveChannel::Green:
    return Color{70, 200, 90, alpha};
  case CurveChannel::Blue:
    return Color{80, 130, 255, alpha};
  default:
    return Color{255, 255, 255, alpha};
  }
}

// The histogram that matches a curve: luma for the master, else that channel.
static const std::array<uint32_t, 256> &curveHistogram(CurveChannel ch)
{
  switch (ch)
  {
  case CurveChannel::Red:
    return state.histR;
  case CurveChannel::Green:
    return state.histG;
  case CurveChannel::Blue:
    return state.histB;
  default:
    return state.histL;
  }
}

// Dark plot with the edited channel's histogram faintly behind it, a quarter
// grid, the identity diagonal, every non-straight curve (the others dimmed in
// their own color), and the edited curve's control points.
static void paintCurve(CanvasContext &ctx)
{
  const float W = ctx.width(), H = ctx.height();
  const float pad = kCurvePadPx, plot = curvePlotPx();
  ctx.setFillColor(Color{34, 34, 38, 255});
  ctx.fillRect(0, 0, W, H);

  const auto &hist = curveHistogram(gCurveChannel);
  uint32_t peak = 1;
  for (int i = 1; i < 255; ++i)
    peak = std::max(peak, hist[i]);
  const float invPeak = 1.0f / std::sqrt(static_cast<float>(peak));
  ctx.setFillColor(curveChannelColor(gCurveChannel, 45));
  for (int i = 0; i < 256; ++i)
  {
    const float hh =
        std::min(1.0f, std::sqrt(static_cast<float>(hist[i])) * invPeak) * plot;
    if (hh > 0.0f)
      ctx.fillRect(pad + static_cast<float>(i) / 256.0f * plot, pad + plot - hh,
                   plot / 256.0f + 0.5f, hh);
  }

  ctx.setLineWidth(1.0f);
  ctx.setStrokeColor(Color{255, 255, 255, 30});
  ctx.beginPath();
  for (int q = 0; q <= 4; ++q)
  {
    const float t = pad + plot * static_cast<float>(q) / 4.0f;
    ctx.moveTo(t, pad);
    ctx.lineTo(t, pad + plot);
    ctx.moveTo(pad, t);
    ctx.lineTo(pad + plot, t);
  }
  ctx.stroke();

  ctx.setStrokeColor(Color{255, 255, 255, 70});
  ctx.beginPath();
  ctx.moveTo(pad, pad + plot);
  ctx.lineTo(pad + plot, pad);
  ctx.stroke();

  // Draws one curve as a polyline. Non-edited, non-straight curves first (dim),
  // so the edited curve ends up on top.
  auto strokeCurve = [&](CurveChannel ch, uint8_t alpha, float width)
  {
    const auto lut = curveToLut(state.adjust.curve(ch));
    ctx.setStrokeColor(curveChannelColor(ch, alpha));
    ctx.setLineWidth(width);
    ctx.beginPath();
    for (int i = 0; i < 256; ++i)
    {
      float lx, ly;
      curveToLocal(static_cast<float>(i), lut[i], lx, ly);
      if (i == 0)
        ctx.moveTo(lx, ly);
      else
        ctx.lineTo(lx, ly);
    }
    ctx.stroke();
  };
  const CurvePoints straight = identityCurve();
  for (int c = 0; c < static_cast<int>(kCurveChannelCount); ++c)
  {
    const auto ch = static_cast<CurveChannel>(c);
    if (ch != gCurveChannel && !(state.adjust.curve(ch) == straight))
      strokeCurve(ch, 110, 1.0f);
  }
  strokeCurve(gCurveChannel, 255, 1.75f);

  const CurvePoints &pts = curveActiveCurve();
  ctx.setLineWidth(1.0f);
  for (size_t i = 0; i < pts.size(); ++i)
  {
    float lx, ly;
    curveToLocal(pts[i].x, pts[i].y, lx, ly);
    const bool active = static_cast<int>(i) == gCurveActive;
    ctx.setFillColor(active ? kColorDividerHover : kColorWhite);
    ctx.setStrokeColor(Color{20, 20, 20, 255});
    ctx.fillRect(lx - kCurvePointPx / 2.0f, ly - kCurvePointPx / 2.0f,
                 kCurvePointPx, kCurvePointPx);
    ctx.strokeRect(lx - kCurvePointPx / 2.0f, ly - kCurvePointPx / 2.0f,
                   kCurvePointPx, kCurvePointPx);
  }
}

// ---------------- Mask effects: Amount, Color Mix, Grading, Curve ----------
// The Effect tab of the right-hand mask panel. Everything here edits the
// SELECTED mask through maskEdit, so it is undoable and a drag folds into one
// step. The ids below are the "control" part of that undo key: they must stay
// below 100 and differ from the kMaskCtl* ids above.
constexpr int kMaskCtlAmount = 6;
constexpr int kMaskCtlMix = 30;   // + chan * kMixBandCount + band   (30..53)
constexpr int kMaskCtlGrade = 60; // + field * 4 + range             (60..71)
constexpr int kMaskCtlBlend = 72;
constexpr int kMaskCtlBalance = 73;
constexpr int kMaskCtlCurve = 80; // + channel                       (80..83)
constexpr float kMaskCurveSizePx = 240.0f;

static MixChan gMaskMixChan = MixChan::Hue;              // Mix: Hue / Sat / Lum
static int gMaskGradeRange = 1;                          // Grade: 0..3 (Midtones)
static CurveChannel gMaskCurveCh = CurveChannel::Master; // Curve: channel
static int gMaskCurveActive = -1;                        // point being dragged
static bool gMaskCurveUiDirty = false;                   // repaint-only change
static float gMaskCurveGrabDx = 0.0f, gMaskCurveGrabDy = 0.0f;

static std::string maskIntText(float v)
{
  return std::to_string(static_cast<int>(std::lround(v)));
}
static std::string maskSignedText(float v)
{
  const int i = static_cast<int>(std::lround(v));
  return i > 0 ? "+" + std::to_string(i) : std::to_string(i);
}

// Amount: how strongly all of this mask's adjustments apply (Lightroom's
// mask "Amount"). 100 = full, 0 = the mask does nothing.
static View buildMaskAmountSlider()
{
  SliderSpec s;
  s.name = "Amount";
  s.lo = 0.0f;
  s.hi = 100.0f;
  s.trackW = kMaskTrackW;
  s.get = []
  {
    const Mask *m = selectedMask();
    return m ? m->amount : 100.0f;
  };
  s.set = [](float v)
  {
    maskEdit(selMask(), [&](Mask &m)
             { m.amount = std::round(v); }, kMaskCtlAmount);
  };
  s.format = maskIntText;
  s.onEnd = []
  { state.adjust.endMaskEdit(); };
  return buildSliderRow(s);
}

// One Color Mixer slider (a band's Hue / Saturation / Luminance) of the mask.
static View buildMaskMixSlider(MixChan chan, size_t band)
{
  SliderSpec s;
  s.name = mixBandName(band);
  s.lo = static_cast<float>(kMixMin);
  s.hi = static_cast<float>(kMixMax);
  s.trackW = kMaskTrackW;
  s.get = [chan, band]
  {
    const Mask *m = selectedMask();
    return m ? m->mix[mixIndex(chan, band)] : 0.0f;
  };
  s.set = [chan, band](float v)
  {
    maskEdit(selMask(), [&](Mask &m)
             { m.mix[mixIndex(chan, band)] = std::round(v); }, kMaskCtlMix + static_cast<int>(chan) * static_cast<int>(kMixBandCount) + static_cast<int>(band));
  };
  s.format = maskSignedText;
  s.hasAccent = true;
  s.accent = mixBandColor(band);
  s.onEnd = []
  { state.adjust.endMaskEdit(); };
  return buildSliderRow(s);
}

// One hue / saturation / luminance slider of one grading range of the mask.
// fieldIdx (0 hue, 1 sat, 2 lum) only keeps the undo keys apart.
static View buildMaskGradeSlider(const char *name, int range,
                                 float GradeWheel::*field, int fieldIdx,
                                 float lo, float hi, bool isSigned)
{
  SliderSpec s;
  s.name = name;
  s.lo = lo;
  s.hi = hi;
  s.trackW = kMaskTrackW;
  s.get = [range, field]
  {
    const Mask *m = selectedMask();
    return m ? m->grade.wheel[static_cast<size_t>(range)].*field : 0.0f;
  };
  s.set = [range, field, fieldIdx](float v)
  {
    maskEdit(selMask(), [&](Mask &m)
             { m.grade.wheel[static_cast<size_t>(range)].*field = std::round(v); }, kMaskCtlGrade + fieldIdx * 4 + range);
  };
  s.format = [isSigned](float v)
  { return isSigned ? maskSignedText(v) : maskIntText(v); };
  s.onEnd = []
  { state.adjust.endMaskEdit(); };
  return buildSliderRow(s);
}

// Blending (0..100) or Balance (-100..100), shared by the mask's ranges.
static View buildMaskGradeShared(bool balance)
{
  SliderSpec s;
  s.name = balance ? "Balance" : "Blending";
  s.lo = balance ? -100.0f : 0.0f;
  s.hi = 100.0f;
  s.trackW = kMaskTrackW;
  s.get = [balance]
  {
    const Mask *m = selectedMask();
    if (!m)
      return 0.0f;
    return balance ? m->grade.balance : m->grade.blending;
  };
  s.set = [balance](float v)
  {
    maskEdit(selMask(), [&](Mask &m)
             {
      if (balance)
        m.grade.balance = std::round(v);
      else
        m.grade.blending = std::round(v); }, balance ? kMaskCtlBalance : kMaskCtlBlend);
  };
  s.format = [balance](float v)
  { return balance ? maskSignedText(v) : maskIntText(v); };
  s.onEnd = []
  { state.adjust.endMaskEdit(); };
  return buildSliderRow(s);
}

// ---- Mask tone curve editor (canvas side) ----
// Same behavior as the Curve pane (click = add point, drag = move,
// right-click = delete) but on the selected mask's own curves, and a bit
// larger (kMaskCurveSizePx).
static float mcPlotPx() { return kMaskCurveSizePx - 2.0f * kCurvePadPx; }
static CurvePoint mcFromLocal(float lx, float ly)
{
  const float plot = mcPlotPx();
  return {
      std::clamp((lx - kCurvePadPx) / plot * 255.0f, 0.0f, 255.0f),
      std::clamp(255.0f - (ly - kCurvePadPx) / plot * 255.0f, 0.0f, 255.0f)};
}
static void mcToLocal(float x, float y, float &lx, float &ly)
{
  const float plot = mcPlotPx();
  lx = kCurvePadPx + x / 255.0f * plot;
  ly = kCurvePadPx + (255.0f - y) / 255.0f * plot;
}
static int mcHit(const CurvePoints &pts, float lx, float ly)
{
  int hit = -1;
  float best = kCurveHitPx * kCurveHitPx;
  for (size_t i = 0; i < pts.size(); ++i)
  {
    float px, py;
    mcToLocal(pts[i].x, pts[i].y, px, py);
    const float d2 = (px - lx) * (px - lx) + (py - ly) * (py - ly);
    if (d2 <= best)
    {
      best = d2;
      hit = static_cast<int>(i);
    }
  }
  return hit;
}

// The curve being edited, copied (empty list if no mask is selected).
static CurvePoints maskCurveCopy()
{
  const Mask *m = selectedMask();
  return m ? m->curves[static_cast<size_t>(gMaskCurveCh)] : CurvePoints{};
}
// coalesce = true folds a drag into one undo step; false = its own step.
static void maskCurveCommit(const CurvePoints &pts, bool coalesce)
{
  const CurveChannel ch = gMaskCurveCh;
  maskEdit(selMask(), [&](Mask &m)
           { m.curves[static_cast<size_t>(ch)] = pts; }, coalesce ? kMaskCtlCurve + static_cast<int>(ch) : -1);
}

static void maskCurvePress(float lx, float ly)
{
  if (!state.original || !selectedMask())
    return;
  CurvePoints pts = maskCurveCopy();
  int hit = mcHit(pts, lx, ly);
  gMaskCurveGrabDx = gMaskCurveGrabDy = 0.0f;
  if (hit >= 0)
  {
    const CurvePoint m = mcFromLocal(lx, ly);
    gMaskCurveGrabDx = pts[static_cast<size_t>(hit)].x - m.x;
    gMaskCurveGrabDy = pts[static_cast<size_t>(hit)].y - m.y;
  }
  else
  {
    if (pts.size() >= kCurveMaxPoints)
      return;
    const CurvePoint m = mcFromLocal(lx, ly);
    hit = curveInsert(pts, m.x, m.y);
    if (hit < 0)
      return;
    maskCurveCommit(pts, true); // the drag that follows folds into this step
    state.markAdjustDirty();
  }
  gMaskCurveActive = hit;
  gMaskCurveUiDirty = true;
}

static void maskCurveDrag(float lx, float ly)
{
  if (gMaskCurveActive < 0 || !state.original || !selectedMask())
    return;
  CurvePoints pts = maskCurveCopy();
  if (gMaskCurveActive >= static_cast<int>(pts.size()))
    return;
  const CurvePoint m = mcFromLocal(lx, ly);
  const CurvePoints before = pts;
  curveMovePoint(pts, gMaskCurveActive, m.x + gMaskCurveGrabDx,
                 m.y + gMaskCurveGrabDy);
  if (pts == before)
    return;
  maskCurveCommit(pts, true);
  enterDrag(false); // switches recompute+paint to the preview path
  state.markAdjustDirty();
}

static void maskCurveEnd()
{
  gMaskCurveActive = -1;
  gMaskCurveUiDirty = true;
  state.adjust.endMaskEdit();
  if (state.dragging)
  { // leave preview mode: one full-resolution recompute
    state.dragging = false;
    state.dragIsAngle = false;
    state.markAdjustDirty();
  }
}

static void maskCurveRightPress(float lx, float ly)
{
  if (!state.original || !selectedMask())
    return;
  CurvePoints pts = maskCurveCopy();
  if (curveRemove(pts, mcHit(pts, lx, ly)))
  {
    maskCurveCommit(pts, false);
    state.markAdjustDirty();
  }
}

static void paintMaskCurve(CanvasContext &ctx)
{
  const float W = ctx.width(), H = ctx.height();
  const float pad = kCurvePadPx, plot = mcPlotPx();
  ctx.setFillColor(Color{34, 34, 38, 255});
  ctx.fillRect(0, 0, W, H);

  ctx.setLineWidth(1.0f);
  ctx.setStrokeColor(Color{255, 255, 255, 30});
  ctx.beginPath();
  for (int q = 0; q <= 4; ++q)
  {
    const float t = pad + plot * static_cast<float>(q) / 4.0f;
    ctx.moveTo(t, pad);
    ctx.lineTo(t, pad + plot);
    ctx.moveTo(pad, t);
    ctx.lineTo(pad + plot, t);
  }
  ctx.stroke();
  ctx.setStrokeColor(Color{255, 255, 255, 70});
  ctx.beginPath();
  ctx.moveTo(pad, pad + plot);
  ctx.lineTo(pad + plot, pad);
  ctx.stroke();

  const Mask *m = selectedMask();
  if (!m)
    return;
  auto strokeCurve = [&](CurveChannel ch, uint8_t alpha, float width)
  {
    const auto lut = curveToLut(m->curves[static_cast<size_t>(ch)]);
    ctx.setStrokeColor(curveChannelColor(ch, alpha));
    ctx.setLineWidth(width);
    ctx.beginPath();
    for (int i = 0; i < 256; ++i)
    {
      float lx, ly;
      mcToLocal(static_cast<float>(i), lut[static_cast<size_t>(i)], lx, ly);
      if (i == 0)
        ctx.moveTo(lx, ly);
      else
        ctx.lineTo(lx, ly);
    }
    ctx.stroke();
  };
  const CurvePoints straight = identityCurve();
  for (int c = 0; c < static_cast<int>(kCurveChannelCount); ++c)
  {
    const auto ch = static_cast<CurveChannel>(c);
    if (ch != gMaskCurveCh && !(m->curves[static_cast<size_t>(c)] == straight))
      strokeCurve(ch, 110, 1.0f);
  }
  strokeCurve(gMaskCurveCh, 255, 1.75f);

  const CurvePoints &pts = m->curves[static_cast<size_t>(gMaskCurveCh)];
  ctx.setLineWidth(1.0f);
  for (size_t i = 0; i < pts.size(); ++i)
  {
    float lx, ly;
    mcToLocal(pts[i].x, pts[i].y, lx, ly);
    const bool active = static_cast<int>(i) == gMaskCurveActive;
    ctx.setFillColor(active ? kColorDividerHover : kColorWhite);
    ctx.setStrokeColor(Color{20, 20, 20, 255});
    ctx.fillRect(lx - kCurvePointPx / 2.0f, ly - kCurvePointPx / 2.0f,
                 kCurvePointPx, kCurvePointPx);
    ctx.strokeRect(lx - kCurvePointPx / 2.0f, ly - kCurvePointPx / 2.0f,
                   kCurvePointPx, kCurvePointPx);
  }
}

static View buildMaskCurveCanvas()
{
  View c;
  c.isCanvas = true;
  c.style.width = Size::pixel(kMaskCurveSizePx);
  c.style.height = Size::pixel(kMaskCurveSizePx);
  c.style.flexShrink = 0;
  c.onPaint = [](CanvasContext &ctx)
  { paintMaskCurve(ctx); };
  c.canvasDirtySource = []
  {
    // Repaint on any edit (the version moves with every change, undo and
    // redo too), a different mask / tab, or a UI-only change (channel,
    // selected point).
    static uint64_t lastVer = ~0ull;
    static int lastSel = -2, lastTabs = -2;
    const uint64_t ver = state.adjust.version();
    const int sel = selMask();
    const int tabs = state.maskTab * 10 + state.maskFx;
    const bool ui = gMaskCurveUiDirty;
    gMaskCurveUiDirty = false;
    if (ver == lastVer && sel == lastSel && tabs == lastTabs && !ui)
      return false;
    lastVer = ver;
    lastSel = sel;
    lastTabs = tabs;
    return true;
  };
  c.onPressAt = [](float lx, float ly)
  { maskCurvePress(lx, ly); };
  c.onDragTo = [](float lx, float ly)
  { maskCurveDrag(lx, ly); };
  c.onDragEnd = []
  { maskCurveEnd(); };
  c.onRightPressAt = [](float lx, float ly)
  { maskCurveRightPress(lx, ly); };
  return c;
}

// The pipeline indexes kAdjustmentSpecs by AdjId (AdjustmentSet::buildLut, the
// sidebar loop, the color pass), so entry i must describe AdjId i. This can't
// be a static_assert: the table holds std::function and std::string members,
// which aren't usable in constant expressions. So it's checked once at
// startup instead, and a mistake aborts with a message naming the bad entry
// rather than silently moving the wrong slider.
static void verifyAdjustmentSpecs()
{
  for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i)
  {
    const AdjustmentSpec &s = kAdjustmentSpecs[i];
    const char *problem = nullptr;
    if (static_cast<size_t>(s.id) != i)
      problem = "is out of order: its id doesn't match its position in AdjId "
                "(or an entry is missing)";
    else if (!(s.min < s.max))
      problem = "has min >= max";
    else if (s.stage == AdjustmentSpec::Stage::Tone && !s.apply)
      problem = "is a Tone adjustment with no apply function";
    if (problem)
    {
      std::fprintf(stderr, "literoom: kAdjustmentSpecs[%zu] (\"%s\") %s\n",
                   i, std::string(s.label).c_str(), problem);
      std::abort();
    }
  }
}

int main()
try
{
  verifyAdjustmentSpecs();
  LiteUI ui("Literoom", -1, -1, /*hideTitlebar=*/true);
  // A finished background render wakes the (otherwise sleeping) event loop.
  renderWorker.setNotify([&ui]
                         { ui.wake(); });
  geometryWorker.setNotify([&ui]
                           { ui.wake(); });
  thumbWorker.setNotify([&ui]
                        { ui.wake(); });
  decodeWorker.setNotify([&ui]
                         { ui.wake(); });
  segmentWorker.setNotify([&ui]
                          { ui.wake(); });

  // ---- Keyboard shortcuts ----
  // LiteUI's addShortcut is global: it fires whatever has focus, so these work
  // after clicking a sidebar button too. Modifiers must match exactly.
  // Zoom uses the unshifted Ctrl+= / Ctrl+-: on Linux LiteUI reports shifted
  // symbols like '+' as Key::Unknown, so Ctrl+Shift+= can't be bound there.
  {
    auto withMods = [](bool ctrl, bool shift)
    {
      KeyModifiers m;
      m.ctrl = ctrl;
      m.shift = shift;
      return m;
    };
    const KeyModifiers none = withMods(false, false);
    const KeyModifiers shiftOnly = withMods(false, true);
    const KeyModifiers ctrl = withMods(true, false);
    const KeyModifiers ctrlShift = withMods(true, true);
    // Edits and dialogs are ignored mid-drag (a slider or crop handle is
    // being held): changing the image under the drag would fight it.
    auto idle = [](std::function<void()> fn)
    {
      return [fn = std::move(fn)]
      {
        if (!state.dragging)
          fn();
      };
    };
    // Plain-key shortcuts must not fire while the mask name field is being
    // typed in (see typingInTextField).
    auto notTyping = [idle](std::function<void()> fn)
    {
      return idle([fn = std::move(fn)]
                  {
        if (!typingInTextField())
          fn(); });
    };
    auto zoomIn = []
    { setZoom(state.zoom * kZoomStep); };
    auto zoomOut = []
    { setZoom(state.zoom / kZoomStep); };

    ui.addShortcut(ctrl, Key::Z, idle(undo));
    ui.addShortcut(ctrl, Key::Y, idle(redo));
    ui.addShortcut(ctrlShift, Key::Z, idle(redo));
    ui.addShortcut(ctrl, Key::O, idle(openImage));
    ui.addShortcut(ctrl, Key::E, idle(exportImage));
    ui.addShortcut(ctrl, Key::Q, [&ui]
                   { ui.requestClose(); });
    ui.addShortcut(ctrl, Key::B, toggleSidebar);
    ui.addShortcut(ctrl, Key::Equal, zoomIn);
    ui.addShortcut(ctrlShift, Key::Equal,
                   zoomIn); // '+' on layouts that report it
    ui.addShortcut(ctrl, Key::Minus, zoomOut);
    ui.addShortcut(ctrl, Key::N0, []
                   { setZoom(1.0f); });
    ui.addShortcut(ctrl, Key::LeftBracket,
                   idle([]
                        { applyGeoOp(GeoOp::RotateCCW); }));
    ui.addShortcut(ctrl, Key::RightBracket,
                   idle([]
                        { applyGeoOp(GeoOp::RotateCW); }));
    ui.addShortcut(ctrlShift, Key::O, idle(openFolder));
    ui.addShortcut(none, Key::Left, notTyping([]
                                              { filmStep(-1); }));
    ui.addShortcut(none, Key::Right, notTyping([]
                                               { filmStep(1); }));
    ui.addShortcut(none, Key::F6, toggleFilmstrip);
    ui.addShortcut(none, Key::Backslash, notTyping(toggleBefore));       // Before / After
    ui.addShortcut(none, Key::Y, notTyping(toggleSplit));                // split view
    ui.addShortcut(none, Key::J, notTyping(toggleClipping));             // clipping warnings
    ui.addShortcut(shiftOnly, Key::J, notTyping(toggleChannelClipping)); // per-channel colors
    ui.addShortcut(none, Key::Escape, []
                   { state.openMenu = -1; });
    ui.addShortcut(none, Key::Delete, notTyping([]
                                                {
                     if (maskToolActive())
                       deleteSelectedMask(); }));
    ui.addShortcut(none, Key::Enter, idle([]
                                          {
                                            if (cropToolActive())
                                              applyCrop(); // commit the selection, like the button
                                          }));
  }

  const std::vector<Activity> activities = {
      {0, "Info", "I"},
      {kHistoryActivity, "History", "H"},
      {1, "Adjust", "A"},
      {4, "Curve", "~"},
      {5, "Color Mix", "M"},
      {6, "Color Grading", "G"},
      {7, "Masks", "\u25D0"},
      {2, "Crop", "C"},
      {3, "Rotate", "R"},
      {8, "Export", "E"},
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
  titlebar.style.backgroundColor = kColorTitlebarBg;
  titlebar.style.borderWidth = 0.0f;
  // Empty space in the titlebar (i.e. not over a button) drags the
  // window, and also closes any open dropdown menu.
  titlebar.onPressAt = [&](float, float)
  {
    state.openMenu = -1;
    ui.requestMove();
  };

  Text appTitle;
  appTitle.label = std::string("Lite Room");
  appTitle.fontSize = 13.0f;
  appTitle.fontWeight = FontWeight::SemiBold;
  appTitle.color = kColorTextDark;
  appTitle.style.margin = EdgeInsets{0, 16, 0, 14};
  titlebar.addChild(std::move(appTitle));

  titlebar.addChild(
      menuButton("File", 0,
                 {
                     {"Open Image...", openImage, "Ctrl+O"},
                     {"Open Folder...", openFolder, "Ctrl+Shift+O"},
                     {"Export...", exportImage, "Ctrl+E"},
                     {"Exit", [&]
                      { ui.requestClose(); }, "Ctrl+Q"},
                 }));
  titlebar.addChild(menuButton(
      "View", 1,
      {
          {"Zoom In", []
           { setZoom(state.zoom * kZoomStep); }, "Ctrl+="},
          {"Zoom Out", []
           { setZoom(state.zoom / kZoomStep); }, "Ctrl+-"},
          {"Actual Size (100%)", []
           { setZoom(1.0f); }, "Ctrl+0"},
          {"Toggle Sidebar", toggleSidebar, "Ctrl+B"},
          {"Toggle Filmstrip", toggleFilmstrip, "F6"},
          {"Previous Image", []
           { filmStep(-1); }, "Left"},
          {"Next Image", []
           { filmStep(1); }, "Right"},
          {"Before / After", toggleBefore, "\\"},
          {"Before | After Split", toggleSplit, "Y"},
          {"Clipping Warnings", toggleClipping, "J"},
          {"Clipping: Per-Channel Colors", toggleChannelClipping, "Shift+J"},
      }));

  titlebar.addChild(
      menuButton("Image", 2,
                 {
                     {"Rotate Left 90\u00B0",
                      []
                      { applyGeoOp(GeoOp::RotateCCW); }, "Ctrl+["},
                     {"Rotate Right 90\u00B0",
                      []
                      { applyGeoOp(GeoOp::RotateCW); }, "Ctrl+]"},
                     {"Flip Horizontal", []
                      { applyGeoOp(GeoOp::FlipH); }},
                     {"Flip Vertical", []
                      { applyGeoOp(GeoOp::FlipV); }},
                 }));

  View undoRedoGap;
  undoRedoGap.style.width = Size::pixel(8);
  titlebar.addChild(std::move(undoRedoGap));

  titlebar.addChild(
      titlebarIconBtn("\u21B6", undo, []
                      { return state.adjust.canUndo(); }));
  titlebar.addChild(
      titlebarIconBtn("\u21B7", redo, []
                      { return state.adjust.canRedo(); }));

  View titlebarSpacer;
  titlebarSpacer.style.flexGrow = 1;
  titlebarSpacer.style.height = Size::full();
  titlebar.addChild(std::move(titlebarSpacer));

  titlebar.addChild(
      winButton("\u2500", kColorMenuHover, [&]
                { ui.requestMinimize(); }));
  titlebar.addChild(winButton("\u25A1", kColorMenuHover, [&]
                              {
    ui.requestMaximize();
    state.maximized = !state.maximized; }));
  titlebar.addChild(
      winButton("\u00D7", kColorCloseHover, [&]
                { ui.requestClose(); }));

  root.addChild(std::move(titlebar));

  // Workspace: activity bar, sidebar, divider, canvas viewport.

  View workspace;
  workspace.style.direction = FlexDirection::Row;
  workspace.style.width = Size::full();
  workspace.style.flexGrow = 1;
  // Clicking anywhere in the workspace that isn't itself a button
  // (e.g. the canvas, or blank sidebar space) closes an open menu too.
  workspace.onPressAt = [](float, float)
  { state.openMenu = -1; };

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

  for (const auto &item : activities)
  {
    View button;
    button.style.width = Size::pixel(32);
    button.style.height = Size::pixel(32);
    button.style.borderRadius = 6.0f;
    button.style.justifyContent = Justify::Center;
    button.style.alignItems = Align::Center;
    button.style.hoverColor = kColorActivityHover;

    int id = item.id;
    button.style.backgroundColor = std::function<Color()>([id]
                                                          { return state.activeActivity == id ? kColorActivityActive
                                                                                              : kColorActivityBarBg; });
    button.onClick = [id]
    {
      state.activeActivity = (state.activeActivity == id) ? -1 : id;
      // The wheels are laid out for a minimum sidebar width.
      if (state.activeActivity == 6 && state.sidebarWidth < kGradePaneW)
        state.sidebarWidth = kGradePaneW;
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
  // Shell: owns width/background/padding and holds the pinned header
  // (title + histogram). It does NOT scroll.
  View sidebarShell;
  sidebarShell.style.height = Size::full();
  sidebarShell.style.flexShrink = 0;
  sidebarShell.style.direction = FlexDirection::Column;
  sidebarShell.style.backgroundColor = kColorPanelBg;
  sidebarShell.style.padding = EdgeInsets::all(12.0f);
  sidebarShell.style.overflowX = Overflow::Hidden;
  sidebarShell.style.overflowY = Overflow::Visible;
  sidebarShell.style.width = std::function<Size()>([]
                                                   { return Size::pixel(state.activeActivity != -1 ? state.sidebarWidth : 0.0f); });

  // Body: all the panes live here, and this is the part that scrolls.
  View sidebar;
  sidebar.style.width = Size::full();
  sidebar.style.flexGrow = 1;
  sidebar.style.minHeight = 0;
  sidebar.style.direction = FlexDirection::Column;
  sidebar.style.overflowX = Overflow::Hidden;
  sidebar.style.overflowY = Overflow::Auto;

  Text sidebarTitle;
  sidebarTitle.label =
      std::function<std::string()>([activities]() -> std::string
                                   {
        for (const auto &a : activities)
          if (a.id == state.activeActivity)
            return a.title;
        return ""; });
  sidebarTitle.fontSize = 12.0f;
  sidebarTitle.fontWeight = FontWeight::SemiBold;
  sidebarTitle.color = kColorTextMuted;
  sidebarTitle.style.margin = EdgeInsets{0, 0, 10, 0};
  sidebarTitle.style.flexShrink = 0;
  sidebarShell.addChild(std::move(sidebarTitle));

  // Histogram of the picture as currently shown (see computeHistogram).
  // Pinned above every pane, like Lightroom.
  View histogram;
  histogram.isCanvas = true;
  histogram.style.width = Size::full();
  histogram.style.height = Size::pixel(kHistHeightPx);
  histogram.style.flexShrink = 0;
  histogram.style.margin = EdgeInsets{0, 0, 12, 0};
  histogram.onPaint = [](CanvasContext &ctx)
  { paintHistogram(ctx); };
  histogram.onPressAt = [](float lx, float ly)
  {
    if (!state.image || ly > kHistTriHitPx)
      return;
    if (lx <= kHistTriHitPx)
      setClipping(!state.clipShadowsOn, state.clipHighlightsOn);
    else if (lx >= gHistCanvasW - kHistTriHitPx)
      setClipping(state.clipShadowsOn, !state.clipHighlightsOn);
  };
  histogram.canvasDirtySource = []
  {
    static uint32_t lastPainted = 0;
    static int lastActivity = -2;
    // Sidebar is polled before the canvas: do the pending recompute now.
    refreshHistogramIfStale();
    // Also repaint when the sidebar is switched or reopened, in case it
    // was hidden while the picture changed.
    const bool tabChanged = state.activeActivity != lastActivity;
    lastActivity = state.activeActivity;
    if (state.histVersion == lastPainted && !tabChanged)
      return false;
    lastPainted = state.histVersion;
    return true;
  };
  sidebarShell.addChild(std::move(histogram));

  // -- "Info" pane: real data about the open image.
  View infoPane;
  infoPane.style.direction = FlexDirection::Column;
  infoPane.style.width = Size::full();
  infoPane.style.gap = 6.0f;
  infoPane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == 0 ? Display::Flex : Display::None; });

  Text infoName;
  infoName.label = std::function<std::string()>([]
                                                { return state.image ? ("File: " + baseName(state.imagePath))
                                                                     : std::string("No image open"); });
  infoName.fontSize = 13.0f;
  infoName.color = kColorTextMid;
  infoPane.addChild(std::move(infoName));

  Text infoDims;
  infoDims.label = std::function<std::string()>([]
                                                {
    if (!state.image)
      return std::string();
    return "Size: " + std::to_string(state.image->width) + " x " +
           std::to_string(state.image->height) + " px"; });
  infoDims.fontSize = 13.0f;
  infoDims.color = kColorTextMid;
  infoPane.addChild(std::move(infoDims));

  Text infoZoom;
  infoZoom.label = std::function<std::string()>([]
                                                { return "Zoom: " +
                                                         std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%"; });
  infoZoom.fontSize = 13.0f;
  infoZoom.color = kColorTextMid;
  infoPane.addChild(std::move(infoZoom));

  sidebar.addChild(std::move(infoPane));

  // -- "History" pane: every undo step, newest first. Click to jump.
  View historyPane;
  historyPane.style.width = Size::full();
  historyPane.style.direction = FlexDirection::Column;
  historyPane.style.gap = 2.0f;
  historyPane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == kHistoryActivity ? Display::Flex : Display::None; });

  Text historyHint;
  historyHint.label = std::function<std::string()>([]() -> std::string
                                                   { return state.image
                                                                ? "Click a step to go back to it. Dimmed steps are ahead of "
                                                                  "you: a new edit discards them."
                                                                : "Open an image to see its edit history."; });
  historyHint.fontSize = 12.0f;
  historyHint.color = kColorTextMuted;
  historyHint.style.margin = EdgeInsets{0, 0, 6, 0};
  historyPane.addChild(std::move(historyHint));

  for (size_t r = 0; r < kMaxUndoSteps + 1; ++r)
    historyPane.addChild(buildHistoryRow(r));
  sidebar.addChild(std::move(historyPane));

  // -- "Adjust" pane: one slider row per entry in kAdjustmentSpecs. Adding
  // a new adjustment later is a one-line addition to that table — this
  // loop, the render pass (renderAdjusted), and buildAdjustmentSlider all need no
  // changes.
  View adjustPane;
  adjustPane.style.width = Size::full();
  adjustPane.style.direction = FlexDirection::Column;
  adjustPane.style.gap = 16.0f;
  adjustPane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == 1 ? Display::Flex : Display::None; });

  auto sectionLabel = [](const char *text)
  {
    Text h;
    h.label = std::string(text);
    h.fontSize = 12.0f;
    h.color = kColorTextMuted;
    return h;
  };
  bool colorHeaderAdded = false, detailHeaderAdded = false;
  adjustPane.addChild(sectionLabel("Tone"));
  for (const auto &spec : kAdjustmentSpecs)
  {
    if (!colorHeaderAdded && spec.stage == AdjustmentSpec::Stage::Color)
    {
      adjustPane.addChild(sectionLabel("Color"));
      colorHeaderAdded = true;
    }
    if (!detailHeaderAdded && spec.stage == AdjustmentSpec::Stage::Detail)
    {
      adjustPane.addChild(sectionLabel("Detail"));
      detailHeaderAdded = true;
    }
    adjustPane.addChild(buildAdjustmentSlider(spec));
  }

  sidebar.addChild(std::move(adjustPane));

  // -- "Curve" pane: tone curves — RGB (master) plus per-channel R, G, B, one
  // tab each. Click the plot to add a point, drag to move it, right-click a
  // point to delete it. The whole gesture (add + drag, or a plain drag) is one
  // undo step; the edited channel's histogram sits behind the curve, and
  // other channels' curves that have been changed stay visible, dimmed.
  View curvePane;
  curvePane.style.width = Size::full();
  curvePane.style.direction = FlexDirection::Column;
  curvePane.style.gap = 8.0f;
  curvePane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == 4 ? Display::Flex : Display::None; });

  // Channel tabs. A bullet after a name marks a curve that has been changed,
  // so an edit on a channel you've since switched away from isn't forgotten.
  View curveTabs;
  curveTabs.style.direction = FlexDirection::Row;
  curveTabs.style.gap = 4.0f;
  curveTabs.style.width = Size::pixel(kCurveSizePx);
  curveTabs.style.flexShrink = 0;
  for (int c = 0; c < static_cast<int>(kCurveChannelCount); ++c)
  {
    const auto ch = static_cast<CurveChannel>(c);
    View tab;
    tab.style.width = Size::pixel(kCurveTabW);
    tab.style.height = Size::pixel(26);
    tab.style.flexShrink = 0;
    tab.style.borderRadius = 4.0f;
    tab.style.justifyContent = Justify::Center;
    tab.style.alignItems = Align::Center;
    tab.style.hoverColor = kColorActivityHover;
    tab.style.backgroundColor = std::function<Color()>([ch]() -> Color
                                                       { return gCurveChannel == ch ? kColorDropdownRowHover : kColorActivityBarBg; });
    tab.style.borderWidth = std::function<float()>(
        [ch]
        { return gCurveChannel == ch ? 1.0f : 0.0f; });
    tab.style.borderColor = kColorDividerHover;
    tab.onClick = [ch]
    { setCurveChannel(ch); };

    Text tabText;
    tabText.label = std::function<std::string()>([ch]() -> std::string
                                                 {
      std::string name = ch == CurveChannel::Master  ? "RGB"
                         : ch == CurveChannel::Red   ? "R"
                         : ch == CurveChannel::Green ? "G"
                                                     : "B";
      if (!(state.adjust.curve(ch) == identityCurve()))
        name += " \u2022";
      return name; });
    tabText.fontSize = 12.0f;
    tabText.fontWeight = FontWeight::SemiBold;
    tabText.color = ch == CurveChannel::Master  ? kColorTextDark
                    : ch == CurveChannel::Red   ? Color{235, 95, 95, 255}
                    : ch == CurveChannel::Green ? Color{95, 200, 115, 255}
                                                : Color{115, 155, 255, 255};
    tab.addChild(std::move(tabText));
    curveTabs.addChild(std::move(tab));
  }
  curvePane.addChild(std::move(curveTabs));

  View curveCanvas;
  curveCanvas.isCanvas = true;
  curveCanvas.style.width = Size::pixel(kCurveSizePx);
  curveCanvas.style.height = Size::pixel(kCurveSizePx);
  curveCanvas.style.flexShrink = 0;
  curveCanvas.onPaint = [](CanvasContext &ctx)
  { paintCurve(ctx); };
  curveCanvas.canvasDirtySource = []
  {
    static uint32_t lastPainted = 0;
    static int lastActivity = -2;
    // Every image recompute bumps histVersion, which covers curve edits,
    // undo/redo, and a newly opened image.
    refreshHistogramIfStale();
    const bool tabChanged = state.activeActivity != lastActivity;
    lastActivity = state.activeActivity;
    const bool uiDirty = gCurveUiDirty; // channel switch / point selection
    gCurveUiDirty = false;
    if (state.histVersion == lastPainted && !tabChanged && !uiDirty)
      return false;
    lastPainted = state.histVersion;
    return true;
  };
  curveCanvas.onPressAt = [](float lx, float ly)
  { curvePress(lx, ly); };
  curveCanvas.onDragTo = [](float lx, float ly)
  { curveDrag(lx, ly); };
  curveCanvas.onDragEnd = []
  { curveEnd(); };
  curveCanvas.onRightPressAt = [](float lx, float ly)
  {
    curveRightPress(lx, ly);
  };
  curvePane.addChild(std::move(curveCanvas));

  Text curveInfo;
  curveInfo.label = std::function<std::string()>([]() -> std::string
                                                 {
    const auto &p = curveActiveCurve();
    const std::string name = curveChannelName(gCurveChannel);
    if (gCurveActive >= 0 && gCurveActive < static_cast<int>(p.size()))
      return name + ": In " +
             std::to_string(static_cast<int>(std::lround(p[gCurveActive].x))) +
             "  Out " +
             std::to_string(static_cast<int>(std::lround(p[gCurveActive].y)));
    return name + ": " + std::to_string(p.size()) + " points"; });
  curveInfo.fontSize = 13.0f;
  curveInfo.color = kColorTextMid;
  curvePane.addChild(std::move(curveInfo));

  Text curveHint1;
  curveHint1.label = std::string("Click: add point   Drag: move");
  curveHint1.fontSize = 11.0f;
  curveHint1.color = kColorTextFaint;
  curvePane.addChild(std::move(curveHint1));
  Text curveHint2;
  curveHint2.label = std::string("Right-click: delete point");
  curveHint2.fontSize = 11.0f;
  curveHint2.color = kColorTextFaint;
  curvePane.addChild(std::move(curveHint2));

  curvePane.addChild(sidebarButton("Reset Curve", resetCurve));
  curvePane.addChild(sidebarButton("Reset All Curves", resetAllCurves));

  sidebar.addChild(std::move(curvePane));

  // -- "Color Mix" pane: HSL mixer. Eight color bands, each with Hue,
  // Saturation and Luminance; the tabs pick which of the three sets of eight
  // sliders is shown (a bullet marks a tab that has been changed, so an edit
  // on a tab you've since switched away from isn't forgotten).
  View mixPane;
  mixPane.style.width = Size::full();
  mixPane.style.direction = FlexDirection::Column;
  mixPane.style.gap = 12.0f;
  mixPane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == 5 ? Display::Flex : Display::None; });

  View mixTabs;
  mixTabs.style.direction = FlexDirection::Row;
  mixTabs.style.gap = 6.0f;
  mixTabs.style.width = Size::pixel(3.0f * kMixTabW + 2.0f * 6.0f);
  mixTabs.style.flexShrink = 0;
  for (int c = 0; c < static_cast<int>(kMixChanCount); ++c)
  {
    const auto chan = static_cast<MixChan>(c);
    View tab;
    tab.style.width = Size::pixel(kMixTabW);
    tab.style.height = Size::pixel(26);
    tab.style.flexShrink = 0;
    tab.style.borderRadius = 4.0f;
    tab.style.justifyContent = Justify::Center;
    tab.style.alignItems = Align::Center;
    tab.style.hoverColor = kColorActivityHover;
    tab.style.backgroundColor = std::function<Color()>([chan]() -> Color
                                                       { return gMixChan == chan ? kColorDropdownRowHover : kColorActivityBarBg; });
    tab.style.borderWidth = std::function<float()>(
        [chan]
        { return gMixChan == chan ? 1.0f : 0.0f; });
    tab.style.borderColor = kColorDividerHover;
    tab.onClick = [chan]
    { gMixChan = chan; };

    Text tabText;
    tabText.label = std::function<std::string()>([chan]() -> std::string
                                                 {
      std::string name = chan == MixChan::Hue          ? "Hue"
                         : chan == MixChan::Saturation ? "Sat"
                                                       : "Lum";
      if (mixChanAnyNonZero(state.adjust.mixValues(), chan))
        name += " \u2022";
      return name; });
    tabText.fontSize = 12.0f;
    tabText.fontWeight = FontWeight::SemiBold;
    tabText.color = kColorTextDark;
    tab.addChild(std::move(tabText));
    mixTabs.addChild(std::move(tab));
  }
  mixPane.addChild(std::move(mixTabs));

  // One group of eight sliders per tab; only the selected tab's is displayed.
  for (int c = 0; c < static_cast<int>(kMixChanCount); ++c)
  {
    const auto chan = static_cast<MixChan>(c);
    View group;
    group.style.width = Size::full();
    group.style.direction = FlexDirection::Column;
    group.style.gap = 12.0f;
    group.style.display = std::function<Display()>(
        [chan]
        { return gMixChan == chan ? Display::Flex : Display::None; });
    for (size_t band = 0; band < kMixBandCount; ++band)
      group.addChild(buildMixSlider(chan, band));
    mixPane.addChild(std::move(group));
  }

  mixPane.addChild(sidebarButton("Reset Color Mix", resetColorMix));

  sidebar.addChild(std::move(mixPane));

  // -- "Color Grade" pane: 3-way / single-range color wheels (Shadows,
  // Midtones, Highlights, Global) plus shared Blending and Balance sliders.
  View gradePane;
  gradePane.style.width = Size::full();
  gradePane.style.direction = FlexDirection::Column;
  gradePane.style.gap = 10.0f;
  gradePane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == 6 ? Display::Flex : Display::None; });

  // Mode tabs: 3-Way | Shadows | Mid | High | Global. A bullet marks a range
  // that has been adjusted.
  View gradeTabs;
  gradeTabs.style.direction = FlexDirection::Row;
  gradeTabs.style.gap = 4.0f;
  gradeTabs.style.flexShrink = 0;
  for (int m = 0; m < 5; ++m)
  {
    View tab;
    tab.style.width = Size::pixel(kGradeTabW);
    tab.style.height = Size::pixel(26);
    tab.style.flexShrink = 0;
    tab.style.borderRadius = 4.0f;
    tab.style.justifyContent = Justify::Center;
    tab.style.alignItems = Align::Center;
    tab.style.hoverColor = kColorActivityHover;
    tab.style.backgroundColor = std::function<Color()>([m]() -> Color
                                                       { return gGradeMode == m ? kColorDropdownRowHover : kColorActivityBarBg; });
    tab.style.borderWidth =
        std::function<float()>([m]
                               { return gGradeMode == m ? 1.0f : 0.0f; });
    tab.style.borderColor = kColorDividerHover;
    tab.onClick = [m]
    {
      gGradeMode = m;
      if (m > 0)
        gGradeActive = m - 1;
    };

    Text tabText;
    tabText.label = std::function<std::string()>([m]() -> std::string
                                                 {
      static const char *const names[5] = {"3-Way", "Shad", "Mid", "High",
                                           "Glob"};
      std::string name = names[m];
      if (m > 0 && !state.adjust.grade().wheel[m - 1].neutral())
        name += "\u2022";
      return name; });
    tabText.fontSize = 11.0f;
    tabText.fontWeight = FontWeight::SemiBold;
    tabText.color = kColorTextDark;
    tab.addChild(std::move(tabText));
    gradeTabs.addChild(std::move(tab));
  }
  gradePane.addChild(std::move(gradeTabs));

  // H:13 S:72 L:+1 for the wheel touched last
  Text gradeReadout;
  gradeReadout.label = std::function<std::string()>([]() -> std::string
                                                    {
    const int r = gGradeMode > 0 ? gGradeMode - 1 : gGradeActive;
    const GradeWheel &w = state.adjust.grade().wheel[r];
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s   H:%d S:%d L:%+d", gradeRangeName(r),
                  static_cast<int>(std::lround(w.hue)),
                  static_cast<int>(std::lround(w.sat)),
                  static_cast<int>(std::lround(w.lum)));
    return buf; });
  gradeReadout.fontSize = 12.0f;
  gradeReadout.color = kColorTextMid;
  gradePane.addChild(std::move(gradeReadout));

  // One body per mode; only the selected one is displayed.
  auto modeBody = [](int m, View body)
  {
    body.style.display = std::function<Display()>(
        [m]
        { return gGradeMode == m ? Display::Flex : Display::None; });
    return body;
  };
  gradePane.addChild(modeBody(0, buildGradeThreeWay()));
  for (int m = 1; m <= 4; ++m)
    gradePane.addChild(modeBody(m, buildGradeSingle(m - 1)));

  // Blending / Balance, shared by every mode.
  SliderSpec blending;
  blending.name = "Blending";
  blending.lo = 0;
  blending.hi = 100;
  blending.get = []
  { return state.adjust.grade().blending; };
  blending.set = [](float v)
  {
    gradeEdit([&](ColorGrade &g)
              { g.blending = std::round(v); }, 16);
  };
  blending.format = [](float v)
  {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  blending.onEnd = []
  { state.adjust.endGradeEdit(); };
  gradePane.addChild(buildSliderRow(blending));

  SliderSpec balance = blending;
  balance.name = "Balance";
  balance.lo = -100;
  balance.hi = 100;
  balance.get = []
  { return state.adjust.grade().balance; };
  balance.set = [](float v)
  {
    gradeEdit([&](ColorGrade &g)
              { g.balance = std::round(v); }, 17);
  };
  balance.format = [](float v)
  {
    const int i = static_cast<int>(std::lround(v));
    return i > 0 ? "+" + std::to_string(i) : std::to_string(i);
  };
  gradePane.addChild(buildSliderRow(balance));

  gradePane.addChild(sidebarButton("Reset Color Grading", resetColorGrade));
  sidebar.addChild(std::move(gradePane));

  // -- "Masks" pane: local adjustments. Each mask is a shape plus its own copy
  // of the Adjust sliders, applied only where the shape is (see the "masks"
  // block). Pick or add a mask in the list; the selected one is drawn on the
  // image, where it can be dragged (inside = move, square handles = resize).
  View maskPane;
  maskPane.style.width = Size::full();
  maskPane.style.direction = FlexDirection::Column;
  maskPane.style.gap = 6.0f;
  maskPane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == 7 ? Display::Flex : Display::None; });

  Text maskHint;
  maskHint.label = std::function<std::string()>([]() -> std::string
                                                { return state.image
                                                             ? "Add a mask, then adjust it in the panel on the right. "
                                                               "Drag the shape on the image to move it, or its square "
                                                               "handles to resize (on a linear gradient they also turn "
                                                               "it). With a brush, drag on the image to paint. Pick "
                                                               "another mask from the list."
                                                             : "Open an image to add masks."; });
  maskHint.fontSize = 12.0f;
  maskHint.color = kColorTextMuted;
  maskHint.style.margin = EdgeInsets{0, 0, 6, 0};
  maskPane.addChild(std::move(maskHint));

  // One button per kind of mask.
  maskPane.addChild(sidebarButton(
      "+ Radial Gradient", []
      { addMask(MaskType::Radial); }, {}, true,
      []
      { return state.original != nullptr; }));
  maskPane.addChild(sidebarButton(
      "+ Linear Gradient", []
      { addMask(MaskType::Linear); }, {}, true,
      []
      { return state.original != nullptr; }));
  maskPane.addChild(sidebarButton(
      "+ Brush", []
      { addMask(MaskType::Brush); }, {}, true,
      []
      { return state.original != nullptr; }));
  maskPane.addChild(sidebarButton(
      "+ Luminosity Range", []
      { addMask(MaskType::Luminosity); }, {}, true,
      []
      { return state.original != nullptr; }));
  maskPane.addChild(sidebarButton(
      "+ Object", []
      { addMask(MaskType::Object); }, {}, true,
      []
      { return state.original != nullptr; }));
  maskPane.addChild(sidebarButton(
      "+ Color Range", []
      { addMask(MaskType::ColorRange); }, {}, true,
      []
      { return state.original != nullptr; }));

  for (size_t i = 0; i < kMaxMasks; ++i)
    maskPane.addChild(buildMaskRow(i));

  // Everything below applies to the selected mask and is hidden without one.
  // It is shown in the RIGHT-HAND panel (built after the canvas, see
  // maskPanel below): On / Invert / Tint chips, the brush settings, then
  // three tabs: Effect (the sliders), Shape and Parts.
  View maskEditor;
  maskEditor.style.width = Size::full();
  maskEditor.style.direction = FlexDirection::Column;
  maskEditor.style.gap = 12.0f;
  maskEditor.style.display = std::function<Display()>(
      []
      { return selMask() >= 0 ? Display::Flex : Display::None; });

  // Equal-width chip / tab: `n` of them share a row of the panel.
  const auto maskChipFn = [](std::function<std::string()> label, int n,
                             std::function<void()> onClick,
                             std::function<bool()> selected)
  {
    View c;
    c.style.width = std::function<Size()>([n]
                                          { return Size::pixel(maskChipW(n)); });
    c.style.height = Size::pixel(26);
    c.style.flexShrink = 0;
    c.style.borderRadius = 4.0f;
    c.style.justifyContent = Justify::Center;
    c.style.alignItems = Align::Center;
    c.style.hoverColor = kColorActivityHover;
    c.style.backgroundColor = std::function<Color()>([selected]() -> Color
                                                     { return selected() ? kColorDropdownRowHover : kColorActivityBarBg; });
    c.style.borderWidth = std::function<float()>(
        [selected]
        { return selected() ? 1.0f : 0.0f; });
    c.style.borderColor = kColorDividerHover;
    c.onClick = std::move(onClick);
    Text t;
    t.label = std::move(label);
    t.fontSize = 12.0f;
    t.fontWeight = FontWeight::SemiBold;
    t.color = kColorTextDark;
    c.addChild(std::move(t));
    return c;
  };

  // Same chip with a fixed label.
  const auto maskChip = [maskChipFn](const char *label, int n,
                                     std::function<void()> onClick,
                                     std::function<bool()> selected)
  {
    const std::string text = label;
    return maskChipFn([text]
                      { return text; },
                      n, std::move(onClick), std::move(selected));
  };

  View maskFlags;
  maskFlags.style.direction = FlexDirection::Row;
  maskFlags.style.gap = 4.0f;
  maskFlags.style.flexShrink = 0;
  maskFlags.addChild(maskChip(
      "On", 3,
      []
      { maskEditDiscrete([](Mask &m)
                         { m.enabled = !m.enabled; }); },
      []
      {
        const Mask *m = selectedMask();
        return m && m->enabled;
      }));
  maskFlags.addChild(maskChip(
      "Invert", 3,
      []
      { maskEditDiscrete([](Mask &m)
                         { m.invert = !m.invert; }); },
      []
      {
        const Mask *m = selectedMask();
        return m && m->invert;
      }));
  maskFlags.addChild(maskChip(
      "Tint", 3,
      []
      {
        bool &flag = tintFlag();
        flag = !flag;
        state.markDirty();
      },
      []
      { return tintFlag(); }));

  // Name field. TextInput is Canvas-backed and wires up the caret, click to
  // place it, arrows, Backspace / Delete, Ctrl+V and Enter itself.
  View maskNameRow;
  maskNameRow.style.width = Size::full();
  maskNameRow.style.flexShrink = 0;
  maskNameRow.style.display = std::function<Display()>([]() -> Display
                                                       {
    syncMaskNameField(); // polled every cycle: follows selection and undo
    return Display::Flex; });
  {
    TextInput ti;
    ti.placeholder = "Name this mask";
    ti.fontSize = 13.0f;
    ti.textColor = kColorTextDark;
    ti.placeholderColor = kColorTextMuted;
    ti.caretColor = kColorTextDark;
    ti.borderColor = kColorDividerHover;
    ti.focusedBorderColor = Color{90, 150, 235, 255};
    ti.style.width = Size::full();
    ti.style.height = Size::pixel(28);
    ti.style.borderWidth = 1.0f;
    ti.style.borderRadius = 4.0f;
    ti.style.backgroundColor = kColorActivityBarBg;
    ti.onChange = [](const std::string &s)
    { renameSelectedMask(s); };
    ti.onSubmit = [](const std::string &)
    { state.adjust.endMaskEdit(); }; // Enter closes the undo step
    gMaskNameState = ti.state;
    maskNameRow.addChild(std::move(ti));
  }
  maskEditor.addChild(std::move(maskNameRow));

  maskEditor.addChild(std::move(maskFlags));

  // The three tab bodies. Only the one picked in state.maskTab is shown.
  const auto maskTabView = [](int tab)
  {
    View v;
    v.style.width = Size::full();
    v.style.direction = FlexDirection::Column;
    v.style.gap = 12.0f;
    v.style.display = std::function<Display()>([tab]() -> Display
                                               { return state.maskTab == tab ? Display::Flex : Display::None; });
    return v;
  };
  View maskTabEffect = maskTabView(0);
  View maskTabShape = maskTabView(1);
  View maskTabParts = maskTabView(2);

  // Parts: combine several shapes into this mask. Each part after the first
  // is joined to the ones before it (Add / Subtract / Intersect); the rows
  // only appear once there is more than one.
  View partsSec;
  partsSec.style.width = Size::full();
  partsSec.style.direction = FlexDirection::Column;
  partsSec.style.gap = 6.0f;
  partsSec.addChild(sectionLabel("Parts"));
  for (size_t j = 0; j < kMaxMaskParts; ++j)
    partsSec.addChild(buildPartRow(j));
  View opRow; // how the selected part joins the rest (not for the first)
  opRow.style.width = Size::full();
  opRow.style.direction = FlexDirection::Column;
  opRow.style.gap = 6.0f;
  opRow.style.display = std::function<Display()>(
      []() -> Display
      { return selPart() > 0 ? Display::Flex : Display::None; });
  for (const MaskOp op : {MaskOp::Add, MaskOp::Subtract, MaskOp::Intersect})
    opRow.addChild(sidebarButton(
        maskOpName(op), [op]
        { setPartOp(op); },
        [op]
        {
          const MaskPart *p = selectedPart();
          return p && selPart() > 0 && p->op == op;
        }));
  partsSec.addChild(std::move(opRow));
  partsSec.addChild(sidebarButton("+ Part: Radial",
                                  []
                                  { addPart(MaskType::Radial); }));
  partsSec.addChild(sidebarButton("+ Part: Linear",
                                  []
                                  { addPart(MaskType::Linear); }));
  partsSec.addChild(sidebarButton("+ Part: Brush",
                                  []
                                  { addPart(MaskType::Brush); }));
  partsSec.addChild(sidebarButton("+ Part: Luminosity",
                                  []
                                  { addPart(MaskType::Luminosity); }));
  partsSec.addChild(sidebarButton("+ Part: Object",
                                  []
                                  { addPart(MaskType::Object); }));
  partsSec.addChild(sidebarButton("+ Part: Color Range",
                                  []
                                  { addPart(MaskType::ColorRange); }));

  View removeRow;
  removeRow.style.width = Size::full();
  removeRow.style.display = std::function<Display()>([]() -> Display
                                                     {
    const Mask *m = selectedMask();
    return m && m->parts.size() > 1 ? Display::Flex : Display::None; });
  removeRow.addChild(sidebarButton("Remove Part", removeSelectedPart));
  partsSec.addChild(std::move(removeRow));
  maskTabParts.addChild(std::move(partsSec));

  // Shape sliders, per mask type (shown only for that type).
  View radialShape;
  radialShape.style.width = Size::full();
  radialShape.style.direction = FlexDirection::Column;
  radialShape.style.gap = 12.0f;
  radialShape.style.display = std::function<Display()>([]() -> Display
                                                       { return selectedPartType() == MaskType::Radial ? Display::Flex
                                                                                                       : Display::None; });
  radialShape.addChild(sectionLabel("Shape"));
  radialShape.addChild(buildSliderRow(maskRadialSlider(
      "Width", kMaskMinRadius * 100.0f, kMaskMaxRadius * 100.0f,
      [](const RadialShape &r)
      { return r.rx * 100.0f; },
      [](RadialShape &r, float v)
      { r.rx = v / 100.0f; }, kMaskCtlWidth)));
  radialShape.addChild(buildSliderRow(maskRadialSlider(
      "Height", kMaskMinRadius * 100.0f, kMaskMaxRadius * 100.0f,
      [](const RadialShape &r)
      { return r.ry * 100.0f; },
      [](RadialShape &r, float v)
      { r.ry = v / 100.0f; }, kMaskCtlHeight)));
  radialShape.addChild(buildSliderRow(maskRadialSlider(
      "Angle", -90.0f, 90.0f, [](const RadialShape &r)
      { return r.angle; },
      [](RadialShape &r, float v)
      { r.angle = v; }, kMaskCtlAngle)));
  radialShape.addChild(buildSliderRow(maskRadialSlider(
      "Feather", 0.0f, 100.0f,
      [](const RadialShape &r)
      { return r.feather * 100.0f; },
      [](RadialShape &r, float v)
      { r.feather = v / 100.0f; },
      kMaskCtlFeather)));
  maskTabShape.addChild(std::move(radialShape));

  View linearShape;
  linearShape.style.width = Size::full();
  linearShape.style.direction = FlexDirection::Column;
  linearShape.style.gap = 12.0f;
  linearShape.style.display = std::function<Display()>([]() -> Display
                                                       { return selectedPartType() == MaskType::Linear ? Display::Flex
                                                                                                       : Display::None; });
  linearShape.addChild(sectionLabel("Shape"));
  linearShape.addChild(buildSliderRow(maskLinearSlider(
      "Length", kMaskMinRadius * 100.0f, kMaskMaxRadius * 100.0f,
      [](const LinearShape &l)
      { return l.len * 100.0f; },
      [](LinearShape &l, float v)
      { l.len = v / 100.0f; }, kMaskCtlLength)));
  linearShape.addChild(buildSliderRow(maskLinearSlider(
      "Angle", -180.0f, 180.0f, [](const LinearShape &l)
      { return l.angle; },
      [](LinearShape &l, float v)
      { l.angle = v; }, kMaskCtlAngle)));
  maskTabShape.addChild(std::move(linearShape));

  // Luminosity range (shown for a luminosity mask): which brightnesses it
  // selects. Min can never pass Max, nor Max go below Min.
  View lumShape;
  lumShape.style.width = Size::full();
  lumShape.style.direction = FlexDirection::Column;
  lumShape.style.gap = 12.0f;
  lumShape.style.display = std::function<Display()>([]() -> Display
                                                    { return selectedPartType() == MaskType::Luminosity ? Display::Flex
                                                                                                        : Display::None; });
  lumShape.addChild(sectionLabel("Range"));
  lumShape.addChild(buildSliderRow(maskShapeSlider<LuminosityShape>(
      "Min", 0.0f, 100.0f,
      [](const LuminosityShape &l)
      { return l.lo * 100.0f; },
      [](LuminosityShape &l, float v)
      { l.setLo(v / 100.0f); },
      kMaskCtlLumLo)));
  lumShape.addChild(buildSliderRow(maskShapeSlider<LuminosityShape>(
      "Max", 0.0f, 100.0f,
      [](const LuminosityShape &l)
      { return l.hi * 100.0f; },
      [](LuminosityShape &l, float v)
      { l.setHi(v / 100.0f); },
      kMaskCtlLumHi)));
  lumShape.addChild(buildSliderRow(maskShapeSlider<LuminosityShape>(
      "Smoothness", 0.0f, 100.0f,
      [](const LuminosityShape &l)
      { return l.feather * 100.0f; },
      [](LuminosityShape &l, float v)
      { l.feather = v / 100.0f; },
      kMaskCtlFeather)));
  maskTabShape.addChild(std::move(lumShape));

  View crShape;
  crShape.style.width = Size::full();
  crShape.style.direction = FlexDirection::Column;
  crShape.style.gap = 12.0f;
  crShape.style.display = std::function<Display()>([]() -> Display
                                                   { return selectedPartType() == MaskType::ColorRange ? Display::Flex
                                                                                                       : Display::None; });
  crShape.addChild(sectionLabel("Color Range"));
  {
    View hint;
    Text t;
    t.label = std::function<std::string()>([]() -> std::string
                                           {
      const MaskPart *p = selectedPart();
      const auto *s = p ? std::get_if<ColorRangeShape>(&p->shape) : nullptr;
      return s && s->set ? "Click the picture to pick another color"
                         : "Click the picture to pick a color"; });
    t.fontSize = 12.0f;
    t.color = kColorTextMuted;
    crShape.addChild(std::move(t));
  }
  crShape.addChild(buildSliderRow(maskShapeSlider<ColorRangeShape>(
      "Range", 0.0f, 100.0f,
      [](const ColorRangeShape &s)
      { return s.range * 100.0f; },
      [](ColorRangeShape &s, float v)
      { s.range = v / 100.0f; },
      kMaskCtlRange)));
  crShape.addChild(buildSliderRow(maskShapeSlider<ColorRangeShape>(
      "Smoothness", 0.0f, 100.0f,
      [](const ColorRangeShape &s)
      { return s.feather * 100.0f; },
      [](ColorRangeShape &s, float v)
      { s.feather = v / 100.0f; },
      kMaskCtlFeather)));
  maskTabShape.addChild(std::move(crShape));

  // Brush settings (shown for a brush mask). They apply to the NEXT stroke;
  // strokes already painted keep the values they were painted with.
  const auto brushSlider = [](const char *name, float lo, float hi,
                              std::function<float()> get,
                              std::function<void(float)> set)
  {
    SliderSpec s;
    s.name = name;
    s.lo = lo;
    s.hi = hi;
    s.get = std::move(get);
    s.set = std::move(set);
    s.format = [](float v)
    {
      return std::to_string(static_cast<int>(std::lround(v)));
    };
    s.affectsPixels = false;
    s.trackW = kMaskTrackW;
    return buildSliderRow(s);
  };
  View brushShape;
  brushShape.style.width = Size::full();
  brushShape.style.direction = FlexDirection::Column;
  brushShape.style.gap = 12.0f;
  brushShape.style.display = std::function<Display()>(
      []() -> Display
      { return selectedIsBrush() ? Display::Flex : Display::None; });
  brushShape.addChild(sidebarButton(
      "Erase",
      []
      {
        state.mask.brushErase = !state.mask.brushErase;
        state.markDirty();
      },
      []
      { return state.mask.brushErase; }));
  brushShape.addChild(brushSlider(
      "Size", 1.0f, kBrushMaxSize * 100.0f,
      []
      { return state.mask.brushSize * 100.0f; },
      [](float v)
      {
        state.mask.brushSize =
            std::clamp(v / 100.0f, kBrushMinSize, kBrushMaxSize);
      }));
  brushShape.addChild(brushSlider(
      "Feather", 0.0f, 100.0f, []
      { return state.mask.brushFeather * 100.0f; },
      [](float v)
      { state.mask.brushFeather = std::clamp(v / 100.0f, 0.0f, 1.0f); }));
  brushShape.addChild(brushSlider(
      "Density", 1.0f, 100.0f, []
      { return state.mask.brushDensity * 100.0f; },
      [](float v)
      { state.mask.brushDensity = std::clamp(v / 100.0f, 0.01f, 1.0f); }));
  brushShape.addChild(brushSlider(
      "Flow", 1.0f, 100.0f, []
      { return state.mask.brushFlow * 100.0f; },
      [](float v)
      { state.mask.brushFlow = std::clamp(v / 100.0f, 0.01f, 1.0f); }));
  brushShape.addChild(sidebarButton("Clear Strokes", []
                                    {
    const size_t part = static_cast<size_t>(selPart());
    maskEditDiscrete([part](Mask &m) {
      if (part < m.parts.size())
        if (auto *b = std::get_if<BrushShape>(&m.parts[part].shape))
          b->strokes.clear();
    }); }));
  maskEditor.addChild(std::move(brushShape)); // pinned above the tabs: size/feather
                                              // are used while painting

  View objectShape;
  objectShape.style.width = Size::full();
  objectShape.style.direction = FlexDirection::Column;
  objectShape.style.gap = 12.0f;
  objectShape.style.display = std::function<Display()>([]() -> Display
                                                       { return selectedPartType() == MaskType::Object ? Display::Flex
                                                                                                       : Display::None; });
  objectShape.addChild(sidebarButton("Set test map (bright areas)", []
                                     {
  if (auto m = makeTestObjectMap())
    setSelectedObjectMap(std::move(m)); }));
  objectShape.addChild(buildSliderRow(maskShapeSlider<ObjectShape>(
      "Feather", 0.0f, 100.0f,
      [](const ObjectShape &o)
      { return o.feather * 100.0f; },
      [](ObjectShape &o, float v)
      { o.feather = v / 100.0f; },
      kMaskCtlFeather)));
  objectShape.addChild(buildSliderRow(maskShapeSlider<ObjectShape>(
      "Grow", -100.0f, 100.0f,
      [](const ObjectShape &o)
      { return o.grow * 100.0f; },
      [](ObjectShape &o, float v)
      { o.grow = v / 100.0f; },
      kMaskCtlWidth)));
  objectShape.addChild(sidebarButton(
      "Click adds: Background",
      []
      { state.mask.objBg = !state.mask.objBg; state.markDirty(); },
      []
      { return state.mask.objBg; }));
  objectShape.addChild(sidebarButton("Clear selection", clearSelectedObject));
  {
    Text segInfo;
    segInfo.label = std::function<std::string()>([]
                                                 { return getSegInfo(); });
    segInfo.fontSize = 11.0f;
    segInfo.color = kColorTextFaint;
    objectShape.addChild(std::move(segInfo));
  }
  maskTabShape.addChild(std::move(objectShape));

  // ---- Effect tab ----
  // A second row of chips picks Basic / Mix / Grade / Curve; a bullet marks
  // a section that has been changed for this mask.
  const auto chipRow = []
  {
    View r;
    r.style.direction = FlexDirection::Row;
    r.style.gap = 4.0f;
    r.style.flexShrink = 0;
    return r;
  };
  const auto maskFxView = [](int fx)
  {
    View v;
    v.style.width = Size::full();
    v.style.direction = FlexDirection::Column;
    v.style.gap = 12.0f;
    v.style.display = std::function<Display()>([fx]() -> Display
                                               { return state.maskFx == fx ? Display::Flex : Display::None; });
    return v;
  };
  View fxBasic = maskFxView(0);
  View fxMix = maskFxView(1);
  View fxGrade = maskFxView(2);
  View fxCurve = maskFxView(3);

  // Basic: Amount plus the same table as the Adjust pane.
  fxBasic.addChild(buildMaskAmountSlider());
  bool maskColorHeaderAdded = false, maskDetailHeaderAdded = false;
  fxBasic.addChild(sectionLabel("Tone"));
  for (const auto &spec : kAdjustmentSpecs)
  {

    if (!maskColorHeaderAdded && spec.stage == AdjustmentSpec::Stage::Color)
    {
      fxBasic.addChild(sectionLabel("Color"));
      maskColorHeaderAdded = true;
    }
    if (!maskDetailHeaderAdded && spec.stage == AdjustmentSpec::Stage::Detail)
    {
      fxBasic.addChild(sectionLabel("Detail"));
      maskDetailHeaderAdded = true;
    }
    fxBasic.addChild(buildMaskAdjustmentSlider(spec));
  }
  fxBasic.addChild(sidebarButton("Reset Basic", []
                                 { maskEditDiscrete([](Mask &m)
                                                    { m.adj.fill(0.0f);
                                                      m.amount = 100.0f; }); }));

  // Mix: Hue / Sat / Lum chips, eight band sliders each.
  {
    View chanRow = chipRow();
    for (int c = 0; c < static_cast<int>(kMixChanCount); ++c)
    {
      const auto chan = static_cast<MixChan>(c);
      chanRow.addChild(maskChipFn(
          [chan]() -> std::string
          {
            std::string name = chan == MixChan::Hue          ? "Hue"
                               : chan == MixChan::Saturation ? "Sat"
                                                             : "Lum";
            const Mask *m = selectedMask();
            if (m && mixChanAnyNonZero(m->mix, chan))
              name += " \u2022";
            return name;
          },
          3, [chan]
          { gMaskMixChan = chan; },
          [chan]
          { return gMaskMixChan == chan; }));
    }
    fxMix.addChild(std::move(chanRow));
    for (int c = 0; c < static_cast<int>(kMixChanCount); ++c)
    {
      const auto chan = static_cast<MixChan>(c);
      View group;
      group.style.width = Size::full();
      group.style.direction = FlexDirection::Column;
      group.style.gap = 12.0f;
      group.style.display = std::function<Display()>([chan]() -> Display
                                                     { return gMaskMixChan == chan ? Display::Flex : Display::None; });
      for (size_t band = 0; band < kMixBandCount; ++band)
        group.addChild(buildMaskMixSlider(chan, band));
      fxMix.addChild(std::move(group));
    }
    fxMix.addChild(sidebarButton("Reset Color Mix", []
                                 { maskEditDiscrete([](Mask &m)
                                                    { m.mix.fill(0.0f); }); }));
  }

  // Grade: one chip per range, Hue / Saturation / Luminance sliders for the
  // picked range, then Blending and Balance for all of them.
  {
    static const char *const rangeShort[kGradeRangeCount] = {"Shad", "Mid",
                                                             "High", "Glob"};
    View rangeRow = chipRow();
    for (int r = 0; r < static_cast<int>(kGradeRangeCount); ++r)
      rangeRow.addChild(maskChipFn(
          [r]() -> std::string
          {
            std::string name = rangeShort[r];
            const Mask *m = selectedMask();
            if (m && !m->grade.wheel[static_cast<size_t>(r)].neutral())
              name += " \u2022";
            return name;
          },
          4, [r]
          { gMaskGradeRange = r; },
          [r]
          { return gMaskGradeRange == r; }));
    fxGrade.addChild(std::move(rangeRow));
    for (int r = 0; r < static_cast<int>(kGradeRangeCount); ++r)
    {
      View group;
      group.style.width = Size::full();
      group.style.direction = FlexDirection::Column;
      group.style.gap = 12.0f;
      group.style.display = std::function<Display()>([r]() -> Display
                                                     { return gMaskGradeRange == r ? Display::Flex : Display::None; });
      group.addChild(buildMaskGradeSlider("Hue", r, &GradeWheel::hue, 0, 0.0f, 359.0f, false));
      group.addChild(buildMaskGradeSlider("Saturation", r, &GradeWheel::sat, 1, 0.0f, 100.0f, false));
      group.addChild(buildMaskGradeSlider("Luminance", r, &GradeWheel::lum, 2, -100.0f, 100.0f, true));
      fxGrade.addChild(std::move(group));
    }
    fxGrade.addChild(buildMaskGradeShared(false));
    fxGrade.addChild(buildMaskGradeShared(true));
    fxGrade.addChild(sidebarButton("Reset Color Grading", []
                                   { maskEditDiscrete([](Mask &m)
                                                      { m.grade = ColorGrade{}; }); }));
  }

  // Curve: RGB / R / G / B chips, the plot, a readout and resets.
  {
    View chRow = chipRow();
    for (int c = 0; c < static_cast<int>(kCurveChannelCount); ++c)
    {
      const auto ch = static_cast<CurveChannel>(c);
      chRow.addChild(maskChipFn(
          [ch]() -> std::string
          {
            std::string name = ch == CurveChannel::Master  ? "RGB"
                               : ch == CurveChannel::Red   ? "R"
                               : ch == CurveChannel::Green ? "G"
                                                           : "B";
            const Mask *m = selectedMask();
            if (m && !(m->curves[static_cast<size_t>(ch)] == identityCurve()))
              name += " \u2022";
            return name;
          },
          4, [ch]
          {
            gMaskCurveActive = -1;
            gMaskCurveCh = ch;
            gMaskCurveUiDirty = true; },
          [ch]
          { return gMaskCurveCh == ch; }));
    }
    fxCurve.addChild(std::move(chRow));
    fxCurve.addChild(buildMaskCurveCanvas());

    Text curveInfo;
    curveInfo.label = std::function<std::string()>([]() -> std::string
                                                   {
      const CurvePoints p = maskCurveCopy();
      const std::string name = curveChannelName(gMaskCurveCh);
      if (gMaskCurveActive >= 0 && gMaskCurveActive < static_cast<int>(p.size()))
        return name + ": In " +
               std::to_string(static_cast<int>(std::lround(p[static_cast<size_t>(gMaskCurveActive)].x))) +
               "  Out " +
               std::to_string(static_cast<int>(std::lround(p[static_cast<size_t>(gMaskCurveActive)].y)));
      return name + ": " + std::to_string(p.size()) + " points"; });
    curveInfo.fontSize = 13.0f;
    curveInfo.color = kColorTextMid;
    fxCurve.addChild(std::move(curveInfo));

    Text curveHint;
    curveHint.label = std::string("Click: add   Drag: move   Right-click: delete");
    curveHint.fontSize = 11.0f;
    curveHint.color = kColorTextFaint;
    fxCurve.addChild(std::move(curveHint));

    fxCurve.addChild(sidebarButton("Reset Curve", []
                                   {
      gMaskCurveActive = -1;
      gMaskCurveUiDirty = true;
      maskEditDiscrete([](Mask &m)
                       { m.curves[static_cast<size_t>(gMaskCurveCh)] = identityCurve(); }); }));
    fxCurve.addChild(sidebarButton("Reset All Curves", []
                                   {
      gMaskCurveActive = -1;
      gMaskCurveUiDirty = true;
      maskEditDiscrete([](Mask &m)
                       { m.curves = identityCurves(); }); }));
  }

  View fxTabs = chipRow();
  {
    static const char *const fxNames[4] = {"Basic", "Mix", "Grade", "Curve"};
    for (int t = 0; t < 4; ++t)
      fxTabs.addChild(maskChipFn(
          [t]() -> std::string
          {
            std::string name = fxNames[t];
            const Mask *m = selectedMask();
            bool changed = false;
            if (m)
              switch (t)
              {
              case 0:
                changed = m->amount != 100.0f;
                for (float v : m->adj)
                  changed = changed || v != 0.0f;
                break;
              case 1:
                changed = mixAnyNonZero(m->mix);
                break;
              case 2:
                changed = !(m->grade == ColorGrade{});
                break;
              default:
                changed = !(m->curves == identityCurves());
                break;
              }
            if (changed)
              name += " \u2022";
            return name;
          },
          4, [t]
          { state.maskFx = t; },
          [t]
          { return state.maskFx == t; }));
  }
  maskTabEffect.addChild(std::move(fxTabs));
  maskTabEffect.addChild(std::move(fxBasic));
  maskTabEffect.addChild(std::move(fxMix));
  maskTabEffect.addChild(std::move(fxGrade));
  maskTabEffect.addChild(std::move(fxCurve));

  // Tab bar, then the tab bodies, then the always-visible Delete.
  View maskTabs;
  maskTabs.style.direction = FlexDirection::Row;
  maskTabs.style.gap = 4.0f;
  maskTabs.style.flexShrink = 0;
  {
    const char *const tabNames[3] = {"Effect", "Shape", "Parts"};
    for (int t = 0; t < 3; ++t)
      maskTabs.addChild(maskChip(
          tabNames[t], 3, [t]
          { state.maskTab = t; },
          [t]
          { return state.maskTab == t; }));
  }
  maskEditor.addChild(std::move(maskTabs));
  maskEditor.addChild(std::move(maskTabEffect));
  maskEditor.addChild(std::move(maskTabShape));
  maskEditor.addChild(std::move(maskTabParts));
  // Mask list operations. Order matters: masks apply top to bottom.
  View maskOps1 = chipRow();
  maskOps1.addChild(maskChip(
      "Duplicate", 3, duplicateSelectedMask, []
      { return false; }));
  maskOps1.addChild(maskChip(
      "Move Up", 3, []
      { moveSelectedMask(-1); }, []
      { return false; }));
  maskOps1.addChild(maskChip(
      "Move Down", 3, []
      { moveSelectedMask(+1); }, []
      { return false; }));
  maskEditor.addChild(std::move(maskOps1));
  View maskOps2 = chipRow();
  maskOps2.addChild(maskChip(
      "Copy Effects", 2, copyMaskEffects, []
      { return false; }));
  maskOps2.addChild(maskChip(
      "Paste Effects", 2, pasteMaskEffects, []
      { return false; }));
  maskEditor.addChild(std::move(maskOps2));
  maskEditor.addChild(sidebarButton("Delete Mask", deleteSelectedMask));
  // maskEditor is added to the right-hand panel below (after the canvas).

  sidebar.addChild(std::move(maskPane));

  // -- "Crop" pane: aspect-ratio presets, a live readout, and the
  // Apply/Reset/Remove buttons. The selection itself is drawn and dragged
  // directly on the canvas (paintCropOverlay / cropPress / cropDrag).
  View cropPane;
  cropPane.style.width = Size::full();
  cropPane.style.direction = FlexDirection::Column;
  cropPane.style.gap = 6.0f;
  cropPane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == 2 ? Display::Flex : Display::None; });

  Text cropHint;
  cropHint.label = std::function<std::string()>([]() -> std::string
                                                { return state.image
                                                             ? "Drag on the image to choose an area. Drag the handles to "
                                                               "resize, or the inside to move."
                                                             : "Open an image to crop it."; });
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
        kCropAspects[i].label, [i]
        { setCropAspect(i); },
        [i]
        { return state.crop.aspectIdx == i; }));

  Text cropInfo;
  cropInfo.label = std::function<std::string()>([]() -> std::string
                                                {
    if (!state.image)
      return std::string();
    const int w = static_cast<int>(std::lround(state.crop.r - state.crop.l));
    const int h = static_cast<int>(std::lround(state.crop.b - state.crop.t));
    return "Selection: " + std::to_string(w) + " x " + std::to_string(h) +
           " px"; });
  cropInfo.fontSize = 13.0f;
  cropInfo.color = kColorTextMid;
  cropInfo.style.margin = EdgeInsets{8, 0, 4, 0};
  cropPane.addChild(std::move(cropInfo));

  cropPane.addChild(sidebarButton("Apply Crop", applyCrop, {}, true,
                                  []
                                  { return !geometryBusy(); }));
  cropPane.addChild(sidebarButton("Reset Selection", []
                                  {
    resetCropSelection();
    state.markDirty(); }));
  cropPane.addChild(sidebarButton("Remove Crop", removeCrop, {}, false,
                                  []
                                  { return !geometryBusy(); }));

  sidebar.addChild(std::move(cropPane));

  // -- "Rotate" pane: 90-degree turns, mirrors, and a reset. Lossless (pure
  // pixel moves) and each button press is one undo step.
  View rotatePane;
  rotatePane.style.width = Size::full();
  rotatePane.style.direction = FlexDirection::Column;
  rotatePane.style.gap = 6.0f;
  rotatePane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == 3 ? Display::Flex : Display::None; });

  Text rotateHint;
  rotateHint.label = std::function<std::string()>([]() -> std::string
                                                  { return state.image ? "Turn or flip the whole image, or straighten it with "
                                                                         "the angle slider. Straightening clears any crop."
                                                                       : "Open an image to rotate it."; });
  rotateHint.fontSize = 12.0f;
  rotateHint.color = kColorTextMuted;
  rotateHint.style.margin = EdgeInsets{0, 0, 6, 0};
  rotatePane.addChild(std::move(rotateHint));

  const auto geoIdle = []
  { return !geometryBusy(); };
  rotatePane.addChild(sidebarButton(
      "\u21BA  Rotate Left 90\u00B0", []
      { applyGeoOp(GeoOp::RotateCCW); }, {},
      false, geoIdle));
  rotatePane.addChild(sidebarButton(
      "\u21BB  Rotate Right 90\u00B0", []
      { applyGeoOp(GeoOp::RotateCW); }, {},
      false, geoIdle));
  rotatePane.addChild(sidebarButton(
      "\u2194  Flip Horizontal", []
      { applyGeoOp(GeoOp::FlipH); }, {}, false,
      geoIdle));
  rotatePane.addChild(sidebarButton(
      "\u2195  Flip Vertical", []
      { applyGeoOp(GeoOp::FlipV); }, {}, false,
      geoIdle));
  rotatePane.addChild(sidebarButton("Reset Turns & Flips", resetOrientation, {},
                                    false, geoIdle));

  // ---- Straighten: arbitrary angle ----
  Text straightenTitle;
  straightenTitle.label = std::string("Straighten");
  straightenTitle.fontSize = 12.0f;
  straightenTitle.fontWeight = FontWeight::SemiBold;
  straightenTitle.color = kColorTextMuted;
  straightenTitle.style.margin = EdgeInsets{10, 0, 0, 0};
  rotatePane.addChild(std::move(straightenTitle));

  SliderSpec angleSlider;
  angleSlider.name = "Angle";
  angleSlider.lo = -kAngleMax;
  angleSlider.hi = kAngleMax;
  angleSlider.get = []
  { return state.adjust.angle(); };
  angleSlider.set = setAngleFromSlider;
  angleSlider.format = [](float v)
  {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f\u00B0", static_cast<double>(v));
    return std::string(buf);
  };
  angleSlider.isAngle = true;
  rotatePane.addChild(buildSliderRow(angleSlider));

  // Fine adjustment: the track is ~0.6 degrees per pixel, too coarse for
  // levelling a horizon, so step buttons sit right under it.
  auto nudgeBtn = [](const std::string &label, float delta)
  {
    View v;
    v.style.width = Size::pixel(36);
    v.style.height = Size::pixel(26);
    v.style.flexShrink = 0;
    v.style.borderRadius = 4.0f;
    v.style.justifyContent = Justify::Center;
    v.style.alignItems = Align::Center;
    v.style.backgroundColor = kColorActivityBarBg;
    v.style.hoverColor = kColorActivityHover;
    v.onClick = [delta]
    { nudgeAngle(delta); };
    Text t;
    t.label = label;
    t.fontSize = 11.0f;
    t.color = kColorTextDark;
    v.addChild(std::move(t));
    return v;
  };
  View nudgeRow;
  nudgeRow.style.direction = FlexDirection::Row;
  nudgeRow.style.gap = 3.0f;
  nudgeRow.addChild(nudgeBtn("-1\u00B0", -1.0f));
  nudgeRow.addChild(nudgeBtn("-0.1\u00B0", -0.1f));
  nudgeRow.addChild(nudgeBtn("+0.1\u00B0", 0.1f));
  nudgeRow.addChild(nudgeBtn("+1\u00B0", 1.0f));
  rotatePane.addChild(std::move(nudgeRow));

  Text cornersTitle;
  cornersTitle.label = std::string("Corners");
  cornersTitle.fontSize = 12.0f;
  cornersTitle.fontWeight = FontWeight::SemiBold;
  cornersTitle.color = kColorTextMuted;
  cornersTitle.style.margin = EdgeInsets{4, 0, 0, 0};
  rotatePane.addChild(std::move(cornersTitle));

  rotatePane.addChild(sidebarButton(
      "Auto-crop empty corners", []
      { setCornerMode(false); },
      []
      { return !state.adjust.expand(); }));
  rotatePane.addChild(sidebarButton(
      "Keep full canvas", []
      { setCornerMode(true); },
      []
      { return state.adjust.expand(); }));
  rotatePane.addChild(
      sidebarButton("Reset Angle", []
                    { setAngleDiscrete(0.0f); }));

  sidebar.addChild(std::move(rotatePane));

  // -- "Export" pane: format, JPEG quality, metadata, and the Export button.
  // (The File menu's Export... and Ctrl+E use these same settings.)
  View exportPane;
  exportPane.style.width = Size::full();
  exportPane.style.direction = FlexDirection::Column;
  exportPane.style.gap = 6.0f;
  exportPane.style.display = std::function<Display()>(
      []
      { return state.activeActivity == 8 ? Display::Flex : Display::None; });

  Text exportHint;
  exportHint.label = std::function<std::string()>([]() -> std::string
                                                  { return state.image ? "Saves the picture as you see it, with every edit, "
                                                                         "crop and rotation applied."
                                                                       : "Open an image to export it."; });
  exportHint.fontSize = 12.0f;
  exportHint.color = kColorTextMuted;
  exportHint.style.margin = EdgeInsets{0, 0, 6, 0};
  exportPane.addChild(std::move(exportHint));

  exportPane.addChild(sidebarButton(
      "PNG  (lossless)", []
      { state.exportJpeg = false; },
      []
      { return !state.exportJpeg; }));
  exportPane.addChild(sidebarButton(
      "JPEG", []
      { state.exportJpeg = true; },
      []
      { return state.exportJpeg; }));

  View jpegOptions;
  jpegOptions.style.width = Size::full();
  jpegOptions.style.direction = FlexDirection::Column;
  jpegOptions.style.gap = 6.0f;
  jpegOptions.style.margin = EdgeInsets{6, 0, 0, 0};
  jpegOptions.style.display = std::function<Display()>(
      []
      { return state.exportJpeg ? Display::Flex : Display::None; });

  SliderSpec qualitySlider;
  qualitySlider.name = "Quality";
  qualitySlider.lo = 1.0f;
  qualitySlider.hi = 100.0f;
  qualitySlider.get = []
  { return static_cast<float>(state.jpegQuality); };
  qualitySlider.set = [](float v)
  { state.jpegQuality = std::clamp(static_cast<int>(std::lround(v)), 1, 100); };
  qualitySlider.format = [](float v)
  { return std::to_string(static_cast<int>(std::lround(v))); };
  qualitySlider.affectsPixels = false; // a setting, not an edit: no re-render
  jpegOptions.addChild(buildSliderRow(qualitySlider));

  jpegOptions.addChild(sidebarButton(
      "Keep EXIF & color profile", []
      { state.keepMetadata = !state.keepMetadata; },
      []
      { return state.keepMetadata; }));
  exportPane.addChild(std::move(jpegOptions));

  View exportGap;
  exportGap.style.height = Size::pixel(6);
  exportPane.addChild(std::move(exportGap));
  exportPane.addChild(sidebarButton(
      "Export...", exportImage, {}, true, []
      { return state.image != nullptr; }));
  sidebar.addChild(std::move(exportPane));

  sidebarShell.addChild(std::move(sidebar));
  workspace.addChild(std::move(sidebarShell));

  // ---- Vertical divider — drag to resize the sidebar ----
  View vDivider;
  vDivider.style.height = Size::full();
  vDivider.style.flexShrink = 0;
  vDivider.style.backgroundColor = kColorDivider;
  vDivider.style.hoverColor = kColorDividerHover;
  vDivider.style.width = std::function<Size()>([]
                                               { return Size::pixel(state.activeActivity != -1 ? kDividerW : 0.0f); });

  auto updateSidebarWidth = [](float localX)
  {
    float next = state.sidebarWidth + localX - kDividerW / 2.0f;
    state.sidebarWidth = std::clamp(next, kMinSidebarW, kMaxSidebarW);
  };
  vDivider.onPressAt = [updateSidebarWidth](float localX, float)
  {
    updateSidebarWidth(localX);
  };
  vDivider.onDragTo = [updateSidebarWidth](float localX, float)
  {
    updateSidebarWidth(localX);
  };

  workspace.addChild(std::move(vDivider));

  // ---- Center column: canvas viewport on top, filmstrip underneath ----
  // Keeps the filmstrip under the picture only, so the sidebar keeps its
  // full height.
  View centerColumn;
  centerColumn.style.direction = FlexDirection::Column;
  centerColumn.style.height = Size::full();
  centerColumn.style.flexGrow = 1;

  // ---- Scrollable canvas viewport ----
  View viewport;
  viewport.style.width = Size::full();
  viewport.style.flexGrow = 1;
  viewport.style.backgroundColor = kColorViewportBg;
  viewport.style.overflowX = Overflow::Auto;
  viewport.style.overflowY = Overflow::Auto;
  viewport.style.justifyContent = Justify::Center;
  viewport.style.alignItems = Align::Center;
  viewport.style.padding = EdgeInsets::all(24);
  viewport.style.wheelScrollEnabled = false;
  viewport.style.scrollThumbColor = kColorScrollThumb;
  viewport.style.scrollTrackColor = kColorTrack;
  viewport.style.scrollbarAutoHide = true;
  // Only occupies the slot in `workspace` while an image is open; the
  // "no image" placeholder below is workspace's other child for that
  // same slot and shows exactly when this one doesn't.
  viewport.style.display = std::function<Display()>(
      []
      { return state.image ? Display::Flex : Display::None; });

  View canvas;
  canvas.isCanvas = true;
  canvas.style.width = std::function<Size()>(
      []
      { return Size::pixel(state.docWidth() * state.zoom); });
  canvas.style.height = std::function<Size()>(
      []
      { return Size::pixel(state.docHeight() * state.zoom); });
  canvas.style.borderWidth = 1.0f;
  canvas.style.borderColor = kColorCanvasBorder;
  canvas.style.backgroundColor = kColorWhite;

  canvas.onPaint = [](CanvasContext &ctx)
  { paintCanvas(ctx); };
  canvas.canvasDirtySource = []
  {
    // Polled once per frame (see liteui.hpp's checkForUpdates) — the right
    // place to do the deferred, expensive part of a slider drag exactly
    // once per frame, no matter how many onDragTo events fired since the
    // last poll.
    flushPendingAdjust();
    bool d = state.dirty;
    state.dirty = false;
    return d;
  };
  canvas.onHoverAt = [](float lx, float ly)
  { return hoverSample(lx, ly); };
  canvas.onHoverEnd = []
  { return hoverClear(); };
  // Crop-selection dragging (no-ops unless the Crop pane is open).
  // Each is a no-op unless its own pane is open (crop: 2, masks: 7).
  // While comparing, the canvas does nothing but move the split line.
  canvas.onPressAt = [](float lx, float ly)
  {
    if (state.compare != CompareMode::Off)
    {
      if (state.compare == CompareMode::Split)
        splitFromX(lx);
      return;
    }
    cropPress(lx, ly);
    maskPress(lx, ly);
  };

  canvas.onRightPressAt = [](float lx, float ly)
  {
    if (state.compare == CompareMode::Off)
      objectPress(lx, ly, true);
  };
  canvas.onDragTo = [](float lx, float ly)
  {
    if (state.compare != CompareMode::Off)
    {
      if (state.compare == CompareMode::Split)
        splitFromX(lx);
      return;
    }
    cropDrag(lx, ly);
    maskDrag(lx, ly);
  };
  canvas.onDragEnd = []
  {
    state.crop.drag = CropUi::Drag::None;
    if (state.mask.drag != MaskUi::Drag::None)
      maskEndDrag();
  };
  canvas.onScrollUp = []
  { setZoom(state.zoom * kZoomStep); };
  canvas.onScrollDown = []
  { setZoom(state.zoom / kZoomStep); };

  viewport.addChild(std::move(canvas));

  centerColumn.addChild(std::move(viewport));

  View noImagePlaceholder;
  noImagePlaceholder.style.width = Size::full();
  noImagePlaceholder.style.flexGrow = 1;
  noImagePlaceholder.style.backgroundColor = kColorViewportBg;
  noImagePlaceholder.style.justifyContent = Justify::Center;
  noImagePlaceholder.style.alignItems = Align::Center;
  noImagePlaceholder.style.display = std::function<Display()>(
      []
      { return state.image ? Display::None : Display::Flex; });

  Text noImageText;
  noImageText.label = std::string("No image selected");
  noImageText.fontSize = 14.0f;
  noImageText.color = kColorTextFaint;
  noImagePlaceholder.addChild(std::move(noImageText));

  centerColumn.addChild(std::move(noImagePlaceholder));

  // Filmstrip: the open folder's pictures, under the canvas.
  View filmstrip;
  filmstrip.isCanvas = true;
  filmstrip.style.width = Size::full();
  filmstrip.style.flexShrink = 0;
  filmstrip.style.height = std::function<Size()>(
      []
      { return Size::pixel(filmstripShown() ? kFilmH : 0.0f); });
  filmstrip.style.display = std::function<Display()>(
      []
      { return filmstripShown() ? Display::Flex : Display::None; });
  filmstrip.onPaint = [](CanvasContext &ctx)
  { paintFilmstrip(ctx); };
  filmstrip.canvasDirtySource = []
  {
    collectThumbJobs();
    const bool d = state.film.dirty;
    state.film.dirty = false;
    return d;
  };
  filmstrip.onPressAt = [](float lx, float)
  { filmClick(lx); };
  filmstrip.onScrollUp = []
  { filmScrollBy(-2.0f * kFilmCellW); };
  filmstrip.onScrollDown = []
  { filmScrollBy(2.0f * kFilmCellW); };
  centerColumn.addChild(std::move(filmstrip));

  workspace.addChild(std::move(centerColumn));

  // ---- Right-hand panel: the selected mask's controls ----
  // Only on the Masks pane with a mask selected, so every other pane keeps
  // the full canvas width. Drag its divider to resize.
  const auto maskPanelShown = []
  { return maskToolActive() && selMask() >= 0; };

  View rDivider;
  rDivider.style.height = Size::full();
  rDivider.style.flexShrink = 0;
  rDivider.style.backgroundColor = kColorDivider;
  rDivider.style.hoverColor = kColorDividerHover;
  rDivider.style.width = Size::pixel(kDividerW);
  rDivider.style.display = std::function<Display()>([maskPanelShown]() -> Display
                                                    { return maskPanelShown() ? Display::Flex : Display::None; });
  const auto updateMaskPanelWidth = [](float localX)
  {
    // The panel sits to the right, so it grows as the divider moves LEFT.
    const float next = state.maskPanelW - (localX - kDividerW / 2.0f);
    state.maskPanelW = std::clamp(next, kMinMaskPanelW, kMaxMaskPanelW);
  };
  rDivider.onPressAt = [updateMaskPanelWidth](float localX, float)
  { updateMaskPanelWidth(localX); };
  rDivider.onDragTo = [updateMaskPanelWidth](float localX, float)
  { updateMaskPanelWidth(localX); };
  workspace.addChild(std::move(rDivider));

  View maskPanel;
  maskPanel.style.height = Size::full();
  maskPanel.style.flexShrink = 0;
  maskPanel.style.direction = FlexDirection::Column;
  maskPanel.style.backgroundColor = kColorPanelBg;
  maskPanel.style.padding = EdgeInsets::all(kMaskPanelPad);
  maskPanel.style.overflowX = Overflow::Hidden;
  maskPanel.style.overflowY = Overflow::Visible;
  maskPanel.style.width = std::function<Size()>([]
                                                { return Size::pixel(state.maskPanelW); });
  maskPanel.style.display = std::function<Display()>([maskPanelShown]() -> Display
                                                     { return maskPanelShown() ? Display::Flex : Display::None; });

  Text maskPanelTitle;
  maskPanelTitle.label = std::function<std::string()>([]() -> std::string
                                                      {
    const Mask *m = selectedMask();
    if (!m)
      return std::string();
    return "MASK " + std::to_string(selMask() + 1) + "  " + maskLabel(*m); });
  maskPanelTitle.fontSize = 12.0f;
  maskPanelTitle.fontWeight = FontWeight::SemiBold;
  maskPanelTitle.color = kColorTextMuted;
  maskPanelTitle.style.margin = EdgeInsets{0, 0, 10, 0};
  maskPanelTitle.style.flexShrink = 0;
  maskPanel.addChild(std::move(maskPanelTitle));

  // Scrolling body, like the left sidebar's.
  View maskPanelBody;
  maskPanelBody.style.width = Size::full();
  maskPanelBody.style.flexGrow = 1;
  maskPanelBody.style.minHeight = 0;
  maskPanelBody.style.direction = FlexDirection::Column;
  maskPanelBody.style.overflowX = Overflow::Hidden;
  maskPanelBody.style.overflowY = Overflow::Auto;
  maskPanelBody.addChild(std::move(maskEditor));
  maskPanel.addChild(std::move(maskPanelBody));
  workspace.addChild(std::move(maskPanel));

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
  statusLeft.label = std::function<std::string()>([]
                                                  {
    std::string s = state.image
                        ? baseName(state.imagePath) + "  —  " +
                              std::to_string(state.image->width) + " x " +
                              std::to_string(state.image->height) + " px"
                        : std::string("No image open");
    if (state.image && state.compare == CompareMode::Before)
      s += "     •     Before (press \\ to return)";
    else if (state.image && state.compare == CompareMode::Split)
      s += "     •     Before | After (Y to close, drag to move the line)";
    if (!state.statusMsg.empty())
      s += "     •     " + state.statusMsg;
    return s; });
  statusLeft.fontSize = 12.0f;
  statusLeft.wrap = TextWrap::NoWrap; // one line, even with a long message
  statusLeft.color = std::function<Color()>(
      []
      { return state.statusIsError ? kColorStatusError : kColorWhite; });
  statusBar.addChild(std::move(statusLeft));

  // Right side of the status bar: zoom controls.
  auto statusGlyphBtn = [](const std::string &label,
                           std::function<void()> onClick)
  {
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

  // RGB under the cursor: swatch + values. Fixed width so the zoom controls
  // to its right never shift as the numbers change.
  View rgbReadout;
  rgbReadout.style.width = Size::pixel(176);
  rgbReadout.style.direction = FlexDirection::Row;
  rgbReadout.style.alignItems = Align::Center;
  rgbReadout.style.gap = 6.0f;
  rgbReadout.style.margin = EdgeInsets{0, 14, 0, 0};

  View rgbSwatch;
  rgbSwatch.style.width = Size::pixel(12);
  rgbSwatch.style.height = Size::pixel(12);
  rgbSwatch.style.flexShrink = 0;
  rgbSwatch.style.borderRadius = 2.0f;
  rgbSwatch.style.backgroundColor = std::function<Color()>([]() -> Color
                                                           { return state.hoverValid
                                                                        ? Color{state.hoverR, state.hoverG, state.hoverB, 255}
                                                                        : Color{0, 0, 0, 0}; });
  rgbReadout.addChild(std::move(rgbSwatch));

  Text rgbText;
  rgbText.label = std::function<std::string()>([]
                                               {
    if (!state.hoverValid)
      return std::string();
    return "R " + std::to_string(state.hoverR) + "  G " +
           std::to_string(state.hoverG) + "  B " +
           std::to_string(state.hoverB); });
  rgbText.fontSize = 12.0f;
  rgbText.wrap = TextWrap::NoWrap;
  rgbText.color = kColorWhite;
  rgbReadout.addChild(std::move(rgbText));
  zoomControls.addChild(std::move(rgbReadout));

  zoomControls.addChild(
      statusGlyphBtn("-", []
                     { setZoom(state.zoom / kZoomStep); }));

  Text zoomLabel;
  zoomLabel.label = []
  {
    return std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
  };
  zoomLabel.fontSize = 12.0f;
  zoomLabel.color = kColorWhite;
  zoomLabel.style.margin = EdgeInsets{0, 4, 0, 4};
  zoomControls.addChild(std::move(zoomLabel));

  zoomControls.addChild(
      statusGlyphBtn("+", []
                     { setZoom(state.zoom * kZoomStep); }));

  View zoomResetSpacer;
  zoomResetSpacer.style.width = Size::pixel(6);
  zoomControls.addChild(std::move(zoomResetSpacer));

  View zoomResetBtn;
  zoomResetBtn.style.justifyContent = Justify::Center;
  zoomResetBtn.style.alignItems = Align::Center;
  zoomResetBtn.style.hoverColor = kColorStatusHover;
  zoomResetBtn.style.borderRadius = 3.0f;
  zoomResetBtn.style.padding = EdgeInsets{2, 6, 2, 6};
  zoomResetBtn.onClick = []
  { setZoom(1.0f); };
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
  flushSidecar();            // edits made since the last autosave
  renderWorker.shutdown();   // join the worker before `ui` goes out of scope
  geometryWorker.shutdown(); // ditto
  decodeWorker.shutdown();   // ditto (waits out a decode still running)
  thumbWorker.shutdown();    // ditto
  segmentWorker.shutdown();
  return 0;
}
catch (const std::exception &e)
{
  std::fprintf(stderr, "literoom: fatal: %s\n", e.what());
  MessageBoxA(nullptr, e.what(), "literoom", MB_ICONERROR);
  return 1;
}