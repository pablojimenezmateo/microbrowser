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
  /* --- html ---------------------------------------------------------------- */ \
  X(HtmlTokens, "html.tokens")                                                   \
  X(HtmlDocumentsParsed, "html.documents_parsed")                                \
  X(HtmlUnsupportedInsertionMode, "html.unsupported_insertion_mode")             \
  X(DomNodesCreated, "dom.nodes_created")                                        \
  /* --- css ----------------------------------------------------------------- */ \
  X(CssTokens, "css.tokens")                                                     \
  X(CssSheetsParsed, "css.sheets_parsed")                                        \
  X(CssRulesParsed, "css.rules_parsed")                                          \
  X(CssStylesResolved, "css.styles_resolved")                                    \
  X(EngineStyleSheetsLoaded, "engine.stylesheets_loaded")                          \
  X(EngineStyleSheetsFailed, "engine.stylesheets_failed")                          \
  X(EnginePaintsSkipped, "engine.paints_skipped")                                \
  X(DamageDiffs, "paint.damage_diffs")                                           \
  X(DamageDiffsIdentical, "paint.damage_diffs_identical")                        \
  X(DamageDiffsFullRepaint, "paint.damage_diffs_full_repaint")                   \
  X(DamageRectsProduced, "paint.damage_rects_produced")                          \
  /* --- text ---------------------------------------------------------------- */ \
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
  X(EnginePaintsProduced, "engine.paints_produced")

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
