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
  X(GfxPathFills, "gfx.path_fills")                                              \
  X(GfxPathSegments, "gfx.path_segments")                                        \
  X(GfxPathCells, "gfx.path_cells")                                              \
  X(GfxPathSpans, "gfx.path_spans")                                              \
  X(GfxPathSpanPixels, "gfx.path_span_pixels")                                   \
  X(GfxPathNonFiniteRejections, "gfx.path_non_finite_rejections")                \
  X(GfxStrokes, "gfx.strokes")                                                   \
  X(GfxStrokePieces, "gfx.stroke_pieces")                                        \
  X(GfxStrokeMiterFallbacks, "gfx.stroke_miter_fallbacks")                       \
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
