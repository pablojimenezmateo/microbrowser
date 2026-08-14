#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bindings/Workers.h"
#include "engine/WorkerScope.h"
#include "js/StructuredClone.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::engine {

// Dedicated workers: a separate JavaScript heap on a separate thread.
//
// ADR 0022 §1, session 38. **This is the first thread in the browser that runs a page's code**, and it
// is deliberately the model for every thread after it -- so `AGENTS.md`'s requirement for a thread is
// answered here rather than in a commit message.
//
// **What a worker owns.** Its `js::Interpreter`, its heap, its microtask queue, and its half of the two
// message queues. Nothing outside the worker thread touches any of them. The interpreter is
// *constructed on the worker thread* and destroyed there, which is stronger than a lock: there is no
// window in which two threads could both hold it.
//
// **What a worker borrows.** Nothing. Not the DOM, not the document, not a font, not the loader, not
// the network. That is what makes this tractable in a codebase whose data structures are, in ADR 0011's
// words, "single-threaded by assumption throughout" -- there is no shared state to get wrong, so there
// is no lock discipline to get wrong either. The only things crossing the seam are `js::SerializedValue`
// byte buffers and two atomics.
//
// **When a worker is joined.** In `Workers::Clear`, which runs on navigation, and in the destructor --
// so before the document's objects are destroyed and before `main` returns. `terminate()` is the same
// path taken early. A worker is *always* joined; nothing is detached, because a detached thread holding
// an interpreter is a use-after-free waiting for the process to exit.
//
// **Zero idle CPU.** A worker with no message pending blocks on its condition variable, so it costs
// nothing; a browser with no workers has no threads at all. And the *main* loop is woken by a pipe the
// worker writes one byte to -- `Interest()` hands that descriptor to the platform wait, which is the
// same mechanism a socket uses. No polling, no timer, no periodic check.
//
// What is refused, and the refusals are ADR 0022's: `SharedWorker` (shared mutable state across
// documents, which is the one thing this model exists to avoid), service workers (§2), and
// `SharedArrayBuffer`/`Atomics` (they need cross-origin isolation, which needs the process model).
class Workers {
 public:
  Workers() = default;
  Workers(const Workers&) = delete;
  Workers& operator=(const Workers&) = delete;
  ~Workers();

  // How many workers one document may start. A page controls this -- `new Worker` in a loop is one
  // line -- and each one is a thread and a heap. Sixteen is far past any real page and small enough
  // that a runaway loop costs a bounded number of threads rather than the machine's.
  static constexpr std::size_t kMaxWorkers = 16;

  // How many messages may queue in either direction before further ones are dropped. A bound rather
  // than backpressure, because backpressure on `postMessage` would mean blocking the thread that
  // called it -- and on the main thread that is a frozen page. Dropping is visible in a counter and is
  // the lesser failure.
  static constexpr std::size_t kMaxQueuedMessages = 4096;

  // Reserves an id for a worker whose script has not arrived yet.
  //
  // **The id exists before the thread does**, and that is what makes `new Worker` synchronous the way
  // the specification says while the fetch is not: a page constructs a `Worker`, posts to it
  // immediately, and the messages queue in an inbox that exists. Zero when the limit is reached, which
  // the caller turns into nothing rather than an exception -- `new Worker` is specified not to throw for
  // this, and a page at sixteen workers has a bug rather than a recoverable error.
  //
  // `location` is computed by the caller, on the main thread, because the URL parser has
  // lazily-initialised tables behind it and parsing on a second thread is a data race waiting to be
  // found. See `WorkerLocation`.
  std::uint64_t Reserve(std::string name, WorkerLocation location);

  // The script arrived: the thread starts and drains whatever queued while it was being fetched. False
  // when the id names nothing, which is what a worker terminated before its script arrived gets.
  bool Provide(std::uint64_t id, std::string source);

  // The script did not arrive. The worker is dropped and an `error` delivery is queued, which is what
  // the specification says for a script that fails to load -- and it is why `new Worker` returning an
  // object for a script that will never load is correct rather than a lie.
  void FailToLoad(std::uint64_t id, std::string reason);

  // A message from the page to a worker. The bytes were serialized on the main thread, because
  // serializing touches the *page's* heap and only the main thread may.
  bool Post(std::uint64_t id, js::SerializedValue message);

  // `terminate()`. Joins the thread before returning, so a page that terminates a worker and then
  // navigates cannot race it.
  void Terminate(std::uint64_t id);

  // Messages a worker sent back, taken. Called on the main thread; deserializing them touches the
  // page's heap, which is why they cross as bytes and are rebuilt here rather than there.
  struct Delivery {
    // `bindings::WorkerDelivery`, which is the seam's own vocabulary -- see `bindings/Workers.h`.
    using Kind = bindings::WorkerDelivery;

    std::uint64_t worker_id = 0;
    Kind kind = Kind::Message;
    js::SerializedValue message;
    // An uncaught error inside the worker, or a console line, as text rather than as a value: it
    // becomes an `error` event on the page's `Worker` object. Text only -- a serialized exception from
    // another heap would be an object graph crossing a boundary for a diagnostic.
    std::string text;
  };
  std::vector<Delivery> TakeDeliveries();

