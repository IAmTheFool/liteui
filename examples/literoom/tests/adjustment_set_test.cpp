// Build: g++ -std=c++20 -I.. adjustment_set_test.cpp -o adjustment_set_test && ./adjustment_set_test
#include "edit/adjustment_set.hpp"
#include "check.hpp"

static void fresh(AdjustmentSet &s)
{
  s.reset();
  s.setOriginalSize(400, 300);
}
static Mask radialMask()
{
  Mask m;
  m.parts.push_back({RadialShape{}, MaskOp::Add});
  m.adj[static_cast<size_t>(AdjId::Exposure)] = 20.0f;
  return m;
}
static const std::string &label(const AdjustmentSet &s, size_t i) { return s.historyLabel(i); }
static const std::string &lastLabel(const AdjustmentSet &s) { return s.historyLabel(s.historySize() - 1); }

static void testFreshState()
{
  AdjustmentSet s;
  fresh(s);
  for (size_t i = 0; i < static_cast<size_t>(AdjId::Count); ++i)
    CHECK(s.get(static_cast<AdjId>(i)) == 0.0f);
  CHECK(s.crop().empty() && s.orient().identity() && s.angle() == 0.0f && !s.expand());
  for (size_t c = 0; c < kCurveChannelCount; ++c)
    CHECK(s.curve(static_cast<CurveChannel>(c)) == identityCurve());
  CHECK(!mixAnyNonZero(s.mixValues()) && !s.grade().anyActive() && s.masks().empty());
  CHECK(s.historySize() == 1 && s.historyIndex() == 0 && !s.canUndo() && !s.canRedo());
  CHECK(label(s, 0) == "Original" && label(s, 1).empty() && label(s, 999).empty());
  s.undo();
  s.redo(); // nothing to do, nothing breaks
  CHECK(s.historySize() == 1);
}

static void testSliderStepsAndCoalescing()
{
  AdjustmentSet s;
  fresh(s);
  s.set(AdjId::Exposure, 10.0f);
  s.set(AdjId::Exposure, 20.0f);
  s.set(AdjId::Exposure, 30.0f); // one drag: one step
  CHECK(s.historySize() == 2 && s.get(AdjId::Exposure) == 30.0f && lastLabel(s) == "Exposure +30");
  s.set(AdjId::Contrast, -12.0f); // another slider: a new step
  s.set(AdjId::Exposure, 40.0f);  // back to the first one: a new step too
  CHECK(s.historySize() == 4 && label(s, 2) == "Contrast -12" && label(s, 3) == "Exposure +40");
  s.undo();
  CHECK(s.get(AdjId::Exposure) == 30.0f && s.get(AdjId::Contrast) == -12.0f && s.historyIndex() == 2 && s.canRedo());
  s.set(AdjId::Exposure, 35.0f); // the first touch after an undo is a new step, dropping the redo tail
  CHECK(s.historySize() == 4 && s.historyIndex() == 3 && !s.canRedo() && lastLabel(s) == "Exposure +35");
  s.set(AdjId::Tint, 2.5f);
  CHECK(lastLabel(s) == "Tint +2.5"); // fractions show one decimal
  s.set(AdjId::Temperature, -0.02f);
  CHECK(lastLabel(s) == "Temperature +0" || lastLabel(s) == "Temperature -0"); // whole numbers show none
  s.set(AdjId::Highlights, 3.1f);
  CHECK(lastLabel(s) == "Highlights +3.1"); // 0.1 off a whole number is still shown
  s.set(AdjId::Shadows, 3.04f);
  CHECK(lastLabel(s) == "Shadows +3");      // 0.04 is not
  s.set(AdjId::Sepia, 60.0f);
  CHECK(lastLabel(s) == "Sepia +60");        // the Filters get History labels from the table too
  CHECK(kMaxUndoSteps == 200);
}

