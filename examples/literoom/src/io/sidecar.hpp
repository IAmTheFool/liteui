// sidecar.hpp - The saved edits as JSON: sliders, curves, Color Mixer, Color Grading,
// geometry and every mask, plus the picture's rating / flag / color label (the
// wrapper's "meta" object). Pure functions on plain values; loading
// validates and clamps everything.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/adjustments.hpp"
#include "color/color_grade.hpp"
#include "color/color_mix.hpp"
#include "color/curves.hpp"
#include "core/geometry.hpp"
#include "mask/mask.hpp"
#include "io/minijson.hpp"
#include "io/photo_meta.hpp"

// ---------------- Saved edits (the sidecar file's content) ----------------
// Everything the user did to a picture, as JSON: slider values, curves, color
// mix and grading, crop / rotation / straighten, and every mask with its
// parts (brush strokes and object bitmaps included). Pure functions on plain
// values: AdjustmentSet::toJson / loadJson call them, and the file handling
// lives next to openImage. Loading validates and clamps everything, so a
// hand-edited or damaged file can never put the editor in a state its own UI
// couldn't reach. The undo history is not saved.
constexpr int kSidecarVersion = 1;
constexpr const char *kSidecarExt = ".literoom.json";
// The sidecar file's name for a picture: "photo.jpg" -> "photo.jpg.literoom.json".
inline std::string sidecarFileOf(const std::string &imagePath)
{
  return imagePath + kSidecarExt;
}
constexpr size_t kSidecarMaxPrompts = 256;
constexpr int64_t kSidecarMaxMapPixels = int64_t{1} << 26; // 8192 x 8192

struct SidecarEdits
{
  AdjValues values{};
  CropRect crop;
  Orient orient;
  float angle = 0.0f;
  bool expand = false;
  CurveSet curves = identityCurves();
  MixValues mix{};
  ColorGrade grade;
  MaskSet masks;
};

inline float sidecarNum(const mj::Json &o, const char *key, float def,
                        float lo, float hi)
{
  const mj::Json *x = o.find(key);
  if (!x || !x->isNumber())
    return def;
  return static_cast<float>(std::clamp(x->asNum(def), static_cast<double>(lo),
                                       static_cast<double>(hi)));
}
inline bool sidecarFlag(const mj::Json &o, const char *key, bool def)
{
  const mj::Json *x = o.find(key);
  return x ? x->asFlag(def) : def;
}
inline float sidecarClampNum(const mj::Json &x, float lo, float hi)
{
  return static_cast<float>(std::clamp(x.asNum(0.0), static_cast<double>(lo),
                                       static_cast<double>(hi)));
}

// ---- sliders ----
// Keyed by the slider's label; zero (untouched) sliders are left out.
inline mj::Json sidecarAdjToJson(const AdjValues &v)
{
  mj::Json o = mj::Json::object();
  for (const auto &spec : kAdjustmentSpecs)
  {
    const float x = v[static_cast<size_t>(spec.id)];
    if (x != 0.0f)
      o.set(std::string(spec.label), mj::Json::num(x));
  }
  return o;
}
inline AdjValues sidecarAdjFromJson(const mj::Json *j)
{
  AdjValues v{};
  if (!j || !j->isObject())
    return v;
  for (const auto &spec : kAdjustmentSpecs)
    v[static_cast<size_t>(spec.id)] =
        sidecarNum(*j, std::string(spec.label).c_str(), 0.0f, spec.min, spec.max);
  return v;
}

// ---- curves ----
inline const char *const kSidecarCurveKeys[kCurveChannelCount] = {"rgb", "r",
                                                                  "g", "b"};
