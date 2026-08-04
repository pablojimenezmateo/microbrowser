#include "engine/Engine.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gfx/DisplayListDiff.h"
#include "gfx/PngDecoder.h"
#include "gfx/SvgDecoder.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Escapes text for placement inside an HTML document.
//
// The error page is built by string concatenation, and the URL in it comes from
// whoever asked for the navigation. Interpolating it raw would make a URL
// containing markup a script injection into the browser's own error page --
// the classic self-XSS in the one document a browser writes itself.
std::string EscapeHtml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The blank document. Not an empty string: "" parses to a document with a body
// too, but saying it here means about:blank is a real page rather than a
// failure that happens to look like one.
constexpr std::string_view kBlankDocument =
    "<!DOCTYPE html><html><head><title>New Tab</title></head><body></body></html>";

std::optional<std::string> ResolveLink(std::string_view href, std::string_view document_url) {
  if (const std::optional<url::Url> absolute = url::Url::Parse(href)) {
    return absolute->Serialize();
  }
  const std::optional<url::Url> base = url::Url::Parse(document_url);
  if (!base.has_value()) {
    return std::nullopt;
  }
  const std::optional<url::Url> resolved = url::Url::Parse(href, *base);
  return resolved.has_value() ? std::optional<std::string>(resolved->Serialize()) : std::nullopt;
}

std::vector<std::byte> BodyBytes(std::string_view body) {
  std::vector<std::byte> out;
  out.reserve(body.size());
  for (const char c : body) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  return out;
}

net::FetchOptions FetchOptionsForSubmission(const FormSubmission& submission) {
  net::FetchOptions options;
  options.method = submission.method;
  if (!submission.body.empty()) {
    options.body = BodyBytes(submission.body);
  }
  if (!submission.content_type.empty()) {
    options.headers.Add("Content-Type", submission.content_type);
  }
  return options;
}

}  // namespace

Engine::Engine(ipc::EngineEndpoint& endpoint, gfx::FontProvider& fonts)
    : endpoint_(endpoint), page_(fonts) {}

bool Engine::HandlePendingMessages() {
  bool produced_output = false;

  while (std::optional<ipc::UiToEngine> message = endpoint_.TryReceive()) {
    if (const auto* navigate = std::get_if<ipc::NavigateMessage>(&*message)) {
      Navigate(navigate->url);
      produced_output = true;
    } else if (const auto* resize = std::get_if<ipc::ResizeViewportMessage>(&*message)) {
      SetViewport(resize->size, resize->device_scale);
      produced_output = true;
    } else if (const auto* scroll = std::get_if<ipc::ScrollMessage>(&*message)) {
      ScrollBy(scroll->delta_x, scroll->delta_y);
      produced_output = true;
    } else if (const auto* reload = std::get_if<ipc::ReloadMessage>(&*message)) {
      net::FetchOptions options;
      options.bypass_cache = reload->bypass_cache;
      Navigate(page_.Url(), options);
      produced_output = true;
    } else if (const auto* pointer = std::get_if<ipc::PointerMessage>(&*message)) {
      produced_output = HandlePointer(*pointer) || produced_output;
    } else if (const auto* text = std::get_if<ipc::TextInputMessage>(&*message)) {
      if (page_.InsertTextIntoFocusedTextControl(text->text)) {
        LayoutAndPaint();
        produced_output = true;
      }
    } else if (const auto* command = std::get_if<ipc::InputCommandMessage>(&*message)) {
      using Command = ipc::InputCommandMessage::Command;
      switch (command->command) {
        case Command::Backspace:
          if (page_.DeleteBackwardFromFocusedTextControl()) {
            LayoutAndPaint();
            produced_output = true;
          }
          break;
        case Command::Delete:
          // The current caret model is end-of-text only, so there is no
          // forward character to delete yet.
          break;
        case Command::Enter:
          if (const std::optional<FormSubmission> submission = page_.FocusedFormSubmission()) {
            produced_output = Navigate(*submission) || produced_output;
          }
          break;
      }
    }
    // StopLoad is accepted and ignored: loading is synchronous so there is
    // nothing to stop. It is in the vocabulary now so the UI can be written
    // against the final shape.
  }

  return produced_output;
}