static void testUndoRedoAndJumps()
{
  AdjustmentSet s;
  fresh(s);
  s.set(AdjId::Exposure, 10.0f);
  s.set(AdjId::Contrast, 20.0f);
  s.set(AdjId::Tint, 30.0f);
  s.set(AdjId::Hue, 40.0f);
  CHECK(s.historySize() == 5 && s.historyIndex() == 4);
  s.undo();
  s.undo();
  CHECK(s.historyIndex() == 2 && s.get(AdjId::Contrast) == 20.0f && s.get(AdjId::Tint) == 0.0f && s.get(AdjId::Hue) == 0.0f);
  s.redo();
  CHECK(s.get(AdjId::Hue) == 0.0f && s.historyIndex() == 3);
  s.redo();
  CHECK(s.get(AdjId::Hue) == 40.0f && !s.canRedo());
  s.redo(); // past the end: nothing
  CHECK(s.historyIndex() == 4);
  s.goToHistory(1);
  CHECK(s.get(AdjId::Exposure) == 10.0f && s.get(AdjId::Contrast) == 0.0f && s.historyIndex() == 1);
  s.goToHistory(1);  // already there
  s.goToHistory(99); // past the end
  CHECK(s.historyIndex() == 1 && s.historySize() == 5);
  s.goToHistory(0);
  CHECK(s.get(AdjId::Exposure) == 0.0f && !s.canUndo());
  s.goToHistory(4);
  CHECK(s.get(AdjId::Hue) == 40.0f);
  // After undo + redo (or a jump) the next touch of the SAME slider is a new step,
  // not a continuation of the drag that came before.
  AdjustmentSet r;
  fresh(r);
  r.set(AdjId::Exposure, 10.0f);
  r.undo();
  r.redo();
  r.set(AdjId::Exposure, 20.0f);
  CHECK(r.historySize() == 3 && label(r, 1) == "Exposure +10" && label(r, 2) == "Exposure +20");
  r.set(AdjId::Contrast, 5.0f);
  r.goToHistory(r.historySize() - 1 - 1);
  r.goToHistory(r.historySize() - 1);
  r.set(AdjId::Contrast, 6.0f);
  CHECK(r.historySize() == 5);
  // Going back and editing replaces the future.
  s.goToHistory(2);
  s.set(AdjId::Whites, 5.0f);
  CHECK(s.historySize() == 4 && !s.canRedo() && lastLabel(s) == "Whites +5");
}

static void testHistoryIsCapped()
{
  AdjustmentSet s;
  fresh(s);
  const size_t n = kMaxUndoSteps + 25;
  for (size_t i = 1; i <= n; ++i)
    s.setCrop(CropRect{static_cast<int>(i), 0, 100, 100});
  CHECK(s.historySize() == kMaxUndoSteps + 1 && s.historyIndex() == kMaxUndoSteps);
  CHECK(label(s, 0) == "Original"); // the oldest kept step is always shown as the baseline
  for (size_t i = 0; i < kMaxUndoSteps; ++i)
    s.undo();
  CHECK(!s.canUndo() && s.crop().x == static_cast<int>(n - kMaxUndoSteps)); // earliest reachable state, not the true original
  s.goToHistory(s.historySize() - 1);
  CHECK(s.crop().x == static_cast<int>(n));
}

