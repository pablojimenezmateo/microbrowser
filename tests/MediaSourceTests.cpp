#include <cstddef>
#include <limits>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "Mp4Fixtures.h"
#include "TestSupport.h"
#include "media/BufferedRanges.h"
#include "media/CodecId.h"
#include "media/MediaSourceState.h"

namespace microbrowser::tests {

using media::AppendResult;
using media::BufferedRanges;
using media::MediaSourceState;
using media::SourceBufferState;

namespace {

// `buffered` as a readable string, because the assertions worth making about a range set are about
// its shape and comparing shapes by index is how a test stops saying what it means.
std::string Shape(const BufferedRanges& ranges) {
  std::string out;
  for (const BufferedRanges::Range& range : ranges.Ranges()) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += "[" + std::to_string(range.start) + "," + std::to_string(range.end) + ")";
  }
  return out;
}

std::string Shape(std::initializer_list<std::pair<double, double>> expected) {
  std::string out;
  for (const auto& [start, end] : expected) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += "[" + std::to_string(start) + "," + std::to_string(end) + ")";
  }
  return out;
}

// Every invariant the range set promises, checked after an operation rather than trusted. A range set
// that is not sorted, or holds an empty range, or holds two that touch, is one a player will read and
// act on -- and each of those three shapes makes it act wrongly in a different way.
void ExpectWellFormed(const BufferedRanges& ranges, const char* what) {
  const std::vector<BufferedRanges::Range>& list = ranges.Ranges();
  for (std::size_t i = 0; i < list.size(); ++i) {
    Expect(list[i].end > list[i].start, std::string(what) + ": no empty range");
    if (i > 0) {
      Expect(list[i].start > list[i - 1].end + BufferedRanges::kJoinTolerance,
             std::string(what) + ": sorted, disjoint, and separated by more than the tolerance");
    }
  }
}

std::string EventsOf(SourceBufferState& buffer) {
  std::string out;
  for (const std::string_view event : buffer.TakeEvents()) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += std::string(event);
  }
  return out;
}

std::string EventsOf(MediaSourceState& source) {
  std::string out;
  for (const std::string_view event : source.TakeEvents()) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += std::string(event);
  }
  return out;
}

constexpr std::string_view kMp4 = "video/mp4; codecs=\"avc1.64001f\"";

}  // namespace

