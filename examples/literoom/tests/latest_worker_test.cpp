// Build: g++ -std=c++20 -I.. -pthread latest_worker_test.cpp -o latest_worker_test && ./latest_worker_test
//   (add -fsanitize=thread -g -O1 for the data-race check)
//
// Gates and latches instead of sleeps; every wait has a timeout so a broken
// worker fails the test instead of hanging it. CHECK is not thread-safe
// (check.hpp), so assertions only ever run on the main thread; the worker
// thread only records into atomics / mutex-guarded state.
#include "latest_worker.hpp"
#include "check.hpp"

#include <chrono>
#include <memory>
#include <random>
#include <vector>

using namespace std::chrono_literals;
static constexpr auto kWait = 5s;

// Counts events; waitFor(n) blocks until at least n have happened.
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
  bool waitFor(int n, std::chrono::milliseconds t = std::chrono::duration_cast<std::chrono::milliseconds>(kWait))
  {
    std::unique_lock lk(m_);
    return cv_.wait_for(lk, t, [&] { return n_ >= n; });
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

// A door the job waits at until the test opens it.
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

// Gives the worker time to run code that has no observable signal (e.g. the
// bookkeeping after a job returns). Only used to let a NEGATIVE check bite: too
// short a grace can hide a bug (a false pass), but can never fail a correct
// worker.
static void grace() { std::this_thread::sleep_for(100ms); }

// Records which job ids actually started, in order.
class Log
{
public:
  void add(int v)
  {
    std::lock_guard lk(m_);
    v_.push_back(v);
  }
  std::vector<int> get()
  {
    std::lock_guard lk(m_);
    return v_;
  }

private:
  std::mutex m_;
  std::vector<int> v_;
};

using IntWorker = LatestWorker<int, int>;

// Spins until `c` is cancelled (or the timeout hits). True = it was cancelled.
static bool waitCancelled(const WorkerCancel &c)
{
  const auto end = std::chrono::steady_clock::now() + kWait;
  while (!c.cancelled())
  {
    if (std::chrono::steady_clock::now() > end)
      return false;
    std::this_thread::yield();
  }
  return true;
}

// ------------------------------------------------------------------ basics
static void testBasic()
{
  Latch notified;
  IntWorker w([](int &&j, WorkerCancel) -> std::optional<int>
              { return j * 2; });
  w.setNotify([&] { notified.signal(); });
  CHECK(!w.outstanding());
  CHECK(!w.collect().has_value()); // nothing submitted
  w.submit(21);
  CHECK(w.outstanding());
  CHECK(notified.waitFor(1));
  auto r = w.collect();
  CHECK(r.has_value());
  CHECK(r && *r == 42);
  CHECK(!w.outstanding());
  CHECK(!w.collect().has_value()); // delivered once
  w.shutdown();
  CHECK(notified.count() == 1);
}

// --------------------------------------------- newest wins, queued job skipped
static void testNewestWinsAndQueuedNeverStarts()
{
  Latch started, notified;
  Gate gate;
  Log log;
  IntWorker w([&](int &&j, WorkerCancel) -> std::optional<int>
              {
                log.add(j);
                started.signal();
                if (j == 1)
                  gate.wait(); // ignores cancellation on purpose
                return j * 10; });
  w.setNotify([&] { notified.signal(); });
  w.submit(1);
  CHECK(started.waitFor(1)); // job 1 is running, held at the gate
  w.submit(2);               // queued...
  w.submit(3);               // ...and replaced before it ever starts
  CHECK(!w.collect().has_value());
  gate.open();
  CHECK(notified.waitFor(1));
  auto r = w.collect();
  CHECK(r && *r == 30);      // job 1's result (10) never arrives
  w.shutdown();              // joins: the worker is now fully idle
  CHECK(notified.count() == 1);
  const std::vector<int> expect{1, 3};
  CHECK(log.get() == expect); // 2 never started
}

// ------------------------------------------- a running job sees the cancel flag
static void testCancelOnSubmit()
{
  Latch started, notified;
  std::atomic<int> sawCancel{0};
  IntWorker w([&](int &&j, WorkerCancel c) -> std::optional<int>
              {
                started.signal();
                if (j == 1)
                {
                  if (waitCancelled(c))
                    ++sawCancel;
                  if (c.cancelled())
                    return std::nullopt; // abandoned
                }
                return j; });
  w.setNotify([&] { notified.signal(); });
  w.submit(1);
  CHECK(started.waitFor(1));
  w.submit(2); // supersedes the running job
  CHECK(notified.waitFor(1));
  auto r = w.collect();
  CHECK(r && *r == 2);
  w.shutdown();
  CHECK(sawCancel == 1);
  CHECK(notified.count() == 1); // the abandoned job produced no notify
}

static void testCancelOnInvalidate()
{
  Latch started, finished, notified;
  std::atomic<int> sawCancel{0};
  IntWorker w([&](int &&, WorkerCancel c) -> std::optional<int>
              {
                started.signal();
                if (waitCancelled(c))
                  ++sawCancel;
                finished.signal();
                return std::nullopt; });
  w.setNotify([&] { notified.signal(); });
  w.submit(1);
  CHECK(started.waitFor(1));
  w.invalidate();
  CHECK(!w.outstanding());
  CHECK(finished.waitFor(1));
  CHECK(!w.collect().has_value());
  w.shutdown();
  CHECK(sawCancel == 1);
  CHECK(notified.count() == 0); // abandoned: no result, no notify
}

// ------------------------- invalidate drops pending work and a running result
static void testInvalidateDropsPending()
{
  Latch started, notified;
  Gate gate;
  Log log;
  IntWorker w([&](int &&j, WorkerCancel) -> std::optional<int>
              {
                log.add(j);
                started.signal();
                if (j == 1)
                  gate.wait();
                return j; });
  w.setNotify([&] { notified.signal(); });
  w.submit(1);
  CHECK(started.waitFor(1));
  w.submit(2); // pending behind the running job 1
  w.invalidate(); // drops the pending one as well
  gate.open();
  grace();     // let the worker finish job 1 (and look for more work)
  w.submit(3); // a sentinel: runs after job 1 has been dealt with
  CHECK(notified.waitFor(1));
  auto r = w.collect();
  CHECK(r && *r == 3);
  w.shutdown();
  CHECK(notified.count() == 1); // job 1's finished-but-invalidated result: no notify
  const std::vector<int> expect{1, 3};
  CHECK(log.get() == expect);
}


// A result parked but not yet collected is thrown away by a newer submit() or
// by invalidate(): waitForLatest must then wait for the NEW job.
static void testParkedResultDiscarded()
{
  {
    Latch started, notified;
    Gate gate;
    IntWorker w([&](int &&j, WorkerCancel) -> std::optional<int>
                {
                  started.signal();
                  if (j == 2)
                    gate.wait();
                  return j; });
    w.setNotify([&] { notified.signal(); });
    w.submit(1);
    CHECK(notified.waitFor(1)); // 1's result is parked, not collected
    w.submit(2);                // must discard it
    CHECK(started.waitFor(2));  // job 2 is running, held at the gate
    int got = -1;
    // waitForLatest blocks, so it runs on a helper thread (the test thread
    // opens the gate meanwhile). It must wait for job 2, not hand over job 1.
    std::thread t([&] { got = w.waitForLatest(); });
    grace();
    gate.open();
    t.join();
    CHECK(got == 2);
    w.shutdown();
  }
  {
    Latch started, notified;
    Gate gate;
    IntWorker w([&](int &&j, WorkerCancel) -> std::optional<int>
                {
                  started.signal();
                  if (j == 2)
                    gate.wait();
                  return j; });
    w.setNotify([&] { notified.signal(); });
    w.submit(1);
    CHECK(notified.waitFor(1)); // parked
    w.invalidate();             // discards it, even though nothing else follows
    CHECK(!w.outstanding());
    w.submit(2);
    CHECK(started.waitFor(2));
    gate.open();
    CHECK(w.waitForLatest() == 2);
    w.shutdown();
  }
}

// nullopt means "abandoned": nothing is parked, nothing is notified.
static void testNulloptIsAbandoned()
{
  Latch finished, notified;
  IntWorker w([&](int &&j, WorkerCancel) -> std::optional<int>
              {
                finished.signal();
                if (j == 1)
                  return std::nullopt; // gives up although nobody superseded it
                return j; });
  w.setNotify([&] { notified.signal(); });
  w.submit(1);
  CHECK(finished.waitFor(1));
  grace(); // job 1 has returned nullopt and nobody superseded it
  CHECK(notified.count() == 0);
  CHECK(!w.collect().has_value()); // nothing was parked for it
  w.submit(2);
  CHECK(notified.waitFor(1));
  auto r = w.collect();
  CHECK(r && *r == 2);
  w.shutdown();
  CHECK(notified.count() == 1);
}

// ------------------------------------------ results & notify: where and when
static void testNotifyThreadAndOutsideLock()
{
  Latch inNotify;
  Gate release;
  std::atomic<bool> timedOut{false};
  std::thread::id notifyThread;
  const std::thread::id mainThread = std::this_thread::get_id();
  IntWorker w([](int &&j, WorkerCancel) -> std::optional<int>
              { return j; });
  w.setNotify([&]
              {
                notifyThread = std::this_thread::get_id();
                inNotify.signal();
                // Hold the worker inside notify until main has called collect().
                // If notify ran under the worker's lock, that collect() could not
                // get in and this wait would time out.
                if (!release.wait())
                  timedOut = true; });
  w.submit(5);
  CHECK(inNotify.waitFor(1));
  CHECK(notifyThread != mainThread);
  auto r = w.collect(); // needs the lock the worker must not be holding
  CHECK(r && *r == 5);
  release.open();
  w.shutdown();
  CHECK(!timedOut);
}

static void testWaitForLatest()
{
  Latch started;
  Gate gate;
  IntWorker w([&](int &&j, WorkerCancel) -> std::optional<int>
              {
                started.signal();
                if (j == 1)
                  gate.wait();
                return j * 3; });
  w.submit(1);
  CHECK(started.waitFor(1));
  w.submit(2);
  gate.open();
  CHECK(w.outstanding());
  const int v = w.waitForLatest(); // job 1's result is superseded, so this is 2's
  CHECK(v == 6);
  CHECK(!w.outstanding());
  CHECK(!w.collect().has_value());
  w.shutdown();
}

static void testCollectBeforeCompletion()
{
  Latch started, notified;
  Gate gate;
  IntWorker w([&](int &&j, WorkerCancel) -> std::optional<int>
              {
                started.signal();
                gate.wait();
                return j; });
  w.setNotify([&] { notified.signal(); });
  w.submit(9);
  CHECK(started.waitFor(1));
  CHECK(!w.collect().has_value()); // still running
  CHECK(w.outstanding());          // ...and still outstanding
  gate.open();
  CHECK(notified.waitFor(1));
  auto r = w.collect();
  CHECK(r && *r == 9);
  CHECK(!w.collect().has_value()); // once only
  w.shutdown();
}

// --------------------------------------------------------- shutdown, destruction
static void testShutdown()
{
  {
    Latch notified;
    IntWorker w([](int &&j, WorkerCancel) -> std::optional<int>
                { return j; });
    w.setNotify([&] { notified.signal(); });
    w.shutdown(); // before the first submit: nothing to join
    w.shutdown(); // twice
    CHECK(notified.count() == 0);
  }
  {
    Latch started, notified;
    IntWorker w([&](int &&, WorkerCancel c) -> std::optional<int>
                {
                  started.signal();
                  waitCancelled(c);
                  return 1; }); // returns a value even though it was cancelled
    w.setNotify([&] { notified.signal(); });
    w.submit(1);
    CHECK(started.waitFor(1));
    w.shutdown(); // cancels the running job and joins
    w.shutdown();
    CHECK(notified.count() == 0); // nothing is notified once shut down
    CHECK(!w.collect().has_value());
  }
}

static void testDestruction()
{
  const auto t0 = std::chrono::steady_clock::now();
  {
    Latch started;
    IntWorker w([&](int &&, WorkerCancel c) -> std::optional<int>
                {
                  started.signal();
                  waitCancelled(c);
                  return std::nullopt; });
    w.submit(1);
    w.submit(2); // may be queued behind the running one
    CHECK(started.waitFor(1));
  } // ~LatestWorker cancels and joins
  {
    IntWorker idle([](int &&j, WorkerCancel) -> std::optional<int>
                   { return j; });
  } // never started a thread
  CHECK(std::chrono::steady_clock::now() - t0 < 3s);
}

// ------------------------------------------------------ move-only job and value
static void testMoveOnly()
{
  using W = LatestWorker<std::unique_ptr<int>, std::unique_ptr<int>>;
  Latch notified;
  W w([](std::unique_ptr<int> &&j, WorkerCancel) -> std::optional<std::unique_ptr<int>>
      { return std::make_unique<int>(*j + 1); });
  w.setNotify([&] { notified.signal(); });
  w.submit(std::make_unique<int>(41));
  CHECK(notified.waitFor(1));
  auto r = w.collect();
  CHECK(r && *r && **r == 42);
  w.submit(std::make_unique<int>(1));
  auto v = w.waitForLatest();
  CHECK(v && *v == 2);
  w.shutdown();
}

// --------------------------------------------------------------------- stress
// 2000 submits mixed with random invalidates. Main is the only thread that
// submits/invalidates/collects, so at the moment collect() hands over a value
// the only legal one is that of the newest submit, and only if no invalidate
// followed it.
static void testStress()
{
  std::atomic<int> jobsRun{0};
  std::atomic<int> notifies{0};
  IntWorker w([&](int &&id, WorkerCancel c) -> std::optional<int>
              {
                ++jobsRun;
                const int spins = (id * 7919) % 50;
                for (int i = 0; i < spins; ++i)
                {
                  if (c.cancelled())
                    return std::nullopt;
                  std::this_thread::yield();
                }
                return id; });
  w.setNotify([&] { ++notifies; });
  std::mt19937 rng(12345);
  int lastSubmit = 0;
  bool liveSubmit = false; // a submit with no invalidate after it
  int delivered = 0, stale = 0;
  for (int id = 1; id <= 2000; ++id)
  {
    w.submit(id);
    lastSubmit = id;
    liveSubmit = true;
    const unsigned dice = rng() % 8;
    if (dice == 0)
    {
      w.invalidate();
      liveSubmit = false;
    }
    if (dice <= 3)
    {
      if (auto r = w.collect())
      {
        ++delivered;
        if (!liveSubmit || *r != lastSubmit)
          ++stale;
      }
    }
    if (dice == 4)
      std::this_thread::yield();
    if (dice == 5 && liveSubmit)
    { // let this one finish: nothing supersedes it while main waits
      std::optional<int> r;
      const auto end = std::chrono::steady_clock::now() + kWait;
      while (!(r = w.collect()) && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
      CHECK(r.has_value());
      if (r)
      {
        ++delivered;
        if (*r != lastSubmit)
          ++stale;
      }
    }
  }
  CHECK(stale == 0);
  w.submit(5000); // the last word
  const int last = w.waitForLatest();
  CHECK(last == 5000);
  CHECK(!w.outstanding());
  w.shutdown();
  const int jobs = jobsRun, n = notifies;
  CHECK(jobs >= 1);
  CHECK(n >= delivered + 1); // every delivered value was notified first
  CHECK(n <= jobs);
  std::printf("stress: %d jobs ran, %d notified, %d collected\n", jobs, n, delivered);
}

int main()
{
  testBasic();
  testNewestWinsAndQueuedNeverStarts();
  testCancelOnSubmit();
  testCancelOnInvalidate();
  testInvalidateDropsPending();
  testParkedResultDiscarded();
  testNulloptIsAbandoned();
  testNotifyThreadAndOutsideLock();
  testWaitForLatest();
  testCollectBeforeCompletion();
  testShutdown();
  testDestruction();
  testMoveOnly();
  testStress();
  return finish();
}
