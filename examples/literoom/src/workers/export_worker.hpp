// export_worker.hpp - Batch export: many pictures, each rendered from its own
// saved edits, written to one folder, off the UI thread.
//
// exportPicture() is the whole per-picture pipeline as a PURE function of its
// inputs: it decodes the file itself, reads the picture's sidecar from disk,
// loads the edits into a LOCAL AdjustmentSet (never the editor's), and runs the
// same chain the geometry worker runs (orient -> straighten -> crop -> render,
// all from the untouched original), then resizes and encodes. It never reads
// `state`, so it is safe on a worker thread (see threading.md). The open picture
// is exported like any other: the app flushes its sidecar to disk before the
// batch is queued.
//
// ExportWorker is a FIFO queue like ThumbWorker (no generations: you want ALL
// of them), plus cancel(): it drops what is still queued and abandons the
// picture in flight (renderWith / rotateArbitrary poll an epoch counter, so a
// 24 MP picture stops within a few rows). Every dropped picture still gets an
// outcome (cancelled = true) so the UI can say how many were not exported.
//
// Thread model: start(), cancel(), busy(), collect(), shutdown() are called from
// the UI thread only. setNotify() must be called before the first start().
#pragma once
#include "liteui.hpp"
#include "core/geometry.hpp"
#include "core/raster_ops.hpp"
#include "edit/adjustment_set.hpp"
#include "io/file_utils.hpp"
#include "io/image_codec.hpp"
#include "io/image_meta.hpp"
#include "io/minijson.hpp"
#include "io/sidecar.hpp"
#include "io/virtual_copy.hpp"
#include "render/render.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// Largest sidecar a batch will read: the same limit the editor puts on one.
constexpr std::streamoff kExportMaxSidecarBytes = std::streamoff{64} << 20;

// Encodes `img` as a JPEG at `quality` (1..100). With `keepMetadata`, the EXIF
// block and ICC profile of the photo at `sourcePath` ride along (orientation
// reset to "upright", because the pixels already are; picture size updated; see
// embedSourceMetadata). With `removeGps` as well, the GPS location is cut out of
// that EXIF block. A picture JPEG can't hold (see jpegSizeOk) is refused by
// encodeJpegMemory. (Moved here from main.cpp so the editor's single Export and
// the batch share one function.)
inline bool saveJpegFile(const std::string &path, const CanvasImage &img,
                         int quality, bool keepMetadata, bool removeGps,
                         const std::string &sourcePath, std::string *errorOut)
{
  std::vector<uint8_t> jpeg;
  if (!encodeJpegMemory(img, std::clamp(quality, 1, 100), &jpeg, errorOut))
    return false;
  if (keepMetadata)
    embedSourceMetadata(&jpeg, sourcePath, img.width, img.height, removeGps);
  return writeFileAtomic(path, jpeg, errorOut);
}

// What one batch writes. A copy of the Export pane's settings plus the folder.
struct ExportSettings
{
  bool jpeg = true;
  int quality = 92;
  bool keepMetadata = true;
  bool removeGps = false;
  bool resize = false;
  int longEdge = 2048;
  std::string folder;     // the destination folder (UTF-8)
  bool overwrite = false; // false: a taken name gets "-2", "-3", ...
};

struct ExportOutcome
{
  std::string path;     // the picture's path (a virtual copy's path included)
  bool ok = false;
  bool cancelled = false; // dropped or abandoned by cancel(): not a failure
  std::string outFile;  // the file written (ok only)
  std::string error;    // why it was skipped (not ok, not cancelled)
  int width = 0, height = 0; // size written (ok only)
};

// "photo" for "photo.jpg"; "photo-copy2" for the virtual copy "photo.jpg|vc2".
inline std::string exportStemOf(const std::string &path)
{
  std::string stem = baseName(vcImageOf(path));
  if (const auto dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0)
    stem.resize(dot);
  if (stem.empty())
    stem = "export";
  if (const int n = vcCopyOf(path); n > 0)
    stem += "-copy" + std::to_string(n);
  return stem;
}

// <folder>/<stem>-edit.<ext>, or with "-2", "-3", ... when that name is already
// used in this batch (`used`, compared without case, so a.jpg and A.png can't
// collide on a case-insensitive disk) or, unless `overwrite`, already exists.
// The chosen name is added to `used`. "" if 9999 names are all taken.
inline std::string exportTargetPath(const std::string &folder,
                                    const std::string &stem,
                                    const std::string &ext, bool overwrite,
                                    std::set<std::string> &used)
{
  for (int k = 1; k <= 9999; ++k)
  {
    const std::string name =
        stem + "-edit" + (k == 1 ? "" : "-" + std::to_string(k)) + "." + ext;
    const std::filesystem::path p = fsPath(folder) / fsPath(name);
    const std::string full = u8ToString(p);
    if (used.count(filmLower(full)))
      continue;
    std::error_code ec;
    if (!overwrite && std::filesystem::exists(p, ec))
      continue;
    used.insert(filmLower(full));
    return full;
  }
  return std::string();
}