inline mj::Json sidecarCurvesToJson(const CurveSet &cs)
{
  mj::Json o = mj::Json::object();
  for (size_t c = 0; c < kCurveChannelCount; ++c)
  {
    if (cs[c] == identityCurve())
      continue;
    mj::Json pts = mj::Json::array();
    for (const CurvePoint &p : cs[c])
    {
      mj::Json xy = mj::Json::array();
      xy.push(mj::Json::num(p.x)).push(mj::Json::num(p.y));
      pts.push(std::move(xy));
    }
    o.set(kSidecarCurveKeys[c], std::move(pts));
  }
  return o;
}
// Same invariants the curve editor keeps: 2..kCurveMaxPoints points, end
// points at x = 0 and 255, the rest at least kCurveMinGap apart. Anything
// else becomes the identity curve.
inline CurvePoints sidecarCurveFromJson(const mj::Json &arr)
{
  if (!arr.isArray() || arr.items().size() < 2 ||
      arr.items().size() > kCurveMaxPoints)
    return identityCurve();
  CurvePoints pts;
  for (const mj::Json &p : arr.items())
  {
    if (!p.isArray() || p.items().size() != 2 || !p.items()[0].isNumber() ||
        !p.items()[1].isNumber())
      return identityCurve();
    pts.push_back({sidecarClampNum(p.items()[0], 0.0f, 255.0f),
                   sidecarClampNum(p.items()[1], 0.0f, 255.0f)});
  }
  if (pts.front().x != 0.0f || pts.back().x != 255.0f)
    return identityCurve();
  for (size_t i = 1; i < pts.size(); ++i)
    if (pts[i].x - pts[i - 1].x < kCurveMinGap)
      return identityCurve();
  return pts;
}

// ---- color mix / color grading ----
inline const char *const kSidecarMixKeys[kMixChanCount] = {"hue", "sat",
                                                           "lum"};
inline mj::Json sidecarMixToJson(const MixValues &m)
{
  mj::Json o = mj::Json::object();
  if (!mixAnyNonZero(m))
    return o;
  for (size_t c = 0; c < kMixChanCount; ++c)
  {
    mj::Json row = mj::Json::array();
    for (size_t b = 0; b < kMixBandCount; ++b)
      row.push(mj::Json::num(m[mixIndex(static_cast<MixChan>(c), b)]));
    o.set(kSidecarMixKeys[c], std::move(row));
  }
  return o;
}
inline MixValues sidecarMixFromJson(const mj::Json *j)
{
  MixValues m{};
  if (!j || !j->isObject())
    return m;
  for (size_t c = 0; c < kMixChanCount; ++c)
  {
    const mj::Json *row = j->find(kSidecarMixKeys[c]);
    if (!row || !row->isArray())
      continue;
    for (size_t b = 0; b < kMixBandCount && b < row->items().size(); ++b)
      m[mixIndex(static_cast<MixChan>(c), b)] =
          sidecarClampNum(row->items()[b], static_cast<float>(kMixMin),
                          static_cast<float>(kMixMax));
  }
  return m;
}

inline mj::Json sidecarGradeToJson(const ColorGrade &g)
{
  mj::Json o = mj::Json::object();
  if (g == ColorGrade{})
    return o;
  mj::Json wheels = mj::Json::array();
  for (const GradeWheel &w : g.wheel)
  {
    mj::Json j = mj::Json::object();
    j.set("hue", mj::Json::num(w.hue));
    j.set("sat", mj::Json::num(w.sat));
    j.set("lum", mj::Json::num(w.lum));
    j.set("visible", mj::Json::flag(w.visible));
    wheels.push(std::move(j));
  }
  o.set("wheels", std::move(wheels));
  o.set("blending", mj::Json::num(g.blending));
  o.set("balance", mj::Json::num(g.balance));
  return o;
}
inline ColorGrade sidecarGradeFromJson(const mj::Json *j)
{
  ColorGrade g;
  if (!j || !j->isObject())
    return g;
  if (const mj::Json *ws = j->find("wheels"); ws && ws->isArray())
    for (size_t i = 0; i < kGradeRangeCount && i < ws->items().size(); ++i)
    {
      const mj::Json &w = ws->items()[i];
      if (!w.isObject())
        continue;
      g.wheel[i].hue = sidecarNum(w, "hue", 0.0f, 0.0f, 359.0f);
      g.wheel[i].sat = sidecarNum(w, "sat", 0.0f, 0.0f, 100.0f);
      g.wheel[i].lum = sidecarNum(w, "lum", 0.0f, -100.0f, 100.0f);
      g.wheel[i].visible = sidecarFlag(w, "visible", true);
    }
  g.blending = sidecarNum(*j, "blending", g.blending, 0.0f, 100.0f);
  g.balance = sidecarNum(*j, "balance", g.balance, -100.0f, 100.0f);
  return g;
}

