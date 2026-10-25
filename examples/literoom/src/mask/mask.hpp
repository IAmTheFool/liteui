// mask.hpp - Masks: one alternative per kind (MaskShape / MaskEval), how a mask's
// parts combine (Add / Subtract / Intersect), and Mask / MaskSet, the
// user-facing list. Includes every shape header.
#pragma once
#include <array>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "core/adjustments.hpp"
#include "color/color_grade.hpp"
#include "color/color_mix.hpp"
#include "color/curves.hpp"
#include "core/geometry.hpp"
#include "mask/mask_brush.hpp"
#include "mask/mask_geom.hpp"
#include "mask/mask_object.hpp"
#include "mask/mask_range.hpp"

// ---------------- Masks (local adjustments) ----------------
// A mask is a shape plus its own set of Adjust sliders (the same table as the
// global ones: kAdjustmentSpecs). The picture is rendered as usual first
// (global sliders, curves, mixer, grading); then, for each enabled mask in list
// order, every pixel is run through THAT mask's adjustments and blended back
// in by the mask's weight (0..1) at that pixel:
//
//     out = base + (adjusted(base) - base) * weight
//
// Weight 1 = the full local adjustment, 0 = pixel untouched. Later masks
// stack on top of earlier ones.
//
// Mask geometry is stored against the ORIGINAL picture: centers as a fraction
// of its width / height, radii as a fraction of its shorter side, angles as
// drawn on it. A GeoFrame (see "geometry") maps that to the picture as it is
// now (after orient / straighten / crop) whenever a mask is drawn, edited or
// rendered — so rotating, flipping, straightening or cropping never has to
// touch a mask, undo and redo stay consistent, and removing a crop brings
// back a mask that was outside it. Being normalized also makes the same mask
// render identically on the small drag-preview image and the full-size one.
// The shape structs below have two forms: the stored one, and a "source"
// one (same struct, same fields) already mapped into the current picture,
// which is what rendering and the on-canvas editing work with.
//
// COMBINING: a mask is a list of PARTS (shapes) folded left to right with
// Add / Subtract / Intersect (see MaskComposite). A new kind of shape needs
// nothing for that: it only has to be one more MaskShape alternative.
//
// ADDING ANOTHER KIND OF MASK (color range, depth range...):
//   1. a shape struct with its parameters (like RadialShape) and its three
//      GeoFrame conversions (toSource, fromSource, applySourceEdit),
//   2. an "Eval" struct (like RadialEval): the shape resolved against one
//      concrete image size, able to say which part of a row it touches
//      (rowSpan) and to fill in that row's weights (rowWeights; it is also
//      handed the row's current pixels, for masks that depend on them),
//   3. one more alternative in MaskShape and MaskEval (same order as MaskType)
//      and one more line in maskTypeName and maskShapeToSource,
//   4. its overlay + handles in the UI (paintMaskOverlay / maskPress / ...).
// The pixel pipeline, undo, the preview path, both worker threads, export and
// the per-mask sliders all work on the variants and need no changes.

// Limits of the Masks pane's list.
constexpr size_t kMaxMasks = 16;        // rows in the Masks pane's list
constexpr size_t kMaxMaskParts = 6;     // shapes combined inside one mask

// Same order as the alternatives of MaskShape / MaskEval below.
enum class MaskType : int
{
  Radial,
  Linear,
  Brush,
  Luminosity,
  ColorRange,
  Object,
  Count
};

inline const char *maskTypeName(MaskType t)
{
  switch (t)
  {
  case MaskType::Radial:
    return "Radial";
  case MaskType::Linear:
    return "Linear";
  case MaskType::Brush:
    return "Brush";
  case MaskType::Luminosity:
    return "Luminosity";
  case MaskType::Object:
    return "Object";
  case MaskType::ColorRange:
    return "Color Range";
  default:
    return "Mask";
  }
}

