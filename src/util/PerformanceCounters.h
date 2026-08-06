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
  /* --- dynamic state and invalidation (ADR 0016) --------------------------- */ \
  /* How many rules in the cascade depend on a state a pointer or a keystroke  */ \
  /* can change. Zero is the common case and is what makes a mouse move free;  */ \
  /* the number against css.rules_parsed is what the index is actually worth   */ \
  /* on a given page.                                                          */ \
  X(CssDynamicRulesIndexed, "css.dynamic_rules_indexed")                         \
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
  X(MatroskaFilesParsed, "matroska.files_parsed")                                 \
  X(MatroskaRefusals, "matroska.refusals")                                        \
  X(HlsPlaylistsParsed, "hls.playlists_parsed")                                    \
  X(HlsEmptyPlaylists, "hls.empty_playlists")                                     \
  X(HlsRefusals, "hls.refusals")                                                  \
  X(MediaLoadsStarted, "media.loads_started")                                      \
  X(MediaLoadFailures, "media.load_failures")                                     \
  X(MediaAutoplayRefusals, "media.autoplay_refusals")                             \
  X(MediaSourceAppends, "media.source_appends")                                   \
  X(MediaSourceAppendFailures, "media.source_append_failures")                    \
  X(MediaSourceQuotaRefusals, "media.source_quota_refusals")                      \
  X(MediaSourceInitSegments, "media.source_init_segments")                        \
  X(MediaSourceFramesBuffered, "media.source_frames_buffered")                    \
  X(MediaSourceSegmentsEvicted, "media.source_segments_evicted")                  \
  X(MediaSourceBuffersCreated, "media.source_buffers_created")                    \
  X(MediaObjectUrlsCreated, "media.object_urls_created")                          \
  X(MpegTsPacketsParsed, "media.mpegts_packets")                                  \
  X(MpegTsSamples, "media.mpegts_samples")                                        \
  X(MpegTsResyncs, "media.mpegts_resyncs")                                        \
  X(TransitionsStarted, "animation.transitions_started")                          \
  X(TransitionsFinished, "animation.transitions_finished")                        \
  X(AnimationsStarted, "animation.animations_started")                            \
  X(AnimationsFinished, "animation.animations_finished")                          \
  X(AnimationFramesProduced, "animation.frames_produced")                         \
  X(CanvasesCreated, "canvas.created")                                            \
  X(CanvasDraws, "canvas.draws")                                                  \
  X(CanvasSizeRefusals, "canvas.size_refusals")                                   \
  X(CanvasReadbacks, "canvas.readbacks")                                          \
  X(CanvasesTainted, "canvas.tainted")                                            \
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
  X(ShapedRunCacheHits, "text.shaped_run_cache_hits")                            \
  X(ShapedRunCacheMisses, "text.shaped_run_cache_misses")                        \
  X(TextRunsPainted, "text.runs_painted")                                        \
  /* --- layout -------------------------------------------------------------- */ \
  X(LayoutTreeBuilds, "layout.tree_builds")                                      \
  X(LayoutBoxesCreated, "layout.boxes_created")                                  \
  X(LayoutRuns, "layout.runs")                                                   \
  /* A layout that ran in the middle of a script turn, because the page asked  */ \
  /* a geometry question the box tree could no longer answer. ADR 0015 makes   */ \
  /* this visible rather than cheap: a write-then-read loop can make the       */ \
  /* browser do unbounded work, and the count going up per iteration is the    */ \
  /* only way to tell that page apart from a slow one.                         */ \
  X(LayoutForcedByScript, "layout.forced_by_script")                             \
  X(LayoutDisplayListsBuilt, "layout.display_lists_built")                       \
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
  X(FocusTabCandidates, "focus.tab_candidates")

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
