// Build: links liteui like the other image tests (see CMakeLists.txt).
//   (add -fsanitize=thread -g -O1 -pthread for the data-race check)
//
// Real files in a temp folder, made with the app's own encoders (image_codec.hpp),
// decoded by the real decoder. Every wait has a timeout so a broken worker fails
// instead of hanging; CHECK is not thread-safe (check.hpp), so assertions only
// run on the main thread.
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"
#include "workers/thumb_worker.hpp"
#include "io/exif.hpp"
#include "io/image_codec.hpp"
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <fstream>

using namespace std::chrono_literals;
static constexpr auto kWait = 10s;

class Latch
{
public:
  void signal()
  {
    {
      std::lock_guard lk(m_);
      ++n_;
    }
    cv_.notify_all();
  }
  bool waitFor(int n)
  {
    std::unique_lock lk(m_);
    return cv_.wait_for(lk, kWait, [&] { return n_ >= n; });
  }
  int count()
  {
    std::lock_guard lk(m_);
    return n_;
  }

private:
  std::mutex m_;
  std::condition_variable cv_;
  int n_ = 0;
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
    return cv_.wait_for(lk, kWait, [&] { return open_; });
  }

private:
  std::mutex m_;
  std::condition_variable cv_;
  bool open_ = false;
};

