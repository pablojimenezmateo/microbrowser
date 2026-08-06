// Where a page's `document.cookie` reads and writes land.
//
// ADR 0005 and ADR 0008. Its own translation unit for the reason EngineStorage.cpp
// is: Engine.cpp is at its module cap, and the seam is the same shape — the
// partition key is derived from the document here, and `src/bindings` never sees it.

#include <string>
#include <string_view>

#include "engine/Clock.h"
#include "engine/Engine.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::engine {

namespace {

url::PartitionKey DocumentPartition(const url::Url& document_url) {
  return url::PartitionKey::ForTopLevel(url::ContainerId::Default(), document_url);
}

}  // namespace

std::string Engine::DocumentCookie() {
  const std::optional<url::Url>& base = page_.BaseUrl();
  if (!base.has_value()) {
    return std::string();
  }
  return loader_.Cookies().DocumentCookie(DocumentPartition(*base), *base, NowSeconds());
}

bool Engine::SetDocumentCookie(std::string_view assignment) {
  const std::optional<url::Url>& base = page_.BaseUrl();
  if (!base.has_value()) {
    return false;
  }
  return loader_.Cookies().StoreFromDocument(DocumentPartition(*base), *base, assignment,
                                             NowSeconds());
}

}  // namespace microbrowser::engine
