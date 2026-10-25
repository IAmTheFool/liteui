// Build: g++ -std=c++20 -I.. sidecar_test.cpp -o sidecar_test && ./sidecar_test
#include "io/sidecar.hpp"
#include "check.hpp"

#include <random>

using mj::Json;

static bool load(const std::string &text, SidecarEdits *e, int w = 400, int h = 300)
{
  Json j;
  std::string err;
  if (!Json::parse(text, &j, &err))
    return false;
  return sidecarEditsFromJson(j, w, h, e);
}

// ------------------------------------------------------------ a rich edit set
static std::shared_ptr<ObjectMap> blob()
{
  auto m = std::make_shared<ObjectMap>();
  m->w = 20;
  m->h = 12;
  m->id = nextObjectMapId();
  m->a.assign(240, 0);
  for (int y = 3; y < 9; ++y)
    for (int x = 4; x < 15; ++x)
      m->a[static_cast<size_t>(y) * 20 + x] = (x + y) % 3 ? 255 : 128;
  return m;
}

static SidecarEdits rich()
{
  SidecarEdits e;
  for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i)
    e.values[i] = (i % 2 && kAdjustmentSpecs[i].min < 0) ? kAdjustmentSpecs[i].min * 0.37f : kAdjustmentSpecs[i].max * 0.43f;
  e.crop = CropRect{20, 30, 200, 120};
  e.orient = Orient{1, true};
  e.angle = -7.5f;
  e.expand = true;
  e.curves[0] = CurvePoints{{0, 0}, {64, 40}, {200, 230}, {255, 255}};
  e.curves[2] = CurvePoints{{0, 10}, {255, 240}};
  for (size_t i = 0; i < e.mix.size(); ++i)
    e.mix[i] = static_cast<float>(static_cast<int>(i * 7 % 41) - 20);
  for (size_t r = 0; r < kGradeRangeCount; ++r)
  {
    e.grade.wheel[r].hue = 30.0f + 70.0f * static_cast<float>(r);
    e.grade.wheel[r].sat = 20.0f + 10.0f * static_cast<float>(r);
    e.grade.wheel[r].lum = r % 2 ? 15.0f : -12.5f;
  }
  e.grade.wheel[2].visible = false;
  e.grade.blending = 62.0f;
  e.grade.balance = -17.0f;

  Mask radial;
  radial.name = "Sky";
  radial.amount = 65.0f;
  radial.invert = true;
  radial.adj[static_cast<size_t>(AdjId::Exposure)] = 33.0f;
  radial.mix[mixIndex(MixChan::Hue, 2)] = 11.0f;
  radial.grade.wheel[0].sat = 40.0f;
  radial.curves[1] = CurvePoints{{0, 0}, {120, 90}, {255, 255}};
  RadialShape r;
  r.cx = 0.3f;
  r.cy = 0.4f;
  r.rx = 0.25f;
  r.ry = 0.1f;
  r.angle = 33.0f;
  r.feather = 0.7f;
  radial.parts.push_back({r, MaskOp::Add});
  LinearShape l;
  l.cx = 0.6f;
  l.cy = 0.5f;
  l.len = 0.2f;
  l.angle = -120.0f;
  radial.parts.push_back({l, MaskOp::Subtract});
  e.masks.push_back(radial);

  Mask brush;
  brush.enabled = false;
  BrushShape b;
  BrushStroke s1;
  s1.size = 0.08f;
  s1.feather = 0.25f;
  s1.density = 0.8f;
  s1.flow = 0.4f;
  s1.pts = {{0.1f, 0.2f}, {0.3f, 0.5f}, {0.5f, 0.45f}};
  BrushStroke s2;
  s2.erase = true;
  s2.pts = {{0.3f, 0.5f}};
  b.strokes = {s1, s2};
  brush.parts.push_back({b, MaskOp::Add});
  LuminosityShape lum;
  lum.lo = 0.2f;
  lum.hi = 0.7f;
  lum.feather = 0.3f;
  brush.parts.push_back({lum, MaskOp::Intersect});
  e.masks.push_back(brush);

  Mask obj;
  ObjectShape o;
  o.map = blob();
  o.prompts = {{0.4f, 0.5f, true}, {0.8f, 0.1f, false}};
  o.feather = 0.2f;
  o.grow = -0.3f;
  obj.parts.push_back({o, MaskOp::Add});
  ColorRangeShape cr;
  cr.set = true;
  cr.r = 0.1f;
  cr.g = 0.8f;
  cr.b = 0.3f;
  cr.range = 0.4f;
  cr.feather = 0.6f;
  obj.parts.push_back({cr, MaskOp::Add});
  e.masks.push_back(obj);
  return e;
}

static bool inRange(float v, float lo, float hi) { return v >= lo && v <= hi; }

// What no loaded file may ever violate, however it was damaged.
static void checkValid(const SidecarEdits &e, int w, int h, const char *tag)
{
  bool ok = true;
  auto need = [&](bool c, const char *what)
  {
    if (!c && ok)
    {
      std::printf("  [%s] invalid: %s\n", tag, what);
      ok = false;
    }
  };
  for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i)
    need(inRange(e.values[i], kAdjustmentSpecs[i].min, kAdjustmentSpecs[i].max), "slider out of range");
  need(e.orient.rot >= 0 && e.orient.rot <= 3, "rotation");
  need(inRange(e.angle, -kAngleMax, kAngleMax), "angle");
  if (!e.crop.empty())
  {
    int cw = w, ch = h;
    if (e.orient.rot & 1)
      std::swap(cw, ch);
    const Dims d = rotatedDims(cw, ch, e.angle, e.expand);
    need(e.crop.x >= 0 && e.crop.y >= 0 && e.crop.x + e.crop.w <= d.w && e.crop.y + e.crop.h <= d.h, "crop outside the picture");
  }
  auto curveOk = [&](const CurvePoints &c)
  {
    if (c.size() < 2 || c.size() > kCurveMaxPoints || c.front().x != 0.0f || c.back().x != 255.0f)
      return false;
    for (size_t i = 0; i < c.size(); ++i)
    {
      if (!inRange(c[i].x, 0, 255) || !inRange(c[i].y, 0, 255))
        return false;
      if (i && c[i].x - c[i - 1].x < kCurveMinGap)
        return false;
    }
    return true;
  };
  for (const CurvePoints &c : e.curves)
    need(curveOk(c), "curve");
  for (float v : e.mix)
    need(inRange(v, static_cast<float>(kMixMin), static_cast<float>(kMixMax)), "mix");
  auto gradeOk = [&](const ColorGrade &g)
  {
    for (const auto &wh : g.wheel)
      if (!inRange(wh.hue, 0, 359) || !inRange(wh.sat, 0, 100) || !inRange(wh.lum, -100, 100))
        return false;
    return inRange(g.blending, 0, 100) && inRange(g.balance, -100, 100);
  };
  need(gradeOk(e.grade), "grade");
  need(e.masks.size() <= kMaxMasks, "too many masks");
  for (const Mask &m : e.masks)
  {
    need(!m.parts.empty() && m.parts.size() <= kMaxMaskParts, "mask parts");
    need(inRange(m.amount, 0, 100), "mask amount");
    need(m.name.size() <= kMaskNameMax, "mask name");
    for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i)
      need(inRange(m.adj[i], kAdjustmentSpecs[i].min, kAdjustmentSpecs[i].max), "mask slider");
    for (const CurvePoints &c : m.curves)
      need(curveOk(c), "mask curve");
    need(gradeOk(m.grade), "mask grade");
    for (const MaskPart &p : m.parts)
    {
      if (const auto *r = std::get_if<RadialShape>(&p.shape))
        need(inRange(r->rx, 0.001f, 4) && inRange(r->ry, 0.001f, 4) && inRange(r->feather, 0, 1) && inRange(r->cx, -1, 2), "radial");
      else if (const auto *l = std::get_if<LinearShape>(&p.shape))
        need(inRange(l->len, 0.001f, 4) && inRange(l->angle, -360, 360), "linear");
      else if (const auto *b = std::get_if<BrushShape>(&p.shape))
      {
        need(b->strokes.size() <= kBrushMaxStrokes, "strokes");
        for (const BrushStroke &s : b->strokes)
        {
          need(!s.pts.empty() && s.pts.size() <= kBrushMaxPoints, "stroke points");
          need(inRange(s.size, kBrushMinSize, kBrushMaxSize) && inRange(s.flow, 0.01f, 1) && inRange(s.density, 0, 1), "stroke");
          for (const BrushPt &pt : s.pts)
            need(inRange(pt.x, -1, 2) && inRange(pt.y, -1, 2), "stroke point");
        }
      }
      else if (const auto *lu = std::get_if<LuminosityShape>(&p.shape))
        need(inRange(lu->lo, 0, 1) && inRange(lu->hi, 0, 1) && inRange(lu->feather, 0, 1), "luminosity");
      else if (const auto *cr = std::get_if<ColorRangeShape>(&p.shape))
        need(inRange(cr->r, 0, 1) && inRange(cr->range, 0, 1) && inRange(cr->feather, 0, 1), "color range");
      else if (const auto *re = std::get_if<RectShape>(&p.shape))
        need(inRange(re->rx, 0.001f, 4) && inRange(re->ry, 0.001f, 4) && inRange(re->feather, 0, 1) && inRange(re->cx, -1, 2) && inRange(re->cy, -1, 2) && inRange(re->angle, -360, 360), "rect");
      else if (const auto *pg = std::get_if<PolygonShape>(&p.shape))
      {
        need(pg->pts.size() <= kPolyMaxPoints && inRange(pg->feather, 0, 1), "polygon");
        for (const PolyPt &pt : pg->pts)
          need(inRange(pt.x, -1, 2) && inRange(pt.y, -1, 2), "polygon point");
      }
      else if (const auto *o = std::get_if<ObjectShape>(&p.shape))
      {
        need(o->prompts.size() <= kSidecarMaxPrompts, "prompts");
        for (const ObjectPrompt &pr : o->prompts)
          need(inRange(pr.x, 0, 1) && inRange(pr.y, 0, 1), "prompt");
        if (o->map)
          need(o->map->w > 0 && o->map->h > 0 && o->map->a.size() == static_cast<size_t>(o->map->w) * o->map->h &&
                   static_cast<int64_t>(o->map->a.size()) <= kSidecarMaxMapPixels,
               "object map");
      }
    }
  }
  CHECK(ok);
}

