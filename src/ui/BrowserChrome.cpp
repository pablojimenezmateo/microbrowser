#include "ui/BrowserChrome.h"

#include <algorithm>
#include <utility>

namespace microbrowser::ui {

namespace {

// Schemes worth recognizing before the port heuristic below gets a say.
// `data:1234` is a data URL; `localhost:8080` is a host and a port, and the
// two are indistinguishable by shape alone.
constexpr std::string_view kKnownSchemes[] = {
    "http", "https", "file", "data", "about", "blob", "ws", "wss", "ftp", "view-source",
};

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

bool LooksLikeScheme(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == 0 || colon == std::string_view::npos) {
    return false;
  }
  const std::string_view prefix = text.substr(0, colon);
  for (const char c : prefix) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    if (!allowed) {
      return false;
    }
  }
  // A leading letter, per the URL spec's scheme production. Without it,
  // `12:30 meeting` would be a URL.
  const char first = prefix.front();
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z'))) {
    return false;
  }

  for (const std::string_view known : kKnownSchemes) {
    if (prefix.size() == known.size() &&
        std::equal(prefix.begin(), prefix.end(), known.begin(), [](char a, char b) {
          return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
        })) {
      return true;
    }
  }

  // Not a scheme anyone has heard of, and what follows is a port number. That
  // is `localhost:8080`, which the URL spec reads as scheme `localhost` and
  // which every user means as a host. Reading it as a scheme would send it to
  // a search engine, which leaks what they typed.
  const std::string_view rest = text.substr(colon + 1);
  const bool port_shaped =
      !rest.empty() && std::all_of(rest.begin(), rest.end(), [](char c) {
        return IsAsciiDigit(c) || c == '/';
      });
  return !port_shaped;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

std::string ResolveOmniboxInput(std::string_view typed) {
  const std::string_view text = Trim(typed);
  if (text.empty()) {
    return {};
  }
  if (LooksLikeScheme(text)) {
    return std::string(text);
  }

  // A space means it is not a host. This is the check that decides whether
  // what someone typed leaves the machine as a search query, so it is
  // deliberately the *only* way to reach the search path along with "no dot
  // and no port".
  const bool has_space = text.find(' ') != std::string_view::npos;
  const std::size_t dot = text.find('.');
  const std::size_t colon = text.find(':');
  const bool host_shaped =
      !has_space && ((dot != std::string_view::npos && dot + 1 < text.size()) ||
                     colon != std::string_view::npos || text == "localhost");

  if (host_shaped) {
    // https, not http. Guessing the insecure scheme for something a user typed
    // is a downgrade nobody asked for, and the privacy layer would refuse it
    // anyway -- better to ask for the right thing than to be corrected.
    return "https://" + std::string(text);
  }

  std::string query;
  query.reserve(text.size() * 3);
  for (const char c : text) {
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      query.push_back(c);
    } else if (c == ' ') {
      query.push_back('+');
    } else {
      constexpr char kHex[] = "0123456789ABCDEF";
      query.push_back('%');
      query.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
      query.push_back(kHex[static_cast<unsigned char>(c) & 0xF]);
    }
  }
  return "https://duckduckgo.com/?q=" + query;
}

void BrowserChrome::SetViewportWidth(int width) { toolbar_.SetWidth(width); }

gfx::IntRect BrowserChrome::PageBounds(const gfx::IntSize& window) const {
  return gfx::IntRect{0, Toolbar::kHeight, std::max(0, window.width),
                      std::max(0, window.height - Toolbar::kHeight)};
}

void BrowserChrome::OnHistoryState(bool can_go_back, bool can_go_forward) {
  // Two bools, from the engine, and nothing else. The chrome used to decide this
  // from a list it owned; ADR 0026 §1 moved the list to where the documents are,
  // and this is all that came back.
  toolbar_.SetCanGoBack(can_go_back);
  toolbar_.SetCanGoForward(can_go_forward);
}

BrowserChrome::Response BrowserChrome::Navigate(std::string url) {
  Response response;
  if (url.empty()) {
    return response;
  }
  response.handled = true;
  response.needs_repaint = true;
  response.intent = Intent{Intent::Kind::Navigate, std::move(url), false};
  return response;
}