// ---- masks ----
inline const char *sidecarOpKey(MaskOp op)
{
  switch (op)
  {
  case MaskOp::Subtract:
    return "subtract";
  case MaskOp::Intersect:
    return "intersect";
  default:
    return "add";
  }
}

// RLE + base64 of an object bitmap, memoized by the map's id (maps are
// immutable) so an autosave doesn't re-encode them. UI thread only.
inline std::string sidecarMapData(const ObjectMap &m)
{
  static std::map<uint64_t, std::string> cache;
  auto it = cache.find(m.id);
  if (it == cache.end())
  {
    if (cache.size() >= 16)
      cache.clear();
    it = cache.emplace(m.id, mj::base64Encode(mj::rleEncode(m.a))).first;
  }
  return it->second;
}

inline mj::Json sidecarShapeToJson(const MaskShape &s)
{
  using mj::Json;
  Json o = Json::object();
  if (const auto *r = std::get_if<RadialShape>(&s))
  {
    o.set("type", Json::str("radial"));
    o.set("cx", Json::num(r->cx));
    o.set("cy", Json::num(r->cy));
    o.set("rx", Json::num(r->rx));
    o.set("ry", Json::num(r->ry));
    o.set("angle", Json::num(r->angle));
    o.set("feather", Json::num(r->feather));
  }
  else if (const auto *l = std::get_if<LinearShape>(&s))
  {
    o.set("type", Json::str("linear"));
    o.set("cx", Json::num(l->cx));
    o.set("cy", Json::num(l->cy));
    o.set("len", Json::num(l->len));
    o.set("angle", Json::num(l->angle));
  }
  else if (const auto *b = std::get_if<BrushShape>(&s))
  {
    o.set("type", Json::str("brush"));
    Json strokes = Json::array();
    for (const BrushStroke &st : b->strokes)
    {
      Json j = Json::object();
      j.set("erase", Json::flag(st.erase));
      j.set("size", Json::num(st.size));
      j.set("feather", Json::num(st.feather));
      j.set("density", Json::num(st.density));
      if (st.flow != 1.0f)
        j.set("flow", Json::num(st.flow));
      Json pts = Json::array(); // x0, y0, x1, y1, ...
      for (const BrushPt &p : st.pts)
        pts.push(Json::num(p.x)).push(Json::num(p.y));
      j.set("pts", std::move(pts));
      strokes.push(std::move(j));
    }
    o.set("strokes", std::move(strokes));
  }
  else if (const auto *lu = std::get_if<LuminosityShape>(&s))
  {
    o.set("type", Json::str("luminosity"));
    o.set("lo", Json::num(lu->lo));
    o.set("hi", Json::num(lu->hi));
    o.set("feather", Json::num(lu->feather));
  }
  else if (const auto *cr = std::get_if<ColorRangeShape>(&s))
  {
    o.set("type", Json::str("colorRange"));
    o.set("set", Json::flag(cr->set));
    o.set("r", Json::num(cr->r));
    o.set("g", Json::num(cr->g));
    o.set("b", Json::num(cr->b));
    o.set("range", Json::num(cr->range));
    o.set("feather", Json::num(cr->feather));
  }
  else if (const auto *ob = std::get_if<ObjectShape>(&s))
  {
    o.set("type", Json::str("object"));
    o.set("feather", Json::num(ob->feather));
    o.set("grow", Json::num(ob->grow));
    Json prompts = Json::array();
    for (const ObjectPrompt &p : ob->prompts)
    {
      Json t = Json::array();
      t.push(Json::num(p.x)).push(Json::num(p.y)).push(Json::flag(p.fg));
      prompts.push(std::move(t));
    }
    o.set("prompts", std::move(prompts));
    if (ob->map && ob->map->w > 0 && ob->map->h > 0 &&
        ob->map->a.size() ==
            static_cast<size_t>(ob->map->w) * static_cast<size_t>(ob->map->h))
    {
      Json m = Json::object();
      m.set("w", Json::num(ob->map->w));
      m.set("h", Json::num(ob->map->h));
      m.set("rle", Json::str(sidecarMapData(*ob->map)));
      o.set("map", std::move(m));
    }
  }
  return o;
}