bool Engine::HandlePointer(const ipc::PointerMessage& pointer) {
  if (pointer.kind != ipc::PointerMessage::Kind::Down || pointer.button != 1 ||
      device_scale_ <= 0.0f) {
    return false;
  }
  const gfx::FloatPoint document_point{
      static_cast<float>(pointer.position.x) / device_scale_,
      static_cast<float>(pointer.position.y) / device_scale_ + static_cast<float>(scroll_y_)};
  // The page's own handlers run first, and a `preventDefault` stops everything
  // below. That ordering is the whole contract of the method: a script that
  // intercepts a click on a link expects the link not to be followed, and
  // deciding to navigate before asking would make `preventDefault` a lie.
  const ClickOutcome click = page_.DispatchClickAt(document_point);
  if (click.prevented) {
    page_.InvalidateLayout();
    LayoutAndPaint();
    return true;
  }
  if (const std::optional<FormSubmission> submission =
          page_.FormSubmissionRequestAt(document_point)) {
    return Navigate(*submission);
  }
  if (page_.ResetFormAt(document_point)) {
    LayoutAndPaint();
    return true;
  }
  if (page_.ActivateCheckableInputAt(document_point)) {
    LayoutAndPaint();
    return true;
  }
  if (page_.FocusTextControlAt(document_point)) {
    return false;
  }
  const std::optional<std::string> href = page_.LinkAt(document_point);
  if (!href.has_value()) {
    // Nothing to navigate to, but a handler may still have changed the
    // document -- which is the case that used to run the handler and leave the
    // screen alone.
    if (click.ran) {
      page_.InvalidateLayout();
      LayoutAndPaint();
      return true;
    }
    return false;
  }
  const std::optional<std::string> resolved = ResolveLink(*href, page_.Url());
  if (!resolved.has_value()) {
    return false;
  }
  NavigateFromCurrentDocument(*resolved, {});
  return true;
}

void Engine::Navigate(const std::string& url) {
  Navigate(url, {});
}

void Engine::Navigate(const std::string& url, const net::FetchOptions& options) {
  Navigate(url, options, nullptr);
}

void Engine::Navigate(const std::string& url, const net::FetchOptions& options,
                      const url::Url* referrer_document) {
  util::PerformanceTrace::Scope scope("engine::Navigate");
  AddPerformanceCounter(PerfCounterId::EngineNavigations);

  scroll_y_ = 0;

  if (url.empty() || url == "about:blank") {
    page_.Load(kBlankDocument, url.empty() ? std::string("about:blank") : url);
  } else {
    // Synchronous, and that is a stated limitation rather than a design: the
    // loop blocks for the length of a load. Making it asynchronous is a change
    // to this function and the message vocabulary, not to the seam -- which is
    // why it can wait until there is something worth waiting on.
    const Loader::Result loaded = loader_.Load(url, NowSeconds(), options, referrer_document);
    if (!loaded.ok) {
      ShowError(url, loaded.error == nullptr ? "the load failed" : loaded.error);
      return;
    }
    page_.Load(loaded.body, loaded.final_url.empty() ? url : loaded.final_url);
  }

  LoadSubresources(options.bypass_cache);
  endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
  endpoint_.Send(ipc::TitleChangedMessage{page_.Title()});
  LayoutAndPaint();
  endpoint_.Send(ipc::LoadProgressMessage{1.0f});
}

void Engine::NavigateFromCurrentDocument(const std::string& url,
                                         const net::FetchOptions& options) {
  const std::optional<url::Url> referrer = url::Url::Parse(page_.Url());
  Navigate(url, options, referrer.has_value() ? &*referrer : nullptr);
}

