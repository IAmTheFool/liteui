// adjustment_set.hpp - AdjustmentSet: the live value of every edit, its undo / redo history
// and the labels the History panel shows, the JSON for the sidecar, and
// the RenderParams built from it. No UI and no pixels.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "color/color_grade.hpp"
#include "color/color_mix.hpp"
#include "color/color_pass.hpp"
#include "color/curves.hpp"
#include "core/adjustments.hpp"
#include "core/geometry.hpp"
#include "detail/neighbor_fx.hpp"
#include "edit/render_params.hpp"
#include "io/minijson.hpp"
#include "io/preset.hpp"
#include "io/sidecar.hpp"
#include "mask/mask.hpp"

// Undo history: at most this many steps can be undone; older ones are
// dropped as new edits come in. (A drag on one slider is a single step.)
constexpr size_t kMaxUndoSteps = 200;

// Owns the live value of every slider, the committed crop and orientation,
// their undo/redo history, and the logic to freeze them into RenderParams
// (renderAdjusted in render.hpp turns those into pixels).
// A slider drags continuously (many `set()` calls per second while held)
// with no drag-end event from LiteUI to mark "the gesture is over" — so
// `set` coalesces repeated calls for the *same* slider into one history
// entry instead of growing it every frame; touching a different slider (or
// an undo/redo, or a crop) starts a fresh entry. Dragging Exposure from -20
// to +40 is one undo step, not hundreds. A crop is always exactly one step.
class AdjustmentSet
{
public:
  float get(AdjId id) const { return values_[static_cast<size_t>(id)]; }
  const CropRect &crop() const { return crop_; }
  const Orient &orient() const { return orient_; }
  float angle() const { return angle_; }  // straighten angle, degrees CW
  bool expand() const { return expand_; } // straighten: keep the full canvas?
  // One of the four tone curves (default: the master RGB curve).
  const CurvePoints &curve(CurveChannel ch = CurveChannel::Master) const
  {
    return curves_[static_cast<size_t>(ch)];
  }

  void set(AdjId id, float value)
  {
    values_[static_cast<size_t>(id)] = value;
    if (id == lastEdited_ && historyIndex_ + 1 == history_.size())
    {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
    }
    else
    {
      appendSnapshot();
      lastEdited_ = id;
    }
  }

  // Commits a new crop (in original-image coordinates; an empty CropRect
  // removes the crop). Always its own undo step.
  void setCrop(const CropRect &c)
  {
    if (c == crop_)
      return;
    crop_ = c;
    appendSnapshot();
    lastEdited_ = AdjId::Count; // never coalesce a slider drag into a crop
  }

  // Commits a new orientation and straighten angle together with the crop
  // rectangle remapped into them (the crop is stored relative to the
  // oriented, straightened image, so all three must change together). One
  // undo step.
  void setGeometry(const Orient &o, float angle, const CropRect &c)
  {
    if (o == orient_ && angle == angle_ && c == crop_)
      return;
    orient_ = o;
    angle_ = angle;
    crop_ = c;
    pushStep();
  }

  // Sets the straighten angle. Changing it changes the size of the picture,
  // so any committed crop is dropped (undo brings it back). `coalesce` = true
  // for a slider drag (many calls per second): repeated calls fold into one
  // undo step. false for a discrete action (a nudge button, reset).
  void setAngle(float deg, bool coalesce)
  {
    if (deg == angle_)
      return;
    angle_ = deg;
    crop_ = CropRect{};
    if (coalesce && lastEdited_ == kEditAngle &&
        historyIndex_ + 1 == history_.size())
    {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
      return;
    }
    pushStep();
    lastEdited_ = coalesce ? kEditAngle : AdjId::Count;
  }

  // Straighten's corner handling: false = auto-crop away the empty corners,
  // true = grow the canvas so nothing is lost (corners transparent). Only
  // affects the picture when the angle is non-zero, but is always its own
  // undo step so undo/redo stays predictable.
  void setExpand(bool expand)
  {
    if (expand == expand_)
      return;
    expand_ = expand;
    if (angle_ != 0.0f)
      crop_ = CropRect{}; // the picture just changed size
    pushStep();
  }

