// Build: links liteui and the thread library (see CMakeLists.txt).
//   (add -fsanitize=thread -g -O1 -pthread for the data-race check)
//
// Real .cube files in a temp folder, the real renderer. CHECK is not thread-safe
// (check.hpp), so assertions only run on the main thread; every wait has a timeout
// so a broken worker fails instead of hanging.
#include "liteui.hpp"
#include "workers/lut_preview_worker.hpp"
#include "check.hpp"

#include <charconv>
#include <chrono>
#include <fstream>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
static constexpr auto kWait = 20s;

struct TempDir
{
  fs::path dir;
  TempDir()
  {
    static int n = 0;
    dir = fs::temp_directory_path() /
          ("literoom_lutprev_test_" +
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
};

static void writeText(const std::string &path, const std::string &text)
{
  std::ofstream f(fsPath(path), std::ios::binary);
  f << text;
}

// A cube of n points whose entries are f(r, g, b) (each 0..1), locale-independent text.
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

// The library folder for one test, with a few LUTs of different looks in it.
struct Library
{
  TempDir dir;
  Library()
  {
    lutDirOverride() = dir.root();
    clearLutCache();
    add("A invert", cubeOf(3, [](double r, double g, double b, double *o)
                           { o[0] = 1 - r; o[1] = 1 - g; o[2] = 1 - b; }));
    add("B warm", cubeOf(5, [](double r, double g, double b, double *o)
                         { o[0] = std::min(1.0, r * 1.1 + 0.05); o[1] = g; o[2] = b * 0.8; }));
    add("C same", cubeOf(9, [](double r, double g, double b, double *o)
                         { o[0] = r; o[1] = g; o[2] = b; }));
    add("D SLog3 look", cubeOf(3, [](double r, double g, double b, double *o)
                               { o[0] = r; o[1] = g; o[2] = b; })); // the name says log
  }
  ~Library()
  {
    lutDirOverride().clear();
    clearLutCache();
  }
  void add(const std::string &name, const std::string &text) { writeText(lutPath(dir.root(), name), text); }
  std::vector<LutEntry> entries() const { return listLutEntries(dir.root()); }
};

static std::shared_ptr<const CanvasImage> makePicture(int w, int h)
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

static AdjustmentSet fresh(int w, int h)
{
  AdjustmentSet s;
  s.reset();
  s.setOriginalSize(w, h);
  return s;
}

static bool waitFor(LutPreviewWorker &w, std::vector<LutPreviewResult> *got, size_t n)
{
  const auto t0 = std::chrono::steady_clock::now();
  while (got->size() < n)
  {
    for (auto &r : w.collect())
      got->push_back(std::move(r));
    if (got->size() >= n)
      break;
    if (std::chrono::steady_clock::now() - t0 > kWait)
      return false;
    std::this_thread::sleep_for(2ms);
  }
  return true;
}
static bool waitIdle(LutPreviewWorker &w)
{
  const auto t0 = std::chrono::steady_clock::now();
  while (w.busy())
  {
    if (std::chrono::steady_clock::now() - t0 > kWait)
      return false;
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

// What a preview must be: the picture's reduced copy rendered with this LUT chosen in
// the edit at 100 %, through the real library, without the neighbor effects.
static std::shared_ptr<CanvasImage> reference(const CanvasImage &base, const AdjustmentSet &look,
                                              const std::string &lutName, uint64_t hash)
{
  AdjustmentSet s = look;
  s.setLut(LutRef{lutName, hash});
  s.set(AdjId::LutAmount, 100.0f);
  RenderParams p = s.buildRenderParams();
  p.fx = neighbor_fx::Gains{};
  for (MaskPass &m : p.masks)
    m.fx = neighbor_fx::Gains{};
  const auto reduced = makeThumb(base);
  return renderWith(*reduced, p);
}

static int maxDiff(const CanvasImage &a, const CanvasImage &b)
{
  if (a.pixels.size() != b.pixels.size())
    return 1000;
  int w = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i)
    w = std::max(w, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
  return w;
}

static Mask radialMask()
{
  Mask m;
  RadialShape r;
  r.cx = 0.4f;
  r.cy = 0.5f;
  r.rx = 0.3f;
  r.ry = 0.2f;
  m.parts.push_back({r, MaskOp::Add});
  m.adj[static_cast<size_t>(AdjId::Exposure)] = 35.0f;
  m.adj[static_cast<size_t>(AdjId::Texture)] = 60.0f; // a neighbor effect inside a mask
  return m;
}

static void testPreviewsAreTheRealRender()
{
  Library lib;
  const auto base = makePicture(300, 200);
  const std::vector<LutEntry> items = lib.entries();
  CHECK(items.size() == 4 && items[0].name == "A invert" && items[3].name == "D SLog3 look");

  AdjustmentSet look = fresh(300, 200);
  look.set(AdjId::Exposure, 15.0f);
  look.set(AdjId::Saturation, 25.0f);
  look.set(AdjId::Clarity, 50.0f); // dropped from previews
  look.set(AdjId::Vignette, -40.0f);
  look.setMasks({radialMask()}, -1);

  LutPreviewWorker w;
  std::atomic<int> wakes{0};
  w.setNotify([&]
              { ++wakes; });
  CHECK(!w.busy());
  LutPreviewJob job;
  job.serial = 5;
  job.base = base;
  job.params = look.buildRenderParams();
  job.items = items;
  w.submit(job);
  std::vector<LutPreviewResult> got;
  CHECK(waitFor(w, &got, 4));
  CHECK(waitIdle(w));
  CHECK(wakes >= 5); // one per result, and one when the job ended
  CHECK(got.size() == 4 && w.collect().empty());
  const auto reduced = makeThumb(*base);
  for (size_t i = 0; i < got.size() && i < items.size(); ++i)
  {
    const LutPreviewResult &r = got[i];
    CHECK(r.serial == 5 && r.name == items[i].name && r.path == items[i].path); // in the order given
    CHECK(r.error.empty() && r.size > 0 && r.hash != 0);
    std::vector<uint8_t> bytes;
    std::string err;
    CHECK(readLutBytes(items[i].path, &bytes, &err) && r.hash == lutHashBytes(bytes.data(), bytes.size()));
    CHECK(r.thumb && reduced && r.thumb->width == reduced->width && r.thumb->height == reduced->height);
    const auto want = reference(*base, look, items[i].name, r.hash);
    CHECK(r.thumb && want && r.thumb->pixels == want->pixels); // the real render, to the byte
  }
  CHECK(got.size() == 4 && got[0].size == 3 && got[1].size == 5 && got[2].size == 9);
  // The LUTs really look different from each other and from the picture itself.
  if (got.size() == 4 && got[0].thumb && got[1].thumb && got[2].thumb)
  {
    CHECK(got[0].thumb->pixels != got[1].thumb->pixels);
    const auto plain = renderWith(*reduced, [&]
                                  { RenderParams p = look.buildRenderParams(); p.fx = {}; for (auto &m : p.masks) m.fx = {}; return p; }());
    CHECK(plain && got[0].thumb->pixels != plain->pixels && got[1].thumb->pixels != plain->pixels);
    CHECK(maxDiff(*got[2].thumb, *plain) <= 1); // "C same" is an identity LUT
  }
  // Only the log-looking name warns.
  for (const auto &r : got)
    CHECK(r.warning.empty() == (r.name != "D SLog3 look"));
}

static void testLooksWithNoColorPassAndWithItsOwnLut()
{
  Library lib;
  const auto base = makePicture(200, 150);
  // Nothing else in the look: the previews are exactly "this LUT on the picture".
  {
    AdjustmentSet look = fresh(200, 150);
    LutPreviewWorker w;
    LutPreviewJob job;
    job.serial = 1;
    job.base = base;
    job.params = look.buildRenderParams();
    job.items = lib.entries();
    w.submit(job);
    std::vector<LutPreviewResult> got;
    CHECK(waitFor(w, &got, 4));
    for (const auto &r : got)
    {
      const auto want = reference(*base, look, r.name, r.hash);
      CHECK(r.thumb && want && r.thumb->pixels == want->pixels);
    }
  }
  // The look already has a LUT (at 40 %): a preview REPLACES it, it never stacks on it.
  {
    AdjustmentSet look = fresh(200, 150);
    look.set(AdjId::Contrast, 20.0f);
    look.setLut(LutRef{"A invert", 0});
    look.set(AdjId::LutAmount, 40.0f);
    LutPreviewWorker w;
    LutPreviewJob job;
    job.serial = 2;
    job.base = base;
    job.params = look.buildRenderParams();
    CHECK(job.params.color.lut.any());
    job.items = lib.entries();
    w.submit(job);
    std::vector<LutPreviewResult> got;
    CHECK(waitFor(w, &got, 4));
    AdjustmentSet without = look;
    without.setLut(LutRef{}); // the look minus its own LUT
    for (const auto &r : got)
    {
      const auto want = reference(*base, without, r.name, r.hash);
      CHECK(r.thumb && want && r.thumb->pixels == want->pixels);
    }
  }
}

static void testUnusableFilesAndNoPicture()
{
  Library lib;
  lib.add("E broken", "LUT_3D_SIZE 2\n0 0 0\n1 1\n");
  lib.add("F oneD", "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n");
  const auto base = makePicture(120, 90);
  AdjustmentSet look = fresh(120, 90);
  LutPreviewWorker w;
  LutPreviewJob job;
  job.serial = 3;
  job.base = base;
  job.params = look.buildRenderParams();
  job.items = lib.entries();
  CHECK(job.items.size() == 6);
  w.submit(job);
  std::vector<LutPreviewResult> got;
  CHECK(waitFor(w, &got, 6));
  CHECK(got.size() == 6);
  int bad = 0, good = 0;
  for (const auto &r : got)
  {
    if (r.name == "E broken" || r.name == "F oneD")
    {
      ++bad;
      CHECK(!r.error.empty() && !r.thumb && r.hash == 0 && r.size == 0);
    }
    else
    {
      ++good;
      CHECK(r.error.empty() && r.thumb); // a bad neighbor doesn't stop the batch
    }
  }
  CHECK(bad == 2 && good == 4);
  for (const auto &r : got)
    if (r.name == "F oneD")
      CHECK(r.error.find("1D") != std::string::npos);

  // A file that vanished between listing and reading is just an error result.
  std::vector<LutEntry> items = lib.entries();
  items.push_back(LutEntry{"G gone", lutPath(lib.dir.root(), "G gone"), 0, 0});
  job.serial = 4;
  job.items = items;
  w.submit(job);
  got.clear();
  CHECK(waitFor(w, &got, items.size()));
  CHECK(got.size() == items.size() && !got.back().error.empty() && !got.back().thumb);

  // No picture: the files are still read (hash, size, warning), there are just no thumbnails.
  job.serial = 5;
  job.base = nullptr;
  job.items = lib.entries();
  w.submit(job);
  got.clear();
  CHECK(waitFor(w, &got, job.items.size()));
  for (const auto &r : got)
    if (r.error.empty())
      CHECK(!r.thumb && r.hash != 0 && r.size > 0);
  // An empty list is a job that finishes at once.
  job.items.clear();
  w.submit(job);
  CHECK(waitIdle(w));
  CHECK(w.collect().empty());
}

static void testNewJobReplacesTheOld()
{
  Library lib;
  for (int i = 0; i < 30; ++i) // a long first job
    lib.add("Z" + std::to_string(100 + i), cubeOf(33, [](double r, double g, double b, double *o)
                                                   { o[0] = r; o[1] = g; o[2] = b; }));
  const auto base = makePicture(400, 300);
  AdjustmentSet look = fresh(400, 300);
  look.set(AdjId::Saturation, 30.0f);
  LutPreviewWorker w;
  LutPreviewJob first;
  first.serial = 1;
  first.base = base;
  first.params = look.buildRenderParams();
  first.items = lib.entries();
  CHECK(first.items.size() == 34);
  w.submit(first);
  std::this_thread::sleep_for(5ms);
  LutPreviewJob second = first;
  second.serial = 2;
  second.items.resize(2);
  w.submit(second);
  std::vector<LutPreviewResult> got;
  CHECK(waitFor(w, &got, 2));
  CHECK(waitIdle(w));
  for (auto &r : w.collect())
    got.push_back(std::move(r));
  CHECK(got.size() == 2);
  for (const auto &r : got)
    CHECK(r.serial == 2); // nothing of the replaced job arrives after submit() returned
  CHECK(got.size() == 2 && got[0].name == "A invert" && got[1].name == "B warm");
}

static void testCancelAndShutdown()
{
  Library lib;
  for (int i = 0; i < 20; ++i)
    lib.add("Y" + std::to_string(100 + i), cubeOf(33, [](double r, double g, double b, double *o)
                                                   { o[0] = r; o[1] = g; o[2] = b; }));
  const auto base = makePicture(400, 300);
  AdjustmentSet look = fresh(400, 300);
  LutPreviewJob job;
  job.serial = 1;
  job.base = base;
  job.params = look.buildRenderParams();
  job.items = lib.entries();
  {
    LutPreviewWorker w;
    w.submit(job);
    w.cancel();
    CHECK(waitIdle(w)); // the worker lets go of it quickly
    CHECK(w.collect().empty());
    // reusable afterwards
    job.serial = 2;
    job.items.resize(1);
    w.submit(job);
    std::vector<LutPreviewResult> got;
    CHECK(waitFor(w, &got, 1) && got[0].serial == 2);
  }
  const auto t0 = std::chrono::steady_clock::now();
  {
    LutPreviewWorker w;
    job.items = lib.entries();
    w.submit(job);
    w.shutdown();
    w.shutdown(); // twice is fine
    CHECK(w.collect().empty());
  }
  {
    LutPreviewWorker w; // destruction with work queued doesn't hang either
    w.submit(job);
  }
  { LutPreviewWorker never; } // never started
  CHECK(std::chrono::steady_clock::now() - t0 < kWait);
}

static void testPreviewsLeaveTheCacheAlone()
{
  Library lib;
  clearLutCache();
  auto keep = std::make_shared<const Lut3D>(identityLut(4));
  lutCachePut("Keep me", keep, 77);
  for (int i = 0; i < 12; ++i)
    lib.add("W" + std::to_string(100 + i), cubeOf(5, [](double r, double g, double b, double *o)
                                                   { o[0] = r; o[1] = g; o[2] = b; }));
  const auto base = makePicture(160, 120);
  AdjustmentSet look = fresh(160, 120);
  LutPreviewWorker w;
  LutPreviewJob job;
  job.serial = 1;
  job.base = base;
  job.params = look.buildRenderParams();
  job.items = lib.entries();
  w.submit(job);
  std::vector<LutPreviewResult> got;
  CHECK(waitFor(w, &got, job.items.size()));
  CHECK(got.size() == job.items.size());
  // Sixteen LUTs were read and rendered; the table that was in use is still the cached one.
  CHECK(acquireLut(LutRef{"Keep me", 77}).lut == keep);
}

static void testHelperAndEmptyParams()
{
  // lutPreviewParams on its own: replaces the LUT, drops the neighbor effects, keeps the rest.
  AdjustmentSet look = fresh(50, 50);
  look.set(AdjId::Exposure, 10.0f);
  look.set(AdjId::Texture, 30.0f);
  RenderParams in = look.buildRenderParams();
  CHECK(in.fx.any());
  const auto table = std::make_shared<const Lut3D>(identityLut(3));
  const RenderParams out = lutPreviewParams(in, makeLutPass(table, 100.0f));
  CHECK(!out.fx.any() && out.color.lut.any() && out.color.lut.lut == table && out.luts == in.luts);
}

int main()
{
  testPreviewsAreTheRealRender();
  testLooksWithNoColorPassAndWithItsOwnLut();
  testUnusableFilesAndNoPicture();
  testNewJobReplacesTheOld();
  testCancelAndShutdown();
  testPreviewsLeaveTheCacheAlone();
  testHelperAndEmptyParams();
  return finish();
}