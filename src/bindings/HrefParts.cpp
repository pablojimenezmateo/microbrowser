// Shared URL splitting for `location`, `URL`, and HTMLHyperlinkElementUtils.
//
// Kept in `bindings` rather than `url` because this module's allow-list is a
// security boundary (ADR 0008): bindings may not see `src/url`. The algorithm
// is the same string cut `location` already used — one copy so an `<a>`'s
// `pathname` cannot disagree with `location.pathname` for the same href.

#include "bindings/BindingSupport.h"

namespace microbrowser::bindings {

HrefParts SplitHref(std::string_view href) {
  HrefParts out;
  out.pathname = "/";
  out.origin = "null";

  const std::size_t scheme_end = href.find(':');
  if (scheme_end == std::string_view::npos) {
    return out;
  }
  out.protocol = std::string(href.substr(0, scheme_end + 1));

  std::string_view rest = href.substr(scheme_end + 1);
  if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
    rest.remove_prefix(2);
    const std::size_t authority_end = rest.find_first_of("/?#");
    std::string_view authority =
        authority_end == std::string_view::npos ? rest : rest.substr(0, authority_end);
    rest = authority_end == std::string_view::npos ? std::string_view() : rest.substr(authority_end);
    if (const std::size_t credentials = authority.rfind('@');
        credentials != std::string_view::npos) {
      authority.remove_prefix(credentials + 1);
    }
    out.host = std::string(authority);
    const std::size_t host_end = authority.starts_with('[') ? authority.find(']') : 0;
    const std::size_t colon =
        host_end == std::string_view::npos ? std::string_view::npos : authority.find(':', host_end);
    if (colon == std::string_view::npos) {
      out.hostname = std::string(authority);
    } else {
      out.hostname = std::string(authority.substr(0, colon));
      out.port = std::string(authority.substr(colon + 1));
    }
    out.origin = out.protocol + "//" + out.host;
  }

  if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos) {
    out.hash = std::string(rest.substr(hash));
    rest = rest.substr(0, hash);
  }
  if (const std::size_t query = rest.find('?'); query != std::string_view::npos) {
    out.search = std::string(rest.substr(query));
    rest = rest.substr(0, query);
  }
  if (!rest.empty()) {
    out.pathname = std::string(rest);
  } else if (out.origin == "null") {
    out.pathname.clear();
  }
  return out;
}

}  // namespace microbrowser::bindings