void BrowserChrome::OnNavigationCommitted(std::string url) {
  // The committed URL, not the typed one: a redirect changes where you ended
  // up, and an omnibox that kept showing the aim is lying about the origin the
  // page is running as. This message now also arrives for a same-document
  // navigation -- a `pushState`, a traversal, an in-page anchor -- and it means
  // the same thing every time: *this* is the URL of the document on screen.
  url_ = std::move(url);
  title_.clear();
  if (!toolbar_.IsOmniboxFocused()) {
    toolbar_.Omnibox().SetText(url_);
  }
}

void BrowserChrome::OnTitleChanged(std::string title) { title_ = std::move(title); }

std::string BrowserChrome::WindowTitle() const {
  if (url_.empty()) {
    return "microbrowser";
  }
  return title_.empty() ? url_ : title_;
}

BrowserChrome::Response BrowserChrome::HandleKey(const platform::KeyEvent& event) {
  Response response;
  if (!event.pressed) {
    return response;
  }

  // Focus the omnibox and select what is in it, so typing replaces it. The
  // shortcut works whether or not the omnibox already has focus, which is what
  // makes it a reliable way to start over.
  if (event.modifiers.control && (event.codepoint == U'l' || event.codepoint == U'L')) {
    toolbar_.SetOmniboxFocused(true);
    toolbar_.Omnibox().SelectAll();
    response.handled = true;
    response.needs_repaint = true;
    return response;
  }

  if (event.modifiers.control && (event.codepoint == U'r' || event.codepoint == U'R')) {
    response.handled = true;
    response.needs_repaint = true;
    response.intent = Intent{Intent::Kind::Reload, {}, event.modifiers.shift};
    return response;
  }

  if (toolbar_.IsOmniboxFocused()) {
    if (event.key == platform::Key::Enter) {
      toolbar_.SetOmniboxFocused(false);
      return Navigate(ResolveOmniboxInput(toolbar_.Omnibox().Text()));
    }
    if (event.key == platform::Key::Escape) {
      // Back to what is actually loaded, which is the entry the history says
      // is current -- not the last thing typed.
      toolbar_.SetOmniboxFocused(false);
      toolbar_.Omnibox().SetText(url_);
      response.handled = true;
      response.needs_repaint = true;
      return response;
    }
    if (toolbar_.Omnibox().HandleKey(event)) {
      response.handled = true;
      response.needs_repaint = true;
      return response;
    }
    return response;
  }

  // Nothing else. The arrow and page keys used to scroll from here, which meant
  // a page never saw an ArrowDown and `preventDefault` on one meant nothing --
  // so scrolling moved to where the other keyboard default actions live, after
  // the page's handlers have had the key. See Engine::ScrollByKey, ADR 0017 §2,
  // and app/KeyRouting.h for the rule that decides whose key this was.
  return response;
}

BrowserChrome::Response BrowserChrome::HandlePointer(const platform::PointerEvent& event) {
  Response response;
  if (event.kind != platform::PointerEvent::Kind::Down) {
    return response;
  }

  const Toolbar::Part part = toolbar_.HitTest(event.position);
  if (part == Toolbar::Part::Outside) {
    // A click in the page takes focus away from the omnibox, which is the only
    // way to leave it with the mouse.
    if (toolbar_.IsOmniboxFocused()) {
      toolbar_.SetOmniboxFocused(false);
      response.needs_repaint = true;
    }
    return response;
  }

  response.handled = true;
  response.needs_repaint = true;
  switch (part) {
    case Toolbar::Part::Back:
      // A delta, not a URL. The chrome does not know where back is any more, and
      // that is the point: only the engine can tell a `pushState` entry from a
      // loaded one, and the difference is a paint against a load.
      response.intent = Intent{Intent::Kind::TraverseHistory, {}, false, -1};
      return response;
    case Toolbar::Part::Forward:
      response.intent = Intent{Intent::Kind::TraverseHistory, {}, false, 1};
      return response;
    case Toolbar::Part::Reload:
      response.intent = Intent{Intent::Kind::Reload, {}, false};
      return response;
    case Toolbar::Part::Omnibox:
      toolbar_.SetOmniboxFocused(true);
      toolbar_.Omnibox().SelectAll();
      return response;
    default:
      if (toolbar_.IsOmniboxFocused()) {
        toolbar_.SetOmniboxFocused(false);
      }
      return response;
  }
}

}  // namespace microbrowser::ui