// ----------------------------------------------------------------------- tests
static void testRoundTrip()
{
  const SidecarEdits a = rich();
  const std::string text = sidecarEditsToJson(a).dump();
  SidecarEdits b;
  CHECK(load(text, &b));
  checkValid(b, 400, 300, "round trip");
  // Everything that has a plain equality comes back equal.
  CHECK(b.values == a.values && b.crop == a.crop && b.orient == a.orient && b.angle == a.angle && b.expand == a.expand);
  CHECK(b.curves == a.curves && b.mix == a.mix && b.grade == a.grade);
  CHECK(b.masks.size() == 3);
  // Written again, the text is identical (nothing is lost or reordered).
  CHECK(sidecarEditsToJson(b).dump() == text);
  // The details worth looking at by eye:
  const Mask &m0 = b.masks[0];
  CHECK(m0.name == "Sky" && m0.amount == 65.0f && m0.invert && m0.enabled && m0.parts.size() == 2);
  CHECK(m0.parts[0].op == MaskOp::Add && m0.parts[1].op == MaskOp::Subtract);
  CHECK(std::get<RadialShape>(m0.parts[0].shape) == std::get<RadialShape>(a.masks[0].parts[0].shape));
  CHECK(std::get<LinearShape>(m0.parts[1].shape) == std::get<LinearShape>(a.masks[0].parts[1].shape));
  CHECK(m0.adj == a.masks[0].adj && m0.mix == a.masks[0].mix && m0.grade == a.masks[0].grade && m0.curves == a.masks[0].curves);
  const Mask &m1 = b.masks[1];
  CHECK(!m1.enabled && m1.parts[1].op == MaskOp::Intersect);
  CHECK(std::get<BrushShape>(m1.parts[0].shape) == std::get<BrushShape>(a.masks[1].parts[0].shape));
  CHECK(std::get<LuminosityShape>(m1.parts[1].shape) == std::get<LuminosityShape>(a.masks[1].parts[1].shape));
  const Mask &m2 = b.masks[2];
  const ObjectShape &o2 = std::get<ObjectShape>(m2.parts[0].shape), &o1 = std::get<ObjectShape>(a.masks[2].parts[0].shape);
  CHECK(o2.map && o2.map->a == o1.map->a && o2.map->w == 20 && o2.map->h == 12);
  CHECK(o2.map->id != o1.map->id); // a loaded map is a new object
  CHECK(o2.prompts == o1.prompts && o2.feather == o1.feather && o2.grow == o1.grow);
  CHECK(std::get<ColorRangeShape>(m2.parts[1].shape) == std::get<ColorRangeShape>(a.masks[2].parts[1].shape));
}

static void testFileLayout()
{
  SidecarEdits e;
  e.values[static_cast<size_t>(AdjId::Exposure)] = 20.0f;
  e.orient = Orient{3, true};
  e.angle = 2.5f;
  e.crop = CropRect{1, 2, 30, 40};
  const Json j = sidecarEditsToJson(e);
  CHECK(j.isObject());
  const Json *adj = j.find("adjust");
  CHECK(adj && adj->isObject() && adj->size() == 1); // untouched sliders are left out
  CHECK(adj->find("Exposure") && adj->find("Exposure")->asNum(0) == 20.0);
  const Json *g = j.find("geometry");
  CHECK(g && g->find("flip")->asFlag(false) && g->find("rotate")->asNum(-1) == 3 && g->find("angle")->asNum(0) == 2.5);
  CHECK(g->find("keepFullCanvas") && !g->find("keepFullCanvas")->asFlag(true));
  const Json *crop = g->find("crop");
  CHECK(crop && crop->isArray() && crop->items().size() == 4 && crop->items()[2].asNum(0) == 30);
  // Straight-line curves are left out; edited ones are keyed rgb / r / g / b.
  CHECK(j.find("curves") && j.find("curves")->isObject() && j.find("curves")->size() == 0);
  const Json rj = sidecarEditsToJson(rich());
  const Json *rc = rj.find("curves");
  CHECK(rc && rc->find("rgb") && rc->find("g") && !rc->find("r") && !rc->find("b"));
  CHECK(j.find("colorMix") && j.find("colorGrading") && j.find("masks") && j.find("masks")->isArray());
  // No crop: the key is not written.
  SidecarEdits none;
  CHECK(!sidecarEditsToJson(none).find("geometry")->find("crop"));
  // A mask with nothing special writes only what is not the default.
  Mask m;
  m.parts.push_back({RadialShape{}, MaskOp::Add});
  const Json mj = sidecarMaskToJson(m);
  CHECK(mj.find("enabled") && mj.find("invert") && mj.find("parts") && mj.find("adjust"));
  CHECK(!mj.find("name") && !mj.find("amount") && !mj.find("colorMix") && !mj.find("colorGrading") && !mj.find("curves"));
  CHECK(mj.find("parts")->items()[0].find("type")->asStr() == "radial" && mj.find("parts")->items()[0].find("op")->asStr() == "add");
  // The file extension / version constants.
  CHECK(kSidecarVersion == 1 && std::string(kSidecarExt) == ".literoom.json");
}

