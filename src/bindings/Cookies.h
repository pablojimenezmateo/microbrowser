#pragma once

#include <string>
#include <string_view>

namespace microbrowser::bindings {

// What `document.cookie` reads and writes.
//
// ADR 0005 partitions cookies; ADR 0008 says this module may not see `net` or
// `url`. The interface is declared here and implemented by `src/engine`, which
// derives the partition key from the document and asks the jar — the same
// inversion `StorageSource` and `NetworkSource` use.
//
// **The partition key does not appear in this file.** A binding cannot name a
// partition even by accident: it asks for the document's cookies and the
// implementation decides whose jar that is.
class CookieSource {
 public:
  CookieSource() = default;
  CookieSource(const CookieSource&) = delete;
  CookieSource& operator=(const CookieSource&) = delete;
  virtual ~CookieSource() = default;

  // `name=value; name2=value2` for every non-HttpOnly cookie the document may
  // read, or empty when there are none. Never undefined — pages call `.match`
  // on the result without checking.
  virtual std::string DocumentCookie() = 0;

  // A `document.cookie = "..."` assignment. False when the write was refused —
  // a domain the page does not own, a cookie over the size limit, malformed
  // input. The setter does not throw; refusal is silent, which is what every
  // other browser does.
  virtual bool SetDocumentCookie(std::string_view assignment) = 0;
};

}  // namespace microbrowser::bindings