  // Sets one tone curve. `coalesce` = true while a point is being
  // added/dragged (many calls per second): repeated calls to the SAME channel
  // fold into ONE undo step, until endCurveEdit() closes it. false for a
  // discrete action (delete a point, reset the curve).
  void setCurve(CurveChannel ch, const CurvePoints &c, bool coalesce)
  {
    CurvePoints &cur = curves_[static_cast<size_t>(ch)];
    if (c == cur)
      return;
    cur = c;
    if (coalesce && lastEdited_ == kEditCurve && lastCurveCh_ == ch &&
        historyIndex_ + 1 == history_.size())
    {
      history_[historyIndex_] = snapshot(); // still the same gesture
      return;
    }
    pushStep();
    lastEdited_ = coalesce ? kEditCurve : AdjId::Count;
    lastCurveCh_ = ch;
  }
  // Master-curve shorthand.
  void setCurve(const CurvePoints &c, bool coalesce)
  {
    setCurve(CurveChannel::Master, c, coalesce);
  }
  // Resets all four curves to straight lines as ONE undo step (no-op if they
  // already are).
  void resetAllCurves()
  {
    const CurvePoints id = identityCurve();
    bool any = false;
    for (const auto &c : curves_)
      any = any || !(c == id);
    if (!any)
      return;
    curves_ = identityCurves();
    pushStep();
  }
  // The point drag ended: the next curve edit starts a fresh undo step.
  void endCurveEdit()
  {
    if (lastEdited_ == kEditCurve)
      lastEdited_ = AdjId::Count;
  }

  // ---- Color Mixer ----
  float mix(MixChan c, size_t band) const { return mix_[mixIndex(c, band)]; }
  const MixValues &mixValues() const { return mix_; }
  // Sets one Color Mixer slider. Like set(): repeated calls for the SAME
  // slider (a drag) fold into one undo step; touching another slider, or any
  // other edit / undo / redo, starts a fresh one.
  void setMix(MixChan c, size_t band, float value)
  {
    const size_t idx = mixIndex(c, band);
    if (value == mix_[idx])
      return;
    mix_[idx] = value;
    if (lastEdited_ == kEditMix && lastMixIdx_ == idx &&
        historyIndex_ + 1 == history_.size())
    {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
      return;
    }
    pushStep();
    lastEdited_ = kEditMix;
    lastMixIdx_ = idx;
  }
  // Zeroes all 24 Color Mixer sliders as ONE undo step (no-op if already 0).
  void resetMix()
  {
    if (!mixAnyNonZero(mix_))
      return;
    mix_.fill(0.0f);
    pushStep();
  }

  // ---- Color Grading ----
  const ColorGrade &grade() const { return grade_; }
  // Commits a new grade. `key` identifies the control being dragged: repeated
  // calls with the SAME key fold into one undo step (until endGradeEdit()).
  // key < 0 = a discrete action (eye toggle, reset): always its own step.
  void setGrade(const ColorGrade &g, int key)
  {
    if (g == grade_)
      return;
    grade_ = g;
    if (key >= 0 && lastEdited_ == kEditGrade && lastGradeKey_ == key &&
        historyIndex_ + 1 == history_.size())
    {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
      return;
    }
    pushStep();
    if (key >= 0)
    {
      lastEdited_ = kEditGrade;
      lastGradeKey_ = key;
    }
  }
  // The drag ended: the next grading edit starts a fresh undo step.
  void endGradeEdit()
  {
    if (lastEdited_ == kEditGrade)
      lastEdited_ = AdjId::Count;
  }

  // ---- Masks ----
  // The untouched picture's size: masks are stored against it, and mapped
  // into the current orient / straighten / crop through geoFrame(). Set once
  // per opened file, after reset().
  void setOriginalSize(int w, int h)
  {
    origW_ = w;
    origH_ = h;
  }
  GeoFrame geoFrame() const
  {
    return makeGeoFrame(origW_, origH_, orient_, angle_, expand_, crop_);
  }
  const MaskSet &masks() const { return masks_; }

  // Bumped by every change to the edit state (an edit, undo, redo, reset).
  // The app compares it with its value at the last save / load to know
  // whether the sidecar file is stale. It is not part of the undo history.
  uint64_t version() const { return version_; }