static void testSlidersAndGeometryAreClamped()
{
  SidecarEdits e;
  const std::string text = R"({"adjust":{"Exposure":9999,"Contrast":-9999,"Sharpness":"loud","Nope":5,"Highlights":null},
                               "geometry":{"rotate":7.2,"angle":999,"flip":"yes","keepFullCanvas":true}})";
  CHECK(load(text, &e));
  CHECK(e.values[static_cast<size_t>(AdjId::Exposure)] == kAdjustmentSpecs[static_cast<size_t>(AdjId::Exposure)].max);
  CHECK(e.values[static_cast<size_t>(AdjId::Contrast)] == kAdjustmentSpecs[static_cast<size_t>(AdjId::Contrast)].min);
  CHECK(e.values[static_cast<size_t>(AdjId::Sharpness)] == 0.0f && e.values[static_cast<size_t>(AdjId::Highlights)] == 0.0f);
  CHECK(e.orient.rot == 3 && !e.orient.flip && e.angle == kAngleMax && e.expand); // 7.2 -> clamped to 3; "yes" is not a flag
  SidecarEdits r;
  CHECK(load(R"({"geometry":{"rotate":1.6}})", &r) && r.orient.rot == 2); // rounds to nearest
  CHECK(load(R"({"geometry":{"rotate":-4}})", &r) && r.orient.rot == 0);
  // Bad roots.
  SidecarEdits untouched;
  untouched.angle = 12.0f;
  CHECK(!load("[1,2,3]", &untouched) && untouched.angle == 12.0f); // not an object: refused, output untouched
  CHECK(!load("42", &untouched) && !load("\"x\"", &untouched) && !load("not json", &untouched));
  CHECK(load("{}", &untouched) && untouched.angle == 0.0f);        // an empty object is a valid, empty edit
  CHECK(!load("{}", &untouched, 0, 300) && !load("{}", &untouched, 400, -1));
}

static void testCrop()
{
  SidecarEdits e;
  // Inside the picture: kept. Right at the edge: kept.
  CHECK(load(R"({"geometry":{"crop":[10,20,300,200]}})", &e) && e.crop == (CropRect{10, 20, 300, 200}));
  CHECK(load(R"({"geometry":{"crop":[100,100,300,200]}})", &e) && e.crop == (CropRect{100, 100, 300, 200}));
  // Sticking out, empty, malformed: dropped, never clamped into something else.
  for (const char *bad : {R"({"geometry":{"crop":[100,100,301,200]}})", R"({"geometry":{"crop":[0,0,0,10]}})",
                          R"({"geometry":{"crop":[0,0,10,-5]}})", R"({"geometry":{"crop":[0,0,10]}})",
                          R"({"geometry":{"crop":[0,0,10,10,10]}})", R"({"geometry":{"crop":["a",0,10,10]}})",
                          R"({"geometry":{"crop":"0,0,10,10"}})"})
    CHECK(load(bad, &e) && e.crop.empty());
  // The crop is measured on the turned picture: a quarter turn swaps the sides.
  CHECK(load(R"({"geometry":{"rotate":1,"crop":[0,0,300,400]}})", &e) && e.crop == (CropRect{0, 0, 300, 400}));
  CHECK(load(R"({"geometry":{"rotate":0,"crop":[0,0,300,400]}})", &e) && e.crop.empty());
  // And on the straightened one: with "keepFullCanvas" it can be larger than the original.
  CHECK(load(R"({"geometry":{"angle":45,"keepFullCanvas":true,"crop":[0,0,490,490]}})", &e) && !e.crop.empty());
  CHECK(load(R"({"geometry":{"angle":45,"keepFullCanvas":true,"crop":[0,0,500,500]}})", &e) && e.crop.empty()); // the canvas is 495 x 495
  CHECK(load(R"({"geometry":{"angle":45,"keepFullCanvas":false,"crop":[0,0,490,490]}})", &e) && e.crop.empty());
}

static void testCurvesMixGrade()
{
  SidecarEdits e;
  const char *good = R"({"curves":{"rgb":[[0,0],[100,160],[255,255]],"b":[[0,20],[255,235]]}})";
  CHECK(load(good, &e));
  CHECK(e.curves[0] == (CurvePoints{{0, 0}, {100, 160}, {255, 255}}) && e.curves[1] == identityCurve() &&
        e.curves[3] == (CurvePoints{{0, 20}, {255, 235}}));
  // Anything that is not a usable curve becomes a straight line.
  for (const char *bad : {R"([[0,0]])", R"([[0,0],[255,255],[300,0]])", R"([[5,0],[255,255]])", R"([[0,0],[250,255]])",
                          R"([[0,0],[1,5],[255,255]])", R"([[0,0],["x",1],[255,255]])", R"([[0,0],[1],[255,255]])", R"({"a":1})", "5"})
  {
    SidecarEdits c;
    CHECK(load(std::string(R"({"curves":{"rgb":)") + bad + "}}", &c) && c.curves[0] == identityCurve());
  }
  // y is clamped into range, a curve with too many points is refused.
  SidecarEdits c;
  CHECK(load(R"({"curves":{"rgb":[[0,-50],[255,999]]}})", &c) && c.curves[0] == (CurvePoints{{0, 0}, {255, 255}}));
  std::string many = R"({"curves":{"rgb":[)";
  for (size_t i = 0; i <= kCurveMaxPoints; ++i)
    many += (i ? "," : "") + std::string("[") + std::to_string(i * 2) + "," + std::to_string(i) + "]";
  many += "]}}";
  CHECK(load(many, &c) && c.curves[0] == identityCurve());

  // Color Mixer: clamped; a short row only fills what is there; a non-array row is ignored.
  CHECK(load(R"({"colorMix":{"hue":[999,-999,5],"sat":"x","lum":[1,2,3,4,5,6,7,8,9,10]}})", &c));
  CHECK(c.mix[mixIndex(MixChan::Hue, 0)] == static_cast<float>(kMixMax) && c.mix[mixIndex(MixChan::Hue, 1)] == static_cast<float>(kMixMin));
  CHECK(c.mix[mixIndex(MixChan::Hue, 2)] == 5.0f && c.mix[mixIndex(MixChan::Hue, 3)] == 0.0f);
  CHECK(!mixChanAnyNonZero(c.mix, MixChan::Saturation) && c.mix[mixIndex(MixChan::Luminance, 7)] == 8.0f);

  // Color Grading.
  CHECK(load(R"({"colorGrading":{"wheels":[{"hue":999,"sat":-4,"lum":500,"visible":false},"x",{"hue":-5}],"blending":500,"balance":-500}})", &c));
  CHECK(c.grade.wheel[0].hue == 359.0f && c.grade.wheel[0].sat == 0.0f && c.grade.wheel[0].lum == 100.0f && !c.grade.wheel[0].visible);
  CHECK(c.grade.wheel[1].neutral() && c.grade.wheel[2].hue == 0.0f && c.grade.blending == 100.0f && c.grade.balance == -100.0f);
}

static std::string maskJson(const std::string &parts, const std::string &extra = "")
{
  return R"({"masks":[{"parts":)" + parts + extra + "}]}";
}