// A scratch folder that removes itself.
struct TempDir
{
  std::filesystem::path dir;
  TempDir()
  {
    static int n = 0;
    dir = std::filesystem::temp_directory_path() /
          ("literoom_thumb_test_" +
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

// A JPEG of w x h; `orientation` > 0 adds an EXIF block with that orientation.
static bool writeJpeg(const std::string &path, int w, int h, int orientation)
{
  std::vector<uint8_t> jpeg;
  std::string err;
  if (!encodeJpegMemory(makeImage(w, h), 90, &jpeg, &err) || jpeg.size() < 2)
    return false;
  if (orientation > 0)
  {
    // APP1 "Exif" segment, big-endian TIFF, one IFD0 entry: Orientation (0x0112).
    const uint8_t o = static_cast<uint8_t>(orientation);
    const std::vector<uint8_t> app1 = {
        0xFF, 0xE1, 0x00, 0x22,                         // marker, length 34
        'E', 'x', 'i', 'f', 0, 0,                       //
        'M', 'M', 0x00, 0x2A, 0, 0, 0, 8,               // TIFF header, IFD0 at 8
        0x00, 0x01,                                     // one entry
        0x01, 0x12, 0x00, 0x03, 0, 0, 0, 1, 0x00, o, 0, 0, // Orientation, SHORT, 1
        0, 0, 0, 0};                                    // no next IFD
    jpeg.insert(jpeg.begin() + 2, app1.begin(), app1.end());
  }
  std::ofstream f(fsPath(path), std::ios::binary);
  f.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
  return static_cast<bool>(f);
}

// The app's own EXIF reader isn't reachable from this test; this does the same
// job from exif.hpp (the file's first 256 KB hold every header).
static Orient orientFromExif(const std::string &path)
{
  std::ifstream f(fsPath(path), std::ios::binary);
  std::vector<uint8_t> head(256 * 1024);
  f.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<size_t>(f.gcount()));
  exif::JpegMeta m;
  Orient o;
  if (exif::readJpegMeta(head.data(), head.size(), &m))
  {
    const exif::Transform t = exif::orientationToTransform(m.orientation);
    o.rot = t.rot;
    o.flip = t.flip;
  }
  return o;
}

// ------------------------------------------------------------------ makeThumb
static void testMakeThumb()
{
  CHECK(makeThumb(CanvasImage()) == nullptr); // empty picture
  {
    CanvasImage shortBuf(10, 10);
    shortBuf.pixels.assign(10 * 10 * 4 - 1, 0); // one byte too few
    CHECK(makeThumb(shortBuf) == nullptr);
  }
  struct Case { int w, h, ew, eh; };
  const Case cases[] = {{200, 100, 96, 48},  // fit width
                        {100, 200, 36, 72},  // fit height
                        {192, 144, 96, 72},  // exact 2:1 reduction
                        {96, 72, 96, 72},    // already the size
                        {50, 40, 50, 40},    // never enlarged
                        {1, 1, 1, 1},
                        {4000, 10, 96, 1}};  // never below 1 px
  for (const Case &c : cases)
  {
    auto t = makeThumb(makeImage(c.w, c.h));
    CHECK(t != nullptr);
    if (t)
    {
      CHECK(t->width == c.ew);
      CHECK(t->height == c.eh);
      CHECK(t->width <= kFilmThumbW && t->height <= kFilmThumbH);
    }
  }
  { // a flat colour stays that colour
    CanvasImage flat(200, 100);
    flat.pixels.resize(200 * 100 * 4);
    for (size_t i = 0; i < flat.pixels.size(); i += 4)
    {
      flat.pixels[i] = 10;
      flat.pixels[i + 1] = 20;
      flat.pixels[i + 2] = 30;
      flat.pixels[i + 3] = 255;
    }
    auto t = makeThumb(flat);
    CHECK(t != nullptr);
    if (t)
      for (size_t i = 0; i < t->pixels.size(); i += 4)
        if (t->pixels[i] != 10 || t->pixels[i + 1] != 20 || t->pixels[i + 2] != 30 || t->pixels[i + 3] != 255)
        {
          CHECK(false);
          break;
        }
  }
  { // 192x144 -> 96x72: every 2x2 block is averaged ((0 + 100 + 200 + 100) / 4)
    CanvasImage src(192, 144);
    src.pixels.assign(192 * 144 * 4, 255);
    const uint8_t v[4] = {0, 100, 200, 100};
    for (int y = 0; y < 144; ++y)
      for (int x = 0; x < 192; ++x)
        src.pixels[(static_cast<size_t>(y) * 192 + x) * 4] = v[(y & 1) * 2 + (x & 1)];
    auto t = makeThumb(src);
    CHECK(t != nullptr && t->width == 96 && t->height == 72);
    if (t)
      for (int y = 0; y < 72; ++y)
        for (int x = 0; x < 96; ++x)
          if (t->pixels[(static_cast<size_t>(y) * 96 + x) * 4] != 100)
          {
            CHECK(false);
            return;
          }
  }
}

static void testBoxAverageRounds()
{ // each 2x2 block holds {0, 1, 1, 1}: the mean 0.75 rounds to 1 (not down to 0)
  CanvasImage src(192, 144);
  src.pixels.assign(192 * 144 * 4, 255);
  for (int y = 0; y < 144; ++y)
    for (int x = 0; x < 192; ++x)
      src.pixels[(static_cast<size_t>(y) * 192 + x) * 4] = (x & 1) == 0 && (y & 1) == 0 ? 0 : 1;
  auto t = makeThumb(src);
  CHECK(t != nullptr && t->width == 96 && t->height == 72);
  if (t)
    CHECK(t->pixels[0] == 1 && t->pixels[(72 * 96 - 1) * 4] == 1);
}

// ---------------------------------------------------- queue, drain, replacement
static void testDrainAndOrder()
{
  TempDir td;
  std::vector<std::string> paths;
  for (int i = 0; i < 4; ++i)
  {
    paths.push_back(td.path("p" + std::to_string(i) + ".png"));
    CHECK(writePng(paths.back(), 40 + i * 10, 30));
  }
  Latch notified;
  ThumbWorker w(orientFromExif);
  w.setNotify([&] { notified.signal(); });
  CHECK(w.collect().empty()); // nothing yet, before the thread even exists
  w.setQueue(paths);
  CHECK(notified.waitFor(4)); // notify comes after the result is parked
  w.shutdown();
  auto got = w.collect();
  CHECK(got.size() == 4);
  for (size_t i = 0; i < got.size() && i < paths.size(); ++i)
  {
    CHECK(got[i].path == paths[i]); // FIFO
    CHECK(got[i].thumb != nullptr);
    CHECK(!got[i].edited);
  }
  CHECK(w.collect().empty()); // drained: handed out once
}

static void testQueueReplacement()
{
  TempDir td;
  std::vector<std::string> first;
  for (int i = 0; i < 6; ++i)
  {
    first.push_back(td.path("a" + std::to_string(i) + ".png"));
    CHECK(writePng(first.back(), 32, 32));
  }
  const std::string z = td.path("z.png");
  CHECK(writePng(z, 32, 32));

  Latch inNotify, notified;
  Gate release;
  std::atomic<int> calls{0};
  std::atomic<bool> timedOut{false};
  ThumbWorker w(orientFromExif);
  w.setNotify([&]
              {
                if (calls++ == 0)
                {
                  inNotify.signal();
                  // Hold the worker right after its first result. The test thread
                  // calls setQueue() meanwhile, which needs the worker's lock: if
                  // notify ran under that lock this wait would time out.
                  if (!release.wait())
                    timedOut = true;
                }
                notified.signal(); });
  w.setQueue(first);
  CHECK(inNotify.waitFor(1)); // a0 is done; a1..a5 are still queued
  w.setQueue({z});            // replaces them
  release.open();
  CHECK(notified.waitFor(2));
  w.shutdown();               // joins: the worker is idle
  auto got = w.collect();
  CHECK(got.size() == 2);
  if (got.size() == 2)
  {
    CHECK(got[0].path == first[0]); // the one in flight finished
    CHECK(got[1].path == z);        // the rest of the old queue was dropped
  }
  CHECK(notified.count() == 2);
  CHECK(!timedOut);
}

// ----------------------------------------------------------- per-file results
static void testFailedDecode()
{
  TempDir td;
  const std::string bad = td.path("bad.png"), missing = td.path("missing.png"),
                    good = td.path("good.png");
  {
    std::ofstream f(fsPath(bad), std::ios::binary);
    f << "this is not a picture";
  }
  CHECK(writePng(good, 30, 20));
  Latch notified;
  ThumbWorker w(orientFromExif);
  w.setNotify([&] { notified.signal(); });
  w.setQueue({bad, missing, good});
  CHECK(notified.waitFor(3));
  w.shutdown();
  auto got = w.collect();
  CHECK(got.size() == 3);
  if (got.size() == 3)
  {
    CHECK(got[0].path == bad && got[0].thumb == nullptr);
    CHECK(got[1].path == missing && got[1].thumb == nullptr);
    CHECK(got[2].path == good && got[2].thumb != nullptr); // a failure doesn't stop the queue
    CHECK(!got[0].edited && !got[1].edited && !got[2].edited);
  }
}

static void testEditedFlag()
{
  TempDir td;
  const std::string withEdits = td.path("a.png"), plain = td.path("b.png");
  CHECK(writePng(withEdits, 30, 20));
  CHECK(writePng(plain, 30, 20));
  {
    std::ofstream f(fsPath(sidecarFileOf(withEdits)), std::ios::binary);
    f << "{}";
  }
  Latch notified;
  ThumbWorker w(orientFromExif);
  w.setNotify([&] { notified.signal(); });
  w.setQueue({withEdits, plain});
  CHECK(notified.waitFor(2));
  w.shutdown();
  auto got = w.collect();
  CHECK(got.size() == 2);
  if (got.size() == 2)
  {
    CHECK(got[0].path == withEdits && got[0].edited && got[0].thumb);
    CHECK(got[1].path == plain && !got[1].edited && got[1].thumb);
  }
  // the sidecar's name is the picture's name plus the extension
  CHECK(sidecarFileOf("x/photo.jpg") == std::string("x/photo.jpg") + kSidecarExt);
}

static void testExifOrientation()
{
  TempDir td;
  const std::string plain = td.path("plain.jpg"), rot = td.path("rot6.jpg"),
                    half = td.path("rot3.jpg");
  CHECK(writeJpeg(plain, 200, 100, 0)); // no EXIF
  CHECK(writeJpeg(rot, 200, 100, 6));   // "rotate 90 degrees": a portrait once upright
  CHECK(writeJpeg(half, 200, 100, 3));  // 180 degrees: still landscape
  Latch notified;
  ThumbWorker w(orientFromExif);
  w.setNotify([&] { notified.signal(); });
  w.setQueue({plain, rot, half});
  CHECK(notified.waitFor(3));
  w.shutdown();
  auto got = w.collect();
  CHECK(got.size() == 3);
  if (got.size() == 3 && got[0].thumb && got[1].thumb && got[2].thumb)
  {
    CHECK(got[0].thumb->width == 96 && got[0].thumb->height == 48);
    CHECK(got[1].thumb->width == 36 && got[1].thumb->height == 72); // turned upright
    CHECK(got[2].thumb->width == 96 && got[2].thumb->height == 48);
  }
  else
    CHECK(false);
}

// ------------------------------------------------------------------- shutdown
static void testShutdown()
{
  { // before the first setQueue, and twice
    ThumbWorker w(orientFromExif);
    w.shutdown();
    w.shutdown();
    CHECK(w.collect().empty());
  }
  { // shutdown drops what is still queued; nothing is notified afterwards
    TempDir td;
    std::vector<std::string> paths;
    for (int i = 0; i < 20; ++i)
    {
      paths.push_back(td.path("s" + std::to_string(i) + ".png"));
      CHECK(writePng(paths.back(), 64, 48));
    }
    Latch inNotify, notified;
    Gate release;
    std::atomic<int> calls{0};
    ThumbWorker w(orientFromExif);
    w.setNotify([&]
                {
                  if (calls++ == 0)
                  {
                    inNotify.signal();
                    release.wait(); // the worker sits here, 19 files still queued
                  }
                  notified.signal(); });
    w.setQueue(paths);
    CHECK(inNotify.waitFor(1));
    // shutdown() clears the queue first, then joins; a helper lets the worker
    // out of notify only after that has had time to happen.
    std::thread opener([&]
                       {
                         std::this_thread::sleep_for(100ms);
                         release.open(); });
    w.shutdown();
    opener.join();
    CHECK(notified.count() == 1); // the one in flight finished; the queue was dropped
    w.shutdown();
    CHECK(notified.count() == 1);
    auto got = w.collect();
    CHECK(got.size() == 1);
    if (!got.empty())
      CHECK(got[0].path == paths[0]);
    CHECK(w.collect().empty());
  }
  { // destruction with work queued doesn't hang
    TempDir td;
    std::vector<std::string> paths;
    for (int i = 0; i < 20; ++i)
    {
      paths.push_back(td.path("d" + std::to_string(i) + ".png"));
      CHECK(writePng(paths.back(), 64, 48));
    }
    const auto t0 = std::chrono::steady_clock::now();
    {
      ThumbWorker w(orientFromExif);
      w.setQueue(paths);
    }
    CHECK(std::chrono::steady_clock::now() - t0 < 10s);
  }
}

int main()
{
  testMakeThumb();
  testBoxAverageRounds();
  testDrainAndOrder();
  testQueueReplacement();
  testFailedDecode();
  testEditedFlag();
  testExifOrientation();
  testShutdown();
  return finish();
}