static void testGeometrySteps()
{
  AdjustmentSet s;
  fresh(s);
  s.setCrop(CropRect{10, 10, 100, 80});
  CHECK(lastLabel(s) == "Crop" && s.historySize() == 2);
  s.setCrop(CropRect{10, 10, 100, 80}); // same again: nothing
  CHECK(s.historySize() == 2);
  s.setCrop(CropRect{});
  CHECK(lastLabel(s) == "Remove Crop");
  s.setCrop(CropRect{5, 5, 50, 50});

  // setGeometry: orientation, angle and crop together, one step; identical values: nothing.
  const size_t before = s.historySize();
  s.setGeometry(Orient{1, true}, 3.0f, CropRect{1, 2, 30, 40});
  CHECK(s.historySize() == before + 1 && s.orient() == (Orient{1, true}) && s.angle() == 3.0f && s.crop() == (CropRect{1, 2, 30, 40}));
  CHECK(lastLabel(s) == "Rotate / Flip");
  s.setGeometry(Orient{1, true}, 3.0f, CropRect{1, 2, 30, 40});
  CHECK(s.historySize() == before + 1);
  s.undo();
  CHECK(s.orient().identity() && s.angle() == 0.0f && s.crop() == (CropRect{5, 5, 50, 50}));

  // The straighten angle: a new angle drops the crop (a new picture size); a drag folds into one step.
  s.setAngle(1.0f, true);
  s.setAngle(1.5f, true);
  s.setAngle(2.0f, true);
  CHECK(s.crop().empty() && s.angle() == 2.0f && lastLabel(s).rfind("Straighten +2.0", 0) == 0);
  const size_t steps = s.historySize();
  s.setAngle(2.0f, true); // unchanged: nothing
  CHECK(s.historySize() == steps);
  s.setAngle(-3.0f, false); // a discrete action: its own step each time
  s.setAngle(-4.0f, false);
  CHECK(s.historySize() == steps + 2 && lastLabel(s) == "Straighten -4.0\xC2\xB0");
  s.undo();
  s.undo();
  s.undo();
  CHECK(s.crop() == (CropRect{5, 5, 50, 50}) && s.angle() == 0.0f); // undo brings the crop back

  // The corner mode: only a picture-changing event when the angle is not 0.
  AdjustmentSet e;
  fresh(e);
  e.setCrop(CropRect{1, 1, 20, 20});
  e.setExpand(true);
  CHECK(e.expand() && e.crop() == (CropRect{1, 1, 20, 20}) && lastLabel(e) == "Straighten: Keep Canvas");
  e.setExpand(true); // same: nothing
  CHECK(e.historySize() == 3);
  e.setAngle(5.0f, false);
  e.setCrop(CropRect{1, 1, 20, 20});
  e.setExpand(false);
  CHECK(!e.expand() && e.crop().empty() && lastLabel(e) == "Straighten: Auto-Crop");
}

static void testCurveSteps()
{
  AdjustmentSet s;
  fresh(s);
  const CurvePoints a{{0, 0}, {100, 140}, {255, 255}}, b{{0, 0}, {100, 150}, {255, 255}}, c{{0, 0}, {80, 60}, {255, 255}};
  s.setCurve(a, true);
  s.setCurve(b, true); // same channel, same gesture
  CHECK(s.historySize() == 2 && s.curve() == b && lastLabel(s) == "Tone Curve: RGB");
  s.setCurve(CurveChannel::Red, c, true); // another channel: a new step
  CHECK(s.historySize() == 3 && lastLabel(s) == "Tone Curve: Red" && s.curve(CurveChannel::Red) == c);
  s.setCurve(CurveChannel::Red, c, true); // identical: nothing
  CHECK(s.historySize() == 3);
  s.endCurveEdit();
  s.setCurve(CurveChannel::Red, a, true); // after the gesture ended: a new step
  CHECK(s.historySize() == 4);
  s.setCurve(CurveChannel::Green, a, false);
  s.setCurve(CurveChannel::Blue, a, false);
  CHECK(s.historySize() == 6 && lastLabel(s) == "Tone Curve: Blue");
  s.resetAllCurves(); // all four at once, one step
  CHECK(s.historySize() == 7);
  for (size_t i = 0; i < kCurveChannelCount; ++i)
    CHECK(s.curve(static_cast<CurveChannel>(i)) == identityCurve());
  s.resetAllCurves(); // nothing left to reset
  CHECK(s.historySize() == 7);
}