static void testMasksAreValidated()
{
  SidecarEdits e;
  // Fields are clamped to what the UI could produce.
  CHECK(load(maskJson(R"([{"type":"radial","cx":50,"rx":0,"ry":99,"angle":900,"feather":-3}])"), &e));
  const RadialShape &r = std::get<RadialShape>(e.masks[0].parts[0].shape);
  CHECK(r.cx == 2.0f && r.rx == 0.001f && r.ry == 4.0f && r.angle == 360.0f && r.feather == 0.0f);
  CHECK(load(maskJson(R"([{"type":"linear","len":0,"angle":-999}])"), &e));
  const LinearShape &l = std::get<LinearShape>(e.masks[0].parts[0].shape);
  CHECK(l.len == 0.001f && l.angle == -360.0f);
  // Missing fields take the shape's defaults.
  CHECK(load(maskJson(R"([{"type":"radial"}])"), &e) && std::get<RadialShape>(e.masks[0].parts[0].shape) == RadialShape{});
  // A mask part of an unknown or missing kind is dropped; a mask left with no part is dropped.
  CHECK(load(maskJson(R"([{"type":"wavy"},{"type":"radial"},{"cx":1},5,"x"])"), &e) && e.masks.size() == 1 && e.masks[0].parts.size() == 1);
  CHECK(load(maskJson(R"([{"type":"wavy"}])"), &e) && e.masks.empty());
  CHECK(load(R"({"masks":[{"parts":[]},{"parts":"x"},{"enabled":true},7,{"parts":[{"type":"linear"}]}]})", &e) && e.masks.size() == 1);
  // Operations: the three names, anything else is Add.
  CHECK(load(maskJson(R"([{"type":"radial"},{"type":"linear","op":"subtract"},{"type":"linear","op":"intersect"},{"type":"linear","op":"zzz"},{"type":"linear","op":5}])"), &e));
  const auto &parts = e.masks[0].parts;
  CHECK(parts.size() == 5 && parts[0].op == MaskOp::Add && parts[1].op == MaskOp::Subtract && parts[2].op == MaskOp::Intersect &&
        parts[3].op == MaskOp::Add && parts[4].op == MaskOp::Add);
  // Limits.
  std::string many = "[";
  for (size_t i = 0; i < kMaxMaskParts + 5; ++i)
    many += (i ? "," : "") + std::string(R"({"type":"radial"})");
  many += "]";
  CHECK(load(maskJson(many), &e) && e.masks[0].parts.size() == kMaxMaskParts);
  std::string masks = R"({"masks":[)";
  for (size_t i = 0; i < kMaxMasks + 4; ++i)
    masks += (i ? "," : "") + std::string(R"({"parts":[{"type":"radial"}]})");
  CHECK(load(masks + "]}", &e) && e.masks.size() == kMaxMasks);
  // Mask-level fields.
  CHECK(load(maskJson(R"([{"type":"radial"}])", R"(,"name":"A\nB \u00e9 x","amount":250,"enabled":false,"invert":true)"), &e));
  CHECK(e.masks[0].name == "AB  x" && e.masks[0].amount == 100.0f && !e.masks[0].enabled && e.masks[0].invert);
  CHECK(load(maskJson(R"([{"type":"radial"}])", R"(,"amount":-5)"), &e) && e.masks[0].amount == 0.0f);
  CHECK(load(maskJson(R"([{"type":"radial"}])", R"(,"adjust":{"Exposure":500},"curves":{"g":[[0,0]]})"), &e));
  CHECK(e.masks[0].adj[static_cast<size_t>(AdjId::Exposure)] == kAdjustmentSpecs[static_cast<size_t>(AdjId::Exposure)].max &&
        e.masks[0].curves[2] == identityCurve());
}

static void testRectAndPolygon()
{
  // Round trip: written, read back equal, written again identical.
  SidecarEdits a;
  Mask m;
  RectShape r;
  r.cx = 0.3f;
  r.cy = 0.6f;
  r.rx = 0.25f;
  r.ry = 0.1f;
  r.angle = -33.0f;
  r.feather = 0.45f;
  PolygonShape p;
  p.pts = {{0.1f, 0.2f}, {0.8f, 0.15f}, {0.9f, 0.7f}, {0.4f, 0.95f}, {0.05f, 0.5f}};
  p.feather = 0.35f;
  m.parts.push_back({r, MaskOp::Add});
  m.parts.push_back({p, MaskOp::Subtract});
  a.masks.push_back(m);
  Mask few; // fewer than 3 points is kept (it just selects nothing)
  PolygonShape two;
  two.pts = {{0.2f, 0.2f}, {0.7f, 0.7f}};
  few.parts.push_back({two, MaskOp::Add});
  a.masks.push_back(few);
  const std::string text = sidecarEditsToJson(a).dump();
  SidecarEdits b;
  CHECK(load(text, &b));
  checkValid(b, 400, 300, "rect and polygon");
  CHECK(b.masks.size() == 2 && b.masks[0].parts.size() == 2);
  CHECK(std::get<RectShape>(b.masks[0].parts[0].shape) == r);
  CHECK(std::get<PolygonShape>(b.masks[0].parts[1].shape) == p);
  CHECK(b.masks[0].parts[1].op == MaskOp::Subtract);
  CHECK(std::get<PolygonShape>(b.masks[1].parts[0].shape) == two);
  CHECK(sidecarEditsToJson(b).dump() == text);
  const Json j = sidecarShapeToJson(MaskShape{r});
  CHECK(j.find("type")->asStr() == "rect");
  CHECK(sidecarShapeToJson(MaskShape{p}).find("type")->asStr() == "polygon");

  // Damaged input is clamped / skipped, never trusted.
  SidecarEdits e;
  CHECK(load(maskJson(R"([{"type":"rect","cx":50,"cy":-9,"rx":0,"ry":99,"angle":900,"feather":-3}])"), &e));
  const RectShape &q = std::get<RectShape>(e.masks[0].parts[0].shape);
  CHECK(q.cx == 2.0f && q.cy == -1.0f && q.rx == 0.001f && q.ry == 4.0f && q.angle == 360.0f && q.feather == 0.0f);
  CHECK(load(maskJson(R"([{"type":"rect"}])"), &e) && std::get<RectShape>(e.masks[0].parts[0].shape) == RectShape{});
  CHECK(load(maskJson(R"([{"type":"polygon","feather":9,"pts":[0.1,0.2,5,-5,"a",1,0.3,0.4,0.9]}])"), &e));
  const PolygonShape &g = std::get<PolygonShape>(e.masks[0].parts[0].shape);
  CHECK(g.feather == 1.0f);
  CHECK(g.pts.size() == 3 && g.pts[1].x == 2.0f && g.pts[1].y == -1.0f); // clamped
  CHECK(g.pts[2].x == 0.3f && g.pts[2].y == 0.4f);                       // the pair with "a" is skipped, the odd last number ignored
  CHECK(load(maskJson(R"([{"type":"polygon"}])"), &e) && std::get<PolygonShape>(e.masks[0].parts[0].shape) == PolygonShape{});
  CHECK(load(maskJson(R"([{"type":"polygon","pts":"nope"}])"), &e) && std::get<PolygonShape>(e.masks[0].parts[0].shape).pts.empty());
  // The point count is capped.
  std::string pts = R"([{"type":"polygon","pts":[)";
  for (size_t i = 0; i < kPolyMaxPoints + 50; ++i)
    pts += (i ? "," : "") + std::string("0.5,0.5");
  pts += "]}]";
  CHECK(load(maskJson(pts), &e) && std::get<PolygonShape>(e.masks[0].parts[0].shape).pts.size() == kPolyMaxPoints);
}

