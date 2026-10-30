// lut_pane_test.cpp - the LUTs pane's logic, without a window.
//
// Like golden_render_test.cpp, this compiles src/main.cpp itself into the test (its main() renamed:
// everything in it is static, so this is the only way to reach it) and drives the same functions the
// pane's buttons and the per-frame poll call: refreshLuts, pollLutPreviews, applyLutRow, removeLut,
// importLutPaths, the status and detail texts. No view is built and nothing is painted; the file
// picker is never opened (importLuts, the thin wrapper around it, is not called).
//
// A real LUT library in a temp folder, the real preview worker, the real renderer. Every wait has a
// timeout, so a stuck worker fails the test instead of hanging it.
#ifdef LUT_PANE_HARNESS // (used only to run the test body against an extract of main.cpp)
#include LUT_PANE_HARNESS
#else
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"
#include "mask/sam_segmenter.hpp"
#include "io/exif.hpp"
#include "io/minijson.hpp"
#include "detail/detail_layer.hpp"
#include "detail/neighbor_fx.hpp"
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
#define main literoom_app_main
#include "main.cpp"
#undef main
#endif

#include "check.hpp"

#include <charconv>
#include <chrono>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
static constexpr auto kWait = 20s;

// ---------------------------------------------------------------- helpers
struct TempDir
{
  fs::path dir;
  TempDir()
  {
    static int n = 0;
    dir = fs::temp_directory_path() /
          ("literoom_lutpane_test_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
           std::to_string(n++));
    fs::create_directories(dir);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::string root() const { return u8ToString(dir); }
  std::string path(const std::string &name) const { return u8ToString(dir / fsPath(name)); }
};

static void writeText(const std::string &path, const std::string &text)
{
  fs::create_directories(fsPath(path).parent_path());
  std::ofstream f(fsPath(path), std::ios::binary);
  f << text;
}

template <class F>
static std::string cubeOf(int n, F f)
{
  std::string s = "LUT_3D_SIZE " + std::to_string(n) + "\n";
  char buf[40];
  const auto num = [&](double v)
  {
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 6);
    s.append(buf, r.ptr);
  };
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r)
      {
        double o[3];
        f(r / double(n - 1), g / double(n - 1), b / double(n - 1), o);
        num(o[0]);
        s += ' ';
        num(o[1]);
        s += ' ';
        num(o[2]);
        s += '\n';
      }
  return s;
}
static std::string invertCube(int n = 3)
{
  return cubeOf(n, [](double r, double g, double b, double *o)
                { o[0] = 1 - r; o[1] = 1 - g; o[2] = 1 - b; });
}
static std::string warmCube(int n = 5)
{
  return cubeOf(n, [](double r, double g, double b, double *o)
                { o[0] = std::min(1.0, r * 1.1 + 0.05); o[1] = g; o[2] = b * 0.8; });
}
static std::string sameCube(int n = 3)
{
  return cubeOf(n, [](double r, double g, double b, double *o)
                { o[0] = r; o[1] = g; o[2] = b; });
}

