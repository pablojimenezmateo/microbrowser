#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

namespace microbrowser::util {

// Free-running process-wide event counters. Cheap enough (one relaxed atomic
// add) to leave armed in release builds, which is the point: they answer "how
// many times did this actually run?" from a real session, where a sampling
// profiler only says "this was hot".
//
// The id and its wire name are declared once, here. They used to live in two
// parallel lists -- an enum here and a positionally-indexed name table in the
// .cpp -- so inserting an id without inserting its name at the same position
// still compiled and silently relabelled every counter after that point,
// attributing one subsystem's numbers to another. A test existed only to catch
// that; the X-macro removes the failure mode instead of guarding it.
//
// Naming: "<subsystem>.<event>". Counters ending in a plural noun count that
// noun (lines, bytes, cells); everything else counts calls or events.
#define MICROBROWSER_PERF_COUNTERS(X)                                             \
  /* --- event loop ------------------------------------------------------- */   \
  X(LoopIterations, "loop.iterations")                                           \
  X(LoopBlockingWaits, "loop.blocking_waits")                                    \
  X(LoopTimedWaits, "loop.timed_waits")                                          \
  X(LoopPolls, "loop.polls")                                                     \
  X(LoopEventsProcessed, "loop.events_processed")                                \
  X(LoopEventDrainYields, "loop.event_drain_yields")                             \
  /* --- frame / present --------------------------------------------------- */  \
  X(FramesPresented, "frame.presented")                                          \
  X(FramesFullRepaint, "frame.full_repaint")                                     \
  X(FramesPartialRepaint, "frame.partial_repaint")                               \
  X(FrameDirtyRects, "frame.dirty_rects")                                        \
  X(FrameDirtyPixels, "frame.dirty_pixels")                                      \
  X(FrameTexturePixelsUploaded, "frame.texture_pixels_uploaded")                 \
  /* --- gfx rasterization -------------------------------------------------- */ \
  X(GfxFillRectCalls, "gfx.fill_rect_calls")                                     \
  X(GfxFillRectPixels, "gfx.fill_rect_pixels")                                   \
  X(GfxClipPushes, "gfx.clip_pushes")                                            \
  X(GfxOpaqueFills, "gfx.opaque_fills")                                          \
  X(GfxBlendedFills, "gfx.blended_fills")                                        \
  X(GfxBlendedPixels, "gfx.blended_pixels")                                      \
  X(GfxMaskPixels, "gfx.mask_pixels")                                            \
  X(GfxPathFills, "gfx.path_fills")                                              \
  X(GfxPathSegments, "gfx.path_segments")                                        \
  X(GfxPathCells, "gfx.path_cells")                                              \
  X(GfxPathSpans, "gfx.path_spans")                                              \
  X(GfxPathSpanPixels, "gfx.path_span_pixels")                                   \
  X(GfxPathNonFiniteRejections, "gfx.path_non_finite_rejections")                \
  X(GfxStrokes, "gfx.strokes")                                                   \
  X(GfxStrokePieces, "gfx.stroke_pieces")                                        \
  X(GfxStrokeMiterFallbacks, "gfx.stroke_miter_fallbacks")                       \
  /* --- text --------------------------------------------------------------- */ \
  X(GfxFontsLoaded, "gfx.fonts_loaded")                                          \
  X(GfxFontLoadFailures, "gfx.font_load_failures")                               \
  X(GfxGlyphOutlines, "gfx.glyph_outlines")                                      \
  X(GfxTextShapes, "gfx.text_shapes")                                            \
  X(GfxShapedGlyphs, "gfx.shaped_glyphs")                                        \
  X(GfxGlyphsDrawn, "gfx.glyphs_drawn")                                          \
  X(GfxGlyphsRasterized, "gfx.glyphs_rasterized")                                \
  X(GfxGlyphCacheHits, "gfx.glyph_cache_hits")                                   \
  X(GfxGlyphCacheMisses, "gfx.glyph_cache_misses")                               \
  X(GfxGlyphCacheEvictions, "gfx.glyph_cache_evictions")                         \
  X(GfxPngDecodes, "gfx.png_decodes")                                            \
  X(GfxPngDecodeFailures, "gfx.png_decode_failures")                             \
  X(GfxPngPixelsDecoded, "gfx.png_pixels_decoded")                               \
  X(GfxJpegDecodes, "gfx.jpeg_decodes")                                          \
  X(GfxJpegDecodeFailures, "gfx.jpeg_decode_failures")                           \
  X(GfxJpegPixelsDecoded, "gfx.jpeg_pixels_decoded")                             \
  X(GfxImagesDrawn, "gfx.images_drawn")                                          \
  /* --- url ----------------------------------------------------------------- */ \
  X(UrlParses, "url.parses")                                                     \
  X(UrlParseFailures, "url.parse_failures")                                      \
  /* --- privacy ------------------------------------------------------------- */ \
  X(PrivacyDecisions, "privacy.decisions")                                       \
  X(PrivacyRequestsMatched, "privacy.requests_matched")                          \
  X(PrivacyRequestsBlocked, "privacy.requests_blocked")                          \
  X(PrivacyRulesProbed, "privacy.rules_probed")                                  \
  X(PrivacyHttpsUpgrades, "privacy.https_upgrades")                              \
  X(PrivacyUrlsSanitized, "privacy.urls_sanitized")                              \
  /* --- net ----------------------------------------------------------------- */ \
  X(NetResponsesParsed, "net.responses_parsed")                                  \
  X(NetResponseParseFailures, "net.response_parse_failures")                     \
  X(NetHeaderRejections, "net.header_rejections")                                \
  X(NetCookiesStored, "net.cookies_stored")                                      \
  X(NetCookiesRejected, "net.cookies_rejected")                                  \
  X(NetFetches, "net.fetches")                                                   \
  X(NetFetchFailures, "net.fetch_failures")                                      \
  X(NetRedirects, "net.redirects")                                               \
  X(NetCacheHits, "net.cache_hits")                                              \
  X(NetCacheMisses, "net.cache_misses")                                          \
  X(NetCacheStale, "net.cache_stale")                                            \
  X(NetCacheStores, "net.cache_stores")                                          \
  X(NetConnections, "net.connections")                                           \
  X(NetConnectFailures, "net.connect_failures")                                  \
  /* Blocking `getaddrinfo` calls. The one call in the network stack that      */ \
  /* stops the loop, so this counts stalls rather than work -- see the         */ \
  /* `net::Resolve` scope beside it. `hits` is the cache answering instead,    */ \
  /* and the pair is deliberately on the *expensive* half: a hit counter       */ \
  /* alone would read as health while every miss stalled the loop.             */ \
  X(NetHostResolves, "net.host_resolves")                                        \
  X(NetResolverCacheHits, "net.resolver_cache_hits")                             \
  X(NetResolverCacheMisses, "net.resolver_cache_misses")                         \
  X(NetTlsHandshakes, "net.tls_handshakes")                                      \
  X(NetTlsFailures, "net.tls_failures")                                          \
  X(NetRequestsStarted, "net.requests_started")                                  \
  X(NetRequestsDeferred, "net.requests_deferred")                                \
  X(NetRequestTimeouts, "net.request_timeouts")                                  \
  X(NetBytesReceived, "net.bytes_received")                                      \
  X(NetBytesCoded, "net.bytes_coded")                                            \
  X(NetBytesDecoded, "net.bytes_decoded")                                        \
  X(NetContentDecodeFailures, "net.content_decode_failures")                     \
  X(NetConnectionsReused, "net.connections_reused")                              \
  X(NetConnectionsPooled, "net.connections_pooled")                              \
  X(NetConnectionsClosedIdle, "net.connections_closed_idle")                     \
  /* --- CORS (ADR 0020 §2). A response refused here was discarded inside net  */ \
  /* rather than delivered and hidden, so `blocked` is a count of              */ \
  /* cross-origin reads that did not happen. Preflights are counted apart      */ \
  /* from the requests they authorise: a page whose every request costs an     */ \
  /* OPTIONS is a page making twice as many round trips as it looks like.      */ \
  X(NetCorsBlocked, "net.cors_blocked")                                          \
  X(NetCorsOpaque, "net.cors_opaque")                                            \
  X(NetCorsPreflights, "net.cors_preflights")                                    \
  X(NetCorsPreflightsCached, "net.cors_preflights_cached")                       \
  /* --- HTTP/2 (ADR 0032). The pair that says whether multiplexing is         */ \
  /* happening at all is `h2_sessions` against `h2_streams`: one session       */ \
  /* carrying nineteen streams is the whole point, and nineteen sessions       */ \
  /* carrying one stream each is the HTTP/1.1 burst wearing a new protocol.    */ \
  /* `h2_connect_waits` is the coalescing working -- a request that did not    */ \
  /* open a second socket because one to the same origin was still deciding    */ \
  /* which protocol it spoke.                                                  */ \
  X(NetHttp2Sessions, "net.h2_sessions")                                         \
  X(NetHttp2Streams, "net.h2_streams")                                           \
  X(NetHttp2ConnectWaits, "net.h2_connect_waits")                                \
  X(NetHttp2FramesReceived, "net.h2_frames_received")                            \
  X(NetHttp2StreamResets, "net.h2_stream_resets")                                \
  X(NetHttp2GoAways, "net.h2_goaways")                                           \
  X(NetHttp2ProtocolErrors, "net.h2_protocol_errors")                            \
  X(NetHttp2WindowUpdates, "net.h2_window_updates")                              \
  /* HPACK, as the two halves of one ratio. On the wire against decoded, so    */ \
  /* the compression the protocol claims is a division rather than a belief;   */ \
  /* the failure count is beside them because a decoder that has failed has    */ \
  /* ended a connection, and that is a load event rather than a statistic.     */ \
  X(NetHpackBlockBytes, "net.hpack_block_bytes")                                 \
  X(NetHpackDecodedBytes, "net.hpack_decoded_bytes")                             \
  X(NetHpackFailures, "net.hpack_failures")                                      \
  /* --- html ---------------------------------------------------------------- */ \
  X(HtmlTokens, "html.tokens")                                                   \
  X(HtmlDocumentsParsed, "html.documents_parsed")                                \
  /* A fragment parse is the tree builder reached from *script*, with markup    */ \
  /* and a context element a page chose. Counted apart from a document parse    */ \
  /* because they are different risks and different costs: a feed that fills    */ \
  /* itself in runs hundreds of these on a page that parsed one document, and   */ \
  /* the bytes are what says whether `innerHTML` is where a page's time goes.   */ \
  X(HtmlFragmentsParsed, "html.fragments_parsed")                                \
  X(HtmlFragmentBytes, "html.fragment_bytes")                                    \
  X(HtmlFragmentNodes, "html.fragment_nodes")                                    \
  X(HtmlUnsupportedInsertionMode, "html.unsupported_insertion_mode")             \
  X(DomNodesCreated, "dom.nodes_created")                                        \
  /* --- css ----------------------------------------------------------------- */ \
  X(CssTokens, "css.tokens")                                                     \
  X(CssSheetsParsed, "css.sheets_parsed")                                        \
  X(CssRulesParsed, "css.rules_parsed")                                          \
  X(CssStylesResolved, "css.styles_resolved")                                    \
  /* Hits on the TD-0021 computed-style cache: same cascade generation,        */ \
  /* structure version, attr version, dynamic state, and parent style id.      */ \
  X(CssStyleCacheHits, "css.style_cache_hits")                                   \
  X(CssStyleCacheMisses, "css.style_cache_misses")                               \
  /* What one cascade actually does, per element. Counted on the expensive     */ \
  /* halves rather than on the cheap one: `styles_resolved` alone reads as     */ \
  /* health while a resolve costs fifty selector matches and a hundred string  */ \
  /* copies. `candidates_tested` counts full selector evaluations,             */ \
  /* `declarations_cascaded` counts declarations sorted and applied, and       */ \
  /* `declaration_values_copied` counts the ones whose text was duplicated     */ \
  /* only to be handed straight back unchanged.                                */ \
  X(CssCandidatesTested, "css.candidates_tested")                                \
  X(CssDeclarationsCascaded, "css.declarations_cascaded")                        \
  X(CssVarSubstitutions, "css.var_substitutions")                                 \
  X(CssInlineStyleParses, "css.inline_style_parses")                             \
  /* --- dynamic state and invalidation (ADR 0016) --------------------------- */ \
  /* How many rules in the cascade depend on a state a pointer or a keystroke  */ \
  /* can change. Zero is the common case and is what makes a mouse move free;  */ \
  /* the number against css.rules_parsed is what the index is actually worth   */ \
  /* on a given page.                                                          */ \
  X(CssDynamicRulesIndexed, "css.dynamic_rules_indexed")                         \
  X(CssSupportsQueries, "css.supports_queries")                                   \
  X(CssEscapeCalls, "css.escape_calls")                                           \
  /* A dynamic state bit that actually flipped on an element. Counted apart    */ \
  /* from the moves that flipped nothing, because a pointer crossing a page    */ \
  /* generates far more of the second than of the first.                       */ \
  X(StyleStateChanges, "style.state_changes")                                    \
  /* A state change the index answered `None` for: nothing was recomputed and  */ \
  /* nothing was drawn. This is the counter that says the headline property of */ \
  /* ADR 0016 is still holding -- if it stops moving on a page with no `:hover`*/ \
  /* rules, something has started restyling on every mouse move.               */ \
  X(StyleInvalidationSkips, "style.invalidation_skips")                          \
  /* The hit test a pointer move pays for, which happens only when some rule   */ \
  /* depends on `:hover` at all. It walks the box tree, so it is the one part  */ \
  /* of this path that grows with the page.                                    */ \
  X(StyleHoverHitTests, "style.hover_hit_tests")                                 \
  /* A cascade re-resolved over the *existing* box tree, because every rule    */ \
  /* keyed on what changed only affects paint. The pair to watch is this       */ \
  /* against layout.runs: a `:hover { color }` rule that starts moving         */ \
  /* layout.runs means the property table has lost an entry.                   */ \
  X(StyleRestylesWithoutLayout, "style.restyles_without_layout")                 \
  /* --- the view observers (ADR 0018 §5) ------------------------------------ */ \
  /* A frame at which observers were sampled at all. Zero on a page that has  */ \
  /* constructed none, which is the property that matters: the observers cost */ \
  /* one pointer comparison per frame until something asks for them.          */ \
  X(ViewObservationFrames, "view.observation_frames")                            \
  /* Targets measured, against records delivered. The ratio is what the whole */ \
  /* design is for -- an observer samples every target every frame and fires  */ \
  /* only when an answer changed, so records climbing with samples means      */ \
  /* something is reporting a change that is not one.                         */ \
  X(ViewIntersectionSamples, "view.intersection_samples")                        \
  X(ViewIntersectionRecords, "view.intersection_records")                        \
  X(ViewResizeSamples, "view.resize_samples")                                    \
  X(ViewResizeRecords, "view.resize_records")                                    \
  /* A frame where a resize callback resized something and the loop was cut   */ \
  /* off at the depth bound. Non-zero means a page is fighting itself and the */ \
  /* browser stopped it, which is a fact worth having a number for rather     */ \
  /* than a hang.                                                             */ \
  X(ViewResizeLoopLimit, "view.resize_loop_limit")                               \
  /* An `<img loading="lazy">` passed over at collection time, and one that    */ \
  /* later came within reach of the scrollport and was fetched. Read the first */ \
  /* as deferrals rather than as distinct images: collection re-runs whenever  */ \
  /* the document or its stylesheets change, so an image below the fold is     */ \
  /* counted once per collection. The second is per image, exactly once.       */ \
  /* Against net.requests_started they say what the feature is worth: on       */ \
  /* www.reddit.com, 29 requests and 26 images decoded became 13 and 10.       */ \
  X(EngineImagesDeferred, "engine.images_deferred")                              \
  X(EngineImagesRevealed, "engine.images_revealed")                              \
  /* --- fetch (ADR 0020 §1) -------------------------------------------------*/ \
  /* What a *page* asked the network for, as against what the browser asked    */ \
  /* for on its behalf. The pair to read is `requests` against `delivered`     */ \
  /* plus `failed` plus `aborted`: anything missing from that sum is a promise */ \
  /* nobody settled, which is a page that hangs with no error anywhere.        */ \
  X(FetchRequests, "fetch.requests")                                             \
  X(FetchDelivered, "fetch.delivered")                                           \
  X(FetchFailed, "fetch.failed")                                                 \
  X(FetchAborted, "fetch.aborted")                                               \
  /* The same four for `XMLHttpRequest`, counted apart from `fetch` even though */ \
  /* both go out through one path. Which API a page reaches for says what era   */ \
  /* its code is from, and on a page that hangs it is the first thing worth     */ \
  /* knowing -- an XHR nobody delivered has no rejected promise to notice.      */ \
  X(XhrRequests, "xhr.requests")                                                 \
  X(XhrDelivered, "xhr.delivered")                                               \
  X(XhrFailed, "xhr.failed")                                                     \
  X(XhrAborted, "xhr.aborted")                                                   \
  /* Session history. `origin_refusals` is the one to watch: it counts a page  */ \
  /* trying to move the URL bar to an origin that is not its own, which is the */ \
  /* shape every address-bar spoof is built from. ADR 0026 §2.                 */ \
  X(HistoryPushStates, "history.push_states")                                    \
  X(HistoryReplaceStates, "history.replace_states")                              \
  X(HistoryTraversals, "history.traversals")                                     \
  X(HistorySameDocumentTraversals, "history.same_document_traversals")            \
  X(HistoryOriginRefusals, "history.origin_refusals")                            \
  /* What a page measured about itself, and how often it was told. A page whose */ \
  /* observer never fires is one whose telemetry silently reports nothing --    */ \
  /* which is what four roadmap sessions were blocked by.                       */ \
  X(PerformanceEntries, "performance.entries")                                   \
  X(PerformanceObserverCallbacks, "performance.observer_callbacks")               \
  /* Why the bytecode compiler gave up on a program, when it does. A bailout is */ \
  /* not a bug -- the tree-walker takes the program -- but it is otherwise      */ \
  /* *invisible*, and the tree-walker refuses an async function at the call, so */ \
  /* the visible failure is a TypeError somewhere else naming neither the bound */ \
  /* nor the file. `js.compile_bailouts` above zero on a page that misbehaves   */ \
  /* is the first thing to look at.                                            */ \
  X(JsCompileBailouts, "js.compile_bailouts")                                     \
  X(JsCompileBailoutDepth, "js.compile_bailout_depth")                            \
  X(JsCompileBailoutInstructions, "js.compile_bailout_instructions")               \
  X(JsCompileBailoutSlots, "js.compile_bailout_slots")                            \
  /* The three scope reasons are apart from the bounds on purpose: a bound is a */ \
  /* decision and `unreserved` and `arithmetic` are *defects* in the compiler.  */ \
  X(JsCompileBailoutCaptured, "js.compile_bailout_captured")                      \
  X(JsCompileBailoutUnreserved, "js.compile_bailout_unreserved")                   \
  X(JsCompileBailoutArithmetic, "js.compile_bailout_arithmetic")                    \
  /* Source bytes compiled and instructions emitted, across every program. The */ \
  /* ratio between them is what the instruction bound is expressed in, so this */ \
  /* is the measurement that says whether the bound is anywhere near a real    */ \
  /* page: youtube's kevlar bundle is 10.7MB and was refused by a flat cap.    */ \
  X(JsCompiledSourceBytes, "js.compiled_source_bytes")                            \
  X(JsCompiledInstructions, "js.compiled_instructions")                           \
  /* Peak steps_ inside one host turn, and how often the hang budget threw. A  */ \
  /* non-zero `js.steps_exhausted` on youtube.com meant custom-element         */ \
  /* reactions nested under kevlar shared one 20M budget and aborted mid-      */ \
  /* stamp — browse `__data` arrived, `ytd-rich-grid-renderer` never did.     */ \
  X(JsStepsPeak, "js.steps_peak")                                                 \
  X(JsStepsExhausted, "js.steps_exhausted")                                       \
  /* Engine-built throws (`MakeError`), including ones a page's own try/catch  */ \
  /* swallows. youtube's setmediasrc path catches Gal's throw and turns it into */ \
  /* `fmt.unplayable` without ever reaching ReportUncaught -- so the only way  */ \
  /* to see the *original* name/message is to count and (with                */ \
  /* MICROBROWSER_JS_THROWS=1) log here.                                      */ \
  X(JsThrows, "js.throws")                                                       \
  /* Hits on the hard heap cell limit. A non-zero count that coincides with an */ \
  /* `out of memory` throw means the live set does not fit, or safepoints are  */ \
  /* not collecting -- the youtube Polymer-upgrade OOM of 2026-08-06 was the   */ \
  /* second: CallFunction raised call_depth_ around CallCompiled and every     */ \
  /* safepoint became a no-op for the duration of a custom-element reaction.   */ \
  X(JsHeapOom, "js.heap_oom")                                                     \
  /* Peak live cells (objects + environments) observed at a successful         */ \
  /* collection. The number to read against the hard limit when a page gets    */ \
  /* close: a peak near the limit with heap_oom still zero is headroom left.   */ \
  X(JsHeapLivePeak, "js.heap_live_peak")                                          \
  /* The module graph. `dynamic_imports` against `dynamic_imports_settled` is   */ \
  /* the pair to read: the difference is a page waiting on a promise nobody     */ \
  /* answered, which is a page that hangs with no error anywhere.               */ \
  X(JsModulesLoaded, "js.modules_loaded")                                         \
  X(JsModuleFetches, "js.module_fetches")                                         \
  X(JsDynamicImports, "js.dynamic_imports")                                       \
  X(JsDynamicImportsSettled, "js.dynamic_imports_settled")                        \
  X(JsDynamicImportsRefused, "js.dynamic_imports_refused")                        \
  /* Shadow trees. `slot_changes` is the one to watch on a page that renders    */ \
  /* the wrong thing inside a custom element: a slot whose assignment keeps     */ \
  /* changing is a component being rebuilt rather than updated.                */ \
  X(DomShadowRootsAttached, "dom.shadow_roots_attached")                          \
  X(DomSlotChanges, "dom.slot_changes")                                          \
  /* Custom elements. An upgrade that ran and an element that ended up an       */ \
  /* instance of its own class are different facts, and the gap between these   */ \
  /* two counters is the second one failing. Worth counting rather than         */ \
  /* asserting because the failure is invisible from the page: the element is   */ \
  /* in the tree, it has a prototype, its methods are simply somebody else's.   */ \
  X(DomCustomElementUpgrades, "dom.custom_element_upgrades")                      \
  X(DomCustomElementConstructorThrows, "dom.custom_element_constructor_throws")   \
  /* An upgrade whose class had no usable `prototype` to apply, so the element  */ \
  /* kept whatever the construction left on it. Above zero means every          */ \
  /* component of that class renders as a plain element.                       */ \
  X(DomCustomElementPrototypeMissing, "dom.custom_element_prototype_missing")     \
  /* Own data properties written *before* upgrade that shadow a prototype       */ \
  /* accessor, re-applied through the setter after construction. youtube's     */ \
  /* monomer `yt-button-shape` queues on `data` until the setter fills          */ \
  /* `rawProps`; an own `data` from a pre-upgrade assignment skips that path   */ \
  /* and leaves Accept/Reject unstamped (consent dialog).                      */ \
  X(DomCustomElementPreupgradePropsReapplied,                                    \
    "dom.custom_element_preupgrade_props_reapplied")                             \
  /* Inserts into `template.content` that skipped custom-element upgrade.       */ \
  /* Should rise with youtube Polymer `_template` fills; zero with bindings     */ \
  /* still missing means the inert guard is not on the path that builds them.   */ \
  X(DomTemplateContentUpgradeSkips, "dom.template_content_upgrade_skips")         \
  /* Web fonts. `refused` above zero means a face this browser cannot decode -- */ \
  /* a WOFF2 until ADR 0024's brotli lands -- and the page rendered in the next */ \
  /* family of its stack, which is what a stack is for.                        */ \
  X(GfxWebFontsRegistered, "gfx.web_fonts_registered")                            \
  X(GfxWebFontsRefused, "gfx.web_fonts_refused")                                 \
  X(GfxWebFontsOutOfRange, "gfx.web_fonts_out_of_range")                          \
  /* Brotli. `refusals` counts a stream that was malformed, truncated, or would */ \
  /* have expanded past the ceiling -- and unlike gzip there is no declared     */ \
  /* output size to refuse from, so the ceiling is the only defence there is.   */ \
  X(UtilBrotliStreams, "util.brotli_streams")                                     \
  X(UtilBrotliBytesProduced, "util.brotli_bytes_produced")                        \
  X(UtilBrotliRefusals, "util.brotli_refusals")                                   \
  /* WOFF2. `transformed_refusals` is the honest limit rather than a failure: a  */ \
  /* transformed `glyf` needs every outline rebuilt from parallel substreams,    */ \
  /* and a half-reconstruction is mangled glyphs rather than a refusal.          */ \
  X(GfxSfntRefusals, "gfx.sfnt_refusals")                                         \
  X(GfxSfntOverlapRefusals, "gfx.sfnt_overlap_refusals")                          \
  X(GfxWoff2Decoded, "gfx.woff2_decoded")                                         \
  X(GfxWoff2GlyfReconstructed, "gfx.woff2_glyf_reconstructed")                     \
  X(GfxWoff2Refusals, "gfx.woff2_refusals")                                       \
  X(GfxWoff2TransformedRefusals, "gfx.woff2_transformed_refusals")                 \
  X(JsCompileBailoutNode, "js.compile_bailout_node")                              \
  /* The page's own policy, and the resource integrity it asked for. Both are  */ \
  /* enforced silently by design -- nothing is reported to a server, ADR 0020  */ \
  /* §3 -- so these counters are the only signal that a page's policy is doing */ \
  /* anything at all. `csp.violations` on a page that renders wrong is the     */ \
  /* first thing to look at, and `sri.mismatches` above zero means a resource  */ \
  /* the site itself does not trust was served to it.                          */ \
  X(CspPolicies, "csp.policies")                                                 \
  X(CspViolations, "csp.violations")                                             \
  X(CspInlineBlocked, "csp.inline_blocked")                                      \
  X(SriChecks, "sri.checks")                                                     \
  X(TextBreakOpportunities, "text.break_opportunities")                           \
  X(TextBidiLines, "text.bidi_lines")                                             \
  X(TextBidiRuns, "text.bidi_runs")                                               \
  X(TextMirroredRuns, "text.mirrored_runs")                                       \
  X(EncodingReplacements, "encoding.replacements")                                 \
  X(EncodingFromPrescan, "encoding.from_prescan")                                 \
  X(EncodingFellBackToWindows1252, "encoding.fell_back_to_windows1252")            \
  X(EncodingTextEncoderConstructed, "encoding.text_encoder_constructed")           \
  X(EncodingTextEncoderEncode, "encoding.text_encoder_encode")                     \
  X(EncodingTextEncoderBytes, "encoding.text_encoder_bytes")                       \
  X(EncodingTextDecoderConstructed, "encoding.text_decoder_constructed")           \
  X(EncodingTextDecoderDecode, "encoding.text_decoder_decode")                     \
  X(EncodingTextDecoderBytes, "encoding.text_decoder_bytes")                       \
  X(EncodingBtoa, "encoding.btoa")                                                 \
  X(EncodingAtob, "encoding.atob")                                                 \
  X(CryptoSubtleInstalled, "crypto.subtle_installed")                              \
  X(CryptoSubtleImportKey, "crypto.subtle_import_key")                              \
  X(CryptoSubtleEncrypt, "crypto.subtle_encrypt")                                   \
  X(CryptoSubtleEncryptBytes, "crypto.subtle_encrypt_bytes")                        \
  X(CryptoSubtleSign, "crypto.subtle_sign")                                         \
  X(MatroskaFilesParsed, "matroska.files_parsed")                                 \
  X(MatroskaRefusals, "matroska.refusals")                                        \
  X(HlsPlaylistsParsed, "hls.playlists_parsed")                                    \
  X(HlsEmptyPlaylists, "hls.empty_playlists")                                     \
  X(HlsRefusals, "hls.refusals")                                                  \
  X(MediaLoadsStarted, "media.loads_started")                                      \
  X(MediaLoadFailures, "media.load_failures")                                     \
  X(MediaAutoplayRefusals, "media.autoplay_refusals")                             \
  X(MediaErrorEvents, "media.error_events")                                       \
  X(MediaSourceAppends, "media.source_appends")                                   \
  X(MediaSourceAppendFailures, "media.source_append_failures")                    \
  X(MediaSourceQuotaRefusals, "media.source_quota_refusals")                      \
  X(MediaSourceInitSegments, "media.source_init_segments")                        \
  X(MediaSourceFramesBuffered, "media.source_frames_buffered")                    \
  X(MediaSourceSegmentsEvicted, "media.source_segments_evicted")                  \
  X(MediaSourceBuffersCreated, "media.source_buffers_created")                    \
  X(MediaObjectUrlsCreated, "media.object_urls_created")                          \
  X(MediaVideoSessions, "media.video_sessions")                                   \
  X(MediaVideoConfigureFailures, "media.video_configure_failures")                \
  X(MediaDecoderSamplesFed, "media.decoder_samples_fed")                          \
  X(MediaDecoderFrames, "media.decoder_frames")                                   \
  X(MediaDecoderFramesApplied, "media.decoder_frames_applied")                    \
  X(MediaAudioFramesWritten, "media.audio_frames_written")                        \
  X(MediaDecoderErrors, "media.decoder_errors")                                   \
  X(MpegTsPacketsParsed, "media.mpegts_packets")                                  \
  X(MpegTsSamples, "media.mpegts_samples")                                        \
  X(MpegTsResyncs, "media.mpegts_resyncs")                                        \
  X(TransitionsStarted, "animation.transitions_started")                          \
  X(TransitionsFinished, "animation.transitions_finished")                        \
  X(AnimationsStarted, "animation.animations_started")                            \
  X(AnimationsFinished, "animation.animations_finished")                          \
  X(WaapiAnimationsStarted, "animation.waapi_started")                            \
  X(WaapiAnimationsFinished, "animation.waapi_finished")                          \
  X(WaapiAnimationsCancelled, "animation.waapi_cancelled")                        \
  X(AnimationFramesProduced, "animation.frames_produced")                         \
  X(IdleCallbacksRun, "idle.callbacks_run")                                       \
  X(CanvasesCreated, "canvas.created")                                            \
  X(CanvasDraws, "canvas.draws")                                                  \
  X(CanvasSizeRefusals, "canvas.size_refusals")                                   \
  X(CanvasReadbacks, "canvas.readbacks")                                          \
  X(CanvasesTainted, "canvas.tainted")                                            \
  X(WorkersStarted, "worker.started")                                             \
  X(WorkersTerminated, "worker.terminated")                                       \
  X(WorkerRefusals, "worker.refusals")                                            \
  X(WorkerMessagesHandled, "worker.messages_handled")                             \
  X(WorkerMessagesDropped, "worker.messages_dropped")                             \
  X(AudioDevicesOpened, "audio.devices_opened")                                    \
  X(AudioDeviceUnavailable, "audio.device_unavailable")                           \
  X(EventSourceConnectionsOpened, "eventsource.connections_opened")                \
  X(EventSourceReconnects, "eventsource.reconnects")                              \
  X(EventSourceGaveUp, "eventsource.gave_up")                                     \
  X(EventSourceRefusals, "eventsource.refusals")                                  \
  X(EventStreamEvents, "eventstream.events")                                      \
  X(EventStreamOversizeDrops, "eventstream.oversize_drops")                       \
  X(WebSocketInsecureRefusals, "websocket.insecure_refusals")                     \
  X(WebSocketConnectionsOpened, "websocket.connections_opened")                    \
  X(WebSocketHandshakeRefusals, "websocket.handshake_refusals")                    \
  X(WebSocketFailures, "websocket.failures")                                       \
  X(WebSocketPongsSent, "websocket.pongs_sent")                                    \
  X(WebSocketFramesReceived, "websocket.frames_received")                         \
  X(WebSocketFramesSent, "websocket.frames_sent")                                 \
  X(WebSocketProtocolErrors, "websocket.protocol_errors")                         \
  X(WebSocketOversizeRefusals, "websocket.oversize_refusals")                     \
  X(StorageLookups, "storage.lookups")                                            \
  X(StorageWrites, "storage.writes")                                              \
  X(StorageQuotaRefusals, "storage.quota_refusals")                               \
  X(StoragePartitionsCreated, "storage.partitions_created")                       \
  X(SriMismatches, "sri.mismatches")                                            \
  X(SriUnparseable, "sri.unparseable")                                           \
  X(EngineStyleSheetsLoaded, "engine.stylesheets_loaded")                          \
  X(EngineStyleSheetsFailed, "engine.stylesheets_failed")                          \
  X(EngineScriptsLoaded, "engine.scripts_loaded")                                  \
  X(EngineScriptsFailed, "engine.scripts_failed")                                  \
  X(EngineImagesLoaded, "engine.images_loaded")                                    \
  X(EngineImagesFailed, "engine.images_failed")                                    \
  X(EngineImagesDrawn, "engine.images_drawn")                                      \
  X(BoxTreeBuildSkipped, "engine.box_tree_build_skipped")                          \
  /* Why the box tree was dropped. Script turns that mutate nothing used to     */ \
  /* invalidate unconditionally; hits here vs layout.passes show whether the    */ \
  /* stamp storm is real DOM work or wasted rebuilds.                           */ \
  X(BoxTreeInvalidatedByScript, "engine.box_tree_invalidated_by_script")           \
  X(BoxTreeScriptSkipped, "engine.box_tree_script_skipped")                        \
  X(BoxTreeInvalidatedByImage, "engine.box_tree_invalidated_by_image")             \
  X(BoxTreeImagePaintOnly, "engine.box_tree_image_paint_only")                    \
  X(BoxTreeInvalidatedByFont, "engine.box_tree_invalidated_by_font")               \
  X(BoxTreeInvalidatedByDueWork, "engine.box_tree_invalidated_by_due_work")       \
  X(BoxTreeInvalidatedBySheet, "engine.box_tree_invalidated_by_sheet")           \
  X(EnginePaintsSkipped, "engine.paints_skipped")                                \
  X(GfxImagesTransformed, "gfx.images_transformed")                               \
  X(GfxImagesScaled, "gfx.images_scaled")                                         \
  X(DamageDiffs, "paint.damage_diffs")                                           \
  X(DamageDiffsIdentical, "paint.damage_diffs_identical")                        \
  X(DamageDiffsFullRepaint, "paint.damage_diffs_full_repaint")                   \
  X(DamageRectsProduced, "paint.damage_rects_produced")                          \
  /* --- text ---------------------------------------------------------------- */ \
  X(TextRunsSplitByCoverage, "text.runs_split_by_coverage")                       \
  X(FontFallbacks, "font.fallbacks")                                              \
  X(FontFacesRegistered, "font.faces_registered")                                \
  X(FontLookupHits, "font.lookup_hits")                                          \
  X(FontLookupMisses, "font.lookup_misses")                                      \
  /* Which *face* a font stack settles on, as opposed to which sized Font --    */ \
  /* `lookup_hits` counts the second and was hiding the first. Choosing a face  */ \
  /* is a pass over every registered face, and `faces_ranked` is the product    */ \
  /* that pass actually costs: on wikipedia it was 985,000 queries against 490  */ \
  /* faces, which is where 227 of that page's 259 seconds went. Watch           */ \
  /* faces_ranked rather than the query count -- a page with two faces and a    */ \
  /* page with five hundred ask the same number of times.                      */ \
  X(FontMatchQueries, "font.match_queries")                                      \
  X(FontMatchCacheHits, "font.match_cache_hits")                                 \
  X(FontMatchCacheMisses, "font.match_cache_misses")                             \
  X(FontFacesRanked, "font.faces_ranked")                                        \
  /* And the layer above: resolving a request against the *machine's* fonts,   */ \
  /* which is three passes -- every font file on disk, every loaded face, then */ \
  /* the catalog's own match. `resolve_cache_misses` is how many of those      */ \
  /* triples a page actually costs; it should be the number of distinct font   */ \
  /* stacks on the page, and nothing like the number of text runs.             */ \
  X(FontResolveCacheHits, "font.resolve_cache_hits")                             \
  X(FontResolveCacheMisses, "font.resolve_cache_misses")                         \
  X(ShapedRunCacheHits, "text.shaped_run_cache_hits")                            \
  X(ShapedRunCacheMisses, "text.shaped_run_cache_misses")                        \
  X(TextRunsPainted, "text.runs_painted")                                        \
  /* --- layout -------------------------------------------------------------- */ \
  X(LayoutTreeBuilds, "layout.tree_builds")                                      \
  X(LayoutBoxesCreated, "layout.boxes_created")                                  \
  X(LayoutRuns, "layout.runs")                                                   \
  /* One entry into Page::Layout -- the unit TD-0013 compares against            */ \
  /* layout.forced_by_script and layout.pass_boxes.                              */ \
  X(LayoutPasses, "layout.passes")                                               \
  /* LayoutAndPaint when the box tree and width already match. Without this,     */ \
  /* every rAF on a settled page reflowed (TD-0018 / youtube post-load storm).   */ \
  X(LayoutSkippedClean, "layout.skipped_clean")                                  \
  /* RunDueWork returned true but the document/cascade did not move, so the box  */ \
  /* tree was left standing. The counter that says MessageChannel/rAF churn is   */ \
  /* no longer paying InvalidateLayout per turn (TD-0018).                       */ \
  X(LayoutDueWorkClean, "layout.due_work_clean")                                 \
  X(LayoutAnimationTick, "layout.animation_tick_no_box_rebuild")                 \
  X(LayoutAnimationPaintOnly, "layout.animation_paint_only")                     \
  X(LayoutAttrPaintOnly, "layout.attr_paint_only")                               \
  X(LayoutVideoPaintOnly, "layout.video_paint_only")                             \
  /* MessageChannel (host) tasks run inside one TimerQueue::RunDue after the     */ \
  /* initial due set. Cooperative schedulers post one slice per task; without    */ \
  /* this drain each slice forced its own LayoutAndPaint.                        */ \
  X(TimersHostTasksBatched, "timers.host_tasks_batched")                         \
  /* Boxes in the tree after each pass, summed across passes. Dividing by        */ \
  /* layout.passes gives the tree size a hang is chewing on.                     */ \
  X(LayoutPassBoxes, "layout.pass_boxes")                                        \
  /* A layout that ran in the middle of a script turn, because the page asked  */ \
  /* a geometry question the box tree could no longer answer. ADR 0015 makes   */ \
  /* this visible rather than cheap: a write-then-read loop can make the       */ \
  /* browser do unbounded work, and the count going up per iteration is the    */ \
  /* only way to tell that page apart from a slow one.                         */ \
  X(LayoutForcedByScript, "layout.forced_by_script")                             \
  /* Element→box map lookups from geometry queries. Hits should track lookups  */ \
  /* once the index is built; the O(boxes) walk is gone (Gate C stamp depth).  */ \
  X(LayoutBoxLookups, "layout.box_lookups")                                      \
  X(LayoutBoxLookupHits, "layout.box_lookup_hits")                               \
  X(LayoutDisplayListsBuilt, "layout.display_lists_built")                       \
  /* Every entry into LayoutBlock, against the boxes that exist. This is the    */ \
  /* ratio that matters and the one nothing was reporting: a layout algorithm   */ \
  /* that measures a subtree and then places it walks that subtree twice, and   */ \
  /* two such algorithms nested inside one another walk it four times. On       */ \
  /* youtube.com the ratio was 1200:1 -- a box laid out twelve hundred times    */ \
  /* -- and every scope-level timing said only "layout is slow".                */ \
  X(LayoutBlockPasses, "layout.block_passes")                                    \
  /* Subtree translations that replaced a second LayoutBlock (TD-0001): flex    */ \
  /* place with unchanged forced sizes, float place after probe, and atomic     */ \
  /* inline place-on-line. Hits are the walks we no longer do; misses are the   */ \
  /* stretch / constraint-change cases that still re-lay out.                   */ \
  X(LayoutMeasureCacheHits, "layout.measure_cache_hits")                         \
  X(LayoutMeasureCacheMisses, "layout.measure_cache_misses")                     \
  /* Text handed to the shaper by layout rather than by paint. Intrinsic width  */ \
  /* measurement is the other half of a slow layout and is invisible from the   */ \
  /* box counters, which count boxes rather than the work each one asks for.    */ \
  X(LayoutTextMeasurements, "layout.text_measurements")                          \
  /* --- compression --------------------------------------------------------- */ \
  X(UtilInflateCalls, "util.inflate_calls")                                      \
  X(UtilInflateBytes, "util.inflate_bytes")                                      \
  X(UtilInflateChecksumFailures, "util.inflate_checksum_failures")               \
  /* --- display list ------------------------------------------------------- */ \
  X(DisplayListBuilds, "display_list.builds")                                    \
  X(DisplayListCommands, "display_list.commands")                                \
  X(DisplayListExecutions, "display_list.executions")                            \
  /* --- ipc seam ----------------------------------------------------------- */ \
  X(IpcMessagesSent, "ipc.messages_sent")                                        \
  X(IpcMessagesReceived, "ipc.messages_received")                                \
  X(IpcBytesSerialized, "ipc.bytes_serialized")                                  \
  X(IpcBytesDeserialized, "ipc.bytes_deserialized")                              \
  /* --- engine ------------------------------------------------------------- */ \
  X(EngineNavigations, "engine.navigations")                                     \
  /* A navigation a page asked for rather than the user: a form a script       */ \
  /* submitted. Counted apart because it is the one kind that starts with the  */ \
  /* interpreter still on the stack, and the count going up while the          */ \
  /* navigation count does not is the shape of a page that asked and was       */ \
  /* refused.                                                                  */ \
  X(EngineScriptNavigations, "engine.script_navigations")                        \
  /* The two lifecycle events, so a page that hangs waiting for one can be     */ \
  /* told apart from one that heard it and did nothing.                        */ \
  X(EngineDomContentLoaded, "engine.dom_content_loaded")                         \
  X(EngineLoadEvents, "engine.load_events")                                      \
  X(EngineFormSubmissions, "engine.form_submissions")                            \
  X(EnginePaintsProduced, "engine.paints_produced")                              \
  /* --- focus --------------------------------------------------------------- */ \
  /* Focus is the input router, so these answer "where did the keys go" from a */ \
  /* real session rather than from a guess. Moves are counted apart from Tab   */ \
  /* because a page's own `focus()` calls and the user's are the same event    */ \
  /* here and are not the same problem: a page that focuses something on every */ \
  /* keystroke is a page fighting the user for the caret.                      */ \
  X(FocusMoves, "focus.moves")                                                   \
  /* The Tab walk is the one part of the focus model whose cost grows with the */ \
  /* document: it collects every tab-reachable element and sorts them, once    */ \
  /* per Tab, and each candidate asks its ancestors about `hidden`. Nothing    */ \
  /* caches it, because the answer changes whenever the tree or an attribute   */ \
  /* does and a stale tab order sends a keystroke to the wrong element. These  */ \
  /* two are how a next session decides whether an index is worth its          */ \
  /* invalidation: candidates/walks is the average document's answer.          */ \
  X(FocusTabWalks, "focus.tab_walks")                                            \
  X(FocusTabCandidates, "focus.tab_candidates")                                 \
  /* --- BroadcastChannel (ADR 0038) ----------------------------------------- */ \
  /* Constructed apart from delivered: a page that opens a channel and never   */ \
  /* sees `delivered` move is either alone in its name or posting before the   */ \
  /* other end attached a listener.                                           */ \
  X(BroadcastChannelsConstructed, "broadcast_channel.constructed")               \
  X(BroadcastChannelMessagesPosted, "broadcast_channel.messages_posted")         \
  X(BroadcastChannelMessagesDelivered, "broadcast_channel.messages_delivered")   \
  X(BroadcastChannelClosed, "broadcast_channel.closed")                          \
  /* --- IndexedDB (ADR 0038) ------------------------------------------------- */ \
  /* `opens` against `upgrades` says how much of an `open()` a page's own       */ \
  /* `onupgradeneeded` actually costs -- youtube's Woffle path fires one         */ \
  /* upgrade and then reopens at that version forever.                          */ \
  X(IdbOpens, "idb.opens")                                                       \
  X(IdbUpgrades, "idb.upgrades")                                                 \
  X(IdbPuts, "idb.puts")                                                        \
  X(IdbGets, "idb.gets")                                                        \
  X(IdbDeletes, "idb.deletes")                                                  \
  X(IdbCursorQueries, "idb.cursor_queries")                                     \
  X(IdbConstraintErrors, "idb.constraint_errors")                               \
  X(IdbQuotaRefusals, "idb.quota_refusals")                                     \
  X(IdbPartitionsCreated, "idb.partitions_created")

enum class PerfCounterId : std::size_t {
#define MICROBROWSER_PERF_COUNTER_ENUM(id, name) id,
  MICROBROWSER_PERF_COUNTERS(MICROBROWSER_PERF_COUNTER_ENUM)
#undef MICROBROWSER_PERF_COUNTER_ENUM
      Count,
};

constexpr std::size_t kPerfCounterCount = static_cast<std::size_t>(PerfCounterId::Count);

using PerfCounterSnapshot = std::array<std::uint64_t, kPerfCounterCount>;

void ResetPerformanceCounters();
void AddPerformanceCounter(PerfCounterId id, std::uint64_t delta = 1);
// Raise the counter to `value` when that is larger than what it holds. For
// peaks (live heap cells, watermark sizes) rather than event counts.
void MaxPerformanceCounter(PerfCounterId id, std::uint64_t value);
std::uint64_t ReadPerformanceCounter(PerfCounterId id);
PerfCounterSnapshot CapturePerformanceCounters();
std::vector<std::pair<std::string_view, std::uint64_t>> NonZeroCounterDelta(
    const PerfCounterSnapshot& before,
    const PerfCounterSnapshot& after);
std::string_view PerformanceCounterName(PerfCounterId id);

// Write every non-zero counter to `out`, sorted by name. This is the live-app
// readout: before it existed the counters could only be observed from the perf
// harness, so a real session's numbers were unreachable.
void WritePerformanceCounters(std::FILE* out);

// True when MICROBROWSER_PERF_COUNTERS is set. Callers use it to arm a shutdown dump.
bool PerformanceCounterDumpRequested();

// Idempotent shutdown dump, gated on PerformanceCounterDumpRequested().
void DumpPerformanceCountersOnce();

}  // namespace microbrowser::util
