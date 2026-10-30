// Build: links liteui and the thread library (see CMakeLists.txt).
//   (add -fsanitize=thread -g -O1 -pthread for the data-race check)
//
// Real files in a temp folder, made with the app's own encoders, read back with
// the real decoder. CHECK is not thread-safe (check.hpp), so assertions only run
// on the main thread; every wait has a timeout so a broken worker fails
// instead of hanging.
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"
#include "workers/export_worker.hpp"
#include "check.hpp"

#include <chrono>

using namespace std::chrono_literals;
static constexpr auto kWait = 20s;

struct TempDir
{
  std::filesystem::path dir;
  TempDir()
  {
    static int n = 0;
    dir = std::filesystem::temp_directory_path() /
          ("literoom_export_test_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
           "_" + std::to_string(n++));
    std::filesystem::create_directories(dir);
  }
  ~TempDir()
  {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::string path(const std::string &name) const { return u8ToString(dir / fsPath(name)); }
  std::string root() const { return u8ToString(dir); }
};

class Gate
{
public:
  void open()
  {
    {
      std::lock_guard lk(m_);
      open_ = true;
    }
    cv_.notify_all();
  }
  bool wait()
  {
    std::unique_lock lk(m_);
    return cv_.wait_for(lk, kWait, [&]
                        { return open_; });
  }

private:
  std::mutex m_;
  std::condition_variable cv_;
  bool open_ = false;
};

static CanvasImage makeImage(int w, int h)
{
  CanvasImage img(w, h);
  img.pixels.resize(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *p = &img.pixels[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
      p[1] = static_cast<uint8_t>(y * 255 / std::max(1, h - 1));
      p[2] = 90;
      p[3] = 255;
    }
  return img;
}

static bool writePng(const std::string &path, int w, int h)
{
  std::string err;
  return encodePngFile(path, makeImage(w, h), &err);
}

static void writeText(const std::string &path, const std::string &text)
{
  std::ofstream f(fsPath(path), std::ios::binary);
  f << text;
}

// A sidecar for `imageName` (w x h) holding `adj`'s edits, written to `sidecarOf`'s file.
static void writeSidecar(const std::string &pathKey, int w, int h, const AdjustmentSet &adj)
{
  writeText(sidecarFileOf(pathKey),
            sidecarFileText(baseName(vcImageOf(pathKey)), w, h, adj.toJson(), PhotoMeta{}));
}

static std::optional<CanvasImage> readBack(const std::string &p)
{
  std::string err;
  return liteui_image::decodeFile(p, &err);
}

static ExportSettings pngSettings(const TempDir &out)
{
  ExportSettings s;
  s.jpeg = false;
  s.folder = out.root();
  return s;
}

static bool waitIdle(ExportWorker &w)
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

static void testNaming()
{
  CHECK(exportStemOf("/a/b/photo.jpg") == "photo");
  CHECK(exportStemOf("/a/b/photo.jpg|vc2") == "photo-copy2");
  CHECK(exportStemOf("/a/b/.hidden") == ".hidden");
  TempDir td;
  std::set<std::string> used;
  const std::string a = exportTargetPath(td.root(), "p", "jpg", false, used);
  CHECK(baseName(a) == "p-edit.jpg");
  CHECK(baseName(exportTargetPath(td.root(), "p", "jpg", false, used)) == "p-edit-2.jpg");
  CHECK(baseName(exportTargetPath(td.root(), "p", "jpg", true, used)) == "p-edit-3.jpg"); // used wins over overwrite
  // An existing file is skipped unless overwriting.
  writeText(td.path("q-edit.png"), "x");
  std::set<std::string> u2;
  CHECK(baseName(exportTargetPath(td.root(), "q", "png", false, u2)) == "q-edit-2.png");
  std::set<std::string> u3;
  CHECK(baseName(exportTargetPath(td.root(), "q", "png", true, u3)) == "q-edit.png");
  // Case-insensitive within a batch.
  std::set<std::string> u4;
  exportTargetPath(td.root(), "Z", "png", false, u4);
  CHECK(baseName(exportTargetPath(td.root(), "z", "png", false, u4)) == "z-edit-2.png");
}

static void testUneditedIsIdentity()
{
  TempDir in, out;
  const std::string src = in.path("a.png");
  CHECK(writePng(src, 40, 30));
  std::set<std::string> used;
  const ExportOutcome o = exportPicture(src, pngSettings(out), used);
  CHECK(o.ok);
  CHECK(o.width == 40 && o.height == 30);
  CHECK(baseName(o.outFile) == "a-edit.png");
  auto back = readBack(o.outFile);
  CHECK(back.has_value());
  if (back)
    CHECK(back->pixels == makeImage(40, 30).pixels);
}

static void testEditsGeometryAndRender()
{
  TempDir in, out;
  const std::string src = in.path("b.png");
  CHECK(writePng(src, 64, 48));
  AdjustmentSet adj;
  adj.setOriginalSize(64, 48);
  adj.set(AdjId::Exposure, 40.0f);
  adj.set(AdjId::Saturation, -30.0f);
  adj.setCrop(CropRect{8, 4, 40, 30});
  writeSidecar(src, 64, 48, adj);

  std::set<std::string> used;
  const ExportOutcome o = exportPicture(src, pngSettings(out), used);
  CHECK(o.ok);
  CHECK(o.width == 40 && o.height == 30);
  // Exactly what the editor would show: crop first, then render.
  auto cropped = cropImage(makeImage(64, 48), CropRect{8, 4, 40, 30});
  auto want = renderWith(*cropped, adj.buildRenderParams());
  auto back = readBack(o.outFile);
  CHECK(back.has_value());
  if (back && want)
  {
    CHECK(back->width == 40 && back->height == 30);
    CHECK(back->pixels == want->pixels);
    CHECK(back->pixels != cropped->pixels); // the edit really did something
  }
}

static void testStraightenAndOrient()
{
  TempDir in, out;
  const std::string src = in.path("c.png");
  CHECK(writePng(src, 64, 48));
  AdjustmentSet adj;
  adj.setOriginalSize(64, 48);
  adj.setGeometry(Orient{1, false}, 5.0f, CropRect{});
  writeSidecar(src, 64, 48, adj);
  std::set<std::string> used;
  const ExportOutcome o = exportPicture(src, pngSettings(out), used);
  CHECK(o.ok);
  auto oriented = orientImage(std::make_shared<CanvasImage>(makeImage(64, 48)), Orient{1, false});
  auto rotated = rotateArbitrary(*oriented, 5.0f, adj.expand());
  CHECK(rotated != nullptr);
  if (rotated)
  {
    CHECK(o.width == rotated->width && o.height == rotated->height);
    auto back = readBack(o.outFile);
    CHECK(back.has_value());
    if (back)
      CHECK(back->pixels == renderWith(*rotated, adj.buildRenderParams())->pixels);
  }
}

static void testResizeAndJpeg()
{
  TempDir in, out;
  const std::string src = in.path("d.png");
  CHECK(writePng(src, 200, 100));
  ExportSettings s = pngSettings(out);
  s.resize = true;
  s.longEdge = 50;
  std::set<std::string> used;
  ExportOutcome o = exportPicture(src, s, used);
  CHECK(o.ok && o.width == 50 && o.height == 25);
  // A smaller picture is never enlarged.
  s.longEdge = 400;
  o = exportPicture(src, s, used);
  CHECK(o.ok && o.width == 200 && o.height == 100);
  // JPEG, with metadata options on a PNG source (nothing to carry: still fine).
  ExportSettings j;
  j.jpeg = true;
  j.quality = 80;
  j.keepMetadata = true;
  j.removeGps = true;
  j.folder = out.root();
  o = exportPicture(src, j, used);
  CHECK(o.ok);
  CHECK(baseName(o.outFile) == "d-edit.jpg");
  auto back = readBack(o.outFile);
  CHECK(back.has_value() && back->width == 200 && back->height == 100);
}

static void testVirtualCopy()
{
  TempDir in, out;
  const std::string src = in.path("e.png");
  CHECK(writePng(src, 64, 48));
  AdjustmentSet adj;
  adj.setOriginalSize(64, 48);
  adj.setCrop(CropRect{0, 0, 32, 24});
  const std::string copy = vcPath(src, 2);
  writeSidecar(copy, 64, 48, adj); // only the copy has edits
  std::set<std::string> used;
  const ExportOutcome a = exportPicture(src, pngSettings(out), used);
  const ExportOutcome b = exportPicture(copy, pngSettings(out), used);
  CHECK(a.ok && a.width == 64 && a.height == 48);
  CHECK(b.ok && b.width == 32 && b.height == 24);
  CHECK(baseName(a.outFile) == "e-edit.png");
  CHECK(baseName(b.outFile) == "e-copy2-edit.png");
}

static void testUnusableSidecarIsReported()
{
  TempDir in, out;
  std::set<std::string> used;
  { // wrong size
    const std::string src = in.path("f.png");
    CHECK(writePng(src, 64, 48));
    AdjustmentSet adj;
    adj.setOriginalSize(10, 10);
    adj.set(AdjId::Exposure, 10.0f);
    writeSidecar(src, 10, 10, adj);
    const ExportOutcome o = exportPicture(src, pngSettings(out), used);
    CHECK(!o.ok && !o.cancelled && o.error.find("size") != std::string::npos);
  }
  { // garbage
    const std::string src = in.path("g.png");
    CHECK(writePng(src, 8, 8));
    writeText(sidecarFileOf(src), "{ not json");
    const ExportOutcome o = exportPicture(src, pngSettings(out), used);
    CHECK(!o.ok && !o.error.empty());
  }
  { // a picture that can't be decoded
    writeText(in.path("h.png"), "not a png");
    const ExportOutcome o = exportPicture(in.path("h.png"), pngSettings(out), used);
    CHECK(!o.ok && !o.cancelled && !o.error.empty());
  }
  { // an unwritable destination
    const std::string src = in.path("i.png");
    CHECK(writePng(src, 8, 8));
    ExportSettings s = pngSettings(out);
    s.folder = out.path("no_such_folder");
    const ExportOutcome o = exportPicture(src, s, used);
    CHECK(!o.ok && !o.cancelled && !o.error.empty());
  }
}

static void testCancelledPicture()
{
  TempDir in, out;
  const std::string src = in.path("j.png");
  CHECK(writePng(src, 16, 16));
  std::atomic<uint64_t> latest{5};
  std::set<std::string> used;
  const ExportOutcome o = exportPicture(src, pngSettings(out), used, &latest, 4);
  CHECK(o.cancelled && !o.ok);
  CHECK(!std::filesystem::exists(fsPath(out.path("j-edit.png"))));
}

static void testBatch()
{
  TempDir in, out;
  std::vector<std::string> paths;
  for (int i = 0; i < 6; ++i)
  {
    paths.push_back(in.path("p" + std::to_string(i) + ".png"));
    CHECK(writePng(paths.back(), 32, 24));
  }
  ExportWorker w;
  std::atomic<int> wakes{0};
  w.setNotify([&]
              { ++wakes; });
  CHECK(!w.busy());
  CHECK(!w.start({}, pngSettings(out))); // nothing to do
  CHECK(w.start(paths, pngSettings(out)));
  CHECK(w.busy());
  CHECK(!w.start(paths, pngSettings(out))); // one batch at a time
  CHECK(waitIdle(w));
  CHECK(wakes >= 6);
  auto got = w.collect();
  CHECK(got.size() == 6);
  for (size_t i = 0; i < got.size() && i < paths.size(); ++i)
  {
    CHECK(got[i].path == paths[i]); // in the order given
    CHECK(got[i].ok);
  }
  CHECK(w.collect().empty());
  // A second batch into the same folder gets fresh names, then overwrites on request.
  CHECK(w.start({paths[0]}, pngSettings(out)));
  CHECK(waitIdle(w));
  auto again = w.collect();
  CHECK(again.size() == 1 && again[0].ok && baseName(again[0].outFile) == "p0-edit-2.png");
  ExportSettings ow = pngSettings(out);
  ow.overwrite = true;
  CHECK(w.start({paths[0]}, ow));
  CHECK(waitIdle(w));
  again = w.collect();
  CHECK(again.size() == 1 && again[0].ok && baseName(again[0].outFile) == "p0-edit.png");
  // Names are unique inside one batch even when overwriting (a.png + a.png).
  CHECK(w.start({paths[1], paths[1]}, ow));
  CHECK(waitIdle(w));
  again = w.collect();
  CHECK(again.size() == 2 && again[0].ok && again[1].ok && again[0].outFile != again[1].outFile);
}

// A LUT in the library folder (a real .cube file) for the duration of one test.
struct LutLibrary
{
  TempDir dir;
  LutLibrary()
  {
    lutDirOverride() = dir.root();
    clearLutCache();
  }
  ~LutLibrary()
  {
    lutDirOverride().clear();
    clearLutCache();
  }
  // An inverting 2-point cube; returns the hash of its bytes.
  uint64_t writeInvert(const std::string &name)
  {
    const std::string text = "LUT_3D_SIZE 2\n1 1 1\n0 1 1\n1 0 1\n0 0 1\n1 1 0\n0 1 0\n1 0 0\n0 0 0\n";
    writeText(lutPath(dir.root(), name), text);
    return lutHashBytes(reinterpret_cast<const uint8_t *>(text.data()), text.size());
  }
};

static void testLutExport()
{
  LutLibrary lib;
  const uint64_t hash = lib.writeInvert("Inv");
  TempDir in, out;
  const std::string src = in.path("l.png");
  CHECK(writePng(src, 48, 36));
  AdjustmentSet adj;
  adj.setOriginalSize(48, 36);
  adj.setCrop(CropRect{4, 3, 30, 20});
  adj.set(AdjId::Saturation, 20.0f);
  CHECK(adj.setLut(LutRef{"Inv", hash}));
  writeSidecar(src, 48, 36, adj);

  std::set<std::string> used;
  const ExportOutcome o = exportPicture(src, pngSettings(out), used);
  CHECK(o.ok && o.width == 30 && o.height == 20);
  // The batch renders exactly what the editor would: crop, then the render with the LUT.
  auto cropped = cropImage(makeImage(48, 36), CropRect{4, 3, 30, 20});
  auto want = renderWith(*cropped, adj.buildRenderParams());
  auto back = readBack(o.outFile);
  CHECK(back.has_value() && want != nullptr);
  if (back && want)
  {
    CHECK(back->pixels == want->pixels);
    AdjustmentSet noLut = adj;
    noLut.setLut(LutRef{});
    CHECK(back->pixels != renderWith(*cropped, noLut.buildRenderParams())->pixels); // the LUT really did something
  }

  // A LUT file that has changed since it was chosen (a different hash) is used as it is.
  const ExportOutcome changed = [&]
  {
    AdjustmentSet a2 = adj;
    a2.setLut(LutRef{"Inv", hash + 1});
    writeSidecar(src, 48, 36, a2);
    std::set<std::string> u2;
    return exportPicture(src, pngSettings(out), u2);
  }();
  CHECK(changed.ok);
}

static void testMissingLutIsReported()
{
  LutLibrary lib; // an empty library
  TempDir in, out;
  std::set<std::string> used;
  const std::string a = in.path("m.png");
  CHECK(writePng(a, 32, 24));
  AdjustmentSet adj;
  adj.setOriginalSize(32, 24);
  adj.setLut(LutRef{"Gone", 5});
  writeSidecar(a, 32, 24, adj);
  // In use (amount above 0) but not in the library: skipped with a reason, like any unusable sidecar.
  const ExportOutcome o = exportPicture(a, pngSettings(out), used);
  CHECK(!o.ok && !o.cancelled && o.error.find("LUT") != std::string::npos && o.error.find("Gone") != std::string::npos);
  CHECK(!std::filesystem::exists(fsPath(out.path("m-edit.png"))));

  // A LUT file that exists but is damaged: also skipped.
  writeText(lutPath(lib.dir.root(), "Gone"), "LUT_3D_SIZE 2\n1 2 3\n");
  clearLutCache();
  const ExportOutcome bad = exportPicture(a, pngSettings(out), used);
  CHECK(!bad.ok && !bad.cancelled && bad.error.find("can't be read") != std::string::npos);

  // With the amount at 0 the LUT does nothing, so a missing one is no reason to skip the picture.
  const std::string b = in.path("n.png");
  CHECK(writePng(b, 32, 24));
  AdjustmentSet off;
  off.setOriginalSize(32, 24);
  off.setLut(LutRef{"Absent", 5});
  off.set(AdjId::LutAmount, 0.0f);
  writeSidecar(b, 32, 24, off);
  const ExportOutcome fine = exportPicture(b, pngSettings(out), used);
  CHECK(fine.ok);
  auto back = readBack(fine.outFile);
  CHECK(back.has_value() && back->pixels == makeImage(32, 24).pixels);

  // In a batch, the picture with the missing LUT is skipped and the rest go on.
  const std::string good = in.path("g.png");
  CHECK(writePng(good, 16, 16));
  ExportWorker w;
  CHECK(w.start({a, good}, pngSettings(out)));
  CHECK(waitIdle(w));
  auto got = w.collect();
  CHECK(got.size() == 2);
  if (got.size() == 2)
  {
    CHECK(!got[0].ok && !got[0].cancelled && got[0].error.find("LUT") != std::string::npos);
    CHECK(got[1].ok);
  }
}

static void testFailureDoesNotStopTheBatch()
{
  TempDir in, out;
  const std::string good = in.path("ok.png"), bad = in.path("bad.png");
  CHECK(writePng(good, 16, 16));
  writeText(bad, "nope");
  ExportWorker w;
  CHECK(w.start({bad, good}, pngSettings(out)));
  CHECK(waitIdle(w));
  auto got = w.collect();
  CHECK(got.size() == 2);
  if (got.size() == 2)
  {
    CHECK(!got[0].ok && !got[0].cancelled);
    CHECK(got[1].ok);
  }
}

static void testCancel()
{
  TempDir in, out;
  std::vector<std::string> paths;
  for (int i = 0; i < 5; ++i)
  {
    paths.push_back(in.path("c" + std::to_string(i) + ".png"));
    CHECK(writePng(paths.back(), 32, 24));
  }
  ExportWorker w;
  Gate inNotify, release;
  std::atomic<int> calls{0};
  w.setNotify([&]
              {
                if (calls++ == 0)
                {
                  inNotify.open();
                  release.wait(); // the worker sits here with 4 pictures queued
                } });
  CHECK(w.start(paths, pngSettings(out)));
  CHECK(inNotify.wait());
  w.cancel();
  CHECK(!w.busy()); // the 4 dropped ones were counted at once; the one in flight had finished
  release.open();
  CHECK(waitIdle(w));
  auto got = w.collect();
  CHECK(got.size() == 5);
  int ok = 0, cancelled = 0;
  for (const auto &o : got)
  {
    ok += o.ok;
    cancelled += o.cancelled;
  }
  CHECK(ok == 1);        // the one in flight had already been written
  CHECK(cancelled == 4); // the rest were dropped, and say so
  CHECK(w.done() == w.total());
  // The worker is reusable afterwards.
  CHECK(w.start({paths[0]}, pngSettings(out)));
  CHECK(waitIdle(w));
  CHECK(w.collect().size() == 1);
}

static void testShutdown()
{
  TempDir in, out;
  std::vector<std::string> paths;
  for (int i = 0; i < 20; ++i)
  {
    paths.push_back(in.path("s" + std::to_string(i) + ".png"));
    CHECK(writePng(paths.back(), 64, 48));
  }
  const auto t0 = std::chrono::steady_clock::now();
  {
    ExportWorker w;
    CHECK(w.start(paths, pngSettings(out)));
    w.shutdown();
    w.shutdown(); // twice is fine
  }
  { // destruction with work queued doesn't hang either
    ExportWorker w;
    CHECK(w.start(paths, pngSettings(out)));
  }
  CHECK(std::chrono::steady_clock::now() - t0 < kWait);
}

int main()
{
  testNaming();
  testUneditedIsIdentity();
  testEditsGeometryAndRender();
  testStraightenAndOrient();
  testResizeAndJpeg();
  testVirtualCopy();
  testUnusableSidecarIsReported();
  testLutExport();
  testMissingLutIsReported();
  testCancelledPicture();
  testBatch();
  testFailureDoesNotStopTheBatch();
  testCancel();
  testShutdown();
  return finish();
}