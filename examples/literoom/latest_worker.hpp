// latest_worker.hpp - one background thread that runs "latest wins" jobs.
//
// Replaces the hand-written RenderWorker / GeometryWorker / DecodeWorker /
// SegmentWorker, which all implemented the same protocol:
//   * every submit() and invalidate() bumps a generation number;
//   * only the newest request can win - a newer submit replaces a job that is
//     still waiting, and a running job polls the number (Cancel) to abandon
//     itself when it has been superseded;
//   * a finished value is parked under a lock and re-checked against the
//     newest generation, so a result that was invalidated while it finished
//     is never delivered;
//   * the UI thread is woken by notify() OUTSIDE the lock;
//   * shutdown() joins the thread.
//
// Thread model (unchanged): everything public is called from ONE thread (the
// UI thread). The worker thread, started lazily by the first submit(), only
// runs the job function and touches the parked slot. setNotify() must be
// called before the first submit().
//
// No liteui / editor dependencies: the header is tested with plain ints.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

// What a running job gets to see of the generation protocol. `latest` is the
// raw atomic so existing cancellable routines (renderWith, rotateArbitrary,
// segmentObject) keep their (const std::atomic<uint64_t>* latest, uint64_t gen)
// parameters unchanged: pass c.latest, c.gen.
struct WorkerCancel
{
  std::atomic<uint64_t> *latest = nullptr;
  uint64_t gen = 0;
  // True once a newer submit()/invalidate()/shutdown() has superseded this job.
  bool cancelled() const { return latest->load() != gen; }
};

template <class Job, class Value>
class LatestWorker
{
public:
  using JobType = Job;
  using Result = Value;
  using Cancel = WorkerCancel;
  // Runs on the worker thread. Return nullopt to say "abandoned" (the result
  // is dropped, nothing is parked, nothing is notified). Jobs that cannot be
  // cancelled simply always return a value (a failure value, if need be).
  using JobFn = std::function<std::optional<Value>(Job &&, Cancel)>;

  explicit LatestWorker(JobFn fn) : fn_(std::move(fn)) {}
  LatestWorker(const LatestWorker &) = delete;
  LatestWorker &operator=(const LatestWorker &) = delete;

  ~LatestWorker()
  {
    // Makes a running job bail at its next poll; thread_ (declared last, so
    // destroyed first) then requests stop and joins.
    latest_.store(kShutdown);
  }

  // Called from the worker thread each time a result has been parked. Set it
  // before the first submit(): the thread is created by submit, so it then
  // sees the value without further synchronization.
  void setNotify(std::function<void()> fn) { notify_ = std::move(fn); }

  // Stops the worker and joins it; afterwards notify_ will never run again.
  // Blocks until a running job that ignores Cancel has finished.
  void shutdown()
  {
    latest_.store(kShutdown);
    if (thread_.joinable())
    {
      thread_.request_stop(); // also wakes a worker sleeping for a job
      thread_.join();
    }
  }

  // Queues `job`, superseding any earlier one.
  void submit(Job job)
  {
    if (!thread_.joinable())
      thread_ = std::jthread([this](std::stop_token st)
                             { run(st); });
    const uint64_t gen = ++counter_;
    latest_.store(gen);
    outstanding_ = true;
    {
      std::lock_guard lk(m_);
      pending_.emplace(Pending{gen, std::move(job)});
      done_.reset();
    }
    workCv_.notify_one();
  }

  // Forgets whatever is waiting or running; its result will never be taken.
  void invalidate()
  {
    latest_.store(++counter_);
    outstanding_ = false;
    std::lock_guard lk(m_);
    pending_.reset();
    done_.reset();
  }

  // True from submit() until its result has been taken or invalidate().
  bool outstanding() const { return outstanding_; }

  // Non-blocking: the finished result of the newest request, if it is ready.
  std::optional<Value> collect()
  {
    if (!outstanding_)
      return std::nullopt;
    std::lock_guard lk(m_);
    if (done_ && done_->gen != latest_.load())
      done_.reset(); // stale (defensive: run() already filters these)
    if (!done_)
      return std::nullopt;
    Value v = std::move(done_->value);
    done_.reset();
    outstanding_ = false;
    return v;
  }

  // Blocking: waits for the newest request to finish. Only call while
  // outstanding() (nothing else could ever wake it).
  Value waitForLatest()
  {
    std::unique_lock lk(m_);
    doneCv_.wait(lk, [this]
                 { return done_.has_value(); });
    Value v = std::move(done_->value);
    done_.reset();
    outstanding_ = false;
    return v;
  }

private:
  struct Pending
  {
    uint64_t gen;
    Job job;
  };
  struct Done
  {
    uint64_t gen;
    Value value;
  };
  static constexpr uint64_t kShutdown = ~uint64_t{0};

  void run(std::stop_token st)
  {
    for (;;)
    {
      std::optional<Pending> cur;
      {
        std::unique_lock lk(m_);
        if (!workCv_.wait(lk, st, [this]
                          { return pending_.has_value(); }))
          return; // stop requested and nothing waiting
        cur.emplace(std::move(*pending_));
        pending_.reset();
      }
      const uint64_t gen = cur->gen;
      if (latest_.load() != gen)
        continue; // superseded while queued: don't even start
      std::optional<Value> v = fn_(std::move(cur->job), Cancel{&latest_, gen});
      cur.reset();
      if (!v)
        continue; // abandoned part-way: superseded
      {
        std::lock_guard lk(m_);
        // Re-check under the lock: if invalidate() ran while we finished, it
        // has already cleared the slot, and must not be refilled.
        if (latest_.load() != gen)
          continue;
        done_.emplace(Done{gen, std::move(*v)});
      }
      doneCv_.notify_all();
      if (notify_)
        notify_(); // outside the lock: it must not be able to deadlock on m_
    }
  }

  JobFn fn_;
  std::mutex m_;                       // guards pending_ and done_
  std::condition_variable_any workCv_; // "a job is waiting" (stop-aware)
  std::condition_variable doneCv_;     // "a result is parked"
  std::optional<Pending> pending_;
  std::optional<Done> done_;
  std::function<void()> notify_;    // see setNotify; read by the worker
  std::atomic<uint64_t> latest_{0}; // newest generation; read by the worker
  uint64_t counter_ = 0;            // UI thread only
  bool outstanding_ = false;        // UI thread only
  std::jthread thread_;             // keep last: joined first on destruction
};