static void testMixGradeAndMaskSteps()
{
  AdjustmentSet s;
  fresh(s);
  s.setMix(MixChan::Hue, 0, 10.0f);
  s.setMix(MixChan::Hue, 0, 20.0f);
  CHECK(s.historySize() == 2 && s.mix(MixChan::Hue, 0) == 20.0f && lastLabel(s) == "Color Mix: Red Hue +20");
  s.setMix(MixChan::Hue, 0, 20.0f); // unchanged
  s.setMix(MixChan::Luminance, 3, -5.0f);
  CHECK(s.historySize() == 3 && lastLabel(s) == "Color Mix: Green Luminance -5");
  s.setMix(MixChan::Saturation, 7, 9.0f);
  CHECK(lastLabel(s) == "Color Mix: Magenta Saturation +9");
  s.resetMix();
  CHECK(!mixAnyNonZero(s.mixValues()) && lastLabel(s) == "Color Mix: Reset");
  const size_t n = s.historySize();
  s.resetMix();
  CHECK(s.historySize() == n);

  ColorGrade g;
  g.wheel[0].sat = 20.0f;
  s.setGrade(g, 5);
  g.wheel[0].sat = 30.0f;
  s.setGrade(g, 5); // same control: folded
  CHECK(s.historySize() == n + 1 && lastLabel(s) == "Color Grading: Shadows");
  g.wheel[2].lum = 10.0f;
  s.setGrade(g, 6); // another control: new step
  CHECK(s.historySize() == n + 2 && lastLabel(s) == "Color Grading: Highlights");
  s.endGradeEdit();
  g.wheel[2].lum = 12.0f;
  s.setGrade(g, 6);
  CHECK(s.historySize() == n + 3);
  g.blending = 40.0f;
  s.setGrade(g, -1); // discrete
  CHECK(lastLabel(s) == "Color Grading: Blending");
  g.balance = 11.0f;
  s.setGrade(g, -1);
  CHECK(lastLabel(s) == "Color Grading: Balance");
  s.setGrade(g, -1); // identical
  CHECK(s.historySize() == n + 5);

  AdjustmentSet m;
  fresh(m);
  MaskSet one{radialMask()};
  m.setMasks(one, -1);
  CHECK(lastLabel(m) == "Add Radial Mask" && m.masks().size() == 1);
  m.setMasks(one, -1); // identical
  CHECK(m.historySize() == 2);
  one[0].adj[static_cast<size_t>(AdjId::Exposure)] = 30.0f;
  m.setMasks(one, 4);
  one[0].adj[static_cast<size_t>(AdjId::Exposure)] = 40.0f;
  m.setMasks(one, 4); // a drag of mask control 4: one step
  CHECK(m.historySize() == 3 && lastLabel(m) == "Mask 1: Exposure +40");
  m.endMaskEdit();
  one[0].adj[static_cast<size_t>(AdjId::Exposure)] = 45.0f;
  m.setMasks(one, 4);
  CHECK(m.historySize() == 4);
}

static void testMaskLabels()
{
  AdjustmentSet m;
  fresh(m);
  // "Add <kind> Mask" names the kind of the new mask, for every kind.
  const MaskShape shapes[] = {RadialShape{}, LinearShape{}, BrushShape{}, LuminosityShape{}, ColorRangeShape{}, ObjectShape{}};
  const char *names[] = {"Radial", "Linear", "Brush", "Luminosity", "Color Range", "Object"};
  for (int i = 0; i < 6; ++i)
  {
    AdjustmentSet t;
    fresh(t);
    Mask k;
    k.parts.push_back({shapes[i], MaskOp::Add});
    t.setMasks(MaskSet{k}, -1);
    CHECK(lastLabel(t) == std::string("Add ") + names[i] + " Mask");
  }
  {
    AdjustmentSet t;
    fresh(t);
    Mask lin;
    lin.parts.push_back({LinearShape{}, MaskOp::Add});
    Mask brush;
    brush.parts.push_back({BrushShape{}, MaskOp::Add});
    t.setMasks(MaskSet{lin}, -1);
    t.setMasks(MaskSet{lin, brush}, -1);
    CHECK(label(t, 1) == "Add Linear Mask" && label(t, 2) == "Add Brush Mask"); // the one just added, the last
  }
  MaskSet set{radialMask()};
  Mask second = radialMask();
  second.adj[static_cast<size_t>(AdjId::Exposure)] = 5.0f;
  set.push_back(second);
  m.setMasks(set, -1);
  CHECK(label(m, 1) == "Add Radial Mask");
  auto edit = [&](auto &&fn, const char *expect)
  {
    MaskSet next = m.masks();
    fn(next[0]);
    m.setMasks(next, -1);
    CHECK(lastLabel(m) == expect);
  };
  edit([](Mask &k) { k.name = "Sky"; }, "Mask 1: Rename");
  edit([](Mask &k) { k.adj[static_cast<size_t>(AdjId::Contrast)] = 15.0f; }, "Mask 1: Contrast +15");
  edit([](Mask &k) { k.amount = 50.0f; }, "Mask 1: Amount 50");
  edit([](Mask &k) { k.mix[0] = 5.0f; }, "Mask 1: Color Mix");
  edit([](Mask &k) { k.grade.wheel[0].sat = 5.0f; }, "Mask 1: Color Grading");
  edit([](Mask &k) { k.curves[0] = CurvePoints{{0, 0}, {90, 110}, {255, 255}}; }, "Mask 1: Curve");
  edit([](Mask &k) { k.enabled = false; }, "Mask 1: Off");
  edit([](Mask &k) { k.enabled = true; }, "Mask 1: On");
  edit([](Mask &k) { k.invert = true; }, "Mask 1: Invert");
  edit([](Mask &k) { k.parts.push_back({LinearShape{}, MaskOp::Subtract}); }, "Mask 1: Parts");
  edit([](Mask &k) { std::get<RadialShape>(k.parts[0].shape).cx = 0.9f; }, "Mask 1: Edit");
  // Swapping two neighbors is "Reorder", not a change of the first one's sliders.
  MaskSet swapped = m.masks();
  std::swap(swapped[0], swapped[1]);
  m.setMasks(swapped, -1);
  CHECK(lastLabel(m) == "Reorder Masks");
  MaskSet fewer = m.masks();
  fewer.pop_back();
  m.setMasks(fewer, -1);
  CHECK(lastLabel(m) == "Delete Mask");
}