bool Engine::Navigate(const FormSubmission& submission) {
  const std::optional<std::string> resolved = ResolveLink(submission.url, page_.Url());
  if (!resolved.has_value()) {
    return false;
  }
  NavigateFromCurrentDocument(*resolved, FetchOptionsForSubmission(submission));
  return true;
}

void Engine::LoadSubresources(bool bypass_cache) {
  // Synchronously, before the first layout. That is not how a browser should do
  // it -- a slow subresource blocks the page -- but a stylesheet *is*
  // render-blocking and an image's size changes layout, so the ordering is
  // right even though the blocking is crude. What must not happen is laying out
  // without them and reflowing after: that is the flash of unstyled content and
  // the layout shift, and both are hard to retrofit away.
  const std::optional<url::Url> document = url::Url::Parse(page_.Url());
  if (!document.has_value()) {
    // A data: or about: document has no base to resolve against, so a relative
    // href in one has nowhere to point.
    return;
  }

  net::FetchOptions options;
  options.bypass_cache = bypass_cache;

  const std::vector<std::string>& sheets = page_.PendingStyleSheets();
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    const Loader::Result sheet =
        loader_.LoadSubresource(sheets[i], *document, privacy::ResourceType::Stylesheet,
                                NowSeconds(), options);
    if (sheet.ok) {
      page_.AddStyleSheet(i, sheet.body);
      AddPerformanceCounter(PerfCounterId::EngineStyleSheetsLoaded);
    } else {
      // A stylesheet that does not load is a page rendered without it, which is
      // what every browser does. It is not a navigation failure.
      AddPerformanceCounter(PerfCounterId::EngineStyleSheetsFailed);
    }
  }

  // Scripts after stylesheets and before images, which is the order that makes
  // a script see the styles it may ask about and lets it add elements whose
  // images are then collected.
  const std::vector<std::string>& scripts = page_.PendingScripts();
  for (std::size_t i = 0; i < scripts.size(); ++i) {
    const Loader::Result script = loader_.LoadSubresource(
        scripts[i], *document, privacy::ResourceType::Script, NowSeconds(), options);
    if (script.ok) {
      page_.AddScript(i, script.body);
      AddPerformanceCounter(PerfCounterId::EngineScriptsLoaded);
    } else {
      // A script that does not load leaves its slot empty and the ones after
      // it still run. A page whose analytics tag is blocked is a page, which
      // is the whole reason the blocking engine can be pointed at one.
      AddPerformanceCounter(PerfCounterId::EngineScriptsFailed);
    }
  }
  page_.RunScripts();

  for (const std::string& src : page_.PendingImages()) {
    const Loader::Result fetched =
        loader_.LoadSubresource(src, *document, privacy::ResourceType::Image, NowSeconds(),
                                options);
    if (!fetched.ok) {
      AddPerformanceCounter(PerfCounterId::EngineImagesFailed);
      continue;
    }
    // The bytes are attacker-controlled and the decoder says so: a failure here
    // is an image that does not draw, not a page that does not render.
    //
    // Which decoder is chosen by sniffing rather than by the Content-Type
    // header, for the reason every browser sniffs: the header is a claim by the
    // server, and a server that mislabels a PNG must not stop it rendering.
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(fetched.body.data()), fetched.body.size());
    gfx::Image image;
    if (gfx::LooksLikeSvg(bytes)) {
      // SVG is a document, so it has to be rasterized at a size. The element's
      // attributes are the size the page asked for; the document's own is the
      // fallback, applied inside the decoder.
      const gfx::IntSize requested = page_.RequestedImageSize(src);
      gfx::SvgDecodeResult decoded = gfx::DecodeSvg(bytes, requested.width, requested.height);
      if (decoded.Ok()) {
        image = std::move(decoded.image);
      }
    } else {
      gfx::PngDecodeResult decoded = gfx::DecodePng(bytes);
      if (decoded.Ok()) {
        image = std::move(decoded.image);
      }
    }
    if (!image.IsValid()) {
      AddPerformanceCounter(PerfCounterId::EngineImagesFailed);
      continue;
    }
    page_.AddImage(src, std::make_shared<const gfx::Image>(std::move(image)));
    AddPerformanceCounter(PerfCounterId::EngineImagesLoaded);
  }
}