static void testBrushAndRanges()
{
  SidecarEdits e;
  CHECK(load(maskJson(R"([{"type":"brush","strokes":[
      {"size":9,"feather":9,"density":9,"flow":0,"erase":true,"pts":[0.1,0.2,0.3,0.4,0.9]},
      {"pts":[]},{"size":0.1},"x",{"pts":["a",1,2,3]},{"pts":[5,-5]}]}])"), &e));
  const BrushShape &b = std::get<BrushShape>(e.masks[0].parts[0].shape);
  CHECK(b.strokes.size() == 3); // empty / pointless strokes are dropped
  const BrushStroke &s = b.strokes[0];
  CHECK(s.erase && s.size == kBrushMaxSize && s.feather == 1.0f && s.density == 1.0f && s.flow == 0.01f);
  CHECK(s.pts.size() == 2 && s.pts[1].x == 0.3f && s.pts[1].y == 0.4f); // the odd last number is ignored
  // {"pts":["a",1,2,3]}: the pair ("a", 1) is skipped, (2, 3) is clamped to (2, 2); {"pts":[5,-5]} -> (2, -1).
  CHECK(b.strokes[1].pts.size() == 1 && b.strokes[1].pts[0].x == 2.0f && b.strokes[1].pts[0].y == 2.0f);
  CHECK(b.strokes[2].pts.size() == 1 && b.strokes[2].pts[0].x == 2.0f && b.strokes[2].pts[0].y == -1.0f);
  // Limits: strokes and points per stroke.
  std::string strokes = R"([{"type":"brush","strokes":[)";
  for (size_t i = 0; i < kBrushMaxStrokes + 3; ++i)
    strokes += (i ? "," : "") + std::string(R"({"pts":[0.5,0.5]})");
  strokes += "]}]";
  CHECK(load(maskJson(strokes), &e) && std::get<BrushShape>(e.masks[0].parts[0].shape).strokes.size() == kBrushMaxStrokes);
  std::string pts = R"([{"type":"brush","strokes":[{"pts":[)";
  for (size_t i = 0; i < kBrushMaxPoints * 2 + 8; ++i)
    pts += (i ? "," : "") + std::string("0.5");
  pts += "]}]}]";
  CHECK(load(maskJson(pts), &e) && std::get<BrushShape>(e.masks[0].parts[0].shape).strokes[0].pts.size() == kBrushMaxPoints);
  // Luminosity and Color Range.
  CHECK(load(maskJson(R"([{"type":"luminosity","lo":-1,"hi":5,"feather":2},{"type":"colorRange","set":true,"r":2,"g":-1,"b":0.5,"range":9,"feather":9}])"), &e));
  const LuminosityShape &lu = std::get<LuminosityShape>(e.masks[0].parts[0].shape);
  CHECK(lu.lo == 0.0f && lu.hi == 1.0f && lu.feather == 1.0f);
  const ColorRangeShape &cr = std::get<ColorRangeShape>(e.masks[0].parts[1].shape);
  CHECK(cr.set && cr.r == 1.0f && cr.g == 0.0f && cr.b == 0.5f && cr.range == 1.0f && cr.feather == 1.0f);
}

static void testObjectMaps()
{
  SidecarEdits e;
  const std::vector<uint8_t> px = {0, 0, 255, 255, 255, 9, 9, 9, 0, 0, 0, 0};
  const std::string good = mj::base64Encode(mj::rleEncode(px));
  auto json = [&](const std::string &w, const std::string &h, const std::string &data)
  {
    return maskJson(R"([{"type":"object","map":{"w":)" + w + R"(,"h":)" + h + R"(,"rle":")" + data + R"("},)" +
                    R"("prompts":[[0.5,0.5,true],[2,-2,false],[0.1],"x",[0.1,0.2],[0.2,0.3,0]],"feather":3,"grow":-3}])");
  };
  CHECK(load(json("4", "3", good), &e));
  const ObjectShape &o = std::get<ObjectShape>(e.masks[0].parts[0].shape);
  CHECK(o.map && o.map->w == 4 && o.map->h == 3 && o.map->a == px);
  CHECK(o.prompts.size() == 3 && o.prompts[1].x == 1.0f && o.prompts[1].y == 0.0f && !o.prompts[1].fg); // [0.1,0.2] and malformed ones skipped
  CHECK(o.prompts[2].x == 0.2f && !o.prompts[2].fg == false); // a non-bool flag defaults to foreground
  // A bad bitmap costs the bitmap, not the mask: the shape loads without one.
  for (const std::string &bad : {json("4", "3", "not base64!"), json("5", "3", good), json("0", "3", good), json("-1", "3", good),
                                 json("4", "3", mj::base64Encode({1, 7})), json("100000", "100000", good)})
  {
    SidecarEdits b;
    CHECK(load(bad, &b) && b.masks.size() == 1);
    CHECK(!std::get<ObjectShape>(b.masks[0].parts[0].shape).map);
  }
  // Too many prompts are cut off.
  std::string prompts = R"([{"type":"object","prompts":[)";
  for (size_t i = 0; i < kSidecarMaxPrompts + 10; ++i)
    prompts += (i ? "," : "") + std::string("[0.5,0.5,true]");
  prompts += "]}]";
  CHECK(load(maskJson(prompts), &e) && std::get<ObjectShape>(e.masks[0].parts[0].shape).prompts.size() == kSidecarMaxPrompts);
  CHECK(kSidecarMaxMapPixels == (int64_t{1} << 26) && kSidecarMaxPrompts == 256);
  // Writing: the same bitmap gives the same text again (it is memoized by id).
  auto m = std::make_shared<ObjectMap>();
  m->w = 4;
  m->h = 3;
  m->a = px;
  m->id = nextObjectMapId();
  ObjectShape os;
  os.map = m;
  const std::string t1 = sidecarShapeToJson(os).dump(), t2 = sidecarShapeToJson(os).dump();
  CHECK(t1 == t2 && sidecarMapData(*m) == good);
}

static void testFuzz()
{
  const std::string base = sidecarEditsToJson(rich()).dump();
  std::mt19937 rng(2024);
  const char *replacements[] = {"null", "true", "false", "\"x\"", "[]", "{}", "[1,2]", "1e30", "-1e30", "0", "-1", "999999999999", "0.5"};
  int loaded = 0;
  for (int iter = 0; iter < 2500; ++iter)
  {
    std::string t = base;
    const int kind = iter % 4;
    if (kind == 0)
    { // damage characters / cut the file
      for (int k = 1 + static_cast<int>(rng() % 6); k > 0; --k)
        t[rng() % t.size()] = static_cast<char>(rng() % 128);
      if (rng() % 3 == 0)
        t.resize(rng() % t.size());
    }
    else
    { // swap whole values for something of another type or size
      for (int k = 1 + static_cast<int>(rng() % 5); k > 0; --k)
      {
        std::vector<std::pair<size_t, size_t>> runs; // numbers, strings and literals
        for (size_t i = 0; i < t.size();)
        {
          const char c = t[i];
          size_t j = i + 1;
          if (c == '-' || (c >= '0' && c <= '9'))
            while (j < t.size() && (std::isdigit(static_cast<unsigned char>(t[j])) || t[j] == '.' || t[j] == 'e' || t[j] == '-' || t[j] == '+'))
              ++j;
          else if (c == '"')
          {
            while (j < t.size() && t[j] != '"')
              ++j;
            ++j;
            if (j < t.size() && t[j] == ':') // a key: keep it, only values are swapped
            {
              i = j;
              continue;
            }
          }
          else
          {
            i = j;
            continue;
          }
          runs.push_back({i, j - i});
          i = j;
        }
        if (runs.empty())
          break;
        const auto r = runs[rng() % runs.size()];
        t.replace(r.first, r.second, replacements[rng() % (sizeof replacements / sizeof *replacements)]);
      }
    }
    Json j;
    std::string err;
    if (!Json::parse(t, &j, &err))
      continue;
    SidecarEdits e;
    if (sidecarEditsFromJson(j, 400, 300, &e))
    {
      ++loaded;
      checkValid(e, 400, 300, "fuzz");
      if (tcheck::failures)
        return;
    }
  }
  CHECK(loaded > 1000); // most of the damaged files still parse and load, so the checks above ran on them
}

// ------------------------------------------------------------ sync a look
static std::string fileText(const SidecarEdits &e, int w, int h, int version = kSidecarVersion)
{
  Json img = Json::object();
  img.set("name", Json::str("a.jpg"));
  img.set("width", Json::num(w));
  img.set("height", Json::num(h));
  Json root = Json::object();
  root.set("app", Json::str("literoom"));
  root.set("version", Json::num(version));
  root.set("image", std::move(img));
  root.set("edits", sidecarEditsToJson(e));
  return root.dump();
}

// Reads a whole sidecar file (wrapper + edits) back.
static bool loadFile(const std::string &text, SidecarEdits *e, int w, int h)
{
  Json root;
  std::string err;
  if (!Json::parse(text, &root, &err) || !root.isObject())
    return false;
  const Json *edits = root.find("edits");
  return edits && sidecarEditsFromJson(*edits, w, h, e);
}