static void testVersionAndLabelCache()
{
  AdjustmentSet s;
  fresh(s);
  const uint64_t v0 = s.version();
  s.set(AdjId::Exposure, 10.0f);
  const uint64_t v1 = s.version();
  CHECK(v1 > v0);
  s.set(AdjId::Exposure, 20.0f);
  CHECK(s.version() > v1); // even a folded drag step is a change
  const uint64_t v2 = s.version();
  s.undo();
  CHECK(s.version() > v2);
  const uint64_t v3 = s.version();
  s.redo();
  CHECK(s.version() > v3);
  const uint64_t v4 = s.version();
  CHECK(lastLabel(s) == "Exposure +20"); // reading labels changes nothing
  CHECK(s.version() == v4);
  s.reset();
  CHECK(s.version() > v4);
  // A drag that overwrites a step leaves no stale label behind.
  s.set(AdjId::Exposure, 10.0f);
  CHECK(lastLabel(s) == "Exposure +10");
  s.set(AdjId::Exposure, 55.0f);
  CHECK(lastLabel(s) == "Exposure +55" && s.historySize() == 2);
  // No-ops do not count as changes.
  const uint64_t before = s.version();
  s.setCrop(CropRect{});
  s.setExpand(false);
  s.setAngle(0.0f, false);
  s.setGeometry(Orient{}, 0.0f, CropRect{});
  s.resetMix();
  s.resetAllCurves();
  CHECK(s.version() == before);
}

static void testJson()
{
  AdjustmentSet a;
  fresh(a);
  a.set(AdjId::Exposure, 25.0f);
  a.set(AdjId::Texture, -30.0f);
  a.set(AdjId::Vintage, 45.0f);
  a.setGeometry(Orient{1, false}, 4.5f, CropRect{});
  a.setCurve(CurveChannel::Blue, CurvePoints{{0, 0}, {120, 100}, {255, 255}}, false);
  a.setMix(MixChan::Saturation, 2, -15.0f);
  ColorGrade g;
  g.wheel[1].hue = 200.0f;
  g.wheel[1].sat = 33.0f;
  a.setGrade(g, -1);
  a.setMasks(MaskSet{radialMask()}, -1);
  const std::string text = a.toJson().dump();
  // A Filter is saved under its label, and one at 0 is left out like every other slider.
  CHECK(text.find("\"Vintage\"") != std::string::npos && text.find("\"Sepia\"") == std::string::npos);

  AdjustmentSet b;
  fresh(b);
  b.set(AdjId::Hue, 77.0f); // whatever was there is replaced
  mj::Json j;
  std::string err;
  CHECK(mj::Json::parse(text, &j, &err) && b.loadJson(j));
  CHECK(b.get(AdjId::Exposure) == 25.0f && b.get(AdjId::Texture) == -30.0f && b.get(AdjId::Hue) == 0.0f);
  CHECK(b.get(AdjId::Vintage) == 45.0f && b.get(AdjId::Sepia) == 0.0f);
  CHECK(b.orient() == (Orient{1, false}) && b.angle() == 4.5f && b.curve(CurveChannel::Blue) == a.curve(CurveChannel::Blue));
  CHECK(b.mix(MixChan::Saturation, 2) == -15.0f && b.grade() == a.grade() && b.masks().size() == 1);
  CHECK(b.toJson().dump() == text);
  // The undo history starts again from what was loaded.
  CHECK(b.historySize() == 1 && !b.canUndo() && label(b, 0) == "Original");
  b.set(AdjId::Hue, 5.0f);
  b.undo();
  CHECK(b.get(AdjId::Exposure) == 25.0f && b.get(AdjId::Hue) == 0.0f);
  // An unusable file changes nothing and says so.
  const std::string keep = b.toJson().dump();
  mj::Json arr;
  CHECK(mj::Json::parse("[1,2]", &arr, &err) && !b.loadJson(arr) && b.toJson().dump() == keep);
  // Masks are loaded against the picture's size; a crop outside it is dropped.
  AdjustmentSet c;
  c.reset();
  c.setOriginalSize(100, 100);
  mj::Json cj;
  CHECK(mj::Json::parse(R"({"geometry":{"crop":[0,0,300,300]}})", &cj, &err) && c.loadJson(cj) && c.crop().empty());
}

