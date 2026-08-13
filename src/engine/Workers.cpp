#include "engine/Workers.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <functional>
#include <unistd.h>
#include <utility>

#include "js/Interpreter.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// One byte down the pipe, once per batch. Retried on EINTR and *ignored* on a full pipe: a full pipe
// means the loop has not drained the last signal yet, which is precisely the case where another one
// would tell it nothing.
void Signal(int descriptor) {
  if (descriptor < 0) {
    return;
  }
  const char byte = 1;
  while (::write(descriptor, &byte, 1) < 0 && errno == EINTR) {
  }
}

void Drain(int descriptor) {
  if (descriptor < 0) {
    return;
  }
  char buffer[64];
  while (::read(descriptor, buffer, sizeof(buffer)) > 0) {
  }
}

}  // namespace

Workers::~Workers() { Clear(); }

std::uint64_t Workers::Reserve(std::string name, WorkerLocation location) {
  std::lock_guard<std::mutex> guard(workers_mutex_);
  if (workers_.size() >= kMaxWorkers) {
    AddPerformanceCounter(PerfCounterId::WorkerRefusals);
    return 0;
  }
  int pipes[2] = {-1, -1};
  // `O_NONBLOCK` on both ends, and it is load-bearing on each: a non-blocking write means a worker
  // signalling a loop that has not drained yet does not block, and a non-blocking read means the drain
  // does not block when there is nothing there.
  if (::pipe(pipes) != 0) {
    return 0;
  }
  for (const int descriptor : pipes) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0) {
      (void)::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    }
  }
  auto worker = std::make_unique<Worker>();
  worker->id = ++next_id_;
  worker->name = std::move(name);
  worker->location = std::move(location);
  worker->wake_read = pipes[0];
  worker->wake_write = pipes[1];
  const std::uint64_t id = worker->id;
  workers_.emplace(id, std::move(worker));
  return id;
}

bool Workers::Provide(std::uint64_t id, std::string source) {
  std::lock_guard<std::mutex> guard(workers_mutex_);
  const auto found = workers_.find(id);
  if (found == workers_.end() || found->second->thread.joinable()) {
    return false;  // gone, or already running
  }
  Worker* raw = found->second.get();
  raw->source = std::move(source);
  // The thread is started *last*, after the entry is in the map and the source is set, so that a worker
  // which posts back immediately finds its own outbox rather than racing the insertion. Anything a page
  // posted while the script was being fetched is already in the inbox and is drained on the first loop.
  raw->thread = std::thread([this, raw] { Run(*raw); });
  AddPerformanceCounter(PerfCounterId::WorkersStarted);
  return true;
}

void Workers::FailToLoad(std::uint64_t id, std::string reason) {
  std::unique_ptr<Worker> owned;
  {
    std::lock_guard<std::mutex> guard(workers_mutex_);
    const auto found = workers_.find(id);
    if (found == workers_.end() || found->second->thread.joinable()) {
      return;
    }
    // Kept in the map long enough to queue the delivery, because the page still holds the object and its
    // `error` handler is how it finds out. Removed after: a worker with no script will never run.
    (void)reason;
    owned = std::move(found->second);
    workers_.erase(found);
  }
  // The thread never started, so `JoinAndClose` only closes the pipe. Which is why the delivery cannot
  // go through the queue: the read end is gone before the loop's next turn. The *caller* reports this
  // failure -- see `Page::FailWorkerLoad`, which delivers the `error` event directly.
  if (owned != nullptr) {
    JoinAndClose(*owned);
  }
}

void Workers::Wake(Worker& worker) {
  // One byte per batch: if the loop has not drained the last signal, another tells it nothing.
  if (!worker.signalled.exchange(true)) {
    Signal(worker.wake_write);
  }
}