  // One `importScripts` a worker is blocked on. The URL is the *specifier* the script wrote; the base
  // it resolves against is the worker's own script URL, which is why both cross.
  struct ImportRequest {
    std::uint64_t worker_id = 0;
    std::string specifier;
    std::string base_url;
  };

  // Requests no one has started yet, marked as started. Called on the main thread, from the same drain
  // that takes deliveries -- a worker signals the loop through the same pipe for both.
  std::vector<ImportRequest> TakeImportRequests();

  // The answer, which unblocks the worker's thread. `ok` false is the specification's `NetworkError`.
  void CompleteImport(std::uint64_t worker_id, bool ok, std::string body);

  // A `fetch` or an `XMLHttpRequest` a worker started. Unlike `importScripts` this is **not**
  // blocking: the worker gets an id immediately and its promise settles on a later turn, which is
  // the same "started, never awaited" rule `bindings::NetworkSource` states for the page.
  struct FetchRequest {
    std::uint64_t worker_id = 0;
    std::uint64_t request_id = 0;
    bindings::ScriptRequest request;
  };
  struct FetchAbort {
    std::uint64_t worker_id = 0;
    std::uint64_t request_id = 0;
  };
  std::vector<FetchRequest> TakeFetchRequests();
  std::vector<FetchAbort> TakeFetchAborts();

  // The answer to one, queued for the worker's next turn. It goes through the *inbox* rather than a
  // queue of its own so that one condition variable wakes the worker for either -- a response and a
  // message are both "something happened for you".
  void DeliverFetch(std::uint64_t worker_id, std::uint64_t request_id,
                    bindings::ScriptResponse response);

  // The descriptors the platform wait should watch, so that a message from a worker wakes the loop.
  // One read end per worker, and nothing at all when there are no workers.
  void AppendDescriptors(util::WaitDescriptorList& out) const;

  // Whether any worker has something queued for the page *right now*, which the loop asks so that a
  // message posted between the wait and the drain is not left until the next wakeup.
  bool HasWork() const;

  std::size_t Count() const;

  // A navigation. Every worker is terminated and joined -- ADR 0022's "joined when its document dies,
  // before the document's objects are destroyed".
  void Clear();

 private:
  // One worker. Not copyable or movable: the thread captures `this`, so a move would leave the running
  // thread pointing at a moved-from object. Held by `unique_ptr` for exactly that reason.
  // One `importScripts` in flight. The worker thread fills `specifier` and blocks; the main thread
  // fills `body` and sets `done`. Its own mutex rather than the inbox's, because the worker holds this
  // one while it waits and a `postMessage` from the page must not block behind it.
  struct SyncFetch {
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::string specifier;
    std::string body;
    // Set by the worker when it starts waiting; cleared when the answer is taken. `started` is what
    // stops the main loop handing the same request out twice on two turns.
    bool pending = false;
    bool started = false;
    bool done = false;
    bool ok = false;
  };

  struct Worker {
    std::uint64_t id = 0;
    std::string name;
    std::string source;
    WorkerLocation location;
    std::thread thread;
    SyncFetch sync;
    // The two queues, each with the one mutex that guards it. Two rather than one, so that a worker
    // draining its inbox does not block the page filling it.
    mutable std::mutex inbox_mutex;
    std::condition_variable inbox_ready;
    std::deque<js::SerializedValue> inbox;
    // Fetch answers, beside the messages and under the same mutex and condition variable: a worker
    // waiting for either is waiting in one place.
    std::deque<std::pair<std::uint64_t, bindings::ScriptResponse>> responses;
    // Requests the worker has started that the main loop has not taken yet, and aborts likewise.
    // Their own mutex, because the worker fills them while the loop may be draining the outbox.
    mutable std::mutex requests_mutex;
    std::vector<FetchRequest> requests;
    std::vector<FetchAbort> aborts;
    mutable std::mutex outbox_mutex;
    std::vector<Delivery> outbox;
    // Set by the main thread, read by the worker. Atomic rather than mutex-guarded because the worker
    // checks it on every loop iteration and a lock there would serialise against `postMessage`.
    std::atomic<bool> stop{false};
    // The pipe that wakes the main loop. Write end is the worker's, read end is the loop's; one byte
    // per batch rather than per message, because the loop drains everything when it wakes.
    int wake_read = -1;
    int wake_write = -1;
    // Set once the worker has signalled since the loop last drained, so that a burst of messages
    // costs one byte in the pipe rather than one per message.
    std::atomic<bool> signalled{false};
  };

  void Run(Worker& worker);
  void Wake(Worker& worker);
  void JoinAndClose(Worker& worker);
  // The worker thread's side of `importScripts`: fills the request, wakes the loop, and blocks until
  // the answer or `stop`. False on either failure, which the scope turns into a `NetworkError`.
  bool FetchForWorker(Worker& worker, const std::string& specifier, std::string* body_out);
  // The worker thread's side of `fetch`: queue and wake, never wait.
  void QueueFetch(Worker& worker, std::uint64_t request_id, const bindings::ScriptRequest& request);
  void QueueFetchAbort(Worker& worker, std::uint64_t request_id);

  mutable std::mutex workers_mutex_;
  std::map<std::uint64_t, std::unique_ptr<Worker>> workers_;
  std::uint64_t next_id_ = 0;
};

}  // namespace microbrowser::engine
