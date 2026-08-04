#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

#include <cstddef>
#include <string>

#include "util/UserAgent.h"

// `window`, `location` and `navigator`: what a page reads about its
// environment rather than about its document.
//
// Split from DomBindings.cpp because that file reached the module's line cap,
// and the cap is written to mean a missing translation unit rather than a
// bigger file. These are the right ones to move: they are globals about the
// browsing context, and nothing in the tree walking refers to them.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// The document's address, split into the parts `location` reports.
//
// Splitting rather than parsing, and the difference is the whole reason this
// is allowed to live here. The string it is given is what `url::Url::Serialize`
// produced -- already parsed, already canonical -- so finding where the host
// ends is arithmetic on a known shape rather than an opinion about a hostile
// string. A second URL *parser* in this module would be exactly the "two
// parsers disagreeing about where the host ends" that url/Url.h names as the
// vulnerability.
//
// Which is also why `origin` here is for a page to *read* and never for the
// browser to decide anything with. When `pushState` arrives (ADR 0026 §2) its
// same-origin check is computed in the engine from `url::Origin`, on the URL
// the engine parsed, and it must not consult this.
struct Address {
  std::string protocol;  // "https:", with the colon, as the DOM reports it
  std::string host;      // "example.org:8080"
  std::string hostname;  // "example.org"
  std::string port;      // "8080", or empty for the scheme's default
  std::string pathname;  // "/a/b"
  std::string search;    // "?q=1", or empty
  std::string hash;      // "#top", or empty
  std::string origin;    // "https://example.org:8080", or "null"
};

Address SplitAddress(std::string_view href) {
  Address out;
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
    // Credentials are part of the authority and part of no `location`
    // property. `rfind`, not `find`: a password may contain an encoded `@`.
    if (const std::size_t credentials = authority.rfind('@');
        credentials != std::string_view::npos) {
      authority.remove_prefix(credentials + 1);
    }
    out.host = std::string(authority);
    // An IPv6 host is bracketed and full of colons, so the port separator is
    // the one after the closing bracket rather than the last one in the string.
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
  // An empty path is "/" for a URL with an authority and "" for one without --
  // `data:text/html,x` has an opaque path, which lands in pathname whole.
  if (!rest.empty()) {
    out.pathname = std::string(rest);
  } else if (out.origin == "null") {
    out.pathname.clear();
  }
  return out;
}

}  // namespace

void DomBindings::InstallWindow() {
  // `window` is the global object, and that is not a convenience alias -- a
  // page writes `window.foo = 1` and then reads `foo`, and the two have to be
  // the same binding or half of what a script sets goes missing.
  js::Object* global = interpreter_->Global();
  const Value window = Value::Obj(global);
  global->Set("window", window);
  global->Set("self", window);
  if (js::Value* document = interpreter_->GlobalScope()->Lookup("document")) {
    // So that `window.document` and `document` are the same object, which a
    // page checks more often than it looks.
    global->Set("document", *document);
  }
  interpreter_->GlobalScope()->Declare("window", window, false);

  // `location`, from the text the loader handed over. This module cannot see
  // `src/url` and should not: what a URL *means* is the loader's problem, and
  // all a page reads back here is the parts it was given.
  const Value location = interpreter_->NewObjectValue();
  if (location.IsObject()) {
    const Address address = SplitAddress(url_);
    location.object->Set("href", Value::String(url_));
    location.object->Set("protocol", Value::String(address.protocol));
    location.object->Set("host", Value::String(address.host));
    location.object->Set("hostname", Value::String(address.hostname));
    location.object->Set("port", Value::String(address.port));
    location.object->Set("pathname", Value::String(address.pathname));
    // `search` and `hash` were the two missing ones, and their absence is not
    // cosmetic: a page reading `new URLSearchParams(location.search)` off an
    // `undefined` gets an empty parameter set and carries on, which is how
    // reddit's challenge form submits without the fields it was meant to add.
    location.object->Set("search", Value::String(address.search));
    location.object->Set("hash", Value::String(address.hash));
    location.object->Set("origin", Value::String(address.origin));
    // `toString`, because a page concatenates `location` with a string as
    // often as it reads `href`.
    const Value to_string = interpreter_->NewNativeValue("toString", [](NativeCall& call) {
      if (!call.self.IsObject()) {
        return Value::String(std::string());
      }
      const Value* href = call.self.object->Get("href");
      return href == nullptr ? Value::String(std::string()) : *href;
    });
    if (to_string.IsObject()) {
      location.object->Set("toString", to_string);
    }
    global->Set("location", location);
    interpreter_->GlobalScope()->Declare("location", location, false);
    // `document.location` is the same object as `window.location`, which is
    // what it is in a browser and what a page checks by identity. reddit's
    // interstitial reads `document.location.search`.
    if (js::Value* document = interpreter_->GlobalScope()->Lookup("document")) {
      if (document->IsObject()) {
        document->object->Set("location", location);
        // And `document.URL`, which is the same string by another name.
        document->object->Set("URL", Value::String(url_));
      }
    }
  }

  InstallUrlSearchParams();

  // `navigator`, with one property and a deliberate one.
  //
  // The user agent is a fingerprinting surface before it is anything else, and
  // the string here says what this browser is and nothing about the machine it
  // is on -- no platform, no version of anything installed, no build date. A
  // page that varies its markup by user agent gets one answer from every copy
  // of this browser, which is the point.
  //
  // It is the same constant net sends as the `User-Agent` header, and it is
  // shared rather than repeated because a page may sniff both: two constants
  // would eventually disagree, and a page that renders one way and scripts
  // another is a bug nobody would look for here.
  const Value navigator = interpreter_->NewObjectValue();
  if (navigator.IsObject()) {
    navigator.object->Set("userAgent", Value::String(std::string(util::kUserAgent)));
    global->Set("navigator", navigator);
    interpreter_->GlobalScope()->Declare("navigator", navigator, false);
  }
}

}  // namespace microbrowser::bindings