  // Everything the user has done, as JSON for the sidecar file (the undo
  // history is not included).
  mj::Json toJson() const { return sidecarEditsToJson(currentEdits()); }
  // The whole edit state as a plain value: what toJson writes, and what a named
  // snapshot stores. Does not touch the history or version().
  SidecarEdits currentEdits() const
  {
    return SidecarEdits{values_, crop_, orient_, angle_, expand_,
                        curves_, mix_, grade_, masks_};
  }
  // Replaces every edit with the saved ones in `j` (call after reset() and
  // setOriginalSize()); undo history restarts from them. False, with nothing
  // changed, if `j` isn't usable.
  bool loadJson(const mj::Json &j)
  {
    SidecarEdits e;
    if (!sidecarEditsFromJson(j, origW_, origH_, &e))
      return false;
    values_ = e.values;
    crop_ = e.crop;
    orient_ = e.orient;
    angle_ = e.angle;
    expand_ = e.expand;
    curves_ = e.curves;
    mix_ = e.mix;
    grade_ = e.grade;
    masks_ = std::move(e.masks);
    history_ = {snapshot()};
    historyIndex_ = 0;
    lastEdited_ = AdjId::Count;
    return true;
  }
  // Commits a new mask list (add, delete, move, edit a slider...). `key`
  // identifies the control being dragged, as in setGrade: repeated calls with
  // the SAME key fold into one undo step (until endMaskEdit()); key < 0 = a
  // discrete action (add, delete, toggle): always its own step. Callers pack
  // the mask index into the key so two masks never share a step.
  void setMasks(const MaskSet &m, int key)
  {
    if (m == masks_)
      return;
    masks_ = m;
    if (key >= 0 && lastEdited_ == kEditMask && lastMaskKey_ == key &&
        historyIndex_ + 1 == history_.size())
    {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
      return;
    }
    pushStep();
    if (key >= 0)
    {
      lastEdited_ = kEditMask;
      lastMaskKey_ = key;
    }
  }
  // The drag ended: the next mask edit starts a fresh undo step.
  void endMaskEdit()
  {
    if (lastEdited_ == kEditMask)
      lastEdited_ = AdjId::Count;
  }

  // ---- Presets ----
  // The current look (sliders, curves, Color Mixer, Color Grading) as a Preset.
  // Crop, orientation, straighten and masks are not part of a preset.
  Preset currentPreset(const std::string &name) const
  {
    Preset p;
    p.name = sanitizePresetName(name);
    p.values = values_;
    p.curves = curves_;
    p.mix = mix_;
    p.grade = grade_;
    return p;
  }
  // Replaces the whole look with `p` as ONE undo step labelled "Preset: <name>".
  // Crop, orientation, straighten and masks are left alone. Returns false (and
  // adds no step) if the look is already identical. Values are sanitized again
  // here, so a Preset built by hand can't put an out-of-range slider in.
  bool applyPreset(const Preset &p)
  {
    AdjValues v = p.values;
    for (const auto &spec : kAdjustmentSpecs)
    {
      const size_t k = static_cast<size_t>(spec.id);
      v[k] = sanitizedValue(spec, v[k]);
    }
    if (v == values_ && p.curves == curves_ && p.mix == mix_ && p.grade == grade_)
      return false;
    values_ = v;
    curves_ = p.curves;
    mix_ = p.mix;
    grade_ = p.grade;
    pushStep();
    const std::string n = sanitizePresetName(p.name);
    history_.back().label = n.empty() ? std::string("Preset") : "Preset: " + n;
    return true;
  }

