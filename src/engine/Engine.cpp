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

// A steady clock, unlike the one above. Cache and cookie expiry are about wall
// time and must follow it; a timer's delay is about elapsed time and must not
// -- a page whose `setTimeout` fired early because the machine's clock was
// corrected is a page that broke for a reason nobody will find.
std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
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
    } else if (std::holds_alternative<ipc::StopLoadMessage>(*message)) {
      // Now a real thing to do: the queue drops every outstanding request and
      // their connections close with them. The page keeps whatever had already
      // arrived, which is what a browser does.
      if (load_.active) {
        loader_.CancelAll();
        load_ = PendingLoad{};
        endpoint_.Send(ipc::LoadProgressMessage{1.0f});
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
  }

  return produced_output;
}

bool Engine::Advance() {
  if (!load_.active) {
    return false;
  }
  loader_.Advance(NowMilliseconds());
  std::vector<Loader::Completion> completions = loader_.TakeCompletions();
  for (Loader::Completion& completion : completions) {
    OnCompletion(std::move(completion));
    if (!load_.active) {
      // The document failed, or a navigation replaced this one from inside a
      // completion. Anything still in the batch belongs to a load that is gone.
      return true;
    }
  }
  const bool moved = !completions.empty();
  AdvanceLoad();
  return moved;
}

void Engine::AppendWaitDescriptors(util::WaitDescriptorList& out) const {
  loader_.AppendDescriptors(out);
}

bool Engine::HasRunnableWork() const {
  return load_.active && loader_.HasRunnableWork();
}

std::optional<std::uint32_t> Engine::NextDeadlineMs() const {
  const std::int64_t now_ms = NowMilliseconds();
  const std::optional<std::uint32_t> timers = page_.NextWakeDelay(now_ms);
  const std::optional<std::uint32_t> network =
      load_.active ? loader_.NextDeadlineMs(now_ms) : std::nullopt;
  if (!timers.has_value()) {
    return network;
  }
  if (!network.has_value()) {
    return timers;
  }
  return std::min(*timers, *network);
}

bool Engine::RunDueWork() {
  if (!page_.RunDueWork(NowMilliseconds())) {
    return false;
  }
  LayoutAndPaint();
  return true;
}

void Engine::OnCompletion(Loader::Completion completion) {
  if (!load_.active) {
    return;
  }
  if (completion.id == load_.document && !load_.document_arrived) {
    load_.document_arrived = true;
    OnDocument(std::move(completion.result));
    return;
  }
  const auto found = load_.resources.find(completion.id);
  if (found == load_.resources.end()) {
    // A completion for a request this load did not make. It cannot happen while
    // a navigation cancels everything, and dropping it is the right answer if
    // it ever does.
    return;
  }
  const PendingResource resource = found->second;
  load_.resources.erase(found);
  ++load_.finished_resources;

  switch (resource.kind) {
    case ResourceKind::StyleSheet:
      --load_.sheets_outstanding;
      if (completion.result.ok) {
        page_.AddStyleSheet(resource.index, completion.result.body);
        AddPerformanceCounter(PerfCounterId::EngineStyleSheetsLoaded);
      } else {
        // A stylesheet that does not load is a page rendered without it, which
        // is what every browser does. It is not a navigation failure.
        AddPerformanceCounter(PerfCounterId::EngineStyleSheetsFailed);
      }
      break;
    case ResourceKind::Script: {
      // Which counter this came off decides whether the first paint was
      // waiting for it. `async` says the page is not, and honouring that is
      // the whole of what the attribute means.
      const bool is_async = page_.PendingScriptIsAsync(resource.index);
      --(is_async ? load_.async_scripts_outstanding : load_.scripts_outstanding);
      if (completion.result.ok) {
        page_.AddScript(resource.index, std::move(completion.result.body));
        AddPerformanceCounter(PerfCounterId::EngineScriptsLoaded);
        if (is_async && load_.scripts_ran && page_.RunReadyAsyncScripts()) {
          // It landed after the page was already up. Running it can have
          // changed the tree, so the page is laid out again -- which is what
          // an async script arriving late looks like in every browser.
          page_.InvalidateLayout();
          LayoutAndPaint();
        }
      } else {
        // A script that does not load leaves its slot empty and the ones after
        // it still run. A page whose analytics tag is blocked is a page, which
        // is the whole reason the blocking engine can be pointed at one.
        AddPerformanceCounter(PerfCounterId::EngineScriptsFailed);
      }
      break;
    }
    case ResourceKind::Image:
      --load_.images_outstanding;
      if (completion.result.ok) {
        // Held, not decoded. See PendingLoad::image_bytes.
        load_.image_bytes.emplace_back(resource.src, std::move(completion.result.body));
      } else {
        AddPerformanceCounter(PerfCounterId::EngineImagesFailed);
      }
      break;
  }

  if (load_.total_resources > 0) {
    endpoint_.Send(ipc::LoadProgressMessage{load_.Progress()});
  }
}

void Engine::OnDocument(Loader::Result result) {
  if (!result.ok) {
    const std::string url = load_.url;
    load_ = PendingLoad{};
    ShowError(url, result.error.empty() ? "the load failed" : result.error);
    return;
  }

  page_.Load(result.body, result.final_url.empty() ? load_.url : result.final_url);
  endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
  endpoint_.Send(ipc::TitleChangedMessage{page_.Title()});

  // A data: or about: document has no base to resolve against, so a relative
  // href in one has nowhere to point.
  load_.base = url::Url::Parse(page_.Url());
  if (load_.base.has_value()) {
    StartSubresources();
  }
}

