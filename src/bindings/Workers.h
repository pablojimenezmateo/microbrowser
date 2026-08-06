#pragma once

#include <cstdint>
#include <string>

namespace microbrowser::bindings {

// What `new Worker(url)` and `structuredClone` are answered through.
//
// ADR 0022 §1, session 38. Declared here and implemented by `src/engine`, like every other seam -- and
// here the inversion buys something specific: **`src/bindings` cannot start a thread.** It may see
// `util`, `js`, `dom` and `html`, and a thread that owned a `js::Interpreter` would have to be owned by
// something that outlives a document, which is the engine.
//
// A worker is named by an id rather than by a pointer, for the reason a socket and a MediaSource are: a
// number is not something a page can forge into a pointer, and the table it indexes lives with the
// document.
//
// The messages themselves do **not** cross this interface as values. They cross as
// `js::SerializedValue` bytes, and the serialisation happens on whichever side owns the heap the value
// is in -- the page's heap here, the worker's heap there. That is the whole of ADR 0022's "messages
// cross by value": there is no moment at which two threads hold one object.
class WorkerHost {
 public:
  virtual ~WorkerHost() = default;

  // Starts a worker running the script at `url`. Zero when the script is not available yet, when the
  // URL is refused, or when the limit is reached -- and **not an exception**, because `new Worker` is
  // specified not to throw for a script that fails to load: the failure arrives as an `error` event.
  virtual std::uint64_t StartWorker(const std::string& url) = 0;

  // A message to a worker, already serialised. False when the id names nothing, which is what a page
  // holding a terminated worker gets -- and it is a silent no-op rather than a throw, per the
  // specification: posting to a terminated worker is defined to do nothing.
  virtual bool PostToWorker(std::uint64_t id, const std::string& serialized) = 0;

  virtual void TerminateWorker(std::uint64_t id) = 0;
};

}  // namespace microbrowser::bindings
