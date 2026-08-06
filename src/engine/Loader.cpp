#include "engine/Loader.h"

#include <algorithm>
#include <utility>

#include "util/Base64.h"
#include "util/BlobUrlRegistry.h"
#include "util/PercentEncoding.h"
#include "util/StringUtil.h"

namespace microbrowser::engine {

namespace {

std::string BodyAsString(const std::vector<std::byte>& body) {
  return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

}  // namespace

Loader::Loader() : queue_(policy_, sockets_, cookies_, cache_) {}

DataUrl DecodeDataUrl(std::string_view url) {
  DataUrl result;
  constexpr std::string_view kPrefix = "data:";
  if (url.size() < kPrefix.size() || !util::EqualsAsciiCaseInsensitive(url.substr(0, kPrefix.size()),
                                                                  kPrefix)) {
    return result;
  }
  url.remove_prefix(kPrefix.size());
  // The fragment is not part of the URL's body. It belongs to the *document*
  // this URL identifies -- which is what `:target` reads -- so leaving it on
  // made `data:text/html,<h1 id=x>t</h1>#x` a document with the text "#x"
  // rendered at the end of it. A literal `#` in a payload has to be written
  // `%23`, and it is not valid base64 either, so the first one is always the
  // fragment separator.
  if (const std::size_t hash = url.find('#'); hash != std::string_view::npos) {
    url = url.substr(0, hash);
  }

  const std::size_t comma = url.find(',');
  if (comma == std::string_view::npos) {
    // No comma is not a data URL, it is a string beginning with "data:". The
    // spec says so, and accepting it would make every such string a document.
    return result;
  }
  std::string_view metadata = url.substr(0, comma);
  const std::string_view payload = url.substr(comma + 1);

  bool base64 = false;
  constexpr std::string_view kBase64 = ";base64";
  if (metadata.size() >= kBase64.size() &&
      util::EqualsAsciiCaseInsensitive(metadata.substr(metadata.size() - kBase64.size()), kBase64)) {
    base64 = true;
    metadata.remove_suffix(kBase64.size());
  }

  result.content_type = metadata.empty() ? "text/plain;charset=US-ASCII" : std::string(metadata);
  // **A `data:` URL with no `charset` is UTF-8**, and that is a deliberate deviation from RFC 2397's
  // `US-ASCII` default which every browser also makes. The reason is in the URL itself: the payload
  // arrived percent-encoded, and the bytes a `%E6%97%A5` decodes to are UTF-8 because that is what
  // the encoder that produced them emitted. Following the RFC here means decoding those bytes as
  // windows-1252 -- which is ADR 0025's fallback, correct for a *document from a server* and wrong
  // for one carried in its own URL -- and rendering `æ—¥` where the author wrote 日.
  //
  // Only when no charset was named: a `data:text/html;charset=windows-1252,…` is honoured, because
  // then the URL said so.
  if (!metadata.empty() &&
      util::AsciiLowerCase(result.content_type).find("charset") == std::string::npos) {
    result.content_type += ";charset=utf-8";
  }
  if (base64) {
    // The one decoder, in util, for the reason the percent decoder below is
    // there: Subresource Integrity and CSP's hash-sources decode base64 too,
    // from modules that cannot see this one.
    std::optional<std::string> decoded = util::Base64Decode(payload);
    result.ok = decoded.has_value();
    result.body = decoded.value_or(std::string{});
    return result;
  }
  // The one decoder, in util. The copy that used to live here was
  // byte-for-byte the same as url's, which is how two of them drift.
  result.body = util::PercentDecode(payload);
  result.ok = true;
  return result;
}

Loader::RequestId Loader::Deliver(Result result) {
  const RequestId id = queue_.ReserveId();
  ready_.push_back(Completion{id, std::move(result)});
  return id;
}

Loader::RequestId Loader::Start(const privacy::Request& request, const net::FetchOptions& options,
                                bool top_level, std::int64_t now,
                                const url::Url* referrer_document) {
  privacy::Verdict verdict = policy_.Decide(request, referrer_document);
  if (!verdict.IsAllowed()) {
    Result refused;
    refused.error = verdict.Reason().empty() ? "blocked" : verdict.Reason();
    return Deliver(std::move(refused));
  }

  net::FetchOptions fetch_options = options;
  fetch_options.is_top_level_navigation = top_level;
  return queue_.Start(std::move(verdict), fetch_options, now);
}

Loader::RequestId Loader::StartSubresource(std::string_view url, const url::Url& document,
                                           privacy::ResourceType type, std::int64_t now) {
  return StartSubresource(url, document, type, now, {});
}

Loader::RequestId Loader::StartSubresource(std::string_view url, const url::Url& document,
                                           privacy::ResourceType type, std::int64_t now,
                                           const net::FetchOptions& options) {
  // A data: URL is bytes the page carried with it. It needs no network, so it
  // is answered here and delivered through the same completion path as
  // everything else -- a caller that had to handle two delivery shapes would
  // grow a branch only one of them exercises.
  if (DataUrl data = DecodeDataUrl(url); data.ok) {
    Result result;
    if (options.method != "GET" || !options.body.empty()) {
      result.error = "data URL loads do not support request bodies";
      return Deliver(std::move(result));
    }
    result.ok = true;
    result.body = std::move(data.body);
    result.content_type = std::move(data.content_type);
    result.final_url = std::string(url);
    result.status = 200;
    return Deliver(std::move(result));
  }
  if (blob_registry_ != nullptr && util::StartsWithAsciiCaseInsensitive(url, "blob:")) {
    if (const std::optional<std::string_view> body = blob_registry_->Lookup(url);
        body.has_value()) {
      Result result;
      if (options.method != "GET" || !options.body.empty()) {
        result.error = "blob URL loads do not support request bodies";
        return Deliver(std::move(result));
      }
      result.ok = true;
      result.body = std::string(*body);
      result.content_type = "text/javascript";
      result.final_url = std::string(url);
      result.status = 200;
      return Deliver(std::move(result));
    }
  }

  // Relative to the document, which is what every href in a page is.
  const std::optional<url::Url> parsed = url::Url::Parse(url, document);
  if (!parsed.has_value()) {
    Result result;
    result.error = "that is not a URL";
    return Deliver(std::move(result));
  }

  privacy::Request request;
  request.url = *parsed;
  // The document that asked is the initiator and, with no frames, also the
  // top-level site. Partitioning every piece of state this touches by that
  // pair is the whole point of the privacy layer -- see ADR 0004.
  request.initiator = url::Origin::FromUrl(document);
  request.top_level_site = url::Site::FromUrl(document);
  request.container = url::ContainerId::Default();
  request.type = type;
  request.is_subresource = true;

  return Start(request, options, false, now, &document);
}

Loader::RequestId Loader::StartLoad(std::string_view url, std::int64_t now,
                                    const net::FetchOptions& options,
                                    const url::Url* referrer_document) {
  if (DataUrl data = DecodeDataUrl(url); data.ok) {
    Result result;
    if (options.method != "GET" || !options.body.empty()) {
      result.error = "data URL loads do not support request bodies";
      return Deliver(std::move(result));
    }
    result.ok = true;
    result.body = std::move(data.body);
    result.content_type = std::move(data.content_type);
    result.final_url = std::string(url);
    result.status = 200;
    return Deliver(std::move(result));
  }

  const std::optional<url::Url> parsed = url::Url::Parse(url);
  if (!parsed.has_value()) {
    Result result;
    result.error = "that is not a URL";
    return Deliver(std::move(result));
  }

  // A top-level navigation has no initiator page: the origin is opaque, and
  // that is a value rather than an absence. The top-level site is the one being
  // navigated *to*, because after this load it is the top-level site.
  privacy::Request request;
  request.url = *parsed;
  request.initiator = url::Origin{};
  request.top_level_site = url::Site::FromUrl(*parsed);
  request.container = url::ContainerId::Default();
  request.type = privacy::ResourceType::Document;
  request.is_subresource = false;

  return Start(request, options, true, now, referrer_document);
}

void Loader::Advance(std::int64_t now_ms) { queue_.Advance(now_ms); }

std::vector<Loader::Completion> Loader::TakeCompletions() {
  std::vector<Completion> out = std::exchange(ready_, {});
  for (net::RequestQueue::Completion& fetched : queue_.TakeCompletions()) {
    Completion completion;
    completion.id = fetched.id;
    if (!fetched.result.ok) {
      completion.result.error =
          fetched.result.error.empty() ? "the load failed" : std::move(fetched.result.error);
      out.push_back(std::move(completion));
      continue;
    }
    completion.result.ok = true;
    completion.result.status = fetched.result.response.status;
    completion.result.status_text = fetched.result.response.reason;
    completion.result.final_url = fetched.result.final_url.Serialize();
    completion.result.body = BodyAsString(fetched.result.response.body);
    completion.result.opaque = fetched.result.opaque;
    completion.result.redirected = fetched.result.redirects > 0;
    if (const std::optional<std::string_view> type =
            fetched.result.response.headers.Get("content-type")) {
      completion.result.content_type = std::string(*type);
    }
    // Copied wholesale rather than by name: what a page may read was decided
    // in `net`, and a second filter here would be a second policy to keep in
    // step with the first.
    completion.result.headers.reserve(fetched.result.response.headers.Size());
    for (const net::HttpHeaders::Field& field : fetched.result.response.headers.Fields()) {
      completion.result.headers.emplace_back(field.name, field.value);
    }
    out.push_back(std::move(completion));
  }
  return out;
}

void Loader::AppendDescriptors(util::WaitDescriptorList& out) const {
  queue_.AppendDescriptors(out);
}

bool Loader::HasRunnableWork() const { return !ready_.empty() || queue_.HasRunnableWork(); }

std::optional<std::uint32_t> Loader::NextDeadlineMs(std::int64_t now_ms) const {
  return queue_.NextDeadlineMs(now_ms);
}

void Loader::CancelAll() {
  queue_.CancelAll();
  ready_.clear();
}

bool Loader::Cancel(RequestId id) {
  const auto found = std::remove_if(ready_.begin(), ready_.end(),
                                    [id](const Completion& done) { return done.id == id; });
  const bool was_ready = found != ready_.end();
  ready_.erase(found, ready_.end());
  // Both, rather than the first that matches: an answer this class produced
  // without a network -- a `data:` URL, a refusal -- lives in `ready_`, and one
  // the queue is still working on lives there. An id is in exactly one of them,
  // and looking in both is what makes an abort mean "no completion" rather than
  // "probably no completion".
  return queue_.Cancel(id) || was_ready;
}

}  // namespace microbrowser::engine