// Loads the saved edits of `path` into `adj` (which must already have
// setOriginalSize(w, h)). No sidecar at all is normal: the picture exports
// unedited. A sidecar that exists but can't be used (damaged, newer, too big,
// written for a different size, or using a LUT the library no longer has) is an
// ERROR here, unlike in the editor, which opens such a picture clean: silently exporting an unedited picture in the
// middle of a batch would be worse than skipping it with a reason.
inline bool exportLoadEdits(const std::string &path, int w, int h,
                            AdjustmentSet *adj, std::string *err)
{
  const std::filesystem::path p = fsPath(sidecarFileOf(path));
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || ec)
    return true;
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f)
  {
    *err = "couldn't read its saved edits";
    return false;
  }
  const std::streamoff n = f.tellg();
  if (n <= 0 || n > kExportMaxSidecarBytes)
  {
    *err = "its saved edits have an unexpected size";
    return false;
  }
  std::string text(static_cast<size_t>(n), '\0');
  f.seekg(0);
  f.read(text.data(), n);
  if (!f)
  {
    *err = "couldn't read its saved edits";
    return false;
  }
  mj::Json root;
  std::string perr;
  if (!mj::Json::parse(text, &root, &perr))
  {
    *err = "its saved edits are unreadable";
    return false;
  }
  const mj::Json *ver = root.find("version");
  if (!ver || !ver->isNumber() || ver->asNum(0) > kSidecarVersion)
  {
    *err = "its saved edits are from a newer Literoom";
    return false;
  }
  const mj::Json *img = root.find("image");
  if (!img || !img->isObject() || !img->find("width") || !img->find("height") ||
      img->find("width")->asNum(-1) != w || img->find("height")->asNum(-1) != h)
  {
    *err = "the picture's size has changed since it was edited";
    return false;
  }
  const mj::Json *edits = root.find("edits");
  if (!edits || !adj->loadJson(*edits))
  {
    *err = "its saved edits are unreadable";
    return false;
  }
  // The same strictness for its LUT: a LUT that is in use (amount above 0) but
  // gone from the library (or damaged) would export the picture without its look.
  // A file that has changed since the LUT was chosen is used as it is.
  if (!adj->lut().empty() && adj->get(AdjId::LutAmount) > 0.0f)
  {
    const LutStatus st = adj->lutStatus();
    if (st == LutStatus::Missing)
    {
      *err = "its LUT \"" + adj->lut().name + "\" is not in the LUT library";
      return false;
    }
    if (st == LutStatus::Unreadable)
    {
      *err = "its LUT \"" + adj->lut().name + "\" can't be read";
      return false;
    }
  }
  return true;
}

// Renders and writes one picture. `latest` / `gen` are the usual cancellation
// pair (both may be null / 0 for a synchronous call); once superseded the
// outcome is `cancelled` and nothing is written. `used` holds the output names
// already taken by this batch.
inline ExportOutcome exportPicture(const std::string &path,
                                   const ExportSettings &s,
                                   std::set<std::string> &used,
                                   const std::atomic<uint64_t> *latest = nullptr,
                                   uint64_t gen = 0)
{
  ExportOutcome o;
  o.path = path;
  const auto gone = [&]
  { return latest && latest->load() != gen; };
  const auto cancel = [&]() -> ExportOutcome
  {
    o.cancelled = true;
    return o;
  };
  const auto fail = [&](std::string m) -> ExportOutcome
  {
    o.error = std::move(m);
    return o;
  };
  if (gone())
    return cancel();

  const std::string real = vcImageOf(path);
  std::string derr;
  auto raw = liteui_image::decodeFile(real, &derr);
  if (!raw)
    return fail("couldn't open it: " + derr);
  // The "original" the saved edits are stored against: upright (EXIF applied).
  std::shared_ptr<CanvasImage> original =
      orientImage(std::make_shared<CanvasImage>(std::move(*raw)),
                  exifOrientationOf(real));
  if (gone())
    return cancel();

  AdjustmentSet adj; // LOCAL: the editor's set is never touched
  adj.setOriginalSize(original->width, original->height);
  std::string eerr;
  if (!exportLoadEdits(path, original->width, original->height, &adj, &eerr))
    return fail(eerr);

  // The geometry worker's chain, from the untouched original.
  std::shared_ptr<CanvasImage> oriented = orientImage(original, adj.orient());
  std::shared_ptr<CanvasImage> rotated = oriented;
  if (adj.angle() != 0.0f)
  {
    rotated = rotateArbitrary(*oriented, adj.angle(), adj.expand(), latest, gen);
    if (!rotated)
      return cancel();
  }
  std::shared_ptr<CanvasImage> source =
      adj.crop().empty() ? rotated : cropImage(*rotated, adj.crop());
  original.reset(); // keep the peak low: only `source` is needed from here
  oriented.reset();
  rotated.reset();
  std::shared_ptr<CanvasImage> image =
      renderWith(*source, adj.buildRenderParams(), latest, gen);
  if (!image)
    return cancel();
  source.reset();

  const Dims size = fitLongEdge(image->width, image->height,
                                s.resize ? s.longEdge : 0);
  if (s.jpeg && !jpegSizeOk(size.w, size.h))
    return fail("too large for JPEG (turn on Resize, or export as PNG)");
  const bool resized = size.w != image->width || size.h != image->height;
  const std::shared_ptr<CanvasImage> out =
      resized ? resizeToLongEdge(image, s.longEdge) : image;
  if (gone())
    return cancel();

  const std::string target = exportTargetPath(
      s.folder, exportStemOf(path), s.jpeg ? "jpg" : "png", s.overwrite, used);
  if (target.empty())
    return fail("no free file name in the folder");
  std::string err;
  const bool wrote =
      s.jpeg ? saveJpegFile(target, *out, s.quality, s.keepMetadata,
                            s.removeGps && s.keepMetadata, real, &err)
             : encodePngFile(target, *out, &err);
  if (!wrote)
    return fail(err.empty() ? "couldn't write " + baseName(target) : err);
  o.ok = true;
  o.outFile = target;
  o.width = out->width;
  o.height = out->height;
  return o;
}