static void testBuildTables()
{
  AdjustmentSet s;
  fresh(s);
  const auto id = s.buildLut();
  for (int i = 0; i < 256; ++i)
    CHECK(id[static_cast<size_t>(i)] == i);
  RenderParams rp = s.buildRenderParams();
  CHECK(rp.luts[0] == id && rp.luts[1] == id && rp.luts[2] == id);
  CHECK(!rp.color.active && !rp.fx.any() && rp.masks.empty());
  CHECK(!s.buildColorPass().active);

  // Tone sliders, then the master curve, then each channel's curve.
  s.set(AdjId::Exposure, 40.0f);
  AdjValues tone{};
  tone[static_cast<size_t>(AdjId::Exposure)] = 40.0f;
  CHECK(s.buildLut() == toneLutFromValues(tone, nullptr));
  const CurvePoints master{{0, 0}, {100, 160}, {255, 255}}, red{{0, 0}, {128, 90}, {255, 255}};
  s.setCurve(CurveChannel::Master, master, false);
  const auto masterLut = curveToLut(master);
  CHECK(s.buildLut() == toneLutFromValues(tone, &masterLut));
  s.setCurve(CurveChannel::Red, red, false);
  const auto base = s.buildLut();
  const auto ch = s.buildChannelLuts();
  const auto redLut = curveToLut(red);
  for (int i = 0; i < 256; ++i)
  {
    CHECK(ch[1][static_cast<size_t>(i)] == base[static_cast<size_t>(i)] && ch[2][static_cast<size_t>(i)] == base[static_cast<size_t>(i)]);
    CHECK(ch[0][static_cast<size_t>(i)] == std::clamp(static_cast<int>(std::lround(redLut[base[static_cast<size_t>(i)]])), 0, 255));
  }
  CHECK(s.buildRenderParams().luts == ch);

  // Color and detail sliders land in their own parts of the parameters.
  AdjustmentSet t;
  fresh(t);
  t.set(AdjId::Temperature, 30.0f);
  t.set(AdjId::Clarity, 40.0f);
  const RenderParams r2 = t.buildRenderParams();
  CHECK(r2.color.active && r2.fx.any() && r2.luts[0] == id && r2.masks.empty());
  CHECK(t.buildColorPass().active);
  AdjustmentSet u;
  fresh(u);
  u.setMix(MixChan::Hue, 1, 20.0f);
  CHECK(u.buildColorPass().mix && u.buildRenderParams().color.active);
  u.setMasks(MaskSet{radialMask()}, -1);
  CHECK(u.buildRenderParams().masks.size() == 1);
}