void Engine::StartSubresources() {
  net::FetchOptions options;
  options.bypass_cache = load_.bypass_cache;
  const url::Url& document = *load_.base;

  // All at once. The order they are *started* in is document order and stays
  // deterministic; the order they arrive in is the network's business and this
  // engine must not have an opinion about it, which is what the slot-filling
  // below is for.
  const std::vector<std::string>& sheets = page_.PendingStyleSheets();
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    const Loader::RequestId id = loader_.StartSubresource(
        sheets[i], document, privacy::ResourceType::Stylesheet, NowSeconds(), options);
    load_.resources[id] = PendingResource{ResourceKind::StyleSheet, i, {}};
    ++load_.sheets_outstanding;
  }

  const std::vector<std::string>& scripts = page_.PendingScripts();
  for (std::size_t i = 0; i < scripts.size(); ++i) {
    const Loader::RequestId id = loader_.StartSubresource(
        scripts[i], document, privacy::ResourceType::Script, NowSeconds(), options);
    load_.resources[id] = PendingResource{ResourceKind::Script, i, {}};
    ++(page_.PendingScriptIsAsync(i) ? load_.async_scripts_outstanding
                                     : load_.scripts_outstanding);
  }

  for (const std::string& src : page_.PendingImages()) {
    const Loader::RequestId id = loader_.StartSubresource(
        src, document, privacy::ResourceType::Image, NowSeconds(), options);
    load_.resources[id] = PendingResource{ResourceKind::Image, 0, src};
    ++load_.images_outstanding;
  }

  load_.total_resources = load_.resources.size();
}

void Engine::AdvanceLoad() {
  if (!load_.active || !load_.document_arrived) {
    return;
  }
  // Scripts run once every render-blocking resource has resolved, and before
  // the images are decoded. Stylesheets first so a script that asks about a
  // style sees the ones the document declared; images after, so a script that
  // sets a width has said so before an SVG is rasterized to it.
  if (load_.MayRunScripts()) {
    page_.RunScripts(NowMilliseconds());
    load_.scripts_ran = true;
  }
  if (load_.MayPaint()) {
    Paint();
  }
  if (load_.IsFinished()) {
    // Only now is the navigation over. It stayed alive past the first frame
    // for the `async` scripts the page said it would not wait for, which is
    // the difference between not blocking on one and dropping it.
    load_ = PendingLoad{};
  }
}

void Engine::DecodePendingImages() {
  for (auto& [src, bytes] : load_.image_bytes) {
    // The bytes are attacker-controlled and the decoder says so: a failure here
    // is an image that does not draw, not a page that does not render.
    //
    // Which decoder is chosen by sniffing rather than by the Content-Type
    // header, for the reason every browser sniffs: the header is a claim by the
    // server, and a server that mislabels a PNG must not stop it rendering.
    const std::span<const std::byte> span(reinterpret_cast<const std::byte*>(bytes.data()),
                                          bytes.size());
    gfx::Image image;
    if (gfx::LooksLikeSvg(span)) {
      // SVG is a document, so it has to be rasterized at a size. The element's
      // attributes are the size the page asked for; the document's own is the
      // fallback, applied inside the decoder.
      const gfx::IntSize requested = page_.RequestedImageSize(src);
      gfx::SvgDecodeResult decoded = gfx::DecodeSvg(span, requested.width, requested.height);
      if (decoded.Ok()) {
        image = std::move(decoded.image);
      }
    } else {
      gfx::PngDecodeResult decoded = gfx::DecodePng(span);
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

void Engine::Paint() {
  DecodePendingImages();
  load_.painted = true;
  // The title again. It was sent when the document committed, before its
  // scripts ran, and a page that changes its own title -- which is most of
  // them -- would otherwise keep whatever the markup said, or the URL.
  endpoint_.Send(ipc::TitleChangedMessage{page_.Title()});
  LayoutAndPaint();
  endpoint_.Send(ipc::LoadProgressMessage{1.0f});
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

  // Everything the previous navigation had in flight goes now, connections and
  // all. That is what makes a response for a document that is gone
  // undeliverable rather than merely ignored -- ADR 0011 asked for it by
  // construction, and this is the construction.
  loader_.CancelAll();
  load_ = PendingLoad{};
  load_.active = true;
  load_.url = url.empty() ? std::string("about:blank") : url;
  load_.bypass_cache = options.bypass_cache;

  if (url.empty() || url == "about:blank") {
    // The blank document is not fetched. It still goes through the same state
    // machine, so that "what happens after a document arrives" has one
    // implementation rather than two.
    load_.document_arrived = true;
    Loader::Result blank;
    blank.ok = true;
    blank.body = std::string(kBlankDocument);
    blank.final_url = load_.url;
    blank.status = 200;
    OnDocument(std::move(blank));
    AdvanceLoad();
    return;
  }

  endpoint_.Send(ipc::LoadProgressMessage{0.0f});
  load_.document = loader_.StartLoad(url, NowSeconds(), options, referrer_document);
  // One turn now, so that a data: URL or a cache hit does not wait for the
  // loop to come back round.
  Advance();
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
