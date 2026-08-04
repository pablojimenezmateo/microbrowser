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
    const std::string& href = url_;
    const std::size_t scheme_end = href.find("://");
    const std::string protocol =
        scheme_end == std::string::npos ? std::string() : href.substr(0, scheme_end + 1);
    const std::size_t host_begin =
        scheme_end == std::string::npos ? 0 : scheme_end + 3;
    const std::size_t path_begin = href.find('/', host_begin);
    location.object->Set("href", Value::String(href));
    location.object->Set("protocol", Value::String(protocol));
    location.object->Set("host", Value::String(path_begin == std::string::npos
                                                   ? href.substr(host_begin)
                                                   : href.substr(host_begin,
                                                                 path_begin - host_begin)));
    location.object->Set("pathname",
                         Value::String(path_begin == std::string::npos
                                           ? std::string("/")
                                           : href.substr(path_begin)));
    global->Set("location", location);
    interpreter_->GlobalScope()->Declare("location", location, false);
  }

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