  // ---- Named snapshots ----
  // Puts the picture back exactly as `e` describes it (every slider, curve, the
  // Mixer, Color Grading, crop, rotation, straighten, corner mode and all masks)
  // as ONE undo step labelled "Snapshot: <name>" (or "Snapshot" for a blank
  // name). Unlike applyPreset this replaces the geometry and the masks too, so
  // the caller must refresh the picture the way it does after an undo. Returns
  // false, adding no step, if the picture already looks exactly like `e`.
  // Slider values are sanitized again; everything else is taken as given, so `e`
  // must come from currentEdits() or sidecarSnapshotsFromJson (which checks the
  // crop against the picture's size). Ends any drag in progress, like
  // applyPreset.
  bool restoreEdits(const SidecarEdits &e, const std::string &name)
  {
    AdjValues v = e.values;
    for (const auto &spec : kAdjustmentSpecs)
    {
      const size_t k = static_cast<size_t>(spec.id);
      v[k] = sanitizedValue(spec, v[k]);
    }
    if (v == values_ && e.crop == crop_ && e.orient == orient_ &&
        e.angle == angle_ && e.expand == expand_ && e.curves == curves_ &&
        e.mix == mix_ && e.grade == grade_ && e.masks == masks_)
      return false;
    values_ = v;
    crop_ = e.crop;
    orient_ = e.orient;
    angle_ = e.angle;
    expand_ = e.expand;
    curves_ = e.curves;
    mix_ = e.mix;
    grade_ = e.grade;
    masks_ = e.masks;
    pushStep();
    const std::string n = sanitizeSnapshotName(name);
    history_.back().label = n.empty() ? std::string("Snapshot") : "Snapshot: " + n;
    return true;
  }

  // ---- White balance picker ----
  // Sets Temperature and Tint together as ONE undo step labelled
  // "White Balance +12 / -4" (two set() calls would be two steps). Values are
  // sanitized like a preset's. Returns false, adding no step, if both already
  // have these values. Ends any drag in progress, like applyPreset.
  bool setWhiteBalance(float temperature, float tint)
  {
    const float t = sanitizedValue(kAdjustmentSpecs[adjIndex(AdjId::Temperature)], temperature);
    const float n = sanitizedValue(kAdjustmentSpecs[adjIndex(AdjId::Tint)], tint);
    if (t == get(AdjId::Temperature) && n == get(AdjId::Tint))
      return false;
    values_[adjIndex(AdjId::Temperature)] = t;
    values_[adjIndex(AdjId::Tint)] = n;
    pushStep();
    history_.back().label = "White Balance " + signedNum(t) + " / " + signedNum(n);
    return true;
  }

  // ---- Auto Tone ----
  // Sets Exposure, Contrast, Whites and Blacks together as ONE undo step
  // labelled "Auto Tone" (four set() calls would be four steps). Only those four
  // sliders move: Brightness, Highlights, Shadows, the curves and everything
  // else are left alone. Values are sanitized like a preset's. Returns false,
  // adding no step, if all four already have these values. Ends any drag in
  // progress, like applyPreset. The numbers come from solveAutoTone
  // (core/auto_tone.hpp).
  bool setAutoTone(float exposure, float contrast, float whites, float blacks)
  {
    const AdjId ids[4] = {AdjId::Exposure, AdjId::Contrast, AdjId::Whites,
                          AdjId::Blacks};
    const float in[4] = {exposure, contrast, whites, blacks};
    float v[4];
    bool changed = false;
    for (int i = 0; i < 4; ++i)
    {
      v[i] = sanitizedValue(kAdjustmentSpecs[adjIndex(ids[i])], in[i]);
      changed = changed || v[i] != get(ids[i]);
    }
    if (!changed)
      return false;
    for (int i = 0; i < 4; ++i)
      values_[adjIndex(ids[i])] = v[i];
    pushStep();
    history_.back().label = "Auto Tone";
    return true;
  }

  // Fresh baseline for a newly opened image: every slider back to 0, no
  // crop, no rotation/flip, no history to undo into.
  void reset()
  {
    values_.fill(0.0f);
    mix_.fill(0.0f);
    grade_ = ColorGrade{};
    masks_.clear();
    crop_ = CropRect{};
    orient_ = Orient{};
    angle_ = 0.0f;
    expand_ = false;
    curves_ = identityCurves();
    history_ = {snapshot()};
    historyIndex_ = 0;
    lastEdited_ = AdjId::Count; // sentinel: no in-progress edit
  }

  bool canUndo() const { return historyIndex_ > 0; }
  bool canRedo() const { return historyIndex_ + 1 < history_.size(); }

  void undo()
  {
    if (!canUndo())
      return;
    restore(history_[--historyIndex_]);
    lastEdited_ = AdjId::Count; // next slider touch starts a new entry
  }
  void redo()
  {
    if (!canRedo())
      return;
    restore(history_[++historyIndex_]);
    lastEdited_ = AdjId::Count;
  }

