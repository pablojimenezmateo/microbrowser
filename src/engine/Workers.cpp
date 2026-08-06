#include "engine/Workers.h"

#include <cerrno>
#include <fcntl.h>
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

std::uint64_t Workers::Reserve(std::string name) {
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

  // `self.postMessage`, which is the only thing a worker can do to the outside world. Deliberately the
  // only thing: no fetch, no DOM, no storage. A worker that needs a resource asks the page by message,
  // which keeps the borrow list empty.
  const js::Value post_message = interpreter.NewNativeValue(
      "postMessage", [&interpreter, &post](js::NativeCall& call) -> js::Value {
        const std::optional<js::SerializedValue> serialized =
            js::StructuredSerialize(interpreter, call.arguments.empty() ? js::Value::Undefined()
                                                                        : call.arguments[0]);
        if (!serialized.has_value()) {
          // The specification's `DataCloneError`. Throwing rather than dropping: a page whose message
          // silently vanished because it held a function debugs the receiver.
          return call.Throw("Error", "DataCloneError: the message could not be cloned");
        }
        Delivery delivery;
        delivery.message = *serialized;
        post(std::move(delivery));
        return js::Value::Undefined();
      });
  if (post_message.IsObject()) {
    interpreter.Global()->Set("postMessage", post_message);
    interpreter.GlobalScope()->Declare("postMessage", post_message, false);
  }
  // `self` is the global, which is what a worker script expects -- there is no `window` here, and a
  // script that tests for one correctly concludes it is not on a page.
  const js::Value self = js::Value::Obj(interpreter.Global());
  interpreter.Global()->Set("self", self);
  interpreter.GlobalScope()->Declare("self", self, false);
  interpreter.Global()->Set("name", js::Value::String(worker.name));

  // The script, once. An error in it is reported and the worker stays alive with no `onmessage`, which
  // is what the specification says -- and is more useful than exiting, because the page's `error`
  // handler is how it finds out.
  const js::Result ran = interpreter.Run(worker.source);
  if (ran.completion == js::Completion::Throw) {
    Delivery failure;
    failure.is_error = true;
    failure.error = js::ToString(ran.value);
    post(std::move(failure));
  }

  while (!worker.stop.load()) {
    js::SerializedValue message;
    {
      std::unique_lock<std::mutex> guard(worker.inbox_mutex);
      // **The block that keeps zero-idle-CPU true.** A worker with nothing to do waits here and costs
      // nothing; the predicate re-checks `stop` so that `terminate()` wakes it without a timeout.
      worker.inbox_ready.wait(guard,
                              [&worker] { return worker.stop.load() || !worker.inbox.empty(); });
      if (worker.stop.load()) {
        break;
      }
      message = std::move(worker.inbox.front());
      worker.inbox.pop_front();
    }
    const js::Value* handler = interpreter.Global()->Get("onmessage");
    if (handler == nullptr || !handler->IsObject() || !handler->object->IsCallable()) {
      continue;  // no handler: the message is dropped, which is what a page without one asked for
    }
    const js::Value event = interpreter.NewObjectValue();
    if (event.IsObject()) {
      event.object->Set("type", js::Value::String("message"));
      event.object->Set("data", js::StructuredDeserialize(interpreter, message));
    }
    const js::Result outcome = interpreter.CallFunction(*handler, self, {event});
    if (outcome.completion == js::Completion::Throw) {
      Delivery failure;
      failure.is_error = true;
      failure.error = js::ToString(outcome.value);
      post(std::move(failure));
    }
    // The microtask queue, drained here rather than left: a worker whose promise callbacks ran only
    // when the *next* message arrived would look like it had stalled.
    interpreter.DrainMicrotasks();
    AddPerformanceCounter(PerfCounterId::WorkerMessagesHandled);
  }
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
    std::lock_guard<std::mutex> outbox(worker->outbox_mutex);
    if (!worker->outbox.empty()) {
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