static void testSyncLook()
{
  AdjValues v{};
  v[static_cast<size_t>(AdjId::Exposure)] = 25.0f;
  CurveSet cs = identityCurves();
  cs[0] = CurvePoints{{0, 0}, {100, 140}, {255, 255}};
  MixValues mix{};
  mix[0] = 12.0f;
  ColorGrade grade;
  grade.blending = 71.0f;

  std::string out, err;

  // A picture with no sidecar yet: the size must be given.
  CHECK(!sidecarSyncLook("", "a.jpg", 0, 0, v, cs, mix, grade, &out, &err));
  CHECK(!err.empty());
  CHECK(sidecarSyncLook("", "a.jpg", 400, 300, v, cs, mix, grade, &out, &err));
  SidecarEdits fresh;
  CHECK(loadFile(out, &fresh, 400, 300));
  CHECK(fresh.values[static_cast<size_t>(AdjId::Exposure)] == 25.0f);
  CHECK(fresh.curves[0] == cs[0]);
  CHECK(fresh.mix[0] == 12.0f);
  CHECK(fresh.grade.blending == 71.0f);
  CHECK(fresh.crop.empty());
  CHECK(fresh.masks.empty());

  // A picture with edits: the look is replaced, geometry and masks stay.
  const SidecarEdits old = rich();
  const std::string oldText = fileText(old, 400, 300);
  CHECK(sidecarSyncLook(oldText, "a.jpg", 0, 0, v, cs, mix, grade, &out, &err)); // size from the file
  SidecarEdits e;
  CHECK(loadFile(out, &e, 400, 300));
  CHECK(e.values[static_cast<size_t>(AdjId::Exposure)] == 25.0f);
  CHECK(e.values[static_cast<size_t>(AdjId::Exposure)] != old.values[static_cast<size_t>(AdjId::Exposure)]);
  CHECK(e.curves[0] == cs[0]);
  CHECK(e.curves[2] == identityCurve()); // replaced, not merged
  CHECK(e.mix[0] == 12.0f);
  CHECK(e.grade.blending == 71.0f);
  CHECK(e.crop == old.crop && e.orient == old.orient && e.angle == old.angle && e.expand == old.expand);
  CHECK(e.masks.size() == old.masks.size());
  CHECK(!e.masks.empty());

  // Same size given and recorded: fine. A different size: refused.
  CHECK(sidecarSyncLook(oldText, "a.jpg", 400, 300, v, cs, mix, grade, &out, &err));
  out = "untouched";
  CHECK(!sidecarSyncLook(oldText, "a.jpg", 500, 300, v, cs, mix, grade, &out, &err));
  CHECK(out == "untouched");
  CHECK(!err.empty());

  // Files that can't be trusted are refused, never overwritten.
  CHECK(!sidecarSyncLook("not json", "a.jpg", 400, 300, v, cs, mix, grade, &out, &err));
  CHECK(!sidecarSyncLook("[1,2]", "a.jpg", 400, 300, v, cs, mix, grade, &out, &err));
  CHECK(!sidecarSyncLook(fileText(old, 400, 300, kSidecarVersion + 1), "a.jpg", 400, 300, v, cs, mix, grade, &out, &err));
  CHECK(!sidecarSyncLook("{\"version\":1,\"edits\":{}}", "a.jpg", 400, 300, v, cs, mix, grade, &out, &err)); // no image size
  CHECK(out == "untouched");

  // Syncing twice with the same look gives the same file.
  std::string again;
  CHECK(sidecarSyncLook(oldText, "a.jpg", 0, 0, v, cs, mix, grade, &out, &err));
  CHECK(sidecarSyncLook(out, "a.jpg", 0, 0, v, cs, mix, grade, &again, &err));
  CHECK(again == out);
}

// ------------------------------------------------ rating / flag / color label
static std::string metaFile(const SidecarEdits &e, const PhotoMeta &m, int w = 400, int h = 300)
{
  return sidecarFileText("a.jpg", w, h, sidecarEditsToJson(e), m);
}

static void testMeta()
{
  const PhotoMeta rated{4, PhotoFlag::Pick, 3};

  { // the wrapper: "meta" only when it isn't the default, always before "edits"
    const std::string plain = metaFile(SidecarEdits{}, PhotoMeta{});
    CHECK(plain.find("\"meta\"") == std::string::npos);
    const std::string with = metaFile(SidecarEdits{}, rated);
    const size_t pm = with.find("\"meta\""), pe = with.find("\"edits\"");
    CHECK(pm != std::string::npos && pe != std::string::npos && pm < pe);
    // the new writer gives the old layout when there is no meta
    CHECK(plain == fileText(SidecarEdits{}, 400, 300));
    Json root;
    std::string err;
    CHECK(Json::parse(with, &root, &err));
    CHECK(sidecarMetaFromRoot(root) == rated);
    CHECK(root.get("image").get("width").asNum(0) == 400);
    SidecarEdits back;
    CHECK(loadFile(with, &back, 400, 300)); // a rated file still loads as edits
  }

  { // sidecarScan: edited is false only for a readable, neutral file
    SidecarScan s = sidecarScan(metaFile(SidecarEdits{}, rated));
    CHECK(!s.edited);
    CHECK(s.meta == rated);
    s = sidecarScan(metaFile(SidecarEdits{}, PhotoMeta{}));
    CHECK(!s.edited && s.meta.isDefault());
    SidecarEdits e;
    e.values[static_cast<size_t>(AdjId::Exposure)] = 10.0f;
    s = sidecarScan(metaFile(e, rated));
    CHECK(s.edited && s.meta == rated);
    e = SidecarEdits{};
    e.crop = CropRect{1, 2, 30, 40};
    CHECK(sidecarScan(metaFile(e, PhotoMeta{})).edited);
    e = SidecarEdits{};
    e.masks.push_back(rich().masks[0]);
    CHECK(sidecarScan(metaFile(e, PhotoMeta{})).edited);
    // anything else counts as edited (as a file always did), with no meta
    for (const char *odd : {"", "not json", "[1,2]", "{}", "{\"edits\": 3}", "{\"meta\": {\"rating\": 2}}"})
    {
      s = sidecarScan(odd);
      CHECK(s.edited);
    }
    CHECK(sidecarScan("{\"meta\": {\"rating\": 2}}").meta.rating == 2);
    CHECK(sidecarScan("not json").meta.isDefault());
    CHECK(sidecarNeutralEditsDump() == sidecarEditsToJson(SidecarEdits{}).dump());
  }

  { // a hostile meta object is clamped, never trusted
    Json root;
    std::string err;
    CHECK(Json::parse(R"({"meta": {"rating": 500, "flag": "x", "label": 7}, "edits": {}})", &root, &err));
    const PhotoMeta hostile = sidecarMetaFromRoot(root);
    CHECK(hostile.rating == kMaxRating && hostile.flag == PhotoFlag::None && hostile.label == 0);
    CHECK(Json::parse(R"({"meta": 7})", &root, &err));
    CHECK(sidecarMetaFromRoot(root).isDefault());
    CHECK(Json::parse(R"({"edits": {}})", &root, &err));
    CHECK(sidecarMetaFromRoot(root).isDefault());
  }

  { // Sync Settings leaves the target's own rating, flag and label alone
    AdjValues v{};
    v[static_cast<size_t>(AdjId::Exposure)] = 25.0f;
    CurveSet cs = identityCurves();
    MixValues mix{};
    ColorGrade grade;
    std::string out, err;
    SidecarEdits old = rich();
    const std::string oldText = metaFile(old, rated);
    CHECK(sidecarSyncLook(oldText, "a.jpg", 0, 0, v, cs, mix, grade, &out, &err));
    Json root;
    CHECK(Json::parse(out, &root, &err));
    CHECK(sidecarMetaFromRoot(root) == rated);
    SidecarEdits now;
    CHECK(loadFile(out, &now, 400, 300));
    CHECK(now.values[static_cast<size_t>(AdjId::Exposure)] == 25.0f);
    CHECK(now.crop == old.crop); // geometry kept, as before
    // twice gives the same text, with the meta
    std::string again;
    CHECK(sidecarSyncLook(out, "a.jpg", 0, 0, v, cs, mix, grade, &again, &err));
    CHECK(again == out);
    // a file without meta stays without
    CHECK(sidecarSyncLook(fileText(old, 400, 300), "a.jpg", 0, 0, v, cs, mix, grade, &out, &err));
    CHECK(out.find("\"meta\"") == std::string::npos);
    // a new file has none
    CHECK(sidecarSyncLook("", "a.jpg", 400, 300, v, cs, mix, grade, &out, &err));
    CHECK(out.find("\"meta\"") == std::string::npos);
  }
}