// ---- Shape variants: one alternative per MaskType ----
// The alternatives are in the order of MaskType: MaskPart::type() is the index.
using MaskShape = std::variant<RadialShape, LinearShape, BrushShape,
                               LuminosityShape, ColorRangeShape, ObjectShape>;
using MaskEval = std::variant<RadialEval, LinearEval, BrushEval,
                              LuminosityEval, ColorRangeEval, ObjectEval>;

// Masks whose weights read the picture's pixels (so the overlay must be built
// from the picture just before them).
inline bool maskTypeUsesPixels(MaskType t)
{
  return t == MaskType::Luminosity || t == MaskType::ColorRange;
}

inline MaskShape maskShapeToSource(const MaskShape &shape,
                                   const GeoFrame &f)
{
  return std::visit([&](const auto &s) -> MaskShape
                    { return s.toSource(f); },
                    shape);
}
inline MaskEval prepareMask(const MaskShape &shape, int W, int H)
{
  return std::visit(
      [&](const auto &s) -> MaskEval
      { return s.prepare(W, H); }, shape);
}
inline bool maskRowSpan(const MaskEval &e, int y, int W, int &x0, int &x1)
{
  return std::visit(
      [&](const auto &ev)
      { return ev.rowSpan(y, W, x0, x1); }, e);
}
// `pix` = the RGBA pixels of the row from x0 on, as they are when the mask
// is reached (after the global adjustments and the masks before it). Only the
// masks that depend on the picture's own values read it.
inline void maskRowWeights(const MaskEval &e, int y, int x0, int x1,
                           float *out, const uint8_t *pix)
{
  std::visit([&](const auto &ev)
             { ev.rowWeights(y, x0, x1, out, pix); }, e);
}

// ---- Combining shapes ----
// A mask is made of one or more parts. Part 0 starts the mask; every later
// part is folded into what is there so far, left to right:
//     Add        m = m + c - m c      (union: soft edges blend, never exceed 1)
//     Subtract   m = m (1 - c)        (cut the part out)
//     Intersect  m = m c              (keep only where both are)
// where c is that part's own weight at the pixel. The order matters (it is a
// fold, not a set operation), exactly like the mask's own Invert, which is
// applied last to the result.
enum class MaskOp : int
{
  Add,
  Subtract,
  Intersect
};

inline const char *maskOpName(MaskOp op)
{
  switch (op)
  {
  case MaskOp::Add:
    return "Add";
  case MaskOp::Subtract:
    return "Subtract";
  case MaskOp::Intersect:
    return "Intersect";
  }
  return "Add";
}

// A part with its shape already mapped into the picture being rendered.
struct MaskPassPart
{
  MaskShape shape;
  MaskOp op = MaskOp::Add;
};

// The parts of one mask resolved for one image size.
struct MaskComposite
{
  std::vector<MaskEval> evals;
  std::vector<MaskOp> ops; // ops[0] is ignored: part 0 starts the mask

  // Which pixels [x0, x1) of row `y` can have a non-zero weight? A superset.
  // Add widens the span, Intersect narrows it, Subtract can only remove
  // weight so it leaves it alone. False if nothing is left.
  bool rowSpan(int y, int W, int &x0, int &x1) const
  {
    bool have = false;
    int a = 0, b = 0;
    for (size_t k = 0; k < evals.size(); ++k)
    {
      int s0 = 0, s1 = 0;
      const bool ok = maskRowSpan(evals[k], y, W, s0, s1);
      const MaskOp op = k == 0 ? MaskOp::Add : ops[k];
      if (op == MaskOp::Add)
      {
        if (!ok)
          continue;
        if (!have)
        {
          a = s0;
          b = s1;
          have = true;
        }
        else
        {
          a = a < s0 ? a : s0;
          b = b > s1 ? b : s1;
        }
      }
      else if (op == MaskOp::Intersect)
      {
        if (!have)
          continue; // nothing to keep a part of
        if (!ok)
        {
          have = false;
          continue;
        }
        a = a > s0 ? a : s0;
        b = b < s1 ? b : s1;
        if (b <= a)
          have = false;
      }
    }
    x0 = a;
    x1 = b;
    return have;
  }

