// The images half of the engine: asking for them, and turning bytes into
// pixels the page can draw.
//
// Its own translation unit for the reason EngineInput.cpp and EngineFetch.cpp
// are: Engine.cpp is at its cap, and a file over its cap means a missing module
// rather than a bigger file. What makes this a coherent one is that every
// function here is about a resource whose arrival does not hold up a page --
// an image is requested during the load and may equally be requested long
// after it, when an `<img loading="lazy">` is scrolled towards (ADR 0018 §5),
// and the decode is the same either way.
//
// Sniffing is by *content*, not by the `Content-Type` a server sent: a server
// that mislabels a PNG is common and a server that mislabels a PNG on purpose
// is an attack, and either way the decoder that runs has to be the one the
// bytes are actually for.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/Clock.h"
#include "engine/Engine.h"
#include "gfx/JpegDecoder.h"
#include "gfx/PngDecoder.h"
#include "gfx/SvgDecoder.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

void Engine::StartImageRequests() {
  // The reveal first: an `<img loading="lazy">` becomes an image the page wants
  // at the moment its box comes within reach of the scrollport, and both
  // callers -- the initial subresource pass and every frame after it -- have to
  // ask in the same order or the first frame would fetch nothing lazy.
  page_.RevealLazyImages();
  const std::vector<std::string> wanted = page_.TakeUnrequestedImages();
  if (wanted.empty()) {
    return;
  }
  // The document's own address when no navigation is in flight -- a lazy image
  // revealed by a scroll belongs to a page that finished loading, and there is
  // no `load_.base` left to resolve it against. Parsed here rather than kept
  // as a member so that "where did this document come from" has one answer,
  // which is the page.
  std::optional<url::Url> parsed;
  const url::Url* base = load_.active && load_.base.has_value() ? &*load_.base : nullptr;
  if (base == nullptr) {
    parsed = url::Url::Parse(page_.Url());
    if (!parsed.has_value()) {
      return;
    }
    base = &*parsed;
  }

  net::FetchOptions options;
  options.bypass_cache = load_.active && load_.bypass_cache;
  for (const std::string& src : wanted) {
    const Loader::RequestId id = loader_.StartSubresource(
        src, *base, privacy::ResourceType::Image, NowSeconds(), options);
    // Before the first frame the load owns it, so that an image already on
    // screen is there when the page appears rather than a beat later. After
    // it, the load is over as far as the user is concerned and the image is
    // decoded and painted on its own.
    if (load_.active && !load_.painted) {
      load_.resources[id] = PendingResource{ResourceKind::Image, 0, src};
      ++load_.images_outstanding;
    } else {
      late_images_[id] = src;
    }
  }
}

bool Engine::OnLateImage(Loader::Completion completion) {
  const auto found = late_images_.find(completion.id);
  if (found == late_images_.end()) {
    return false;
  }
  const std::string src = found->second;
  late_images_.erase(found);
  if (!completion.result.ok) {
    AddPerformanceCounter(PerfCounterId::EngineImagesFailed);
    return false;
  }
  DecodeImage(src, completion.result.body);
  LayoutAndPaint();
  return true;
}

void Engine::DecodeImage(const std::string& src, const std::string& bytes) {
  // The bytes are attacker-controlled and the decoder says so: a failure here
  // is an image that does not draw, not a page that does not render.
  //
  // Which decoder is chosen by sniffing rather than by the Content-Type
  // header, for the reason every browser sniffs: the header is a claim by the
  // server, and a server that mislabels a PNG must not stop it rendering.
  // reddit serves a JPEG from a URL ending .png on its own front page.
  //
  // Exactly one decoder is offered the bytes, and only if their magic number
  // named it — ADR 0023 §2. Trying each decoder until one succeeds is the
  // shape that makes every decoder reachable by every image, which is three
  // times the attack surface for no compatibility gained.
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
  } else if (gfx::LooksLikeJpeg(span)) {
    gfx::JpegDecodeResult decoded = gfx::DecodeJpeg(span);
    if (decoded.Ok()) {
      image = std::move(decoded.image);
    }
  } else if (gfx::LooksLikePng(span)) {
    gfx::PngDecodeResult decoded = gfx::DecodePng(span);
    if (decoded.Ok()) {
      image = std::move(decoded.image);
    }
  }
  if (!image.IsValid()) {
    AddPerformanceCounter(PerfCounterId::EngineImagesFailed);
    return;
  }
  page_.AddImage(src, std::make_shared<const gfx::Image>(std::move(image)));
  AddPerformanceCounter(PerfCounterId::EngineImagesLoaded);
}

void Engine::DecodePendingImages() {
  for (auto& [src, bytes] : load_.image_bytes) {
    DecodeImage(src, bytes);
  }
}

}  // namespace microbrowser::engine
