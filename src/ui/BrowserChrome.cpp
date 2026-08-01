#include "ui/BrowserChrome.h"

#include <algorithm>
#include <utility>

namespace microbrowser::ui {

namespace {

constexpr int kPixelsPerArrowKey = 40;
constexpr int kPixelsPerPage = 320;

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

void BrowserChrome::SyncToolbarState() {
  toolbar_.SetCanGoBack(history_.CanGoBack());
  toolbar_.SetCanGoForward(history_.CanGoForward());
}

BrowserChrome::Response BrowserChrome::Navigate(std::string url) {
  Response response;
  if (url.empty()) {
    return response;
  }
  response.handled = true;
  response.needs_repaint = true;
  response.intent = Intent{Intent::Kind::Navigate, std::move(url), 0};
  return response;
}

void BrowserChrome::OnNavigationCommitted(std::string url) {
  if (navigating_through_history_) {
    // A back or forward already moved the cursor. Pushing here would append the
    // destination as a new entry and strand everything in front of it.
    navigating_through_history_ = false;
  } else {
    history_.Push(url, url);
  }
  // The committed URL, not the typed one: a redirect changes where you ended
  // up, and an omnibox that kept showing the aim is lying about the origin the
  // page is running as.
  if (!toolbar_.IsOmniboxFocused()) {
    toolbar_.Omnibox().SetText(std::move(url));
  }
  SyncToolbarState();
}

void BrowserChrome::OnTitleChanged(std::string title) {
  history_.SetCurrentTitle(std::move(title));
}

std::string BrowserChrome::WindowTitle() const {
  const NavigationHistory::Entry* entry = history_.Current();
  if (entry == nullptr) {
    return "microbrowser";
  }
  return entry->title.empty() ? entry->url : entry->title;
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
    response.intent = Intent{Intent::Kind::Reload, {}, 0};
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
      if (const NavigationHistory::Entry* entry = history_.Current()) {
        toolbar_.Omnibox().SetText(entry->url);
      }
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

  // Not typing: the keys that scroll. Returned as an intent because the chrome
  // does not know how tall the page is; the engine clamps.
  int delta = 0;
  switch (event.key) {
    case platform::Key::Down:
      delta = kPixelsPerArrowKey;
      break;
    case platform::Key::Up:
      delta = -kPixelsPerArrowKey;
      break;
    case platform::Key::PageDown:
      delta = kPixelsPerPage;
      break;
    case platform::Key::PageUp:
      delta = -kPixelsPerPage;
      break;
    default:
      break;
  }
  if (delta != 0) {
    response.handled = true;
    response.intent = Intent{Intent::Kind::ScrollPage, {}, delta};
  }
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
      if (const NavigationHistory::Entry* entry = history_.GoBack()) {
        navigating_through_history_ = true;
        Response moved = Navigate(entry->url);
        SyncToolbarState();
        moved.needs_repaint = true;
        return moved;
      }
      return response;
    case Toolbar::Part::Forward:
      if (const NavigationHistory::Entry* entry = history_.GoForward()) {
        navigating_through_history_ = true;
        Response moved = Navigate(entry->url);
        SyncToolbarState();
        moved.needs_repaint = true;
        return moved;
      }
      return response;
    case Toolbar::Part::Reload:
      response.intent = Intent{Intent::Kind::Reload, {}, 0};
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