static void testPatchMeta()
{
  const PhotoMeta rated{4, PhotoFlag::Pick, 3};
  std::string out, err;
  PhotoMeta got;

  { // no sidecar yet: a file is started from the picture's size, with neutral edits
    MetaPatch p;
    p.rating = 3;
    CHECK(sidecarPatchMeta("", "a.jpg", 400, 300, p, &out, &got, &err));
    CHECK(got == PhotoMeta({3, PhotoFlag::None, 0}));
    const SidecarScan s = sidecarScan(out);
    CHECK(!s.edited && s.meta == got); // a rating alone isn't "edits"
    SidecarEdits back;
    CHECK(loadFile(out, &back, 400, 300));
    CHECK(sidecarEditsToJson(back).dump() == sidecarNeutralEditsDump());
    // no size, no file
    out = "untouched";
    CHECK(!sidecarPatchMeta("", "a.jpg", 0, 0, p, &out, &got, &err) && !err.empty());
    CHECK(out == "untouched");
  }

  { // an existing file: only the named fields change, the edits are kept
    const SidecarEdits e = rich();
    const std::string old = metaFile(e, rated);
    MetaPatch p;
    p.flag = PhotoFlag::Reject;
    CHECK(sidecarPatchMeta(old, "a.jpg", 0, 0, p, &out, &got, &err));
    CHECK(got == PhotoMeta({4, PhotoFlag::Reject, 3}));
    CHECK(sidecarScan(out).meta == got);
    SidecarEdits back;
    CHECK(loadFile(out, &back, 400, 300));
    CHECK(sidecarEditsToJson(back).dump() == sidecarEditsToJson(e).dump());
    CHECK(sidecarScan(out).edited);
    // the size, when given, is checked against the file's
    CHECK(sidecarPatchMeta(old, "a.jpg", 400, 300, p, &out, &got, &err));
    CHECK(!sidecarPatchMeta(old, "a.jpg", 401, 300, p, &out, &got, &err));
    // applying the same patch twice gives the same text
    std::string again;
    CHECK(sidecarPatchMeta(out, "a.jpg", 0, 0, p, &again, &got, &err));
    CHECK(again == out);
  }

  { // clearing everything removes the "meta" key but keeps the file
    MetaPatch p;
    p.rating = 0;
    p.flag = PhotoFlag::None;
    p.label = 0;
    CHECK(sidecarPatchMeta(metaFile(rich(), rated), "a.jpg", 0, 0, p, &out, &got, &err));
    CHECK(got.isDefault());
    CHECK(out.find("\"meta\"") == std::string::npos);
    CHECK(out == fileText(rich(), 400, 300));
  }

  { // a file that can't be trusted is refused and never rewritten
    MetaPatch p;
    p.rating = 1;
    const std::string bad[] = {"not json", "[1,2]", fileText(rich(), 400, 300, kSidecarVersion + 1),
                               "{\"version\":1,\"image\":{\"width\":400,\"height\":300}}", // no edits
                               "{\"version\":1,\"edits\":{}}"};                           // no size
    for (const std::string &old : bad)
    {
      out = "untouched";
      err.clear();
      CHECK(!sidecarPatchMeta(old, "a.jpg", 0, 0, p, &out, &got, &err));
      CHECK(!err.empty() && out == "untouched");
    }
  }

  { // Sync Settings and a batch rating don't undo each other
    AdjValues v{};
    v[static_cast<size_t>(AdjId::Exposure)] = 25.0f;
    MetaPatch p;
    p.label = 2;
    std::string synced;
    CHECK(sidecarSyncLook(metaFile(rich(), rated), "a.jpg", 0, 0, v, identityCurves(), MixValues{}, ColorGrade{}, &synced, &err));
    CHECK(sidecarPatchMeta(synced, "a.jpg", 0, 0, p, &out, &got, &err));
    CHECK(got == PhotoMeta({4, PhotoFlag::Pick, 2}));
    SidecarEdits back;
    CHECK(loadFile(out, &back, 400, 300));
    CHECK(back.values[static_cast<size_t>(AdjId::Exposure)] == 25.0f);
  }
}

// ------------------------------------------------------------ named snapshots
static SnapshotList twoSnapshots()
{
  SnapshotList l;
  l.push_back({"Golden hour", rich()});
  SidecarEdits plain;
  plain.values[static_cast<size_t>(AdjId::Exposure)] = 15.0f;
  l.push_back({"Plain", plain});
  return l;
}

static std::string snapFile(const SidecarEdits &e, const PhotoMeta &m,
                            const SnapshotList &l, int w = 400, int h = 300)
{
  return sidecarFileText("a.jpg", w, h, sidecarEditsToJson(e), m, l);
}

static SnapshotList snapsOf(const std::string &text, int w = 400, int h = 300)
{
  Json root;
  std::string err;
  if (!Json::parse(text, &root, &err) || !root.isObject())
    return {};
  return sidecarSnapshotsFromJson(root.find("snapshots"), w, h);
}

static bool sameSnapshots(const SnapshotList &a, const SnapshotList &b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].name != b[i].name ||
        sidecarEditsToJson(a[i].edits).dump() != sidecarEditsToJson(b[i].edits).dump())
      return false;
  return true;
}