// One batch at a time, one picture at a time, in the order given.
class ExportWorker
{
public:
  ExportWorker() = default;
  ExportWorker(const ExportWorker &) = delete;
  ExportWorker &operator=(const ExportWorker &) = delete;
  ~ExportWorker() { shutdown(); }

  void setNotify(std::function<void()> fn) { notify_ = std::move(fn); }

  // Queues `paths` with `settings`. False (nothing changed) while a batch is
  // still running or when `paths` is empty.
  bool start(std::vector<std::string> paths, ExportSettings settings)
  {
    if (paths.empty() || busy())
      return false;
    if (!thread_.joinable())
      thread_ = std::jthread([this](std::stop_token st)
                             { run(st); });
    {
      std::lock_guard lk(m_);
      settings_ = std::move(settings);
      used_.clear(); // the worker is idle (busy() was false), so this is safe
      total_.store(paths.size());
      done_.store(0);
      queue_.assign(std::make_move_iterator(paths.begin()),
                    std::make_move_iterator(paths.end()));
    }
    cv_.notify_one();
    return true;
  }

  // Drops what is still queued (each gets a `cancelled` outcome) and abandons
  // the picture in flight. Returns at once; busy() turns false when the worker
  // has let go of the picture it was on.
  void cancel()
  {
    std::lock_guard lk(m_);
    epoch_.fetch_add(1); // the render / straighten in flight sees this
    for (std::string &p : queue_)
    {
      ExportOutcome o;
      o.path = std::move(p);
      o.cancelled = true;
      results_.push_back(std::move(o));
    }
    done_.fetch_add(queue_.size());
    queue_.clear();
  }

  // True from start() until every picture has an outcome.
  bool busy() const { return done_.load() < total_.load(); }
  size_t total() const { return total_.load(); }
  size_t done() const { return done_.load(); }

  // Every outcome made since the last call, in the order they finished.
  // Check busy() BEFORE calling this: if it was already false, nothing more
  // can arrive, so these are the last ones.
  std::vector<ExportOutcome> collect()
  {
    std::lock_guard lk(m_);
    std::vector<ExportOutcome> out;
    out.swap(results_);
    return out;
  }

  // Stops and joins. The picture in flight is abandoned, the queue dropped.
  void shutdown()
  {
    {
      std::lock_guard lk(m_);
      queue_.clear();
      epoch_.store(~uint64_t{0});
    }
    if (thread_.joinable())
    {
      thread_.request_stop();
      thread_.join();
    }
  }

private:
  void run(std::stop_token st)
  {
    for (;;)
    {
      std::string path;
      ExportSettings s;
      uint64_t gen = 0;
      {
        std::unique_lock lk(m_);
        if (!cv_.wait(lk, st, [this]
                      { return !queue_.empty(); }))
          return;
        path = std::move(queue_.front());
        queue_.pop_front();
        s = settings_;
        gen = epoch_.load();
      }
      ExportOutcome o = exportPicture(path, s, used_, &epoch_, gen);
      {
        std::lock_guard lk(m_);
        results_.push_back(std::move(o));
        done_.fetch_add(1); // with the push, under the lock: see collect()
      }
      if (notify_)
        notify_();
    }
  }

  std::mutex m_; // guards queue_, results_, settings_
  std::condition_variable_any cv_;
  std::deque<std::string> queue_;
  std::vector<ExportOutcome> results_;
  ExportSettings settings_;
  std::set<std::string> used_; // worker thread only (cleared while idle)
  std::function<void()> notify_;
  std::atomic<uint64_t> epoch_{1}; // bumped by cancel(); polled by the render
  std::atomic<size_t> total_{0}, done_{0};
  std::jthread thread_; // last: joined first
};