  // ---- History panel ----
  size_t historySize() const { return history_.size(); }
  size_t historyIndex() const { return historyIndex_; }
  // What step `i` changed compared with step i-1 ("Original" for step 0).
  // Cached per version(), so polling it every frame is cheap.
  const std::string &historyLabel(size_t i) const
  {
    refreshLabels();
    static const std::string none;
    return i < labels_.size() ? labels_[i] : none;
  }
  // Jumps to step `i`: the same as pressing Undo / Redo until you get there.
  void goToHistory(size_t i)
  {
    if (i >= history_.size() || i == historyIndex_)
      return;
    historyIndex_ = i;
    restore(history_[i]);
    lastEdited_ = AdjId::Count; // next slider touch starts a new entry
  }

  // Composes every non-zero adjustment's pointwise transform into a single
  // 256-entry lookup table, applied identically to R/G/B. Building the
  // table is O(256) regardless of image size — the expensive part of a
  // slider drag is applying it, and that's now one array lookup per
  // channel instead of a float multiply/add + lround + clamp per spec per
  // channel.
  std::array<uint8_t, 256> buildLut() const
  {
    // The master curve runs after the tone sliders (Lightroom's order). It's
    // skipped outright when it rounds to identity, so the untouched-curve
    // result stays bit-identical to before the curve existed.
    const auto curveLut = curveToLut(curves_[0]);
    const bool useCurve = !curveIsIdentity(curveLut);
    return toneLutFromValues(values_, useCurve ? &curveLut : nullptr);
  }

  // Per-channel R/G/B tables: buildLut() (tone sliders + master curve), then
  // each channel's own curve on top. A channel whose curve is a straight line
  // just reuses the shared table, so with no per-channel curves all three
  // tables are identical (see renderWith's fast path).
  std::array<std::array<uint8_t, 256>, 3> buildChannelLuts() const
  {
    const auto base = buildLut();
    std::array<std::array<uint8_t, 256>, 3> out{};
    for (size_t c = 0; c < 3; ++c)
    {
      const auto cl = curveToLut(curves_[1 + c]);
      if (curveIsIdentity(cl))
      {
        out[c] = base;
        continue;
      }
      for (int i = 0; i < 256; ++i)
        out[c][i] = static_cast<uint8_t>(
            std::clamp(static_cast<int>(std::lround(cl[base[i]])), 0, 255));
    }
    return out;
  }

  // Freezes the current slider/curve state into a self-contained value that
  // another thread can use (see RenderParams).
  RenderParams buildRenderParams() const
  {
    RenderParams rp;
    rp.luts = buildChannelLuts();
    rp.color = buildColorPass();
    rp.fx = neighbor_fx::gainsFromValues(get(AdjId::Texture), get(AdjId::Clarity),
                                         get(AdjId::Sharpness), get(AdjId::Dehaze),
                                         get(AdjId::NoiseLuma),
                                         get(AdjId::NoiseColor),
                                         get(AdjId::Blur));
    if (!masks_.empty())
      rp.masks = buildMaskPasses(masks_, geoFrame());
    rp.film = filmFromAdj(values_); // Vignette / Grain: global, after the masks
    return rp;
  }

  ColorPass buildColorPass() const
  {
    return makeColorPass(get(AdjId::Temperature), get(AdjId::Tint),
                         get(AdjId::Vibrance), get(AdjId::Saturation), mix_,
                         grade_, get(AdjId::Hue), get(AdjId::ColorizeHue),
                         get(AdjId::ColorizeAmount),
                         filterValuesFromAdj(values_));
  }

private:
  // Everything one undo step restores: slider values, crop, orientation,
  // straighten angle and corner mode.
  struct Snapshot
  {
    std::array<float, static_cast<size_t>(AdjId::Count)> values{};
    CropRect crop;
    Orient orient;
    float angle = 0.0f;
    bool expand = false;
    CurveSet curves = identityCurves();
    MixValues mix{};
    ColorGrade grade;
    MaskSet masks;
    // Non-empty only for steps that carry their own History label (a preset,
    // a white-balance pick, an Auto Tone, a snapshot restore);
    // every other step is labelled by diffing it against the one before.
    std::string label;
  };
  Snapshot snapshot() const
  {
    ++version_; // every state change passes through here (or restore)
    return {values_, crop_, orient_, angle_, expand_,
            curves_, mix_, grade_, masks_};
  }
  void restore(const Snapshot &s)
  {
    ++version_;
    values_ = s.values;
    crop_ = s.crop;
    orient_ = s.orient;
    angle_ = s.angle;
    expand_ = s.expand;
    curves_ = s.curves;
    mix_ = s.mix;
    grade_ = s.grade;
    masks_ = s.masks;
  }