inline bool sidecarShapeFromJson(const mj::Json &o, MaskShape *out)
{
  const mj::Json *t = o.find("type");
  if (!t || !t->isString())
    return false;
  const std::string &ty = t->asStr();
  if (ty == "radial")
  {
    RadialShape s;
    s.cx = sidecarNum(o, "cx", s.cx, -1.0f, 2.0f);
    s.cy = sidecarNum(o, "cy", s.cy, -1.0f, 2.0f);
    s.rx = sidecarNum(o, "rx", s.rx, 0.001f, 4.0f);
    s.ry = sidecarNum(o, "ry", s.ry, 0.001f, 4.0f);
    s.angle = sidecarNum(o, "angle", s.angle, -360.0f, 360.0f);
    s.feather = sidecarNum(o, "feather", s.feather, 0.0f, 1.0f);
    *out = s;
    return true;
  }
  if (ty == "linear")
  {
    LinearShape s;
    s.cx = sidecarNum(o, "cx", s.cx, -1.0f, 2.0f);
    s.cy = sidecarNum(o, "cy", s.cy, -1.0f, 2.0f);
    s.len = sidecarNum(o, "len", s.len, 0.001f, 4.0f);
    s.angle = sidecarNum(o, "angle", s.angle, -360.0f, 360.0f);
    *out = s;
    return true;
  }
  if (ty == "brush")
  {
    BrushShape s;
    const mj::Json *strokes = o.find("strokes");
    if (strokes && strokes->isArray())
      for (const mj::Json &sj : strokes->items())
      {
        if (s.strokes.size() >= kBrushMaxStrokes)
          break;
        if (!sj.isObject())
          continue;
        BrushStroke st;
        st.erase = sidecarFlag(sj, "erase", false);
        st.size = sidecarNum(sj, "size", st.size, kBrushMinSize, kBrushMaxSize);
        st.feather = sidecarNum(sj, "feather", st.feather, 0.0f, 1.0f);
        st.density = sidecarNum(sj, "density", st.density, 0.0f, 1.0f);
        st.flow = sidecarNum(sj, "flow", st.flow, 0.01f, 1.0f);
        const mj::Json *pj = sj.find("pts");
        if (pj && pj->isArray())
        {
          const auto &v = pj->items();
          for (size_t i = 0; i + 1 < v.size() && st.pts.size() < kBrushMaxPoints;
               i += 2)
            if (v[i].isNumber() && v[i + 1].isNumber())
              st.pts.push_back({sidecarClampNum(v[i], -1.0f, 2.0f),
                                sidecarClampNum(v[i + 1], -1.0f, 2.0f)});
        }
        if (!st.pts.empty())
          s.strokes.push_back(std::move(st));
      }
    *out = std::move(s);
    return true;
  }
  if (ty == "luminosity")
  {
    LuminosityShape s;
    s.setLo(sidecarNum(o, "lo", s.lo, 0.0f, 1.0f));
    s.setHi(sidecarNum(o, "hi", s.hi, 0.0f, 1.0f));
    s.feather = sidecarNum(o, "feather", s.feather, 0.0f, 1.0f);
    *out = s;
    return true;
  }
  if (ty == "colorRange")
  {
    ColorRangeShape s;
    s.set = sidecarFlag(o, "set", false);
    s.r = sidecarNum(o, "r", 0.0f, 0.0f, 1.0f);
    s.g = sidecarNum(o, "g", 0.0f, 0.0f, 1.0f);
    s.b = sidecarNum(o, "b", 0.0f, 0.0f, 1.0f);
    s.range = sidecarNum(o, "range", s.range, 0.0f, 1.0f);
    s.feather = sidecarNum(o, "feather", s.feather, 0.0f, 1.0f);
    *out = s;
    return true;
  }
  if (ty == "object")
  {
    ObjectShape s;
    s.feather = sidecarNum(o, "feather", 0.0f, 0.0f, 1.0f);
    s.grow = sidecarNum(o, "grow", 0.0f, -1.0f, 1.0f);
    const mj::Json *prompts = o.find("prompts");
    if (prompts && prompts->isArray())
      for (const mj::Json &p : prompts->items())
      {
        if (s.prompts.size() >= kSidecarMaxPrompts)
          break;
        if (!p.isArray() || p.items().size() != 3 || !p.items()[0].isNumber() ||
            !p.items()[1].isNumber())
          continue;
        ObjectPrompt op;
        op.x = sidecarClampNum(p.items()[0], 0.0f, 1.0f);
        op.y = sidecarClampNum(p.items()[1], 0.0f, 1.0f);
        op.fg = p.items()[2].asFlag(true);
        s.prompts.push_back(op);
      }
    const mj::Json *m = o.find("map");
    const mj::Json *rle = m && m->isObject() ? m->find("rle") : nullptr;
    if (rle && rle->isString())
    {
      const int64_t iw = std::lround(sidecarNum(*m, "w", 0.0f, 0.0f, 16384.0f));
      const int64_t ih = std::lround(sidecarNum(*m, "h", 0.0f, 0.0f, 16384.0f));
      std::vector<uint8_t> packed, pixels;
      if (iw > 0 && ih > 0 && iw * ih <= kSidecarMaxMapPixels &&
          mj::base64Decode(rle->asStr(), &packed) &&
          mj::rleDecode(packed, static_cast<size_t>(iw * ih), &pixels))
      {
        auto map = std::make_shared<ObjectMap>();
        map->w = static_cast<int>(iw);
        map->h = static_cast<int>(ih);
        map->a = std::move(pixels);
        map->id = nextObjectMapId();
        s.map = std::move(map);
      }
    }
    *out = std::move(s);
    return true;
  }
  return false;
}