static std::shared_ptr<CanvasImage> makePicture(int w, int h)
{
  auto img = std::make_shared<CanvasImage>(w, h);
  img->pixels.resize(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *p = &img->pixels[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
      p[1] = static_cast<uint8_t>(y * 255 / std::max(1, h - 1));
      p[2] = static_cast<uint8_t>((x * 5 + y * 2) % 256);
      p[3] = 255;
    }
  return img;
}

// Calls the per-frame poll until `done()` holds (the worker needs a moment); false on timeout.
template <class Pred>
static bool pump(Pred done)
{
  const auto t0 = std::chrono::steady_clock::now();
  for (;;)
  {
    pollLutPreviews();
    if (done())
      return true;
    if (std::chrono::steady_clock::now() - t0 > kWait)
      return false;
    std::this_thread::sleep_for(2ms);
  }
}
static bool allSettled()
{
  for (const LutRow &r : gLuts)
    if (!r.thumb && r.error.empty())
      return false;
  return !gLuts.empty();
}
static const LutRow *rowNamed(const std::string &name)
{
  for (const LutRow &r : gLuts)
    if (r.entry.name == name)
      return &r;
  return nullptr;
}
static size_t indexOf(const std::string &name)
{
  for (size_t i = 0; i < gLuts.size(); ++i)
    if (gLuts[i].entry.name == name)
      return i;
  return ~size_t{0};
}
static uint64_t fileHash(const std::string &path)
{
  std::vector<uint8_t> b;
  std::string err;
  return readLutBytes(path, &b, &err) ? lutHashBytes(b.data(), b.size()) : 0;
}

// ---------------------------------------------------------------- the test
static void testLutPane()
{
  TempDir lib, other;
  lutDirOverride() = lib.root();
  dropLutCache();
  writeText(lutPath(lib.root(), "A invert"), invertCube());
  writeText(lutPath(lib.root(), "B warm"), warmCube());
  writeText(lutPath(lib.root(), "C SLog3 look"), sameCube()); // the name says log
  writeText(lutPath(lib.root(), "D broken"), "LUT_3D_SIZE 2\n0 0 0\n1 1\n");

  // The open picture.
  const auto pic = makePicture(300, 200);
  state.adjust.reset();
  state.adjust.setOriginalSize(300, 200);
  state.original = pic;
  state.source = pic;
  state.previewBase = nullptr;
  state.dragging = false;
  state.activeActivity = kLutsActivity;
  gLutsLoaded = false;

  // ---- the list ----
  refreshLuts();
  CHECK(gLutsLoaded && gLuts.size() == 4);
  CHECK(gLuts.size() == 4 && gLuts[0].entry.name == "A invert" && gLuts[1].entry.name == "B warm" &&
        gLuts[2].entry.name == "C SLog3 look" && gLuts[3].entry.name == "D broken");

  // ---- the previews arrive (the pane is open and a picture is open) ----
  CHECK(pump(allSettled));
  const LutRow *a = rowNamed("A invert"), *b = rowNamed("B warm"), *c = rowNamed("C SLog3 look"), *d = rowNamed("D broken");
  CHECK(a && b && c && d);
  if (!(a && b && c && d))
    return;
  CHECK(a->thumb && b->thumb && c->thumb && a->error.empty() && a->entry.size == 3 && b->entry.size == 5);
  CHECK(a->thumb && b->thumb && a->thumb->pixels != b->thumb->pixels);
  CHECK(a->entry.hash == fileHash(a->entry.path) && a->entry.hash != 0);
  CHECK(!d->thumb && !d->error.empty() && d->entry.size == 0 && d->entry.hash == 0); // a bad file is just a bad row
  CHECK(c->warning.size() > 0 && a->warning.empty() && b->warning.empty());
  CHECK(lutRowDetail(indexOf("A invert")) == "3\u00B3 points");
  CHECK(lutRowDetail(indexOf("B warm")) == "5\u00B3 points");
  CHECK(lutRowDetail(indexOf("C SLog3 look")) == "3\u00B3 points  \u2022  may expect camera-log input");
  CHECK(lutRowDetail(indexOf("D broken")).rfind("Can't be read: ", 0) == 0 &&
        lutRowDetail(indexOf("D broken")).find("D broken") == std::string::npos); // the row already names the file
  CHECK(lutRowDetail(99).empty());

  // Nothing changed: no new batch.
  const uint64_t serial0 = gLutSerial;
  pollLutPreviews();
  pollLutPreviews();
  CHECK(gLutSerial == serial0);

  // ---- applying a LUT ----
  CHECK(lutCurrentText() == "No LUT on this picture");
  state.dirty = false;
  applyLutRow(indexOf("B warm"));
  CHECK(state.adjust.lut().name == "B warm" && state.adjust.lut().hash == fileHash(b->entry.path));
  CHECK(state.adjust.get(AdjId::LutAmount) == 100.0f && state.adjust.lutStatus() == LutStatus::Ok);
  CHECK(state.statusMsg == "Applied LUT: B warm" && !state.statusIsError);
  CHECK(state.adjustDirty && state.dirty); // the render and the repaint are asked for
  CHECK(lutCurrentText() == "LUT: B warm");
  CHECK(state.adjust.buildRenderParams().color.lut.any());
  pollLutPreviews(); // the LUTs pane's own change does not make the previews stale
  CHECK(gLutSerial == serial0);

  // The very same LUT again: nothing happens.
  state.adjustDirty = false;
  applyLutRow(indexOf("B warm"));
  CHECK(state.statusMsg == "Already using the LUT B warm" && !state.adjustDirty);
  // A broken row can't be applied, and says why.
  applyLutRow(indexOf("D broken"));
  CHECK(state.statusIsError && state.statusMsg.rfind("Can't use D broken: ", 0) == 0 &&
        state.adjust.lut().name == "B warm");
  // Out-of-range rows and a busy drag are ignored.
  applyLutRow(1000);
  state.dragging = true;
  applyLutRow(indexOf("A invert"));
  state.dragging = false;
  CHECK(state.adjust.lut().name == "B warm");

  // Another LUT replaces it, keeping the amount the user had set.
  setLutAmount(60.0f); // as the slider does
  applyLutRow(indexOf("A invert"));
  CHECK(state.adjust.lut().name == "A invert" && state.adjust.get(AdjId::LutAmount) == 60.0f);
  pollLutPreviews();
  CHECK(gLutSerial == serial0); // still current: the slider and the pick are the pane's own changes

  // ---- an edit made elsewhere makes the previews stale ----
  state.adjust.set(AdjId::Exposure, 15.0f);
  state.dragging = true; // ...but not in the middle of a drag
  pollLutPreviews();
  CHECK(gLutSerial == serial0);
  state.dragging = false;
  pollLutPreviews();
  CHECK(gLutSerial == serial0 + 1);
  const uint64_t pv = gLuts[0].paintVersion;
  CHECK(pump([&]
             { return gLuts[0].paintVersion != pv; })); // the new previews arrive
  const uint64_t serial1 = gLutSerial;
  // The pane's own change made BEFORE the stale one was handled does not hide it.
  state.adjust.set(AdjId::Contrast, 10.0f); // elsewhere
  applyLutRow(indexOf("B warm"));           // the pane
  pollLutPreviews();
  CHECK(gLutSerial == serial1 + 1);
  CHECK(pump(allSettled));

  // A closed pane or no picture: nothing is started.
  state.adjust.set(AdjId::Contrast, 20.0f);
  state.activeActivity = 0;
  pollLutPreviews();
  CHECK(gLutSerial == serial1 + 1);
  state.activeActivity = kLutsActivity;
  const auto keepOriginal = state.original;
  state.original = nullptr;
  pollLutPreviews();
  CHECK(gLutSerial == serial1 + 1);
  state.original = keepOriginal;
  pollLutPreviews(); // back: it catches up
  CHECK(gLutSerial == serial1 + 2);
  CHECK(pump(allSettled));

  // A different picture: the old previews are dropped at once, new ones come.
  const auto pic2 = makePicture(160, 120);
  state.source = pic2;
  state.original = pic2;
  pollLutPreviews();
  CHECK(gLutSerial == serial1 + 3);
  CHECK(!gLuts[0].thumb && !gLuts[1].thumb);
  CHECK(pump(allSettled));
  CHECK(gLuts[0].thumb && gLuts[0].thumb->width <= kFilmThumbW);
  state.source = pic;
  state.original = pic;
  pollLutPreviews();
  CHECK(pump(allSettled));

  // ---- removing it ----
  const size_t before = state.adjust.historySize();
  removeLut();
  CHECK(state.adjust.lut().empty() && state.adjust.get(AdjId::LutAmount) == 0.0f);
  CHECK(state.statusMsg == "Removed the LUT" && state.adjust.historySize() == before + 1);
  removeLut(); // nothing to remove
  CHECK(state.adjust.historySize() == before + 1);
  state.adjust.undo();
  CHECK(state.adjust.lut().name == "B warm" && state.adjust.get(AdjId::LutAmount) == 60.0f);

  // ---- importing ----
  const std::string good = other.path("D import.cube"), bad = other.path("junk.cube"), log = other.path("E SLog3 to 709.cube");
  writeText(good, warmCube(9));
  writeText(bad, "not a cube");
  writeText(log, invertCube(4));
  const uint64_t serial2 = gLutSerial;
  importLutPaths({});
  CHECK(gLuts.size() == 4 && gLutSerial == serial2); // nothing chosen: nothing happens
  importLutPaths({good});
  CHECK(gLuts.size() == 5 && rowNamed("D import") && state.statusMsg == "Imported LUT: D import" && !state.statusIsError);
  CHECK(fs::exists(fsPath(lutPath(lib.root(), "D import"))));
  importLutPaths({bad});
  CHECK(state.statusIsError && state.statusMsg.rfind("Couldn't import: ", 0) == 0 && gLuts.size() == 5);
  importLutPaths({other.path("Zed.cube"), other.path("Yee.cube")});
  CHECK(state.statusIsError && state.statusMsg.rfind("Couldn't import: ", 0) == 0);
  writeText(other.path("F second.cube"), sameCube(5));
  importLutPaths({other.path("F second.cube"), bad});
  CHECK(!state.statusIsError || state.statusMsg.find("skipped") != std::string::npos);
  CHECK(state.statusMsg.find("Imported LUT: F second") == 0 && state.statusMsg.find("1 skipped") != std::string::npos && gLuts.size() == 6);
  importLutPaths({log});
  CHECK(state.statusIsError && state.statusMsg == "E SLog3 to 709 may expect camera-log input and look wrong on this picture");
  CHECK(gLuts.size() == 7 && rowNamed("E SLog3 to 709"));
  // The previews of the new list are made (the list changed, so everything is made again).
  CHECK(pump(allSettled));
  CHECK(rowNamed("D import")->thumb && rowNamed("E SLog3 to 709")->warning.size() > 0);
  // Importing the same file again changes nothing in the list.
  const size_t n = gLuts.size();
  importLutPaths({good});
  CHECK(gLuts.size() == n && state.statusMsg == "Imported LUT: D import");

  // ---- re-reading an unchanged folder keeps what is known (the pane is opened again) ----
  const uint64_t serial3 = gLutSerial;
  const auto thumbBefore = rowNamed("B warm")->thumb;
  const uint64_t paintBefore = rowNamed("B warm")->paintVersion;
  refreshLuts();
  CHECK(gLuts.size() == n && rowNamed("B warm")->thumb == thumbBefore && rowNamed("B warm")->paintVersion == paintBefore);
  CHECK(rowNamed("B warm")->entry.size == 5 && rowNamed("B warm")->entry.hash != 0);
  pollLutPreviews();
  CHECK(gLutSerial == serial3); // no new batch: nothing changed
  // A file that comes or goes does start one.
  writeText(lutPath(lib.root(), "G late"), sameCube(2));
  refreshLuts();
  CHECK(gLuts.size() == n + 1);
  pollLutPreviews();
  CHECK(gLutSerial == serial3 + 1);
  CHECK(pump(allSettled));
  CHECK(deleteLutFile(lutPath(lib.root(), "G late")));
  refreshLuts();
  CHECK(gLuts.size() == n && !rowNamed("G late"));
  CHECK(pump(allSettled));

  // ---- a LUT the library doesn't have (or can't read, or that changed) ----
  AdjustmentSet keep = state.adjust;
  state.adjust.setLut(LutRef{"Gone", 7});
  dropLutCache();
  CHECK(currentLutStatus() == LutStatus::Missing);
  CHECK(lutCurrentText() == "LUT \"Gone\" is not in the library, so it has no effect");
  CHECK(noteLutProblem() && state.statusIsError && state.statusMsg.find("is not in the LUT library") != std::string::npos);
  writeText(lutPath(lib.root(), "Gone"), "LUT_3D_SIZE 2\n1 2\n");
  dropLutCache();
  CHECK(currentLutStatus() == LutStatus::Unreadable);
  CHECK(lutCurrentText().find("can't be read") != std::string::npos);
  CHECK(noteLutProblem() && state.statusMsg.find("can't be read") != std::string::npos);
  state.adjust.setLut(LutRef{"A invert", fileHash(lutPath(lib.root(), "A invert")) + 1});
  dropLutCache();
  CHECK(currentLutStatus() == LutStatus::HashChanged);
  CHECK(lutCurrentText() == "LUT: A invert (the file has changed since it was chosen)");
  CHECK(noteLutProblem() && !state.statusIsError && state.statusMsg.find("has changed since it was chosen") != std::string::npos);
  state.adjust.setLut(LutRef{"A invert", 0});
  dropLutCache();
  CHECK(currentLutStatus() == LutStatus::Ok && !noteLutProblem());
  state.adjust.setLut(LutRef{});
  CHECK(!noteLutProblem() && currentLutStatus() == LutStatus::None);

  // ---- clean up ----
  lutPreviewWorker.cancel();
  lutDirOverride().clear();
  dropLutCache();
}

int main()
{
  testLutPane();
  lutPreviewWorker.shutdown();
  return finish();
}
