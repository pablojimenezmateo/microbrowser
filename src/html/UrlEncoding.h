#pragma once

#include <string>
#include <string_view>

#include "html/Encoding.h"
#include "url/Url.h"

namespace microbrowser::html {

// The document's encoding, in the shape `src/url` asks for it.
//
// ADR 0025 §2 meets the URL Standard's "percent-encode after encoding". A URL in a Shift_JIS
// document carries Shift_JIS bytes in its query -- `<a href="?q=日本">` sends `%93%FA%96%7B`, not
// `%E6%97%A5%E6%9C%AC` -- because that is what the server on the other end reads back. It is the
// only place a document's character set changes what leaves the browser.
//
// **In its own header rather than in Encoding.h**, because `src/bindings` includes Encoding.h and
// may not see `src/url` (ADR 0008): a header that pulled the URL parser in behind it would breach
// that boundary transitively, which is exactly the kind of widening the allow-lists exist to make
// visible. Only `src/engine` -- the module that sees both -- includes this one.
class DocumentQueryEncoder final : public url::QueryEncoder {
 public:
  explicit DocumentQueryEncoder(Encoding encoding) : encoding_(ForUrls(encoding)) {}

  Encoding EncodingForUrls() const { return encoding_; }

  void EncodeQuery(std::string_view input, bool (*needs_escape)(unsigned char),
                   std::string& out) const override;

  // HTML's rule, applied once here rather than at every call site: a UTF-16 document parses URLs as
  // UTF-8. There is no UTF-16 encoder in the Encoding Standard at all, and there is a reason beyond
  // "nobody wrote one" -- a UTF-16 query would be full of NUL bytes, which nothing between here and
  // the server survives.
  static Encoding ForUrls(Encoding encoding) {
    return encoding == Encoding::Utf16Le || encoding == Encoding::Utf16Be ? Encoding::Utf8
                                                                         : encoding;
  }

 private:
  Encoding encoding_;
};

}  // namespace microbrowser::html