void Engine::ShowError(std::string_view url, std::string_view message) {
  // A browser that shows nothing when a load fails is indistinguishable from a
  // browser that has hung.
  std::string html =
      "<!DOCTYPE html><html><head><title>Cannot load page</title>"
      "<style>body{margin:32px;font-family:sans-serif;color:#202028}"
      "h1{font-size:1.5em;color:#8b1a1a}code{font-family:monospace;color:#404050}</style>"
      "</head><body><h1>Cannot load this page</h1><p>";
  html += EscapeHtml(message);
  html += "</p><p><code>";
  html += EscapeHtml(url);
  html += "</code></p></body></html>";

  page_.Load(html, std::string(url));
  endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
  endpoint_.Send(ipc::TitleChangedMessage{page_.Title()});
  LayoutAndPaint();
  endpoint_.Send(ipc::LoadProgressMessage{1.0f});
}

void Engine::SetViewport(const gfx::IntSize& size, float device_scale) {
  if (size == viewport_size_ && device_scale == device_scale_) {
    return;
  }
  viewport_size_ = size;
  device_scale_ = device_scale;
  // A resize changes the containing block, so it relays out. This is the one
  // input that does.
  LayoutAndPaint();
}

int Engine::MaxScroll() const {
  return std::max(0, static_cast<int>(page_.ContentHeight()) - viewport_size_.height);
}

void Engine::ScrollBy(int delta_x, int delta_y) {
  (void)delta_x;  // No horizontal overflow yet: layout never exceeds the width.
  const int previous = scroll_y_;
  scroll_y_ = std::clamp(scroll_y_ + delta_y, 0, MaxScroll());
  if (scroll_y_ != previous) {
    // Paints without laying out. The geometry has not changed, and a scroll
    // that relaid out is the classic reason scrolling is slow.
    PaintAndSend();
  }
}

void Engine::LayoutAndPaint() {
  if (viewport_size_.width > 0) {
    page_.Layout(static_cast<float>(viewport_size_.width) / device_scale_);
    scroll_y_ = std::clamp(scroll_y_, 0, MaxScroll());
  }
  PaintAndSend();
}

void Engine::PaintAndSend() {
  util::PerformanceTrace::Scope scope("engine::Paint");
  AddPerformanceCounter(PerfCounterId::EnginePaintsProduced);
  AddPerformanceCounter(PerfCounterId::DisplayListBuilds);

  const gfx::IntRect viewport{0, 0, viewport_size_.width, viewport_size_.height};
  if (viewport.IsEmpty()) {
    return;
  }

  pending_.Clear();
  // The canvas behind the document, painted here rather than by the page: a
  // document shorter than the viewport still has a window under it, and the
  // page has no opinion about pixels it does not cover.
  pending_.FillRect(viewport, gfx::Color::Rgb(0xFF, 0xFF, 0xFF));
  page_.Paint(pending_, static_cast<float>(scroll_y_));

  gfx::DirtyRegion damage;
  const bool bounded = gfx::ComputeDamage(display_list_, pending_, viewport, damage);
  if (bounded && damage.IsEmpty()) {
    // Nothing on screen would change. Sending the frame anyway would make the
    // UI upload a texture to draw the same picture, which is most of what a
    // browser wastes power on.
    AddPerformanceCounter(PerfCounterId::EnginePaintsSkipped);
    return;
  }

  ipc::PaintFrameMessage frame;
  frame.display_list = pending_;
  // Empty damage means "the whole viewport", which is what the diff reports
  // when it cannot bound the change -- a clip moved, and every command after a
  // clip reads it as state.
  if (bounded) {
    frame.damage = damage.Rects();
  }
  display_list_ = pending_;
  endpoint_.Send(std::move(frame));
}

}  // namespace microbrowser::engine