  // ---- History panel labels ----
  // A step is described by diffing it against the one before, so the
  // setters don't need to know about labels, and a coalesced drag that
  // overwrites a step can't leave a stale one behind.
  static std::string signedNum(float v)
  {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 0.05f)
      std::snprintf(buf, sizeof buf, "%+d", static_cast<int>(std::lround(v)));
    else
      std::snprintf(buf, sizeof buf, "%+.1f", v);
    return buf;
  }
  static std::string describeStep(const Snapshot &a, const Snapshot &b)
  {
    if (!b.label.empty())
      return b.label;
    for (const auto &s : kAdjustmentSpecs)
    {
      const size_t k = static_cast<size_t>(s.id);
      if (a.values[k] != b.values[k])
        return std::string(s.label) + " " + signedNum(b.values[k]);
    }
    if (!(a.orient == b.orient))
      return "Rotate / Flip";
    if (a.angle != b.angle)
    {
      char buf[40];
      std::snprintf(buf, sizeof buf, "Straighten %+.1f\xC2\xB0", b.angle);
      return buf;
    }
    if (a.expand != b.expand)
      return b.expand ? "Straighten: Keep Canvas" : "Straighten: Auto-Crop";
    if (!(a.crop == b.crop))
      return b.crop.empty() ? "Remove Crop" : "Crop";
    static const char *const chn[kCurveChannelCount] = {"RGB", "Red", "Green", "Blue"};
    for (size_t c = 0; c < kCurveChannelCount; ++c)
      if (!(a.curves[c] == b.curves[c]))
        return std::string("Tone Curve: ") + chn[c];
    int mixDiffs = 0;
    size_t mixIdx = 0;
    for (size_t k = 0; k < b.mix.size(); ++k)
      if (a.mix[k] != b.mix[k])
      {
        if (!mixDiffs)
          mixIdx = k;
        ++mixDiffs;
      }
    if (mixDiffs > 1)
      return "Color Mix: Reset";
    if (mixDiffs == 1)
    {
      static const char *const cn[kMixChanCount] = {"Hue", "Saturation", "Luminance"};
      return std::string("Color Mix: ") + mixBandName(mixIdx % kMixBandCount) +
             " " + cn[mixIdx / kMixBandCount] + " " + signedNum(b.mix[mixIdx]);
    }
    if (!(a.grade == b.grade))
    {
      static const char *const rn[kGradeRangeCount] = {"Shadows", "Midtones", "Highlights", "Global"};
      for (size_t w = 0; w < kGradeRangeCount; ++w)
        if (!(a.grade.wheel[w] == b.grade.wheel[w]))
          return std::string("Color Grading: ") + rn[w];
      return a.grade.blending != b.grade.blending ? "Color Grading: Blending"
                                                  : "Color Grading: Balance";
    }
    if (b.masks.size() > a.masks.size())
      return std::string("Add ") + maskTypeName(b.masks.back().type()) + " Mask";
    if (b.masks.size() < a.masks.size())
      return "Delete Mask";
    // Reorder: exactly two positions differ and each mask now sits where the
    // other was (moveSelectedMask swaps neighbors). Checked before the
    // per-mask loop, which would blame the first differing mask's sliders.
    {
      size_t d[3] = {0, 0, 0};
      size_t nd = 0;
      for (size_t m = 0; m < b.masks.size() && nd < 3; ++m)
        if (!(a.masks[m] == b.masks[m]))
          d[nd++] = m;
      if (nd == 2 && a.masks[d[0]] == b.masks[d[1]] &&
          a.masks[d[1]] == b.masks[d[0]])
        return "Reorder Masks";
    }
    for (size_t m = 0; m < b.masks.size(); ++m)
    {
      const Mask &x = a.masks[m], &y = b.masks[m];
      if (x == y)
        continue;
      const std::string n = "Mask " + std::to_string(m + 1) + ": ";
      if (x.name != y.name)
        return n + "Rename";
      for (const auto &s : kAdjustmentSpecs)
      {
        const size_t k = static_cast<size_t>(s.id);
        if (x.adj[k] != y.adj[k])
          return n + std::string(s.label) + " " + signedNum(y.adj[k]);
      }
      if (x.amount != y.amount)
        return n + "Amount " + std::to_string(static_cast<int>(std::lround(y.amount)));
      if (!(x.mix == y.mix))
        return n + "Color Mix";
      if (!(x.grade == y.grade))
        return n + "Color Grading";
      if (!(x.curves == y.curves))
        return n + "Curve";
      if (x.enabled != y.enabled)
        return n + (y.enabled ? "On" : "Off");
      if (x.invert != y.invert)
        return n + "Invert";
      if (x.parts.size() != y.parts.size())
        return n + "Parts";
      return n + "Edit";
    }
    return "Edit";
  }
  // Rebuilds the label cache when anything changed since it was built.
  // version_ moves on every snapshot / restore, so this covers edits,
  // undo / redo, jumps, reset and trimming of the oldest steps.
  void refreshLabels() const
  {
    if (labelsVersion_ == version_ && labels_.size() == history_.size())
      return;
    labels_.resize(history_.size());
    for (size_t i = 0; i < history_.size(); ++i)
      labels_[i] = i == 0 ? std::string("Original")
                          : describeStep(history_[i - 1], history_[i]);
    labelsVersion_ = version_;
  }
  // The single place a new undo step is added: drops any redo tail, appends
  // the current state, and forgets the oldest steps beyond kMaxUndoSteps
  // (history_ holds the baseline plus at most that many steps). Leaves
  // lastEdited_ to the caller, since slider drags set it and everything
  // else clears it.
  void appendSnapshot()
  {
    history_.resize(historyIndex_ + 1); // discard any redo tail
    history_.push_back(snapshot());
    ++historyIndex_;
    if (history_.size() > kMaxUndoSteps + 1)
    {
      const size_t drop = history_.size() - (kMaxUndoSteps + 1);
      history_.erase(history_.begin(),
                     history_.begin() + static_cast<std::ptrdiff_t>(drop));
      historyIndex_ -= drop;
    }
  }
  // Appends the current state as a new undo step (dropping any redo tail)
  // and ends any in-progress slider/angle drag coalescing.
  void pushStep()
  {
    appendSnapshot();
    lastEdited_ = AdjId::Count;
  }

  // Pseudo-AdjId for `lastEdited_` marking an in-progress angle drag (the
  // angle isn't one of the pointwise sliders in kAdjustmentSpecs).
  static constexpr AdjId kEditAngle =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 1);
  // ...and an in-progress tone-curve point drag.
  static constexpr AdjId kEditCurve =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 2);
  // ...and an in-progress Color Mixer slider drag (which one: lastMixIdx_).
  static constexpr AdjId kEditMix =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 3);

  // ...and an in-progress Color Grading drag (which control: lastGradeKey_).
  static constexpr AdjId kEditGrade =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 4);
  // ...and an in-progress mask edit (which control: lastMaskKey_).
  static constexpr AdjId kEditMask =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 5);

  std::array<float, static_cast<size_t>(AdjId::Count)> values_{};
  CropRect crop_;
  Orient orient_;
  float angle_ = 0.0f;
  bool expand_ = false;
  CurveSet curves_ = identityCurves();
  CurveChannel lastCurveCh_ =
      CurveChannel::Master; // channel of the open curve drag
  MixValues mix_{};
  size_t lastMixIdx_ = 0; // slider of the open Color Mixer drag
  ColorGrade grade_;
  int lastGradeKey_ = -1; // control of the open Color Grading drag
  MaskSet masks_;
  int lastMaskKey_ = -1;      // control of the open mask drag
  int origW_ = 0, origH_ = 0; // see setOriginalSize
  std::vector<Snapshot> history_{Snapshot{}};
  size_t historyIndex_ = 0;
  mutable uint64_t version_ = 0;
  mutable std::vector<std::string> labels_; // historyLabel() cache
  mutable uint64_t labelsVersion_ = ~uint64_t{0};
  AdjId lastEdited_ = AdjId::Count;
};