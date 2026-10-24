// thumb_worker.hpp - the filmstrip's thumbnails, made off the UI thread.
//
// makeThumb() box-averages a picture down to fit kFilmThumbW x kFilmThumbH;
// ThumbWorker decodes a queue of files (EXIF rotation applied, like the
// editor does), thumbnails them, and reads each one's saved-edits sidecar for
// two facts: whether it holds real edits (the filmstrip's dot) and the
// picture's rating, flag and color label. A different kind of worker from LatestWorker: it is a FIFO queue
// with no generations - setQueue() replaces what is still to do, the item in
// flight finishes, and results accumulate until collect() drains them.
#pragma once
#include "liteui.hpp"
#include "core/geometry.hpp"      // Orient
#include "core/raster_ops.hpp"    // orientImage
#include "io/file_utils.hpp"      // fsPath
#include "io/photo_meta.hpp"     // PhotoMeta
#include "io/sidecar.hpp"         // sidecarFileOf, sidecarScan

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

constexpr int kFilmThumbW = 96, kFilmThumbH = 72;

// Largest sidecar the thumbnail worker will read: the same limit the editor
// puts on a sidecar it opens. A bigger file counts as edited, with no meta.
constexpr std::streamoff kThumbMaxSidecarBytes = std::streamoff{64} << 20;

// The sidecar's text; false if there is no file, it is empty, too big or
// unreadable. *exists says whether the file was there at all.
inline bool readSidecarForThumb(const std::string &imagePath, std::string *text,
                                bool *exists)
{
  const std::filesystem::path p = fsPath(sidecarFileOf(imagePath));
  std::error_code ec;
  *exists = std::filesystem::exists(p, ec) && !ec;
  if (!*exists)
    return false;
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f)
    return false;
  const std::streamoff n = f.tellg();
  if (n <= 0 || n > kThumbMaxSidecarBytes)
    return false;
  text->assign(static_cast<size_t>(n), '\0');
  f.seekg(0);
  f.read(text->data(), n);
  return static_cast<bool>(f);
}

// `src` scaled down (box average) to fit kFilmThumbW x kFilmThumbH; never
// enlarged. Null for an empty picture.
inline std::shared_ptr<const CanvasImage> makeThumb(const CanvasImage &src)
{
  if (src.width <= 0 || src.height <= 0 ||
      src.pixels.size() < static_cast<size_t>(src.width) * src.height * 4)
    return nullptr;
  const double s = std::min({1.0, static_cast<double>(kFilmThumbW) / src.width,
                             static_cast<double>(kFilmThumbH) / src.height});
  const int nw = std::max(1, static_cast<int>(std::lround(src.width * s)));
  const int nh = std::max(1, static_cast<int>(std::lround(src.height * s)));
  auto out = std::make_shared<CanvasImage>(nw, nh);
  for (int y = 0; y < nh; ++y)
  {
    const int64_t y0 = static_cast<int64_t>(y) * src.height / nh;
    const int64_t y1 =
        std::max(y0 + 1, static_cast<int64_t>(y + 1) * src.height / nh);
    for (int x = 0; x < nw; ++x)
    {
      const int64_t x0 = static_cast<int64_t>(x) * src.width / nw;
      const int64_t x1 =
          std::max(x0 + 1, static_cast<int64_t>(x + 1) * src.width / nw);
      uint32_t sum[4] = {0, 0, 0, 0};
      for (int64_t yy = y0; yy < y1; ++yy)
      {
        const uint8_t *p = &src.pixels[(static_cast<size_t>(yy) * src.width +
                                        static_cast<size_t>(x0)) *
                                       4];
        for (int64_t xx = x0; xx < x1; ++xx, p += 4)
        {
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          sum[3] += p[3];
        }
      }
      const uint32_t n = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
      uint8_t *o = &out->pixels[(static_cast<size_t>(y) * nw + x) * 4];
      for (int k = 0; k < 4; ++k)
        o[k] = static_cast<uint8_t>((sum[k] + n / 2) / n);
    }
  }
  return out;
}

// Makes the thumbnails off the UI thread. setQueue() replaces what is still
// to do (it is called again whenever the open picture changes, so the work
// always starts next to it); collect() hands back what has been made.
class ThumbWorker
{
public:
  struct Result
  {
    std::string path;
    std::shared_ptr<const CanvasImage> thumb; // null: couldn't be decoded
    bool edited = false;                      // its sidecar holds real edits
                                              // (an unreadable one counts)
    PhotoMeta meta;                           // rating / flag / color label
  };

  // `orientOf` says how a file's camera rotation (EXIF) turns it upright. The
  // app passes its own EXIF reader, so thumbnails are turned exactly like the
  // editor turns the picture.
  using OrientFn = std::function<Orient(const std::string &)>;
  explicit ThumbWorker(OrientFn orientOf) : orientOf_(std::move(orientOf)) {}
  ThumbWorker(const ThumbWorker &) = delete;
  ThumbWorker &operator=(const ThumbWorker &) = delete;
  ~ThumbWorker() { shutdown(); }

  void setNotify(std::function<void()> fn) { notify_ = std::move(fn); }

  void shutdown()
  {
    {
      std::lock_guard lk(m_);
      queue_.clear();
    }
    if (thread_.joinable())
    {
      thread_.request_stop();
      thread_.join();
    }
  }

  void setQueue(std::vector<std::string> paths)
  {
    if (!thread_.joinable())
      thread_ = std::jthread([this](std::stop_token st)
                             { run(st); });
    {
      std::lock_guard lk(m_);
      queue_.assign(std::make_move_iterator(paths.begin()),
                    std::make_move_iterator(paths.end()));
    }
    cv_.notify_one();
  }

  std::vector<Result> collect()
  {
    std::lock_guard lk(m_);
    std::vector<Result> out;
    out.swap(results_);
    return out;
  }

private:
  void run(std::stop_token st)
  {
    for (;;)
    {
      std::string path;
      {
        std::unique_lock lk(m_);
        if (!cv_.wait(lk, st, [this]
                      { return !queue_.empty(); }))
          return;
        path = std::move(queue_.front());
        queue_.pop_front();
      }
      Result r;
      r.path = path;
      std::string err;
      if (auto img = liteui_image::decodeFile(path, &err))
      {
        const Orient o = orientOf_(path); // upright, like the editor
        if (o.identity())
          r.thumb = makeThumb(*img);
        else
        {
          auto turned =
              orientImage(std::make_shared<CanvasImage>(std::move(*img)), o);
          r.thumb = makeThumb(*turned);
        }
      }
      {
        std::string text;
        bool exists = false;
        if (readSidecarForThumb(path, &text, &exists))
        {
          const SidecarScan s = sidecarScan(text);
          r.edited = s.edited;
          r.meta = s.meta;
        }
        else
          r.edited = exists; // present but unreadable / too big: as before
      }
      {
        std::lock_guard lk(m_);
        results_.push_back(std::move(r));
      }
      if (notify_)
        notify_();
    }
  }

  OrientFn orientOf_;
  std::mutex m_;
  std::condition_variable_any cv_;
  std::deque<std::string> queue_;
  std::vector<Result> results_;
  std::function<void()> notify_;
  std::jthread thread_; // last: joined first
};