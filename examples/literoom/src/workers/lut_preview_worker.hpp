// lut_preview_worker.hpp - The LUTs pane's previews, made off the UI thread.
//
// One job = the open picture, its current look (a RenderParams) and a list of
// LUT files. For each file the worker reads and parses the LUT (an uncached
// load: a hundred previews must not push the LUT that is in use out of the
// library's cache), renders the picture with that LUT at 100 % and posts a
// thumbnail, together with what the file turned out to be: the hash of its
// bytes (what a LutRef stores), its table size, the "may expect camera-log
// input" warning, or why it could not be read.
//
// What a preview shows. The picture is box-averaged ONCE to the filmstrip's
// thumbnail size (makeThumb), then rendered with the picture's own look: tone,
// curves, color, Color Mixer, Color Grading, Filters, masks, Vignette and Grain.
// The neighbor effects (Texture, Clarity, Sharpness, Dehaze, Noise Reduction,
// Blur, global and per mask) are left out: they are invisible at this size, and
// leaving them out means a preview never touches the shared detail-plane cache the
// main render uses. The look's own LUT is REPLACED by the one being previewed
// (at 100 %), never stacked on it.
//
// Protocol (a FIFO like ThumbWorker, with generations like LatestWorker):
//   * submit() replaces whatever is still waiting and abandons the picture in
//     flight (renderWith polls the generation, so a thumbnail stops within a few
//     rows); results of the replaced job are never delivered;
//   * results arrive one per LUT, in the order given, and accumulate until
//     collect() drains them; each carries the job's serial so the UI can also
//     tell a late result from the current list;
//   * the UI thread is woken by notify() after every result and when a job ends.
//
// Thread model: submit(), cancel(), busy(), collect(), shutdown() from ONE thread
// (the UI thread). setNotify() must be called before the first submit().
#pragma once
#include "liteui.hpp"
#include "color/color_pass.hpp"
#include "color/lut3d.hpp"
#include "edit/render_params.hpp"
#include "io/lut_cube.hpp"
#include "io/lut_library.hpp"
#include "render/render.hpp"
#include "workers/thumb_worker.hpp" // makeThumb, kFilmThumbW / kFilmThumbH

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct LutPreviewJob
{
  uint64_t serial = 0;                       // echoed in every result
  std::shared_ptr<const CanvasImage> base;   // the picture to preview on; null = no thumbnails
  RenderParams params;                       // its current look (its LUT, if any, is replaced)
  std::vector<LutEntry> items;               // the LUTs to preview (name + path; at most kLutMaxFiles)
};

struct LutPreviewResult
{
  uint64_t serial = 0;
  std::string name, path;                    // as given in the job
  std::shared_ptr<const CanvasImage> thumb;  // null: no picture given, or the file is unusable
  uint64_t hash = 0;                         // hash of the file's bytes (0 if it could not be read)
  int size = 0;                              // grid points per axis (0 if it could not be read)
  std::string warning;                       // lutExpectationWarning, or ""
  std::string error;                         // why the file is unusable, or ""
};

// `in` with its LUT replaced by `lut`, and without the neighbor effects: the look a
// preview is rendered with. (A function of its own so the tests can build the same
// reference.)
inline RenderParams lutPreviewParams(const RenderParams &in, const LutPass &lut)
{
  RenderParams p = in;
  p.color = colorPassWithLut(in.color, lut);
  p.fx = neighbor_fx::Gains{};
  for (MaskPass &m : p.masks)
    m.fx = neighbor_fx::Gains{};
  return p;
}

class LutPreviewWorker
{
public:
  LutPreviewWorker() = default;
  LutPreviewWorker(const LutPreviewWorker &) = delete;
  LutPreviewWorker &operator=(const LutPreviewWorker &) = delete;
  ~LutPreviewWorker() { shutdown(); }

  void setNotify(std::function<void()> fn) { notify_ = std::move(fn); }

  // Replaces whatever is waiting or running with `job`; results of the replaced
  // job that were not collected yet are dropped.
  void submit(LutPreviewJob job)
  {
    if (job.items.size() > kLutMaxFiles)
      job.items.resize(kLutMaxFiles);
    if (!thread_.joinable())
      thread_ = std::jthread([this](std::stop_token st)
                             { run(st); });
    {
      std::lock_guard lk(m_);
      epoch_.fetch_add(1); // the picture in flight sees this and stops
      pending_ = std::move(job);
      results_.clear();
    }
    cv_.notify_one();
  }

  // Forgets the job (waiting or running) and every uncollected result.
  void cancel()
  {
    std::lock_guard lk(m_);
    epoch_.fetch_add(1);
    pending_.reset();
    results_.clear();
  }

  // True from submit() until the job has produced its last result (or was replaced
  // or cancelled and the worker has let go of it).
  bool busy() const
  {
    std::lock_guard lk(m_);
    return running_ || pending_.has_value();
  }

  // Every result made since the last call, in the order they were made.
  std::vector<LutPreviewResult> collect()
  {
    std::lock_guard lk(m_);
    std::vector<LutPreviewResult> out;
    out.swap(results_);
    return out;
  }

  // Stops and joins; the picture in flight is abandoned. Safe to call twice.
  void shutdown()
  {
    {
      std::lock_guard lk(m_);
      pending_.reset();
      results_.clear();
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
      LutPreviewJob job;
      uint64_t gen = 0;
      {
        std::unique_lock lk(m_);
        if (!cv_.wait(lk, st, [this]
                      { return pending_.has_value(); }))
          return;
        job = std::move(*pending_);
        pending_.reset();
        gen = epoch_.load();
        running_ = true;
      }
      process(job, gen);
      {
        std::lock_guard lk(m_);
        running_ = false;
      }
      if (notify_)
        notify_(); // the job is over: the UI may stop showing "working"
    }
  }

  void process(const LutPreviewJob &job, uint64_t gen)
  {
    std::shared_ptr<const CanvasImage> reduced;
    if (job.base)
      reduced = makeThumb(*job.base); // once, for every LUT
    for (const LutEntry &item : job.items)
    {
      if (epoch_.load() != gen)
        return; // superseded
      LutPreviewResult r;
      r.serial = job.serial;
      r.name = item.name;
      r.path = item.path;
      Lut3D lut;
      LutCubeInfo info;
      uint64_t hash = 0;
      // On failure r.error says why; there is then no hash, no size and no thumbnail.
      if (loadLutFile(item.path, &lut, &info, &hash, &r.error))
      {
        r.hash = hash;
        r.size = lut.size;
        r.warning = lutExpectationWarning(item.name, lut, info);
        if (reduced)
        {
          const auto table = std::make_shared<const Lut3D>(std::move(lut));
          const RenderParams p = lutPreviewParams(job.params, makeLutPass(table, 100.0f));
          std::shared_ptr<CanvasImage> img = renderWith(*reduced, p, &epoch_, gen);
          if (!img)
            return; // superseded while rendering
          r.thumb = std::move(img);
        }
      }
      {
        std::lock_guard lk(m_);
        if (epoch_.load() != gen)
          return; // submit() / cancel() ran while we worked: this result is stale
        results_.push_back(std::move(r));
      }
      if (notify_)
        notify_();
    }
  }

  mutable std::mutex m_; // guards pending_, results_, running_
  std::condition_variable_any cv_;
  std::optional<LutPreviewJob> pending_;
  std::vector<LutPreviewResult> results_;
  bool running_ = false;
  std::function<void()> notify_;
  std::atomic<uint64_t> epoch_{1}; // bumped by submit() / cancel(); polled by the render
  std::jthread thread_;            // last: joined first
};