inline mj::Json sidecarMaskToJson(const Mask &m)
{
  mj::Json o = mj::Json::object();
  o.set("enabled", mj::Json::flag(m.enabled));
  o.set("invert", mj::Json::flag(m.invert));
  if (!maskNameBlank(m.name))
    o.set("name", mj::Json::str(m.name));
  o.set("adjust", sidecarAdjToJson(m.adj));
  if (m.amount != 100.0f)
    o.set("amount", mj::Json::num(m.amount));
  if (mixAnyNonZero(m.mix))
    o.set("colorMix", sidecarMixToJson(m.mix));
  if (!(m.grade == ColorGrade{}))
    o.set("colorGrading", sidecarGradeToJson(m.grade));
  if (!(m.curves == identityCurves()))
    o.set("curves", sidecarCurvesToJson(m.curves));
  mj::Json parts = mj::Json::array();
  for (const MaskPart &p : m.parts)
  {
    mj::Json j = sidecarShapeToJson(p.shape);
    j.set("op", mj::Json::str(sidecarOpKey(p.op)));
    parts.push(std::move(j));
  }
  o.set("parts", std::move(parts));
  return o;
}
inline bool sidecarMaskFromJson(const mj::Json &o, Mask *out)
{
  if (!o.isObject())
    return false;
  Mask m;
  m.enabled = sidecarFlag(o, "enabled", true);
  m.invert = sidecarFlag(o, "invert", false);
  if (const mj::Json *nm = o.find("name"); nm && nm->isString())
    m.name = sanitizeMaskName(nm->asStr());
  m.adj = sidecarAdjFromJson(o.find("adjust"));
  m.amount = sidecarNum(o, "amount", 100.0f, 0.0f, 100.0f);
  m.mix = sidecarMixFromJson(o.find("colorMix"));
  m.grade = sidecarGradeFromJson(o.find("colorGrading"));
  if (const mj::Json *cs = o.find("curves"); cs && cs->isObject())
    for (size_t c = 0; c < kCurveChannelCount; ++c)
      if (const mj::Json *a = cs->find(kSidecarCurveKeys[c]))
        m.curves[c] = sidecarCurveFromJson(*a);
  const mj::Json *parts = o.find("parts");
  if (!parts || !parts->isArray())
    return false;
  for (const mj::Json &pj : parts->items())
  {
    if (m.parts.size() >= kMaxMaskParts)
      break;
    MaskPart part;
    if (!pj.isObject() || !sidecarShapeFromJson(pj, &part.shape))
      continue;
    const mj::Json *op = pj.find("op");
    const std::string o2 = op && op->isString() ? op->asStr() : "add";
    part.op = o2 == "subtract"    ? MaskOp::Subtract
              : o2 == "intersect" ? MaskOp::Intersect
                                  : MaskOp::Add;
    m.parts.push_back(std::move(part));
  }
  if (m.parts.empty())
    return false; // a mask without a shape can't be shown or edited
  *out = std::move(m);
  return true;
}