  // Weights for pixels [x0, x1) of row `y`. `pix` is the RGBA row from x0 on
  // (for the parts that depend on it); `tmp` is scratch for one row of
  // weights. Plain loops, no std helpers (same Debug-build reason as
  // RadialEval).
  void rowWeights(int y, int x0, int x1, float *out, const uint8_t *pix,
                  float *tmp) const
  {
    const int len = x1 - x0;
    if (evals.empty())
    {
      for (int i = 0; i < len; ++i)
        out[i] = 0.0f;
      return;
    }
    maskRowWeights(evals[0], y, x0, x1, out, pix);
    for (size_t k = 1; k < evals.size(); ++k)
    {
      maskRowWeights(evals[k], y, x0, x1, tmp, pix);
      switch (ops[k])
      {
      case MaskOp::Add:
        for (int i = 0; i < len; ++i)
          out[i] = out[i] + tmp[i] - out[i] * tmp[i];
        break;
      case MaskOp::Subtract:
        for (int i = 0; i < len; ++i)
          out[i] = out[i] * (1.0f - tmp[i]);
        break;
      case MaskOp::Intersect:
        for (int i = 0; i < len; ++i)
          out[i] = out[i] * tmp[i];
        break;
      }
    }
  }
};

inline MaskComposite prepareComposite(const std::vector<MaskPassPart> &parts,
                                      int W, int H)
{
  MaskComposite c;
  c.evals.reserve(parts.size());
  c.ops.reserve(parts.size());
  for (const MaskPassPart &p : parts)
  {
    c.evals.push_back(prepareMask(p.shape, W, H));
    c.ops.push_back(p.op);
  }
  return c;
}

// One part of a mask: a shape and how it joins the parts before it.
struct MaskPart
{
  MaskShape shape;
  MaskOp op = MaskOp::Add; // ignored on the first part
  MaskType type() const { return static_cast<MaskType>(shape.index()); }
  bool operator==(const MaskPart &) const = default;
};

// One mask as the user sees it: its parts, whether it is on / inverted, and
// its own slider values (indexed by AdjId, 0 = untouched).
struct Mask
{
  std::vector<MaskPart> parts; // never empty for a mask made by addMask
  std::string name;            // the user's name for it; empty = show its kind
  bool enabled = true;
  bool invert = false;   // adjust everything OUTSIDE the shapes instead
  float amount = 100.0f; // 0..100: how strongly the adjustments below apply
  AdjValues adj{};
  MixValues mix{};                    // this mask's own Color Mixer
  ColorGrade grade;                   // ...Color Grading
  CurveSet curves = identityCurves(); // ...and tone curves (RGB, R, G, B)
  // The first part's kind: what a one-part mask is (the list names the rest).
  MaskType type() const
  {
    return parts.empty() ? MaskType::Radial : parts.front().type();
  }
  bool operator==(const Mask &) const = default;
};
using MaskSet = std::vector<Mask>;

constexpr size_t kMaskNameMax = 32;
// Printable ASCII only (the TextInput widget is ASCII / byte-cursor), capped.
inline std::string sanitizeMaskName(const std::string &s)
{
  std::string out;
  for (const char ch : s)
  {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c >= 0x20 && c < 0x7F && out.size() < kMaskNameMax)
      out.push_back(ch);
  }
  return out;
}
inline bool maskNameBlank(const std::string &s)
{
  return s.find_first_not_of(' ') == std::string::npos;
}

// Empties every mask cache (brush maps, warped / grown object maps).
inline void clearMaskCaches()
{
  brushCache().clear();
  objectWarpCache().clear();
  objectEvalCache().clear();
}