void Workers::Run(Worker& worker) {
  // **The interpreter is constructed here, on this thread, and destroyed here.** That is stronger than
  // a lock: there is no window in which two threads could both hold it, so there is nothing to
  // synchronise. ADR 0022's "it owns its heap" is enforced by scope rather than by discipline.
  js::Interpreter interpreter;
  const auto post = [this, &worker](Delivery delivery) {
    {
      std::lock_guard<std::mutex> guard(worker.outbox_mutex);
      if (worker.outbox.size() >= kMaxQueuedMessages) {
        AddPerformanceCounter(PerfCounterId::WorkerMessagesDropped);
        return;
      }
      worker.outbox.push_back(std::move(delivery));
    }
    this->Wake(worker);
  };

  // The host half of the worker's global. Everything a thread with no borrows cannot do for itself,
  // and nothing else -- see `WorkerScopeHost`. It is a local class rather than a member because its
  // whole lifetime is this function's: it captures the worker and the queues, both of which outlive it
  // and neither of which anything else may touch.
  struct Host final : WorkerScopeHost {
    Host(Workers& owner_in, Worker& worker_in, std::function<void(Delivery)> post_in)
        : owner(owner_in), worker(worker_in), post(std::move(post_in)) {}

    Workers& owner;
    Worker& worker;
    std::function<void(Delivery)> post;
    bool closed = false;

    void PostToPage(js::SerializedValue message) override {
      Delivery delivery;
      delivery.kind = Delivery::Kind::Message;
      delivery.message = std::move(message);
      post(std::move(delivery));
    }
    void ReportError(std::string message) override {
      Delivery delivery;
      delivery.kind = Delivery::Kind::Error;
      delivery.text = std::move(message);
      post(std::move(delivery));
    }
    void ReportConsole(std::string line) override {
      Delivery delivery;
      delivery.kind = Delivery::Kind::Console;
      delivery.text = std::move(line);
      post(std::move(delivery));
    }
    bool FetchSync(const std::string& url, std::string* body_out) override {
      return owner.FetchForWorker(worker, url, body_out);
    }
    void RequestClose() override { closed = true; }
  };
  Host host(*this, worker, post);

  WorkerScope scope(interpreter, host, worker.name, worker.location);
  scope.Install();

  // The script, once. An error in it is reported and the worker stays alive with no `onmessage`, which
  // is what the specification says -- and is more useful than exiting, because the page's `error`
  // handler is how it finds out.
  scope.RunScript(worker.source, worker.location.href);
  scope.EndTurn();

  while (!worker.stop.load() && !host.closed) {
    std::vector<js::SerializedValue> batch;
    {
      std::unique_lock<std::mutex> guard(worker.inbox_mutex);
      // **The block that keeps zero-idle-CPU true.** A worker with nothing to do waits here and costs
      // nothing; the predicate re-checks `stop` so that `terminate()` wakes it without a timeout. A
      // worker with a timer armed waits until that timer is due and not one millisecond of polling --
      // which is the same rule `IdleWaitState::next_deadline_ms` states for the main loop.
      const auto ready = [&worker] { return worker.stop.load() || !worker.inbox.empty(); };
      const double delay_ms = scope.NextDelayMs();
      if (delay_ms < 0.0) {
        worker.inbox_ready.wait(guard, ready);
      } else {
        worker.inbox_ready.wait_for(
            guard, std::chrono::duration<double, std::milli>(delay_ms), ready);
      }
      if (worker.stop.load()) {
        break;
      }
      while (!worker.inbox.empty()) {
        batch.push_back(std::move(worker.inbox.front()));
        worker.inbox.pop_front();
      }
    }
    // Timers first, then messages: a timer that came due while the thread was asleep is older than a
    // message that arrived to wake it, and running them the other way round reorders a page's own
    // sequencing.
    scope.RunDueTimers();
    for (const js::SerializedValue& message : batch) {
      scope.DeliverMessage(message);
      AddPerformanceCounter(PerfCounterId::WorkerMessagesHandled);
    }
    scope.EndTurn();
  }
}

bool Workers::FetchForWorker(Worker& worker, const std::string& specifier, std::string* body_out) {
  {
    std::lock_guard<std::mutex> guard(worker.sync.mutex);
    worker.sync.specifier = specifier;
    worker.sync.body.clear();
    worker.sync.pending = true;
    worker.sync.started = false;
    worker.sync.done = false;
    worker.sync.ok = false;
  }
  // The same pipe a message uses. The loop's drain asks for both, so one byte answers either.
  Wake(worker);
  std::unique_lock<std::mutex> guard(worker.sync.mutex);
  // **The one place a worker blocks on the main thread**, and it is what `importScripts` is: the
  // specification defines it as synchronous, and a worker is the thread that may block because it is
  // not the thread that draws. `stop` is in the predicate so `terminate()` and a navigation both free
  // it -- there is no path here that outlives the document.
  worker.sync.ready.wait(guard, [&worker] { return worker.sync.done || worker.stop.load(); });
  worker.sync.pending = false;
  if (!worker.sync.done || !worker.sync.ok) {
    AddPerformanceCounter(PerfCounterId::WorkerImportFailures);
    return false;
  }
  *body_out = std::move(worker.sync.body);
  AddPerformanceCounter(PerfCounterId::WorkerScriptsImported);
  return true;
}

std::vector<Workers::ImportRequest> Workers::TakeImportRequests() {
  std::vector<ImportRequest> out;
  std::lock_guard<std::mutex> guard(workers_mutex_);
  for (auto& [id, worker] : workers_) {
    std::lock_guard<std::mutex> sync(worker->sync.mutex);
    if (!worker->sync.pending || worker->sync.started) {
      continue;
    }
    worker->sync.started = true;
    out.push_back(ImportRequest{id, worker->sync.specifier, worker->location.href});
  }
  return out;
}

