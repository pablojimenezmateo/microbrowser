// `HTMLMediaElement`'s state machines.
//
// ADR 0028 §1: **the states are the API.** Every player on the web is written against them, so
// these assertions are about transitions and *event order* rather than about playback -- and
// they can be, because `media::MediaState` holds no samples, no element and no network. That
// separation is the only way to know the machine is the specification's rather than an
// approximation of it.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "dom/Node.h"
#include "engine/Engine.h"
#include "engine/MediaElements.h"
#include "engine/Page.h"
#include "engine/PageVideo.h"
#include "gfx/FontCatalog.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "media/AudioRing.h"
#include "media/AudioSink.h"
#include "media/MediaState.h"
#include "support/DriveLoop.h"
#include "support/ScriptedTransport.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

using media::MediaState;

std::string Events(MediaState& state) {
  std::string joined;
  for (const std::string_view event : state.TakeEvents()) {
    joined += joined.empty() ? "" : ",";
    joined += std::string(event);
  }
  return joined;
}

// A page with script, which needs the engine's load pipeline: `Page::Load` builds a tree but the
// scripts are collected and run by the engine, so a media test that only loaded a page would
// assert about a script that never ran -- which is how the first version of these three passed
// nothing.
struct ScriptedPage {
  gfx::FontLibrary library;
  gfx::FontCatalog fonts{library};
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts};
  ScriptedFactory factory;

  ScriptedPage(std::string_view body) {
    fonts.Register("Test", 400, false, BuildSyntheticFont());
    fonts.SetDefaultFamily("Test");
    std::string html = "<html><body>";
    html += body;
    html += "</body></html>";
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                           std::to_string(html.size()) + "\r\n\r\n" + html;
    factory.script.push_back(
        ScriptedTransport::Exchange{"page.example", 443, true, std::move(response)});
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
  }

  std::string Console() const {
    std::string joined;
    for (const std::string& line : engine.ConsoleOutput()) {
      joined += line + "|";
    }
    return joined;
  }
};

}  // namespace

void RegisterMediaStateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MediaElement/PlayReturnsAPromiseThatRejectsWhenAutoplayIsRefused", [] {
    // **The mechanism every player on the web uses.** A page calls `play()`, catches
    // `NotAllowedError`, and shows a play button. A `play()` that returned undefined would make
    // those players silently do nothing -- which is why the promise is the part of this API that
    // had to be right.
    ScriptedPage page(
        "<body><video id=v src='movie.mp4'></video><script>"
        "const v = document.getElementById('v');"
        "console.log('paused:' + v.paused + ' ready:' + v.readyState + ' net:' + v.networkState);"
        "v.play().then(() => console.log('resolved')).catch(e => console.log('rejected:' + e.name));"
        "</script></body>");

    const std::string console = page.Console();
    Expect(console.find("paused:true") != std::string::npos, "it starts paused");
    Expect(console.find("net:2") != std::string::npos,
           "and LOADING, because it has a src -- which is what makes the refusal NotAllowed "
           "rather than NotSupported");
    Expect(console.find("rejected:NotAllowedError") != std::string::npos,
           "and autoplay without a gesture rejects with the name pages catch");
  });

  AddTest(tests, "MediaElement/AMutedElementPlaysAndItsPropertiesAnswer", [] {
    // Muted autoplay is allowed, which is what every browser does. And the properties are
    // accessors rather than stored values, so `currentTime` after a seek is the seek -- a copy
    // would have stopped matching the moment the state machine moved.
    ScriptedPage page(
        "<body><video id=v src='movie.mp4' muted></video><script>"
        "const v = document.getElementById('v');"
        "v.muted = true;"
        "v.play().then(() => console.log('playing:' + !v.paused)).catch(e => console.log('no:' + e.name));"
        "v.volume = 5;"
        "v.currentTime = 12.5;"
        "console.log('volume:' + v.volume + ' time:' + v.currentTime + ' ended:' + v.ended);"
        "</script></body>");

    const std::string console = page.Console();
    Expect(console.find("playing:true") != std::string::npos, "muted play resolves and unpauses");
    // Volume clamps rather than throwing: a page overreaching is not a page in error, and the
    // specification clamps.
    Expect(console.find("volume:1 ") != std::string::npos, "volume clamped to 1");
    Expect(console.find("time:12.5") != std::string::npos,
           "and assigning currentTime is a seek, which moves the position immediately");
  });

  AddTest(tests, "MediaElement/CurrentSrcReflectsTheSrcAttribute", [] {
    // youtube's player reads `currentSrc` (not only `src`) when deciding whether a
    // resource is playable. An absent `currentSrc` left `fmt.unplayable` set while
    // MSE already had HAVE_ENOUGH_DATA (TD-0020).
    ScriptedPage page(
        "<body><video id=v src='movie.mp4'></video><script>"
        "const v = document.getElementById('v');"
        "console.log('src:' + v.src + ' current:' + v.currentSrc + ' err:' + v.error);"
        "</script></body>");
    const std::string console = page.Console();
    Expect(console.find("current:movie.mp4") != std::string::npos,
           "currentSrc answers the chosen resource URL");
    Expect(console.find("err:null") != std::string::npos,
           "error is null when nothing has failed -- not undefined");
  });

  AddTest(tests, "MediaElement/ThePlayMethodIsAbsentOnAnythingThatIsNotMedia", [] {
    // `document.body.play` must not exist. A media API on every element would make a typo look
    // like a player that does nothing. `canPlayType` shares the MediaSource allowlist --
    // `video/mp4` is accepted (codecs named at the init segment); an unknown container is not.
    ScriptedPage page(
        "<body><video id=v></video><script>"
        "console.log('body:' + typeof document.body.play + ' video:' + typeof document.getElementById('v').play);"
        "console.log('type:[' + document.getElementById('v').canPlayType('video/mp4') + ']');"
        "console.log('bogus:[' + document.getElementById('v').canPlayType('video/unknown') + ']');"
        "document.getElementById('v').play().catch(e => console.log('empty:' + e.name));"
        "</script></body>");

    const std::string console = page.Console();
    Expect(console.find("body:undefined video:function") != std::string::npos,
           "only a media element has the media API");
    Expect(console.find("type:[probably]") != std::string::npos,
           "canPlayType agrees with MediaSource.isTypeSupported for video/mp4");
    Expect(console.find("bogus:[]") != std::string::npos,
           "and an unknown container is still cannot-play");
    // No `src` is NO_SOURCE, so this is NotSupportedError rather than NotAllowedError -- a page
    // shows an error for one and a play button for the other.
    Expect(console.find("empty:NotSupportedError") != std::string::npos,
           "and an element with no source cannot play for a different reason");
  });

  AddTest(tests, "MediaState/TheReadinessLadderFiresEveryRungItClimbsPast", [] {
    // A whole file can arrive at once, and a page waiting on `canplay` still has to hear it.
    // So the climb fires every rung it passes rather than one event per state change.
    MediaState state;
    state.BeginLoad();
    ExpectEqString(Events(state), "loadstart", "the load started");
    state.MetadataArrived(120.0);
    ExpectEqString(Events(state), "durationchange,loadedmetadata", "duration before readiness");
    Expect(state.Duration() == 120.0, "and the duration is there when loadedmetadata fires");
    state.BufferedAhead(30.0);
    ExpectEqString(Events(state), "loadeddata,canplay,canplaythrough",
                   "three rungs in one step, in order");
    Expect(state.ReadyState() == MediaState::Ready::EnoughData, "and it landed at the top");
    Expect(state.NetworkState() == MediaState::Network::Idle,
           "with the network idle rather than still loading");
  });

  AddTest(tests, "MediaState/DataBeforeMetadataDoesNotClimbAnything", [] {
    // Bytes before the container is parsed are bytes nobody can interpret. The first rung means
    // "we know what this is", so promoting past it on data alone would make `duration` readable
    // before it exists.
    MediaState state;
    state.BeginLoad();
    state.BufferedAhead(10.0);
    Expect(state.ReadyState() == MediaState::Ready::Nothing, "still nothing");
    ExpectEqString(Events(state), "loadstart", "and no readiness events");
  });

  AddTest(tests, "MediaState/NoSourceIsNotASlowLoad", [] {
    // A page polling `networkState` for a stall has to be able to tell "nothing to load" from
    // "loading slowly", which is why NO_SOURCE exists as a separate value.
    MediaState state;
    state.BeginLoad();
    state.FailNoSource();
    Expect(state.NetworkState() == MediaState::Network::NoSource, "NO_SOURCE, not LOADING");
    ExpectEqString(Events(state), "loadstart,error", "and an error");
    // And `play()` on it is NotSupportedError rather than NotAllowedError: a page shows an error
    // message for one and a play button for the other.
    Expect(state.Play(true) == MediaState::PlayRefusal::NotSupported, "play is not supported");
  });

  AddTest(tests, "MediaState/AutoplayIsRefusedUnlessMutedOrActivated", [] {
    // ADR 0028 §1 over ADR 0017's user activation. This is the behaviour pages are written
    // against: they call `play()`, catch `NotAllowedError`, and show a play button.
    MediaState unmuted;
    unmuted.BeginLoad();
    unmuted.MetadataArrived(10.0);
    unmuted.BufferedAhead(5.0);
    Events(unmuted);
    Expect(unmuted.Play(false) == MediaState::PlayRefusal::NotAllowed,
           "no activation and not muted: refused");
    Expect(unmuted.Paused(), "and it did not start");
    ExpectEqString(Events(unmuted), "", "with no events at all, because nothing happened");

    // Muted is allowed, which is what every browser does.
    MediaState muted;
    muted.BeginLoad();
    muted.MetadataArrived(10.0);
    muted.BufferedAhead(5.0);
    muted.SetMuted(true);
    Events(muted);
    Expect(muted.Play(false) == MediaState::PlayRefusal::None, "muted plays");
    ExpectEqString(Events(muted), "play,playing", "and says so");

    // And a user gesture allows an unmuted one.
    MediaState gestured;
    gestured.BeginLoad();
    gestured.MetadataArrived(10.0);
    gestured.BufferedAhead(5.0);
    Events(gestured);
    Expect(gestured.Play(true) == MediaState::PlayRefusal::None, "activation plays");
  });

  AddTest(tests, "MediaState/PlayingWithoutEnoughDataIsWaitingRatherThanPlaying", [] {
    // A page shows its spinner on `waiting` and hides it on `playing`. An element that claimed
    // `playing` with nothing decoded would show video that is not moving.
    MediaState state;
    state.BeginLoad();
    state.MetadataArrived(60.0);
    Events(state);
    Expect(state.Play(true) == MediaState::PlayRefusal::None, "play is allowed");
    ExpectEqString(Events(state), "play,waiting", "but it is waiting, not playing");
    state.BufferedAhead(2.0);
    ExpectEqString(Events(state), "loadeddata,canplay,canplaythrough,playing",
                   "and `playing` comes when the data does");
  });

  AddTest(tests, "MediaState/ASeekDropsReadinessBecauseTheDecodedDataWasForSomewhereElse", [] {
    // **The bug ADR 0028 §1 names.** A `readyState` that stayed at EnoughData across a seek is a
    // player that stalls with no error and no way for the page to tell.
    MediaState state;
    state.BeginLoad();
    state.MetadataArrived(100.0);
    state.BufferedAhead(30.0);
    Events(state);
    state.SeekTo(80.0);
    Expect(state.ReadyState() == MediaState::Ready::Metadata, "readiness dropped");
    Expect(state.Seeking(), "and it is seeking");
    ExpectEqString(Events(state), "seeking", "which is what it says");
    Expect(state.CurrentTime() == 80.0, "the position moved immediately, as the API promises");
    // The clock reporting the new position is the only evidence a decoder arrived there.
    state.AdvanceTo(80.0);
    Expect(!state.Seeking(), "the seek completed");
    ExpectEqString(Events(state), "seeked,timeupdate", "with `seeked` before the time update");
    // A seek past the end is clamped rather than refused: a page dragging a scrubber to the
    // right edge means "the end", not "an error".
    state.SeekTo(500.0);
    Expect(state.CurrentTime() == 100.0, "clamped to the duration");
  });

  AddTest(tests, "MediaState/TimeUpdateIsThrottledAndTheEndIsNotAPause", [] {
    MediaState state;
    state.BeginLoad();
    state.MetadataArrived(1.0);
    state.BufferedAhead(2.0);
    state.Play(true);
    Events(state);
    // About 4Hz. A page that re-rendered a scrubber per audio callback would drop frames doing
    // it, which is why the throttle exists rather than as a nicety.
    int updates = 0;
    for (int step = 1; step <= 20; ++step) {
      state.AdvanceTo(static_cast<double>(step) * 0.05);
      for (const std::string_view event : state.TakeEvents()) {
        updates += event == "timeupdate" ? 1 : 0;
      }
    }
    Expect(updates >= 3 && updates <= 6, "four a second, not twenty");
    Expect(state.Ended(), "and reaching the duration ended it");
    Expect(state.Paused(), "which pauses");

    // `pause` is *not* fired at the end, and that asymmetry is real: `ended` is a stream running
    // out and `pause` is something a page or a user did. A page that treats them the same shows
    // a play button where it should show replay.
    MediaState second;
    second.BeginLoad();
    second.MetadataArrived(2.0);
    second.BufferedAhead(5.0);
    second.Play(true);
    Events(second);
    second.AdvanceTo(2.0);
    const std::string at_end = Events(second);
    Expect(at_end.find("ended") != std::string::npos, "ended fired");
    Expect(at_end.find("pause") == std::string::npos, "and pause did not");
  });

  AddTest(tests, "MediaState/PlayAfterTheEndRewindsRatherThanDoingNothing", [] {
    // Without this a replay button does nothing and the page has to know to seek first.
    MediaState state;
    state.BeginLoad();
    state.MetadataArrived(5.0);
    state.BufferedAhead(10.0);
    state.Play(true);
    state.AdvanceTo(5.0);
    Events(state);
    Expect(state.Ended() && state.CurrentTime() == 5.0, "it ended at the end");
    Expect(state.Play(true) == MediaState::PlayRefusal::None, "play again");
    Expect(!state.Ended() && state.CurrentTime() == 0.0, "rewound to the start");
    ExpectEqString(Events(state), "play,playing", "and started");
  });

  AddTest(tests, "MediaState/APageCannotLicenseItsOwnAutoplay", [] {
    // The property the whole refusal rests on, and it is about *where* activation is set rather
    // than about media: a real click goes through `Page::DispatchPointerDownAt`, and a click a page
    // dispatches itself goes through the binding layer and never reaches that function. So a
    // page that fires its own click events never gains activation, and its unmuted `play()` stays
    // refused. ADR 0017 defines the flag; this is the test that it cannot be forged.
    gfx::FontLibrary library;
    gfx::FontCatalog fonts{library};
    fonts.Register("Test", 400, false, BuildSyntheticFont());
    fonts.SetDefaultFamily("Test");
    engine::Page page(fonts);
    page.Load(
        "<body style='margin:0'><button id=b>go</button>"
        "<script>document.getElementById('b').click();"
        "document.body.dispatchEvent(new Event('click', {bubbles: true}));</script></body>",
        "https://example.org/");
    page.Layout(400.0f);
    Expect(page.CurrentDocument() != nullptr, "there is a document");
    Expect(!page.CurrentDocument()->HasUserActivation(),
           "a page clicking its own button is not the user interacting");

    // A real click is, and it is sticky: "I pressed play once, stop asking".
    bindings::PointerInput pointer;
    page.DispatchClickAt(gfx::FloatPoint{5.0f, 5.0f}, pointer);
    Expect(page.CurrentDocument()->HasUserActivation(), "a trusted click activates the document");
  });

  AddTest(tests, "MediaState/ChangingSourceIsAFreshElement", [] {
    // A page that sets `src` again gets a new element's worth of state. Keeping the old
    // duration is how a page ends up with a scrubber for the previous video.
    MediaState state;
    state.BeginLoad();
    state.MetadataArrived(100.0);
    state.BufferedAhead(30.0);
    state.Play(true);
    state.AdvanceTo(50.0);
    Events(state);
    state.BeginLoad();
    Expect(state.Duration() == 0.0, "no duration");
    Expect(state.CurrentTime() == 0.0, "at the start");
    Expect(state.ReadyState() == MediaState::Ready::Nothing, "and nothing is ready");
    ExpectEqString(Events(state), "loadstart", "with a fresh loadstart");
  });

  AddTest(tests, "PageVideo/TheSinkFollowsMuteAndPauseWithoutDecoding", [] {
    // TD-0019: the device is opened only for unmuted play, and Stop joins before the
    // ring dies. Decode is orthogonal -- UpdateOutput is the half that owns idle CPU.
    // Sink before PageVideo: ~PageVideo clears the sink pointer and must not
    // call Stop on an already-destroyed device (ADR 0028 §4 lifetime).
    struct RecordingSink : media::AudioSink {
      int starts = 0;
      int stops = 0;
      bool running = false;
      bool Start(media::AudioRing& ring) override {
        (void)ring;
        ++starts;
        running = true;
        return true;
      }
      void Stop() override {
        ++stops;
        running = false;
      }
      bool IsRunning() const override { return running; }
      int SampleRate() const override { return running ? 48000 : 0; }
      int Channels() const override { return running ? 2 : 0; }
      std::uint64_t QueuedFrames() const override { return 0; }
    } sink;
    engine::MediaElements media;
    engine::PageVideo video(media);
    video.SetAudioSink(&sink);

    media::MediaState muted;
    muted.BeginLoad();
    muted.MetadataArrived(10.0);
    muted.BufferedAhead(10.0);
    muted.SetMuted(true);
    Expect(muted.Play(false) == media::MediaState::PlayRefusal::None, "muted autoplay");
    video.UpdateOutput(muted);
    Expect(!sink.running && sink.starts == 0, "muted play never opens a device");

    media::MediaState playing;
    playing.BeginLoad();
    playing.MetadataArrived(10.0);
    playing.BufferedAhead(10.0);
    Expect(playing.Play(true) == media::MediaState::PlayRefusal::None, "gestured play");
    video.UpdateOutput(playing);
    Expect(sink.running && sink.starts == 1, "unmuted play starts the sink");

    playing.SetMuted(true);
    video.UpdateOutput(playing);
    Expect(!sink.running && sink.stops == 1, "mute stops and joins");

    playing.SetMuted(false);
    video.UpdateOutput(playing);
    Expect(sink.running && sink.starts == 2, "unmute starts again");

    playing.Pause();
    video.UpdateOutput(playing);
    Expect(!sink.running && sink.stops == 2, "pause stops the device");
  });
}

}  // namespace microbrowser::tests