static void testSnapshots()
{
  const PhotoMeta rated{4, PhotoFlag::Pick, 3};
  const SnapshotList l = twoSnapshots();

  { // names: printable ASCII, trimmed, capped; blank falls back to "Snapshot"
    CHECK(sanitizeSnapshotName("  Day\x01 one  ") == "Day one");
    CHECK(sanitizeSnapshotName("caf\xC3\xA9") == "caf"); // non-ASCII bytes are dropped
    CHECK(sanitizeSnapshotName("   ").empty());
    CHECK(sanitizeSnapshotName(std::string(40, 'a')).size() == kSnapshotNameMax);
    // a cut that lands after a space leaves no trailing space
    CHECK(sanitizeSnapshotName(std::string(kSnapshotNameMax - 1, 'a') + " b") ==
          std::string(kSnapshotNameMax - 1, 'a'));
    CHECK(snapshotNameOrDefault("  ") == "Snapshot" && snapshotNameOrDefault("x") == "x");
  }

  { // layout: nothing without snapshots (the old layout, byte for byte);
    // with them, "snapshots" comes after "edits"
    CHECK(snapFile(rich(), PhotoMeta{}, SnapshotList{}) == fileText(rich(), 400, 300));
    CHECK(sidecarFileText("a.jpg", 400, 300, sidecarEditsToJson(rich()), rated) ==
          snapFile(rich(), rated, SnapshotList{}));
    const std::string with = snapFile(rich(), rated, l);
    const size_t pm = with.find("\"meta\""), pe = with.find("\"edits\""),
                 ps = with.find("\"snapshots\"");
    CHECK(pm != std::string::npos && pe != std::string::npos && ps != std::string::npos);
    CHECK(pm < pe && pe < ps);
    CHECK(snapFile(rich(), PhotoMeta{}, SnapshotList{}).find("snapshots") == std::string::npos);
  }

  { // round trip: names and every edit come back; the live edits are unaffected
    const std::string text = snapFile(rich(), rated, l);
    CHECK(sameSnapshots(snapsOf(text), l));
    SidecarEdits back;
    CHECK(loadFile(text, &back, 400, 300));
    CHECK(sidecarEditsToJson(back).dump() == sidecarEditsToJson(rich()).dump());
    // writing what was read gives the same text
    CHECK(snapFile(rich(), rated, snapsOf(text)) == text);
  }

  { // at most kMaxSnapshots, on the way out and on the way in
    SnapshotList many;
    for (size_t i = 0; i < kMaxSnapshots + 5; ++i)
      many.push_back({"s" + std::to_string(i), SidecarEdits{}});
    CHECK(snapsOf(snapFile(SidecarEdits{}, PhotoMeta{}, many)).size() == kMaxSnapshots);
    Json arr = Json::array();
    for (size_t i = 0; i < kMaxSnapshots + 5; ++i)
    {
      Json o = Json::object();
      o.set("name", Json::str("s"));
      o.set("edits", sidecarEditsToJson(SidecarEdits{}));
      arr.push(std::move(o));
    }
    CHECK(sidecarSnapshotsFromJson(&arr, 400, 300).size() == kMaxSnapshots);
  }

  { // nothing in the file is trusted
    auto entry = [](const char *name, Json edits)
    {
      Json o = Json::object();
      if (name)
        o.set("name", Json::str(name));
      o.set("edits", std::move(edits));
      return o;
    };
    Json wild = sidecarEditsToJson(SidecarEdits{});
    Json adj = Json::object();
    adj.set("Exposure", Json::num(1e9));
    wild.set("adjust", std::move(adj));
    Json geo = Json::object();
    Json crop = Json::array();
    for (double v : {0.0, 0.0, 9999.0, 9999.0})
      crop.push(Json::num(v));
    geo.set("crop", std::move(crop));
    Json bigCrop = sidecarEditsToJson(SidecarEdits{});
    bigCrop.set("geometry", std::move(geo));

    Json arr = Json::array();
    arr.push(entry("wild", wild));                             // kept, clamped
    arr.push(entry("big crop", bigCrop));                      // kept, crop dropped
    arr.push(Json::num(3));                                    // not an object: skipped
    Json noEdits = Json::object();
    noEdits.set("name", Json::str("none"));
    arr.push(std::move(noEdits));                              // no "edits": skipped
    arr.push(entry((std::string(500, 'x') + "\x01").c_str(), sidecarEditsToJson(SidecarEdits{})));
    arr.push(entry(nullptr, sidecarEditsToJson(SidecarEdits{}))); // no name
    arr.push(entry("  ", sidecarEditsToJson(SidecarEdits{})));  // blank name

    const SnapshotList got = sidecarSnapshotsFromJson(&arr, 400, 300);
    CHECK(got.size() == 5);
    if (got.size() == 5)
    {
      const size_t ex = static_cast<size_t>(AdjId::Exposure);
      CHECK(got[0].name == "wild" && got[0].edits.values[ex] == kAdjustmentSpecs[ex].max);
      CHECK(got[1].name == "big crop" && got[1].edits.crop.empty());
      CHECK(got[2].name.size() == kSnapshotNameMax);
      CHECK(got[3].name == "Snapshot" && got[4].name == "Snapshot");
    }
    CHECK(sidecarSnapshotsFromJson(nullptr, 400, 300).empty());
    Json notArray = Json::object();
    CHECK(sidecarSnapshotsFromJson(&notArray, 400, 300).empty());
    CHECK(sidecarSnapshotsFromJson(&arr, 0, 0).empty()); // no picture size: nothing loads
  }

  { // Sync Settings and a batch rating keep the picture's snapshots, each with
    // the look it captured; neither one writes them into a file that had none
    const std::string old = snapFile(rich(), rated, l);
    AdjValues v{};
    v[static_cast<size_t>(AdjId::Exposure)] = 25.0f;
    std::string out, err, again;
    CHECK(sidecarSyncLook(old, "a.jpg", 0, 0, v, identityCurves(), MixValues{}, ColorGrade{}, &out, &err));
    CHECK(sameSnapshots(snapsOf(out), l));
    SidecarEdits back;
    CHECK(loadFile(out, &back, 400, 300) && back.values[static_cast<size_t>(AdjId::Exposure)] == 25.0f);
    CHECK(sidecarSyncLook(out, "a.jpg", 0, 0, v, identityCurves(), MixValues{}, ColorGrade{}, &again, &err));
    CHECK(again == out);

    MetaPatch p;
    p.rating = 2;
    PhotoMeta got;
    CHECK(sidecarPatchMeta(old, "a.jpg", 0, 0, p, &out, &got, &err));
    CHECK(got.rating == 2 && sameSnapshots(snapsOf(out), l));

    CHECK(sidecarSyncLook("", "a.jpg", 400, 300, v, identityCurves(), MixValues{}, ColorGrade{}, &out, &err));
    CHECK(out.find("snapshots") == std::string::npos);

    // a file that can't be trusted is still refused, snapshots or not
    out = "untouched";
    CHECK(!sidecarSyncLook(old, "a.jpg", 500, 300, v, identityCurves(), MixValues{}, ColorGrade{}, &out, &err));
    CHECK(out == "untouched" && !err.empty());
  }

  { // snapshots are validated again when another picture's file is rewritten
    Json root;
    std::string err, out;
    CHECK(Json::parse(snapFile(rich(), PhotoMeta{}, twoSnapshots()), &root, &err));
    Json geo = Json::object();
    Json crop = Json::array();
    for (double v : {0.0, 0.0, 9999.0, 9999.0})
      crop.push(Json::num(v));
    geo.set("crop", std::move(crop));
    Json bad = sidecarEditsToJson(SidecarEdits{});
    bad.set("geometry", std::move(geo));
    Json o = Json::object();
    o.set("name", Json::str("bad"));
    o.set("edits", std::move(bad));
    Json arr = Json::array();
    arr.push(std::move(o));
    root.set("snapshots", std::move(arr));
    MetaPatch p;
    p.rating = 1;
    PhotoMeta got;
    CHECK(sidecarPatchMeta(root.dump(), "a.jpg", 0, 0, p, &out, &got, &err));
    const SnapshotList kept = snapsOf(out);
    CHECK(kept.size() == 1 && kept[0].name == "bad" && kept[0].edits.crop.empty());
  }

  { // the filmstrip's "has edits" dot follows the live edits only: a picture
    // whose edits are neutral is not "edited" just because it has snapshots
    CHECK(!sidecarScan(snapFile(SidecarEdits{}, PhotoMeta{}, l)).edited);
    CHECK(sidecarScan(snapFile(rich(), PhotoMeta{}, l)).edited);
    CHECK(sidecarScan(snapFile(SidecarEdits{}, rated, l)).meta == rated);
  }
}

static void testVirtualCopyNames()
{ // a virtual copy's path names its own sidecar (see virtual_copy.hpp)
  CHECK(sidecarFileOf("x/photo.jpg") == std::string("x/photo.jpg") + kSidecarExt);
  CHECK(sidecarFileOf("x/photo.jpg|vc2") == "x/photo.jpg.vc2.literoom.json");
  CHECK(sidecarFileOf(vcPath("x/photo.jpg", 7)) != sidecarFileOf("x/photo.jpg"));
  CHECK(sidecarFileOf("x/a|vcX.jpg") == std::string("x/a|vcX.jpg") + kSidecarExt); // not a marker
}

int main()
{
  testRoundTrip();
  testFileLayout();
  testSlidersAndGeometryAreClamped();
  testCrop();
  testCurvesMixGrade();
  testMasksAreValidated();
  testBrushAndRanges();
  testRectAndPolygon();
  testObjectMaps();
  testFuzz();
  testSyncLook();
  testMeta();
  testPatchMeta();
  testSnapshots();
  testVirtualCopyNames();
  return finish();
}