static void testFilterSlidersReachTheColorPass()
{
  // A Filter alone makes the global color pass active, and touches no other stage.
  AdjustmentSet s;
  fresh(s);
  s.set(AdjId::Greyscale, 100.0f);
  const ColorPass cp = s.buildColorPass();
  CHECK(cp.active && cp.filters.any && cp.filters.grey == 1.0f);
  CHECK(!cp.mix && !cp.grade && !cp.hue && !cp.colorize);
  const RenderParams rp = s.buildRenderParams();
  CHECK(rp.color.active && rp.color.filters.any && !rp.fx.any() && rp.masks.empty());
  for (int i = 0; i < 256; ++i)
    CHECK(rp.luts[0][static_cast<size_t>(i)] == i); // the tone tables are untouched
  s.set(AdjId::Greyscale, 0.0f);
  CHECK(!s.buildColorPass().active); // back to neutral: the fast path again

  // The global edit and a mask must read the Filters the same way (two call sites of makeColorPass).
  AdjustmentSet t;
  fresh(t);
  Mask m = radialMask();
  m.adj = AdjValues{};
  float v = 15.0f;
  for (AdjId id : {AdjId::Greyscale, AdjId::Sepia, AdjId::Vintage, AdjId::Fade, AdjId::Negative, AdjId::Posterize, AdjId::Solarize})
  {
    t.set(id, v);
    m.adj[static_cast<size_t>(id)] = v;
    v += 12.0f;
  }
  t.setMasks(MaskSet{m}, -1);
  const RenderParams r2 = t.buildRenderParams();
  CHECK(r2.masks.size() == 1 && r2.masks[0].color.active);
  const FilterPass &g = r2.color.filters, &l = r2.masks[0].color.filters;
  CHECK(g.any && l.any && g.grey == l.grey && g.sepia == l.sepia && g.vintage == l.vintage && g.fade == l.fade);
  CHECK(g.negative == l.negative && g.solarize == l.solarize && g.posterLevels == l.posterLevels);
}

static void testMasksFollowTheGeometry()
{
  AdjustmentSet s;
  fresh(s);
  Mask m = radialMask();
  RadialShape r;
  r.cx = 0.1f;
  r.cy = 0.1f;
  m.parts[0].shape = r;
  s.setMasks(MaskSet{m}, -1);
  const RadialShape flat = std::get<RadialShape>(s.buildRenderParams().masks[0].parts[0].shape);
  CHECK(flat.cx == 0.1f && flat.cy == 0.1f); // no geometry: stored as drawn
  s.setGeometry(Orient{1, false}, 0.0f, CropRect{}); // a quarter turn clockwise: top-left -> top-right
  const RadialShape turned = std::get<RadialShape>(s.buildRenderParams().masks[0].parts[0].shape);
  CHECK(turned.cx > 0.8f && turned.cy < 0.2f);
  CHECK(s.geoFrame().srcW == 300 && s.geoFrame().srcH == 400);
}

static void testMaskTablesMatchTheGlobalOnes()
{
  // buildMaskPasses keeps its own copy of the table recipe ("same order as
  // buildChannelLuts"): the same values and curves on a mask must give the same tables.
  AdjustmentSet s;
  fresh(s);
  Mask m = radialMask();
  m.adj = AdjValues{};
  for (AdjId id : {AdjId::Exposure, AdjId::Contrast, AdjId::Highlights, AdjId::Shadows, AdjId::Whites, AdjId::Blacks, AdjId::Brightness})
  {
    const float v = kAdjustmentSpecs[static_cast<size_t>(id)].max * 0.35f;
    s.set(id, v);
    m.adj[static_cast<size_t>(id)] = v;
  }
  const CurvePoints master{{0, 0}, {90, 130}, {255, 255}}, green{{0, 10}, {255, 240}};
  s.setCurve(CurveChannel::Master, master, false);
  s.setCurve(CurveChannel::Green, green, false);
  m.curves[0] = master;
  m.curves[2] = green;
  s.setMasks(MaskSet{m}, -1);
  const RenderParams rp = s.buildRenderParams();
  CHECK(rp.masks.size() == 1 && rp.masks[0].tone);
  CHECK(rp.masks[0].luts == rp.luts);
}

int main()
{
  testFreshState();
  testSliderStepsAndCoalescing();
  testUndoRedoAndJumps();
  testHistoryIsCapped();
  testGeometrySteps();
  testCurveSteps();
  testMixGradeAndMaskSteps();
  testMaskLabels();
  testVersionAndLabelCache();
  testJson();
  testBuildTables();
  testFilterSlidersReachTheColorPass();
  testMasksFollowTheGeometry();
  testMaskTablesMatchTheGlobalOnes();
  return finish();
}