// ---- the whole edit state ----
inline mj::Json sidecarEditsToJson(const SidecarEdits &e)
{
  using mj::Json;
  Json o = Json::object();
  o.set("adjust", sidecarAdjToJson(e.values));
  o.set("curves", sidecarCurvesToJson(e.curves));
  o.set("colorMix", sidecarMixToJson(e.mix));
  o.set("colorGrading", sidecarGradeToJson(e.grade));
  Json g = Json::object();
  g.set("flip", Json::flag(e.orient.flip));
  g.set("rotate", Json::num(e.orient.rot)); // clockwise quarter turns
  g.set("angle", Json::num(e.angle));
  g.set("keepFullCanvas", Json::flag(e.expand));
  if (!e.crop.empty())
  {
    Json c = Json::array();
    c.push(Json::num(e.crop.x)).push(Json::num(e.crop.y));
    c.push(Json::num(e.crop.w)).push(Json::num(e.crop.h));
    g.set("crop", std::move(c)); // x, y, w, h in the straightened picture
  }
  o.set("geometry", std::move(g));
  Json masks = Json::array();
  for (const Mask &m : e.masks)
    masks.push(sidecarMaskToJson(m));
  o.set("masks", std::move(masks));
  return o;
}

// origW / origH: the picture's size, needed to check the crop. False (and
// *out untouched) only if `o` isn't an object; anything wrong inside it is
// dropped or clamped instead.
inline bool sidecarEditsFromJson(const mj::Json &o, int origW, int origH,
                                 SidecarEdits *out)
{
  if (!o.isObject() || origW <= 0 || origH <= 0)
    return false;
  SidecarEdits e;
  e.values = sidecarAdjFromJson(o.find("adjust"));
  if (const mj::Json *cs = o.find("curves"); cs && cs->isObject())
    for (size_t c = 0; c < kCurveChannelCount; ++c)
      if (const mj::Json *a = cs->find(kSidecarCurveKeys[c]))
        e.curves[c] = sidecarCurveFromJson(*a);
  e.mix = sidecarMixFromJson(o.find("colorMix"));
  e.grade = sidecarGradeFromJson(o.find("colorGrading"));

  if (const mj::Json *g = o.find("geometry"); g && g->isObject())
  {
    e.orient.flip = sidecarFlag(*g, "flip", false);
    e.orient.rot = static_cast<int>(
        std::lround(sidecarNum(*g, "rotate", 0.0f, 0.0f, 3.0f)));
    e.angle = sidecarNum(*g, "angle", 0.0f, -kAngleMax, kAngleMax);
    e.expand = sidecarFlag(*g, "keepFullCanvas", false);
    const mj::Json *c = g->find("crop");
    if (c && c->isArray() && c->items().size() == 4)
    {
      int v[4];
      bool ok = true;
      for (int i = 0; i < 4; ++i)
      {
        ok = ok && c->items()[static_cast<size_t>(i)].isNumber();
        v[i] = static_cast<int>(std::lround(
            sidecarClampNum(c->items()[static_cast<size_t>(i)], 0.0f, 1.0e6f)));
      }
      // The crop is measured on the oriented + straightened picture.
      int w = origW, h = origH;
      if (e.orient.rot & 1)
        std::swap(w, h);
      const Dims d = rotatedDims(w, h, e.angle, e.expand);
      if (ok && v[2] > 0 && v[3] > 0 && v[0] + v[2] <= d.w &&
          v[1] + v[3] <= d.h)
        e.crop = CropRect{v[0], v[1], v[2], v[3]};
    }
  }

  const mj::Json *masks = o.find("masks");
  if (masks && masks->isArray())
    for (const mj::Json &mj_ : masks->items())
    {
      if (e.masks.size() >= kMaxMasks)
        break;
      Mask m;
      if (sidecarMaskFromJson(mj_, &m))
        e.masks.push_back(std::move(m));
    }
  *out = std::move(e);
  return true;
}