void RegisterMediaSourceTests(std::vector<TestCase>& tests) {
  // --- The range set --------------------------------------------------------------------------

  AddTest(tests, "MediaSource/RangesCoalesceIncludingWhenTheyMerelyTouch", [] {
    BufferedRanges ranges;
    ranges.Add(0.0, 1.0);
    ranges.Add(2.0, 3.0);
    ExpectEqString(Shape(ranges), Shape({{0.0, 1.0}, {2.0, 3.0}}), "two disjoint ranges stay two");
    // **Adjacent means one.** Two adjacent entries in a `TimeRanges` mean a *gap* to the player
    // reading it, and a gap is the thing it will go and fetch across -- so it would re-fetch bytes it
    // already has, forever.
    ranges.Add(1.0, 2.0);
    ExpectEqString(Shape(ranges), Shape({{0.0, 3.0}}), "a range that touches both joins them");
    ExpectWellFormed(ranges, "after coalescing");
    // An append that spans several existing ranges swallows all of them.
    BufferedRanges many;
    many.Add(0.0, 1.0);
    many.Add(2.0, 3.0);
    many.Add(4.0, 5.0);
    many.Add(0.5, 4.5);
    ExpectEqString(Shape(many), Shape({{0.0, 5.0}}), "one append over three");
    ExpectWellFormed(many, "after a spanning append");
  });

  AddTest(tests, "MediaSource/RemovingFromInsideARangeProducesTwo", [] {
    BufferedRanges ranges;
    ranges.Add(0.0, 10.0);
    ranges.Remove(4.0, 6.0);
    // The case a first draft loses -- and losing it means claiming to hold frames that were evicted,
    // which is worse than claiming to hold none: the player skips the fetch and then stalls.
    ExpectEqString(Shape(ranges), Shape({{0.0, 4.0}, {6.0, 10.0}}), "a hole in the middle");
    ExpectWellFormed(ranges, "after a split");
    ranges.Remove(0.0, 100.0);
    Expect(ranges.Empty(), "and a removal that covers everything empties it");
    // Removing across a boundary trims both sides rather than either.
    BufferedRanges two;
    two.Add(0.0, 4.0);
    two.Add(6.0, 10.0);
    two.Remove(3.0, 7.0);
    ExpectEqString(Shape(two), Shape({{0.0, 3.0}, {7.0, 10.0}}), "trimmed on both sides");
  });

  AddTest(tests, "MediaSource/ANonFiniteRangeIsRefusedRatherThanStored", [] {
    // `remove(NaN, 5)` is one call from script. A NaN in a sorted vector means the vector is not
    // sorted, and every comparison against it is false -- which is not a wrong answer, it is
    // undefined behaviour waiting for the next `std::sort`.
    BufferedRanges ranges;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    ranges.Add(nan, 5.0);
    ranges.Add(0.0, nan);
    ranges.Add(0.0, infinity);
    ranges.Add(5.0, 5.0);
    ranges.Add(6.0, 5.0);
    Expect(ranges.Empty(), "none of those is a range");
    ranges.Add(1.0, 2.0);
    ranges.Remove(nan, nan);
    ExpectEqString(Shape(ranges), Shape({{1.0, 2.0}}), "and a NaN removal removes nothing");
    Expect(!ranges.Contains(nan), "nor does anything contain NaN");
  });

  // --- The type allowlist ---------------------------------------------------------------------

  AddTest(tests, "MediaSource/EveryCodecInATypeMustBeSupported", [] {
    Expect(media::IsSupportedMediaSourceType("video/mp4; codecs=\"avc1.64001f\""), "H.264 in MP4");
    Expect(media::IsSupportedMediaSourceType("video/mp4; codecs=\"avc1.64001f, mp4a.40.2\""),
           "H.264 and AAC");
    Expect(media::IsSupportedMediaSourceType("video/webm; codecs=\"vp09.00.10.08, opus\""),
           "VP9 and Opus in WebM");
    Expect(media::IsSupportedMediaSourceType("video/mp4"), "a type with no codec list at all");
    // **Every, not any.** A stream with one codec this browser has and one it does not cannot play,
    // and saying yes would mean accepting bytes nothing will ever decode.
    Expect(!media::IsSupportedMediaSourceType("video/mp4; codecs=\"avc1.64001f, hev1.1.6.L93\""),
           "one unsupported codec makes the type unsupported");
    Expect(!media::IsSupportedMediaSourceType("video/mp2t; codecs=\"avc1.64001f\""),
           "a container with no demuxer behind it");
    Expect(!media::IsSupportedMediaSourceType("video/mp4; codecs=\"\""), "an empty codec list");
    Expect(!media::IsSupportedMediaSourceType(""), "and nothing at all");
  });

  // --- MediaSource state ----------------------------------------------------------------------

  AddTest(tests, "MediaSource/AttachingIsWhatOpensIt", [] {
    MediaSourceState source;
    Expect(source.State() == MediaSourceState::ReadyState::Closed, "constructed closed");
    // A page that constructs a MediaSource and appends without attaching gets InvalidStateError, which
    // is why `addSourceBuffer` refuses here rather than in the binding layer.
    Expect(source.AddSourceBuffer(std::string(kMp4)) == nullptr, "and refuses a buffer while closed");
    source.Attach();
    Expect(source.State() == MediaSourceState::ReadyState::Open, "attaching opens it");
    ExpectEqString(EventsOf(source), "sourceopen", "and fires sourceopen");
    Expect(source.AddSourceBuffer(std::string(kMp4)) != nullptr, "now a buffer can be added");
    Expect(source.AddSourceBuffer("video/mp2t") == nullptr,
           "but not one whose type has no demuxer -- NotSupportedError");
    source.EndOfStream();
    Expect(source.State() == MediaSourceState::ReadyState::Ended, "endOfStream ends it");
    ExpectEqString(EventsOf(source), "sourceended", "and says so");
    source.Detach();
    Expect(source.State() == MediaSourceState::ReadyState::Closed, "detaching closes it");
    // Every buffer goes with it, which makes "no MediaSource outlives its element" a property of the
    // type rather than a rule a caller has to follow.
    ExpectEqInt(static_cast<long long>(source.BufferCount()), 0, "and takes its buffers");
    ExpectEqString(EventsOf(source), "sourceclose", "sourceclose");
  });

  // --- Append, and the coded frame processing algorithm ---------------------------------------

  AddTest(tests, "MediaSource/AMediaSegmentBeforeAnInitSegmentIsRefused", [] {
    SourceBufferState buffer{std::string(kMp4)};
    // There is no timescale yet, so every timestamp in this segment is meaningless. Refused rather
    // than buffered at a guessed timescale: a frame at the wrong time is indistinguishable from a
    // frame at the right one until playback, which is far too late to find out.
    const AppendResult result =
        buffer.Append(Mp4MediaSegment(0, 4), MediaSourceState::kQuotaBytes);
    Expect(result == AppendResult::ParseFailed, "refused");
    Expect(buffer.Buffered().Empty(), "and nothing was buffered");
    ExpectEqString(EventsOf(buffer), "updatestart error updateend", "and the error event fired");
  });

  AddTest(tests, "MediaSource/BufferedTellsTheTruthAboutWhatWasAppended", [] {
    SourceBufferState buffer{std::string(kMp4)};
    Expect(buffer.Append(Mp4InitSegment(), MediaSourceState::kQuotaBytes) == AppendResult::Ok,
           "the init segment");
    Expect(buffer.HasInitSegment(), "which is what carries the timescale");
    ExpectEqInt(static_cast<long long>(buffer.Tracks().size()), 1, "one track");
    Expect(buffer.Buffered().Empty(), "and an init segment buffers no time");
    ExpectEqString(EventsOf(buffer), "updatestart update updateend", "the event pair");

    // Four samples of 40 ticks at timescale 1000, from decode time 0: [0, 0.16).
    Expect(buffer.Append(Mp4MediaSegment(0, 4), MediaSourceState::kQuotaBytes) == AppendResult::Ok,
           "a media segment");
    ExpectEqString(Shape(buffer.Buffered()), Shape({{0.0, 0.16}}), "a tenth of a second and a bit");
    // The next segment starts exactly where the last ended, so `buffered` must be *one* range. Two
    // would be a gap the player tries to fetch across.
    Expect(buffer.Append(Mp4MediaSegment(160, 4), MediaSourceState::kQuotaBytes) == AppendResult::Ok,
           "the segment after it");
    ExpectEqString(Shape(buffer.Buffered()), Shape({{0.0, 0.32}}), "coalesced into one range");
    ExpectWellFormed(buffer.Buffered(), "after two appends");
    // A gap left by a skipped segment stays a gap, because it is one.
    Expect(buffer.Append(Mp4MediaSegment(480, 4), MediaSourceState::kQuotaBytes) == AppendResult::Ok,
           "a segment past a gap");
    ExpectEqString(Shape(buffer.Buffered()), Shape({{0.0, 0.32}, {0.48, 0.64}}),
                   "and the gap is reported, because the player has to fetch it");
  });

  AddTest(tests, "MediaSource/AnOverlappingAppendReplacesWhatItOverlaps", [] {
    SourceBufferState buffer{std::string(kMp4)};
    buffer.Append(Mp4InitSegment(), MediaSourceState::kQuotaBytes);
    buffer.Append(Mp4MediaSegment(0, 10), MediaSourceState::kQuotaBytes);
    buffer.TakeEvents();
    const std::size_t after_first = buffer.BytesHeld();
    ExpectEqString(Shape(buffer.Buffered()), Shape({{0.0, 0.4}}), "four hundred milliseconds");
    // The same times appended again, which is what a player switching representation does. The frames
    // replace what occupied their times -- without that the old range survives and `buffered`
    // describes a mixture of two bitrates, which is a stream no decoder can play.
    buffer.Append(Mp4MediaSegment(0, 10, 40, 20), MediaSourceState::kQuotaBytes);
    ExpectEqString(Shape(buffer.Buffered()), Shape({{0.0, 0.4}}), "still one range, not two copies");
    Expect(buffer.BytesHeld() != after_first,
           "and the bytes are the new segment's rather than both");
  });

  AddTest(tests, "MediaSource/TimestampOffsetMovesFramesAndAppendWindowDropsThem", [] {
    SourceBufferState buffer{std::string(kMp4)};
    buffer.Append(Mp4InitSegment(), MediaSourceState::kQuotaBytes);
    buffer.SetTimestampOffset(10.0);
    buffer.Append(Mp4MediaSegment(0, 4), MediaSourceState::kQuotaBytes);
    ExpectEqString(Shape(buffer.Buffered()), Shape({{10.0, 10.16}}),
                   "spliced in at the offset, which is what a player uses to stitch periods");

    SourceBufferState windowed{std::string(kMp4)};
    windowed.Append(Mp4InitSegment(), MediaSourceState::kQuotaBytes);
    windowed.SetAppendWindow(0.08, 0.24);
    windowed.Append(Mp4MediaSegment(0, 8), MediaSourceState::kQuotaBytes);
    // Frames outside the window are *dropped*, not clamped: clamping moves a frame to a presentation
    // time it was not encoded for, and the player can fetch a missing frame but cannot detect a moved
    // one. Samples 2..5 are [0.08,0.24); the two before and two after are gone.
    ExpectEqString(Shape(windowed.Buffered()), Shape({{0.08, 0.24}}), "clipped to the window");
  });

  AddTest(tests, "MediaSource/TheQuotaRefusesBeforeItAllocates", [] {
    SourceBufferState buffer{std::string(kMp4)};
    buffer.Append(Mp4InitSegment(), 1024);
    buffer.TakeEvents();
    // A quota small enough that the second append cannot fit. The refusal is the *specified signal a
    // player is waiting for* -- it is how a player is told to evict -- so it fires `error` and reports
    // QuotaExceeded rather than failing quietly or growing.
    const AppendResult result = buffer.Append(Mp4MediaSegment(0, 200, 40, 100), 1024);
    Expect(result == AppendResult::QuotaExceeded, "refused for quota");
    ExpectEqString(EventsOf(buffer), "updatestart error updateend", "and said so");
    Expect(buffer.BytesHeld() <= 1024, "and held no more than the quota allows");
  });

  AddTest(tests, "MediaSource/RemoveFreesBytesAndNotJustTime", [] {
    SourceBufferState buffer{std::string(kMp4)};
    buffer.Append(Mp4InitSegment(), MediaSourceState::kQuotaBytes);
    buffer.Append(Mp4MediaSegment(0, 10), MediaSourceState::kQuotaBytes);
    buffer.Append(Mp4MediaSegment(400, 10), MediaSourceState::kQuotaBytes);
    const std::size_t held = buffer.BytesHeld();
    Expect(held > 0, "something is held");
    buffer.Remove(0.0, 0.4);
    // **The half a first draft leaves out.** A `remove` that shrank `buffered` without freeing
    // anything makes the quota unrecoverable: a player told to evict evicts, retries, and is refused
    // again forever. That is a video that never plays, caused by a bookkeeping omission.
    Expect(buffer.BytesHeld() < held, "and removing time freed bytes");
    ExpectEqString(Shape(buffer.Buffered()), Shape({{0.4, 0.8}}), "the second segment survives");
  });

  AddTest(tests, "MediaSource/TheQuotaIsPerSourceAndNotPerBuffer", [] {
    // A page with an audio buffer and a video buffer would otherwise hold twice the limit, which is
    // the obvious way around a per-buffer quota and the reason the total is asked of the source.
    MediaSourceState source;
    source.Attach();
    SourceBufferState* video = source.AddSourceBuffer(std::string(kMp4));
    SourceBufferState* audio = source.AddSourceBuffer("audio/mp4; codecs=\"mp4a.40.2\"");
    Expect(video != nullptr && audio != nullptr, "two buffers");
    video->Append(Mp4InitSegment(), MediaSourceState::kQuotaBytes);
    video->Append(Mp4MediaSegment(0, 10), MediaSourceState::kQuotaBytes);
    Expect(source.BytesHeld() == video->BytesHeld() + audio->BytesHeld(),
           "and the source's total is the sum");
    Expect(source.Duration() > 0.0, "duration follows what is buffered when nobody set it");
    source.SetDuration(60.0);
    Expect(source.Duration() == 60.0, "and a page can say otherwise");
  });

  AddTest(tests, "MediaSource/AbortDoesNotUnappend", [] {
    SourceBufferState buffer{std::string(kMp4)};
    buffer.Append(Mp4InitSegment(), MediaSourceState::kQuotaBytes);
    buffer.Append(Mp4MediaSegment(0, 4), MediaSourceState::kQuotaBytes);
    buffer.SetAppendWindow(1.0, 2.0);
    buffer.TakeEvents();
    buffer.Abort();
    // A page calling `abort` is cancelling a request, not undoing one. What it *does* reset is the
    // append window and the parser state, which is why the next append lands where it says.
    ExpectEqString(Shape(buffer.Buffered()), Shape({{0.0, 0.16}}), "frames already processed stay");
    ExpectEqString(EventsOf(buffer), "abort updateend", "abort then updateend");
    buffer.Append(Mp4MediaSegment(160, 4), MediaSourceState::kQuotaBytes);
    ExpectEqString(Shape(buffer.Buffered()), Shape({{0.0, 0.32}}),
                   "and the window it reset no longer clips");
  });

  AddTest(tests, "MediaSource/GarbageIsAParseFailureAndNotACrash", [] {
    SourceBufferState buffer{std::string(kMp4)};
    std::vector<std::byte> garbage(64, std::byte{0xAB});
    Expect(buffer.Append(garbage, MediaSourceState::kQuotaBytes) == AppendResult::ParseFailed,
           "bytes that are not a container");
    Expect(buffer.Buffered().Empty(), "buffered nothing");
    // An empty append is a no-op that still fires the pair, which is what a player polling with an
    // empty buffer expects rather than an error.
    Expect(buffer.Append({}, MediaSourceState::kQuotaBytes) == AppendResult::Ok, "an empty append");
    ExpectEqString(EventsOf(buffer), "updatestart error updateend updatestart update updateend",
                   "one failure and one no-op");
  });
}

}  // namespace microbrowser::tests