void Workers::CompleteImport(std::uint64_t worker_id, bool ok, std::string body) {
  std::lock_guard<std::mutex> guard(workers_mutex_);
  const auto found = workers_.find(worker_id);
  if (found == workers_.end()) {
    return;
  }
  Worker& worker = *found->second;
  {
    std::lock_guard<std::mutex> sync(worker.sync.mutex);
    if (!worker.sync.pending) {
      return;
    }
    worker.sync.ok = ok;
    worker.sync.body = std::move(body);
    worker.sync.done = true;
  }
  worker.sync.ready.notify_all();
}

bool Workers::Post(std::uint64_t id, js::SerializedValue message) {
  std::lock_guard<std::mutex> guard(workers_mutex_);
  const auto found = workers_.find(id);
  if (found == workers_.end()) {
    return false;
  }
  Worker& worker = *found->second;
  {
    std::lock_guard<std::mutex> inbox(worker.inbox_mutex);
    if (worker.inbox.size() >= kMaxQueuedMessages) {
      // Dropped rather than blocking. Backpressure here would mean blocking the *main* thread, which
      // is a frozen page -- so the bound is a drop and a counter, which is the lesser failure.
      AddPerformanceCounter(PerfCounterId::WorkerMessagesDropped);
      return false;
    }
    worker.inbox.push_back(std::move(message));
  }
  worker.inbox_ready.notify_one();
  return true;
}

void Workers::JoinAndClose(Worker& worker) {
  worker.stop.store(true);
  worker.inbox_ready.notify_all();
  // And the import channel, which is the *other* place a worker can be asleep. A worker blocked in
  // `importScripts` when its document navigated would otherwise wait for an answer from a loop that is
  // never going to run again, and the join below would never return.
  worker.sync.ready.notify_all();
  if (worker.thread.joinable()) {
    // **Joined, never detached.** A detached thread holding an interpreter is a use-after-free waiting
    // for the process to exit, and this runs while the document is still alive precisely so that the
    // worker's heap is gone before the page's objects are.
    worker.thread.join();
  }
  for (int* descriptor : {&worker.wake_read, &worker.wake_write}) {
    if (*descriptor >= 0) {
      ::close(*descriptor);
      *descriptor = -1;
    }
  }
}

void Workers::Terminate(std::uint64_t id) {
  std::unique_ptr<Worker> owned;
  {
    std::lock_guard<std::mutex> guard(workers_mutex_);
    const auto found = workers_.find(id);
    if (found == workers_.end()) {
      return;
    }
    owned = std::move(found->second);
    workers_.erase(found);
  }
  // Joined *outside* the map lock. Holding it across a join would deadlock against a worker that is
  // mid-`postMessage` and waiting on the same mutex to find its own entry.
  JoinAndClose(*owned);
  AddPerformanceCounter(PerfCounterId::WorkersTerminated);
}

std::vector<Workers::Delivery> Workers::TakeDeliveries() {
  std::vector<Delivery> out;
  std::lock_guard<std::mutex> guard(workers_mutex_);
  for (auto& [id, worker] : workers_) {
    // The pipe is drained *before* the outbox, not after. The other order has a race: a worker that
    // pushed and signalled between the outbox swap and the drain would have its byte eaten and its
    // message left, and the loop would sleep holding it.
    Drain(worker->wake_read);
    worker->signalled.store(false);
    std::lock_guard<std::mutex> outbox(worker->outbox_mutex);
    for (Delivery& delivery : worker->outbox) {
      delivery.worker_id = id;
      out.push_back(std::move(delivery));
    }
    worker->outbox.clear();
  }
  return out;
}

void Workers::AppendDescriptors(util::WaitDescriptorList& out) const {
  std::lock_guard<std::mutex> guard(workers_mutex_);
  for (const auto& [id, worker] : workers_) {
    if (worker->wake_read >= 0) {
      out.push_back(util::WaitDescriptor{worker->wake_read, true, false});
    }
  }
}

bool Workers::HasWork() const {
  std::lock_guard<std::mutex> guard(workers_mutex_);
  for (const auto& [id, worker] : workers_) {
    {
      std::lock_guard<std::mutex> outbox(worker->outbox_mutex);
      if (!worker->outbox.empty()) {
        return true;
      }
    }
    // An `importScripts` nobody has started is work too, and it is the kind the loop must not sleep
    // through: the worker is *blocked* until this turn answers it.
    std::lock_guard<std::mutex> sync(worker->sync.mutex);
    if (worker->sync.pending && !worker->sync.started) {
      return true;
    }
  }
  return false;
}

std::size_t Workers::Count() const {
  std::lock_guard<std::mutex> guard(workers_mutex_);
  return workers_.size();
}

void Workers::Clear() {
  std::map<std::uint64_t, std::unique_ptr<Worker>> owned;
  {
    std::lock_guard<std::mutex> guard(workers_mutex_);
    owned.swap(workers_);
  }
  // Every one joined, outside the lock, for the reason `Terminate` does it that way.
  for (auto& [id, worker] : owned) {
    JoinAndClose(*worker);
  }
}

}  // namespace microbrowser::engine