// ---- rating, flag, color label (the wrapper's "meta") ----
// Not part of `edits`: they have no undo step and Sync Settings must leave a
// picture's own rating alone. The "meta" key is written only when something
// differs from the default, and every code path that rewrites a whole sidecar
// (the app's own save, sidecarSyncLook) goes through sidecarFileText so none of
// them can drop it.

// The wrapper's "meta" object; the default PhotoMeta if it is missing or
// malformed.
inline PhotoMeta sidecarMetaFromRoot(const mj::Json &root)
{
  const mj::Json *m = root.find("meta");
  return m ? photoMetaFromJson(*m) : PhotoMeta{};
}

// The whole sidecar file: the wrapper (app, version, image, and meta when it is
// not the default) around `edits`. `w` / `h` are the picture's size.
inline std::string sidecarFileText(const std::string &imageName, int w, int h,
                                   mj::Json edits, const PhotoMeta &meta)
{
  using mj::Json;
  Json img = Json::object();
  img.set("name", Json::str(imageName));
  img.set("width", Json::num(w));
  img.set("height", Json::num(h));
  Json root = Json::object();
  root.set("app", Json::str("literoom"));
  root.set("version", Json::num(kSidecarVersion));
  root.set("image", std::move(img));
  if (!meta.isDefault())
    root.set("meta", photoMetaToJson(meta));
  root.set("edits", std::move(edits));
  return root.dump();
}

// What sidecarEditsToJson writes for a picture nobody has edited.
inline const std::string &sidecarNeutralEditsDump()
{
  static const std::string s = sidecarEditsToJson(SidecarEdits{}).dump();
  return s;
}

// What the filmstrip wants to know about a sidecar without opening the picture.
//   edited  false only for a readable file whose `edits` are exactly the neutral
//           ones (a file that exists only because the picture was rated); any
//           other file, readable or not, counts as edited, as a file always did.
//   meta    the rating / flag / label (default if there is none or the file
//           can't be read).
struct SidecarScan
{
  bool edited = true;
  PhotoMeta meta;
};
inline SidecarScan sidecarScan(const std::string &text)
{
  SidecarScan s;
  mj::Json root;
  std::string err;
  if (!mj::Json::parse(text, &root, &err) || !root.isObject())
    return s;
  s.meta = sidecarMetaFromRoot(root);
  const mj::Json *edits = root.find("edits");
  if (edits && edits->isObject() && edits->dump() == sidecarNeutralEditsDump())
    s.edited = false;
  return s;
}

