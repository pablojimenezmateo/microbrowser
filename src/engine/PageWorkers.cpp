// Dedicated workers, from the page's side.
//
// ADR 0022 §1, session 38. `engine::Workers` owns the threads and the heaps; this is the layer that
// turns a URL into a script and an id into a delivery. Its own translation unit for the reason
// PageCanvas.cpp is one.
//
// **The script has to already be here.** A worker's script is a subresource, and this browser fetches a
// document's subresources before it runs its scripts -- so `new Worker('w.js')` finds the source in the
// page's own table. A worker whose script has not arrived starts nothing and the page gets an `error`
// event, which is what the specification says for a script that fails to load. Fetching it *now* would
// mean a synchronous network request on the main thread, which is the one thing ADR 0011 forbids.

#include <string>

#include "engine/Page.h"
#include "js/StructuredClone.h"
#include "url/Url.h"

namespace microbrowser::engine {

std::uint64_t Page::StartWorker(const std::string& url) {
  const std::optional<url::Url>& base = BaseUrl();
  if (!base.has_value()) {
    return 0;
  }
  const std::optional<url::Url> target = url::Url::Parse(url, *base);
  if (!target.has_value()) {
    return 0;
  }
  // **Same-origin only**, and this is the one security check on the whole feature. A worker runs a
  // page's own code with its own heap; a cross-origin script would be another origin's code running
  // with this page's messages, which is what the specification forbids and what `import` would
  // otherwise be a way around. The comparison is on `url::Origin`, which is the one parser's answer.
  if (url::Origin::FromUrl(*target) != url::Origin::FromUrl(*base)) {
    return 0;
  }
  // **`location` is computed here, on the main thread, and never on the worker's.** The URL parser has
  // lazily-initialised tables behind it -- IDNA, the public suffix list -- so a second thread reaching
  // into it is a data race that would show up as a corrupted host once in a thousand runs. The worker
  // holds strings, which is the same value-not-pointer rule the messages follow.
  WorkerLocation location;
  location.href = target->Href();
  location.origin = url::Origin::FromUrl(*target).Serialize();
  location.protocol = target->Protocol();
  location.host = target->HostPort();
  location.hostname = target->Hostname();
  location.port = target->PortString();
  location.pathname = target->Pathname();
  location.search = target->Search();
  location.hash = target->Hash();
  const std::uint64_t id = workers_.Reserve(url, std::move(location));
  if (id == 0) {
    return 0;  // the sixteen-worker limit
  }
  // The fetch is the engine's, and it happens on a later turn. **The id and the inbox exist now**, which
  // is what makes `new Worker` synchronous the way the specification says while the request is not: a
  // page that constructs a worker and posts to it immediately has its messages queued in an inbox that
  // is already there.
  unrequested_worker_scripts_.push_back(PendingWorkerScript{id, target->Serialize()});
  return id;
}

std::vector<Page::PendingWorkerScript> Page::TakeUnrequestedWorkerScripts() {
  std::vector<PendingWorkerScript> taken;
  taken.swap(unrequested_worker_scripts_);
  return taken;
}

void Page::ProvideWorkerScript(std::uint64_t worker_id, std::string source) {
  workers_.Provide(worker_id, std::move(source));
}

void Page::FailWorkerLoad(std::uint64_t worker_id, const std::string& reason) {
  workers_.FailToLoad(worker_id, reason);
  // Delivered here rather than through the worker's outbox: the worker never started, so its pipe is
  // closed before the loop's next turn and a queued delivery would be lost. A page's `onerror` is how it
  // finds out that its worker script 404'd, so losing it would make the failure invisible.
  script_.DeliverWorkerMessage(worker_id, bindings::WorkerDelivery::Error, std::string(), reason);
}

bool Page::PostToWorker(std::uint64_t id, const std::string& serialized) {
  js::SerializedValue message;
  message.bytes.assign(serialized.begin(), serialized.end());
  return workers_.Post(id, std::move(message));
}

void Page::TerminateWorker(std::uint64_t id) { workers_.Terminate(id); }

std::vector<Page::PendingWorkerImport> Page::TakeWorkerImportRequests() {
  return workers_.TakeImportRequests();
}

void Page::CompleteWorkerImport(std::uint64_t worker_id, bool ok, std::string body) {
  workers_.CompleteImport(worker_id, ok, std::move(body));
}

bool Page::DeliverWorkerMessages() {
  const std::vector<Workers::Delivery> deliveries = workers_.TakeDeliveries();
  bool ran = false;
  for (const Workers::Delivery& delivery : deliveries) {
    // Deserialising happens inside the binding layer, in the page's heap. What arrived here is bytes,
    // which is the whole of ADR 0022's "messages cross by value".
    const std::string bytes(reinterpret_cast<const char*>(delivery.message.bytes.data()),
                           delivery.message.bytes.size());
    ran = script_.DeliverWorkerMessage(delivery.worker_id, delivery.kind, bytes, delivery.text) ||
          ran;
  }
  return ran;
}

}  // namespace microbrowser::engine