// Sync Settings: puts a look (sliders, curves, Color Mixer, Color Grading) into
// the sidecar text of ANOTHER picture without touching its geometry or masks.
//   oldText   the picture's current sidecar file ("" if it has none)
//   imgW/H    the picture's size. Needed only when oldText is empty (a new file
//             must record it); otherwise the size recorded in the file is used,
//             and if imgW/H are given too they must match it.
// The picture's own rating / flag / color label is kept.
// Returns false, with a reason in *err and *outText untouched, when the old
// file can't be trusted (unreadable, from a newer version, other size): the
// caller skips that picture rather than overwrite edits it can't read.
// Everything else in the old file goes through sidecarEditsFromJson, so it is
// validated and clamped exactly as when the picture is opened.
// Reads a picture's existing sidecar text so it can be rewritten. With no text
// (the picture has no sidecar) the edits are the neutral ones and the size must
// be given (imgW x imgH). With text, the file must be one we can trust: valid
// JSON object, not from a newer version, with a picture size that matches
// imgW x imgH (when those are given). Anything else is refused, with a reason in
// *err, so a file that might still be recoverable by hand is never overwritten.
// On success *e holds the file's edits, *meta its rating / flag / label, and
// *W x *H the picture's size.
inline bool sidecarReadForRewrite(const std::string &oldText, int imgW, int imgH,
                                  SidecarEdits *e, PhotoMeta *meta, int *W, int *H,
                                  std::string *err)
{
  using mj::Json;
  auto fail = [&](const char *why)
  {
    if (err)
      *err = why;
    return false;
  };
  *e = SidecarEdits{};
  *meta = PhotoMeta{};
  *W = imgW;
  *H = imgH;
  if (oldText.empty())
    return *W > 0 && *H > 0 ? true : fail("the picture's size is unknown");
  Json root;
  std::string perr;
  if (!Json::parse(oldText, &root, &perr) || !root.isObject())
    return fail("its saved edits are unreadable");
  const Json *ver = root.find("version");
  if (!ver || !ver->isNumber() || ver->asNum(0) > kSidecarVersion)
    return fail("its saved edits are from a newer Literoom");
  const Json *img = root.find("image");
  const Json *jw = img && img->isObject() ? img->find("width") : nullptr;
  const Json *jh = img && img->isObject() ? img->find("height") : nullptr;
  if (!jw || !jh)
    return fail("its saved edits have no picture size");
  const int fw = static_cast<int>(jw->asNum(-1));
  const int fh = static_cast<int>(jh->asNum(-1));
  if (imgW > 0 && imgH > 0 && (fw != imgW || fh != imgH))
    return fail("the picture's size has changed");
  const Json *edits = root.find("edits");
  if (!edits || !sidecarEditsFromJson(*edits, fw, fh, e))
    return fail("its saved edits are unreadable");
  *meta = sidecarMetaFromRoot(root);
  *W = fw;
  *H = fh;
  return true;
}

inline bool sidecarSyncLook(const std::string &oldText,
                            const std::string &imageName, int imgW, int imgH,
                            const AdjValues &values, const CurveSet &curves,
                            const MixValues &mix, const ColorGrade &grade,
                            std::string *outText, std::string *err)
{
  SidecarEdits e;
  PhotoMeta meta; // the picture's own rating / flag / label: kept as it is
  int W = 0, H = 0;
  if (!sidecarReadForRewrite(oldText, imgW, imgH, &e, &meta, &W, &H, err))
    return false;

  e.values = values;
  e.curves = curves;
  e.mix = mix;
  e.grade = grade;

  if (outText)
    *outText = sidecarFileText(imageName, W, H, sidecarEditsToJson(e), meta);
  return true;
}

// Batch rating: applies `patch` to a picture's rating / flag / color label in
// its sidecar text and leaves everything else (edits, geometry, masks) as the
// file has it. The same trust rules as sidecarSyncLook (see
// sidecarReadForRewrite). *outMeta is the picture's meta after the patch. With
// no old text the picture's size (imgW x imgH) is needed to start a file; the
// caller can skip such a picture when patch.applyTo(PhotoMeta{}) is the default,
// since there is nothing to record. Pure: no files, no threads.
inline bool sidecarPatchMeta(const std::string &oldText,
                             const std::string &imageName, int imgW, int imgH,
                             const MetaPatch &patch, std::string *outText,
                             PhotoMeta *outMeta, std::string *err)
{
  SidecarEdits e;
  PhotoMeta meta;
  int W = 0, H = 0;
  if (!sidecarReadForRewrite(oldText, imgW, imgH, &e, &meta, &W, &H, err))
    return false;
  meta = patch.applyTo(meta);
  if (outMeta)
    *outMeta = meta;
  if (outText)
    *outText = sidecarFileText(imageName, W, H, sidecarEditsToJson(e), meta);
  return true;
}