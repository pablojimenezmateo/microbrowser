// Runs web-platform-tests against this browser and compares the answers to
// what they were last time.
//
// The argument for this existing is in docs/adr/0040-web-platform-tests.md.
// The short version: every rendering bug found in the last ten sessions was
// found by loading a real page and staring at it, and a real page is a terrible
// instrument -- it changes under you, it fails for a reason two layers away
// from the one you are working on, and it cannot tell you that a fix broke
// something else. WPT is the same coverage as a stable, sharded, per-subtest
// signal that a machine can diff.
//
//   microbrowser_wpt                       # everything in the expectations
//   microbrowser_wpt dom/ css/css-flexbox/ # by path prefix
//   microbrowser_wpt --update-expectations dom/
//   microbrowser_wpt --verbose dom/nodes/Node-appendChild.html
//
// Two properties are worth knowing before changing anything here.
//
// **Every test runs in its own process.** A from-scratch browser fails by
// hanging and by crashing at least as often as it fails by answering wrong, and
// both of those take the whole run with them when tests share a process. A fork
// per test turns "the run died" into one CRASH or TIMEOUT line beside 4,000
// other results, and gets parallelism for free.
//
// **Nothing here runs a thread.** The server is forked before anything else
// exists and serves from its own single-threaded process; the runner forks a
// child per test. The browser's zero-idle-CPU, one-place-to-block invariant is
// not something a test harness gets to opt out of, because a harness that
// polls is a harness that measures its own polling.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "engine/Engine.h"
#include "gfx/Canvas.h"
#include "gfx/DisplayList.h"
#include "gfx/Painter.h"
#include "gfx/Surface.h"
#include "gfx/TextRenderer.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "platform/DescriptorWait.h"
#include "platform/SystemFonts.h"
#include "util/WaitDescriptor.h"
#include "wpt/Expectations.h"
#include "wpt/Reftest.h"
#include "wpt/Server.h"
#include "wpt/Summary.h"
#include "wpt/TestList.h"

namespace {

using microbrowser::wpt::ExpectationStore;
using microbrowser::wpt::NormalizePortsInName;
using microbrowser::wpt::Server;
using microbrowser::wpt::ServerOptions;
using microbrowser::wpt::TestExpectation;
using microbrowser::wpt::TestKind;
using microbrowser::wpt::WptTest;

constexpr const char* kUsage =
    "usage: microbrowser_wpt [options] [path-prefix ...]\n"
    "\n"
    "  --wpt-root DIR        checkout to run (default third_party/wpt)\n"
    "  --expectations DIR    expectation files (default tests/wpt/expectations)\n"
    "  --areas FILE          run the path prefixes listed in FILE, one per line\n"
    "  --jobs N              tests in flight at once (default: half the cores). Raising\n"
    "                        it deletes subtests from a CPU-bound area; see the source\n"
    "  --timeout MS          per test, before it is killed (default 10000)\n"
    "  --retries N           re-runs of a result that disagrees (default 1)\n"
    "  --timeout-multiplier N  scale every deadline; use it on a slow build\n"
    "  --shard-index N --shard-count N   deterministic slice of the run\n"
    "  --update-expectations rewrite the expectation files from this run\n"
    "  --summary FILE        write the per-area table and the ranked causes\n"
    "  --summary-state FILE  carry a sharded run's counts between invocations; usable\n"
    "                        without --summary, which is what the first shard wants\n"
    "  --long-timeout MS     for a test marked `timeout=long` (default 60000)\n"
    "  --list                print the tests that would run and exit; with --verbose,\n"
    "                        a reftest's `<meta name=fuzzy>` tolerance too\n"
    "  --refresh-manifest    re-walk the checkout instead of using the cache\n"
    "  --serve               run only the server, in the foreground\n"
    "  --port N              first http port (default: an unused one)\n"
    "  --verbose             one line per test, and the server's requests\n"
    "  --reftest-artifacts DIR  write test/ref/diff PPMs for each failing reftest\n"
    "  --reftest-artifacts-limit N  stop after N of them (default 64)\n"
    "  --testharness-only / --reftests-only\n";

struct Options {
  std::string wpt_root = "third_party/wpt";
  std::string expectations_dir = "tests/wpt/expectations";
  std::string summary_path;
  std::string summary_state_path;
  // Where a failing reftest leaves its three images. Opt-in, and bounded, for
  // one reason: a full reftest run is 20,998 files, three 1.4MB PPMs each, and
  // a flag that quietly wrote 90GB would be a worse instrument than none. The
  // bound is a *budget the parent hands out* rather than a count the children
  // keep, because the children are separate processes -- see where it is spent.
  std::string reftest_artifacts_dir;
  int reftest_artifacts_limit = 64;
  std::vector<std::string> prefixes;
  int jobs = 0;
  int timeout_ms = 10000;
  // Upstream's own number for a test that says `timeout=long`. It is separate
  // from `--timeout` rather than a multiple of it because the two answer
  // different questions: `--timeout` is how long this browser is given to
  // finish work it can do, and this is how long a test that asked for room is
  // given. A baseline run is dominated by the second -- 2,946 of the suite's
  // tests are `long`, and the ones that will never report cost a minute each.
  int long_timeout_ms = 60000;
  int timeout_multiplier = 1;
  int retries = 1;
  int shard_index = 0;
  int shard_count = 1;
  std::uint16_t port = 0;
  bool update_expectations = false;
  bool list_only = false;
  bool refresh_manifest = false;
  bool serve_only = false;
  bool verbose = false;
  bool testharness_only = false;
  bool reftests_only = false;
};

// One subtest, or the harness itself.
struct Report {
  std::string harness = "OK";
  std::string harness_message;
  // name -> status, PASS included: the runner needs to see a pass that was
  // expected to fail as loudly as it sees the other direction.
  std::map<std::string, std::string> subtests;
  std::map<std::string, std::string> messages;
};

std::string Unescape(std::string_view value) {
  std::string out;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\\' && index + 1 < value.size()) {
      const char next = value[index + 1];
      if (next == 'n') {
        out.push_back('\n');
        ++index;
        continue;
      }
      if (next == 't') {
        out.push_back('\t');
        ++index;
        continue;
      }
      if (next == '\\') {
        out.push_back('\\');
        ++index;
        continue;
      }
    }
    out.push_back(value[index]);
  }
  return out;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    fields.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos
                                                                     : tab - start));
    if (tab == std::string_view::npos) {
      break;
    }
    start = tab + 1;
  }
  return fields;
}

// `ports` are this run's bound ports, so a name the page built out of its own origin
// becomes stable across runs. Normalized *here*, at the one place a page's name
// becomes a key, so recording and comparing cannot disagree about what a subtest is
// called. See NormalizePortsInName.
Report ParseReport(std::string_view text, const std::vector<std::uint16_t>& ports) {
  Report report;
  bool saw_harness = false;
  std::size_t position = 0;
  while (position < text.size()) {
    const std::size_t end = text.find('\n', position);
    const std::string_view line =
        text.substr(position, end == std::string_view::npos ? std::string_view::npos
                                                            : end - position);
    const std::vector<std::string_view> fields = SplitTabs(line);
    if (!fields.empty() && fields[0] == "H" && fields.size() >= 2) {
      report.harness = std::string(fields[1]);
      report.harness_message = fields.size() >= 3 ? Unescape(fields[2]) : "";
      saw_harness = true;
    } else if (!fields.empty() && fields[0] == "T" && fields.size() >= 3) {
      const std::string name = NormalizePortsInName(Unescape(fields[2]), ports);
      report.subtests[name] = std::string(fields[1]);
      if (fields.size() >= 4) {
        report.messages[name] = Unescape(fields[3]);
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    position = end + 1;
  }
  if (!saw_harness) {
    report.harness = "ERROR";
    report.harness_message = "the page produced no report";
  }
  return report;
}

// --- The child: one test, one process ----------------------------------------

// Pumps the engine the way tools/snapshot does -- the real seam, no shortcuts --
// until `predicate` says the page is finished or the deadline passes. Returns
// false on the deadline.

// --- testdriver -------------------------------------------------------------
//
// `test_driver.click()` and `test_driver.send_keys()` are how 1,146 of the tests
// in scope ask for input, and without them each one is a `not implemented by
// testdriver-vendor.js` rejection -- 1,158 tests by the baseline's count, the
// second-largest single cause in the suite.
//
// The seam is `tools/wpt/harness/testdriver-vendor.js`, which queues a line on a
// global; this drains it and drives the **real** input path -- the same
// `PointerInputMessage` and `KeyInputMessage` a mouse and a keyboard arrive on.
// It has to be the real one: ADR 0017 makes a page's own synthetic event
// untrusted, so a click that did not follow a link would pass the half of a test
// that counts handlers and fail the half that checks what the click did.

// One field of a tab-separated command, or empty when there are fewer.
std::string_view CommandField(std::string_view line, std::size_t index) {
  std::size_t start = 0;
  for (std::size_t i = 0; i < index; ++i) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      return {};
    }
    start = tab + 1;
  }
  const std::size_t tab = line.find('\t', start);
  return line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start);
}

double CommandNumber(std::string_view field) {
  if (field.empty()) {
    return 0.0;
  }
  return std::strtod(std::string(field).c_str(), nullptr);
}

// The pointer's last position, so a `pointerDown` lands where the `pointerMove`
// before it left off. WebDriver's action sources model exactly this.
struct DriverState {
  float x = 0.0f;
  float y = 0.0f;
};

// A key's three fields, from the one name the shim sends. `text` is what the key
// inserts and is empty for everything that is not a single printable character,
// which is what makes an ArrowDown a navigation rather than an insertion.
microbrowser::ipc::KeyInputMessage KeyMessage(std::string_view name, bool down) {
  microbrowser::ipc::KeyInputMessage message;
  message.kind = down ? microbrowser::ipc::KeyInputMessage::Kind::Down
                      : microbrowser::ipc::KeyInputMessage::Kind::Up;
  message.key = std::string(name);
  if (name.size() == 1 && static_cast<unsigned char>(name[0]) >= 0x20) {
    message.text = std::string(name);
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    message.code = (name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z')
                       ? std::string("Key") + upper
                       : std::string();
  } else if (name == " ") {
    message.key = " ";
    message.text = " ";
    message.code = "Space";
  } else {
    message.code = std::string(name);
  }
  return message;
}

// Runs whatever the page has queued. Returns true when it ran anything, which
// tells the loop the page has moved and is worth another turn.
bool RunDriverCommands(microbrowser::engine::Engine& engine,
                       microbrowser::ipc::UiEndpoint& ui, DriverState& state) {
  if (engine.EvaluateScript(
          "(typeof self.__wpt_driver_pending==='function'&&self.__wpt_driver_pending())?1:0") !=
      "1") {
    return false;
  }
  const std::string taken = engine.EvaluateScript("String(self.__wpt_driver_take())");
  if (taken.empty()) {
    return false;
  }
  std::size_t position = 0;
  bool ran = false;
  while (position <= taken.size()) {
    const std::size_t end = taken.find('\n', position);
    const std::string_view line =
        std::string_view(taken).substr(position, end == std::string::npos ? std::string::npos
                                                                          : end - position);
    position = end == std::string::npos ? taken.size() + 1 : end + 1;
    if (line.empty()) {
      continue;
    }
    const std::string id(CommandField(line, 0));
    const std::string_view type = CommandField(line, 1);
    const auto send_pointer = [&](microbrowser::ipc::PointerInputMessage::Kind kind, float x,
                                  float y, std::uint16_t buttons, std::uint8_t button) {
      microbrowser::ipc::PointerInputMessage message;
      message.kind = kind;
      message.position = microbrowser::gfx::FloatPoint{x, y};
      message.buttons = buttons;
      message.button = button;
      ui.Send(message);
      engine.HandlePendingMessages();
    };
    if (type == "click") {
      const auto x = static_cast<float>(CommandNumber(CommandField(line, 2)));
      const auto y = static_cast<float>(CommandNumber(CommandField(line, 3)));
      state.x = x;
      state.y = y;
      // Move, down, up -- the three a real click is, because a page that listens
      // on `pointerdown` rather than `click` is ordinary and a bare click would
      // never reach it.
      send_pointer(microbrowser::ipc::PointerInputMessage::Kind::Move, x, y, 0, 0);
      send_pointer(microbrowser::ipc::PointerInputMessage::Kind::Down, x, y, 1, 0);
      send_pointer(microbrowser::ipc::PointerInputMessage::Kind::Up, x, y, 0, 0);
    } else if (type == "pointerMove") {
      state.x = static_cast<float>(CommandNumber(CommandField(line, 2)));
      state.y = static_cast<float>(CommandNumber(CommandField(line, 3)));
      send_pointer(microbrowser::ipc::PointerInputMessage::Kind::Move, state.x, state.y, 0, 0);
    } else if (type == "pointerDown") {
      const auto button = static_cast<std::uint8_t>(CommandNumber(CommandField(line, 2)));
      send_pointer(microbrowser::ipc::PointerInputMessage::Kind::Down, state.x, state.y,
                   static_cast<std::uint16_t>(1u << button), button);
    } else if (type == "pointerUp") {
      const auto button = static_cast<std::uint8_t>(CommandNumber(CommandField(line, 2)));
      send_pointer(microbrowser::ipc::PointerInputMessage::Kind::Up, state.x, state.y, 0, button);
    } else if (type == "keys") {
      const std::string_view name = CommandField(line, 2);
      ui.Send(KeyMessage(name, true));
      engine.HandlePendingMessages();
      ui.Send(KeyMessage(name, false));
      engine.HandlePendingMessages();
    } else if (type == "keyDown") {
      ui.Send(KeyMessage(CommandField(line, 2), true));
      engine.HandlePendingMessages();
    } else if (type == "keyUp") {
      ui.Send(KeyMessage(CommandField(line, 2), false));
      engine.HandlePendingMessages();
    } else if (type == "pause") {
      // Nothing to do: the loop turns anyway and the page's own timers are what
      // a pause is measured against. Sleeping here would stall the test process
      // for a duration the test did not ask this side to honour.
    }
    // Settled whatever it was, including a type this build does not know: the
    // promise must not be left pending, or the test hangs on a command rather
    // than failing on it.
    engine.EvaluateScript("self.__wpt_driver_settle(" + id + ",true)");
    ran = true;
  }
  return ran;
}

bool PumpUntil(microbrowser::engine::Engine& engine, microbrowser::ipc::UiEndpoint& ui,
               microbrowser::gfx::DisplayList* last_frame,
               std::chrono::steady_clock::time_point deadline,
               const std::function<bool()>& finished) {
  auto next_poll = std::chrono::steady_clock::now();
  DriverState driver;
  while (std::chrono::steady_clock::now() < deadline) {
    while (std::optional<microbrowser::ipc::EngineToUi> message = ui.TryReceive()) {
      if (auto* paint = std::get_if<microbrowser::ipc::PaintFrameMessage>(&*message);
          paint != nullptr && last_frame != nullptr) {
        *last_frame = std::move(paint->display_list);
      }
    }
    // **Both, every turn, in this order** -- which is what `Application::Turn` does and what this
    // loop did not. `RunDueWork` used to be reached only when `HasRunnableWork()` was false, and a
    // worker with a message queued is precisely a case where it is *true*: the loop span at full
    // speed until the deadline while the delivery that would have finished the page sat in a queue
    // nothing was draining. Every `.any.worker.html` in the suite -- 1,763 files -- timed out on it,
    // and from outside it is indistinguishable from an engine that cannot run workers at all.
    const bool advanced = engine.Advance();
    const bool ran_due_work = engine.RunDueWork();
    if (advanced || ran_due_work || engine.HasRunnableWork()) {
      continue;
    }
    // Asking the page whether it is done costs a script evaluation, so it is
    // asked on a clock rather than on every turn of the loop.
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_poll) {
      // Input the page asked for, before the "are you finished" question: a
      // command that ran may be the thing that finishes it.
      if (RunDriverCommands(engine, ui, driver)) {
        next_poll = now;
        continue;
      }
      if (finished()) {
        return true;
      }
      next_poll = now + std::chrono::milliseconds(10);
    }
    microbrowser::util::WaitDescriptorList descriptors;
    engine.AppendWaitDescriptors(descriptors);
    const std::optional<std::uint32_t> next = engine.NextDeadlineMs();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) {
      break;
    }
    if (descriptors.empty() && !next.has_value()) {
      // Nothing on the wire, nothing due, nothing runnable. The page is as
      // finished as it is going to get; one more look and then give up.
      return finished();
    }
    const std::int64_t wait_ms = std::min<std::int64_t>(
        {remaining, next.has_value() ? static_cast<std::int64_t>(*next) : 10, 10});
    microbrowser::platform::WaitOnDescriptors(descriptors,
                                              static_cast<std::int32_t>(std::max<std::int64_t>(wait_ms, 1)));
  }
  return finished();
}

struct RenderedPage {
  microbrowser::gfx::DisplayList display_list;
  bool painted = false;
};

// Loads one URL and returns what the engine reported, using `evaluate` to ask
// the page whether it has finished.
std::string RunTestharness(microbrowser::platform::SystemFontProvider& fonts,
                           const std::string& url, int timeout_ms) {
  microbrowser::ipc::InProcessChannel channel;
  microbrowser::engine::Engine engine{channel.Engine(), fonts};
  channel.Ui().Send(microbrowser::ipc::ResizeViewportMessage{
      microbrowser::gfx::IntSize{800, 600}, 1.0f});
  channel.Ui().Send(microbrowser::ipc::NavigateMessage{url});
  engine.HandlePendingMessages();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  const bool finished = PumpUntil(engine, channel.Ui(), nullptr, deadline, [&] {
    return engine.EvaluateScript("(typeof self!=='undefined'&&self.__wpt_done)?1:0") == "1";
  });

  if (!finished) {
    std::string message = "the page never reported";
    const std::vector<std::string>& errors = engine.ScriptErrors();
    if (!errors.empty()) {
      message += "; first script error: " + errors.front();
    }
    // Escape it the way the harness would: this goes down the same pipe.
    std::string escaped;
    for (const char c : message) {
      if (c == '\n') {
        escaped += "\\n";
      } else if (c == '\t') {
        escaped += "\\t";
      } else {
        escaped.push_back(c);
      }
    }
    return "H\tTIMEOUT\t" + escaped;
  }
  return engine.EvaluateScript("String(self.__wpt_report)");
}

// Renders one URL and returns its display list, or an unpainted frame.
RenderedPage RenderPage(microbrowser::platform::SystemFontProvider& fonts, const std::string& url,
                        int timeout_ms) {
  microbrowser::ipc::InProcessChannel channel;
  microbrowser::engine::Engine engine{channel.Engine(), fonts};
  channel.Ui().Send(microbrowser::ipc::ResizeViewportMessage{
      microbrowser::gfx::IntSize{800, 600}, 1.0f});
  channel.Ui().Send(microbrowser::ipc::NavigateMessage{url});
  engine.HandlePendingMessages();

  RenderedPage page;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  PumpUntil(engine, channel.Ui(), &page.display_list, deadline,
            [&] { return !engine.IsDocumentLoading() && !engine.HasInFlightScriptFetches(); });
  while (std::optional<microbrowser::ipc::EngineToUi> message = channel.Ui().TryReceive()) {
    if (auto* paint = std::get_if<microbrowser::ipc::PaintFrameMessage>(&*message)) {
      page.display_list = std::move(paint->display_list);
    }
  }
  page.painted = page.display_list.Size() > 0;
  return page;
}

// Renders a reftest and its reference and compares them, within whatever
// tolerance the test asked for.
//
// `artifacts_dir`, when set, is where a *failure* leaves the three images:
// `<stem>.test.ppm`, `<stem>.ref.ppm` and `<stem>.diff.ppm`. Task F2's whole
// argument is that a pixel count cannot distinguish an antialiasing difference
// from a missing feature and a picture can, in one second.
std::string RunReftest(microbrowser::platform::SystemFontProvider& fonts,
                       microbrowser::gfx::TextRenderer& text, const WptTest& test,
                       const std::string& test_url, const std::string& reference_url,
                       const std::string& artifacts_dir, int timeout_ms) {
  const auto rasterize = [&](const std::string& url) {
    RenderedPage page = RenderPage(fonts, url, timeout_ms / 2);
    microbrowser::gfx::Canvas canvas{800, 600};
    canvas.Clear(microbrowser::gfx::Color::Rgb(0xFF, 0xFF, 0xFF));
    microbrowser::gfx::Painter painter{canvas};
    microbrowser::gfx::Execute(page.display_list, painter, canvas.Bounds(), &text);
    return canvas;
  };
  const microbrowser::gfx::Canvas actual = rasterize(test_url);
  const microbrowser::gfx::Canvas expected = rasterize(reference_url);

  const microbrowser::wpt::ImageDifference difference =
      microbrowser::wpt::CompareCanvases(actual, expected);
  // `rel=mismatch` is not "fails the fuzzy check": it asks for a *visible*
  // difference, so it is answered by the same tolerance read the other way
  // round. Two renderings that differ only inside the allowance are the same
  // picture, and a mismatch test they satisfy has not proved anything.
  const bool within = microbrowser::wpt::FuzzyAllows(difference, test.fuzzy);
  if (within != test.reference_mismatch) {
    // A pass says how much room it had left, because "passed" and "passed by
    // one pixel of a 1,500-pixel tolerance" are different facts about the
    // renderer and only one of them survives the next change to it.
    if (difference.pixels_different == 0) {
      return "H\tOK\t";
    }
    return "H\tOK\t" + std::to_string(difference.pixels_different) +
           " pixels differ, worst channel " + std::to_string(difference.max_per_channel) +
           "; within " + microbrowser::wpt::SerializeFuzzy(test.fuzzy);
  }

  std::string message;
  if (test.reference_mismatch) {
    message = "the two rendered the same";
    if (difference.pixels_different != 0) {
      message += " within the tolerance (" + std::to_string(difference.pixels_different) +
                 " pixels differ, worst channel " + std::to_string(difference.max_per_channel) + ")";
    }
  } else {
    message = std::to_string(difference.pixels_different) + " pixels differ, worst channel " +
              std::to_string(difference.max_per_channel);
    if (!test.fuzzy.IsExact()) {
      message += "; allowed " + microbrowser::wpt::SerializeFuzzy(test.fuzzy);
    }
  }

  if (!artifacts_dir.empty()) {
    const std::string stem =
        artifacts_dir + "/" + microbrowser::wpt::ArtifactStem(test.url_path);
    const bool wrote = microbrowser::wpt::WritePpm(actual, stem + ".test.ppm") &&
                       microbrowser::wpt::WritePpm(expected, stem + ".ref.ppm") &&
                       microbrowser::wpt::WritePpm(
                           microbrowser::wpt::DifferenceImage(actual, expected), stem + ".diff.ppm");
    message += wrote ? "; wrote " + stem + ".{test,ref,diff}.ppm"
                     : "; could not write artifacts to " + artifacts_dir;
  }
  // Tabs are the report's field separator and a message carrying one would
  // become a field of its own.
  std::replace(message.begin(), message.end(), '\t', ' ');
  return "H\tFAIL\t" + message;
}

// --- The parent --------------------------------------------------------------

struct RunningTest {
  pid_t pid = -1;
  int descriptor = -1;
  std::size_t index = 0;
  std::string output;
  std::chrono::steady_clock::time_point deadline;
  // This child was given the artifacts directory, so a FAIL from it spends one
  // of the run's artifact budget.
  bool may_write_artifacts = false;
};

enum class Outcome { Expected, Unexpected, Disabled };

struct Comparison {
  Outcome outcome = Outcome::Expected;
  std::vector<std::string> lines;
  TestExpectation observed;
};

Comparison Compare(const WptTest& test, const Report& report, const TestExpectation* expected) {
  Comparison comparison;
  TestExpectation observed;
  observed.harness = report.harness;
  // **A harness status that is not OK subsumes the subtests.** A test that
  // times out half way through reports every subtest after that point as
  // NOTRUN, and recording those is recording the *consequence* of the one
  // failure rather than 1,200 independent ones: the first run of this harness
  // wrote 188,172 NOTRUN lines into `encoding.txt`, which is exactly the
  // unreadable file the expectation format exists to avoid. Fixing the timeout
  // is the task; the subtests behind it are not separate facts.
  if (report.harness == "OK") {
    for (const auto& [name, status] : report.subtests) {
      if (status != "PASS") {
        observed.subtests[name] = status;
      }
    }
  }
  comparison.observed = observed;

  const TestExpectation fallback;
  const TestExpectation& want = expected != nullptr ? *expected : fallback;
  if (report.harness != want.harness) {
    comparison.outcome = Outcome::Unexpected;
    comparison.lines.push_back("  harness: expected " + want.harness + ", got " + report.harness +
                               (report.harness_message.empty() ? ""
                                                               : " (" + report.harness_message + ")"));
    return comparison;
  }
  if (report.harness != "OK") {
    // Expected to fail at the harness level, and it did. The subtest list is
    // not comparable in this state -- see above.
    return comparison;
  }
  for (const auto& [name, status] : report.subtests) {
    const auto found = want.subtests.find(name);
    const std::string wanted = found == want.subtests.end() ? "PASS" : found->second;
    if (status != wanted) {
      comparison.outcome = Outcome::Unexpected;
      std::string line = "  " + status + " (expected " + wanted + "): " + name;
      const auto message = report.messages.find(name);
      if (message != report.messages.end() && !message->second.empty()) {
        line += "\n      " + message->second;
      }
      comparison.lines.push_back(std::move(line));
    }
  }
  // A subtest that was expected to fail and is now missing entirely is a change
  // too: it usually means the test stopped getting that far.
  for (const auto& [name, status] : want.subtests) {
    if (report.subtests.find(name) == report.subtests.end()) {
      comparison.outcome = Outcome::Unexpected;
      comparison.lines.push_back("  MISSING (expected " + status + "): " + name);
    }
  }
  (void)test;
  return comparison;
}

int ParseInt(std::string_view value, int fallback) {
  int result = 0;
  bool any = false;
  for (const char c : value) {
    if (c < '0' || c > '9') {
      return fallback;
    }
    result = result * 10 + (c - '0');
    any = true;
  }
  return any ? result : fallback;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    const auto value = [&]() -> std::string {
      return index + 1 < argc ? argv[++index] : std::string();
    };
    if (argument == "--wpt-root") {
      options.wpt_root = value();
    } else if (argument == "--expectations") {
      options.expectations_dir = value();
    } else if (argument == "--areas") {
      // A file of path prefixes, one per line. This is how `ctest` runs the
      // areas that have committed expectations and nothing else: every other
      // area would default to "expected to pass" and fail by the thousand.
      const std::string path = value();
      std::ifstream stream(path);
      if (!stream) {
        std::fprintf(stderr, "could not read %s\n", path.c_str());
        return 2;
      }
      std::string line;
      while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
          line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
          options.prefixes.push_back(line);
        }
      }
    } else if (argument == "--jobs" || argument == "-j") {
      options.jobs = ParseInt(value(), 0);
    } else if (argument == "--timeout") {
      options.timeout_ms = ParseInt(value(), options.timeout_ms);
    } else if (argument == "--timeout-multiplier") {
      options.timeout_multiplier = std::max(1, ParseInt(value(), 1));
    } else if (argument == "--retries") {
      options.retries = ParseInt(value(), options.retries);
    } else if (argument == "--shard-index") {
      options.shard_index = ParseInt(value(), 0);
    } else if (argument == "--shard-count") {
      options.shard_count = std::max(1, ParseInt(value(), 1));
    } else if (argument == "--port") {
      options.port = static_cast<std::uint16_t>(ParseInt(value(), 0));
    } else if (argument == "--summary") {
      options.summary_path = value();
    } else if (argument == "--summary-state") {
      options.summary_state_path = value();
    } else if (argument == "--long-timeout") {
      options.long_timeout_ms = ParseInt(value(), options.long_timeout_ms);
    } else if (argument == "--update-expectations") {
      options.update_expectations = true;
    } else if (argument == "--list") {
      options.list_only = true;
    } else if (argument == "--refresh-manifest") {
      options.refresh_manifest = true;
    } else if (argument == "--serve") {
      options.serve_only = true;
    } else if (argument == "--verbose" || argument == "-v") {
      options.verbose = true;
    } else if (argument == "--reftest-artifacts") {
      options.reftest_artifacts_dir = value();
    } else if (argument == "--reftest-artifacts-limit") {
      options.reftest_artifacts_limit = ParseInt(value(), options.reftest_artifacts_limit);
    } else if (argument == "--testharness-only") {
      options.testharness_only = true;
    } else if (argument == "--reftests-only") {
      options.reftests_only = true;
    } else if (argument == "--help" || argument == "-h") {
      std::fputs(kUsage, stdout);
      return 0;
    } else if (!argument.empty() && argument.front() == '-') {
      std::fprintf(stderr, "unknown option: %.*s\n\n", static_cast<int>(argument.size()),
                   argument.data());
      std::fputs(kUsage, stderr);
      return 2;
    } else {
      options.prefixes.emplace_back(argument);
    }
  }

  if (!std::filesystem::exists(options.wpt_root)) {
    std::fprintf(stderr,
                 "%s does not exist.\n\nRun tools/wpt/fetch.sh to check web-platform-tests out.\n",
                 options.wpt_root.c_str());
    return 2;
  }

  ServerOptions server_options;
  server_options.wpt_root = std::filesystem::absolute(options.wpt_root).string();
  server_options.harness_overrides_dir =
      (std::filesystem::path(MICROBROWSER_SOURCE_ROOT) / "tools" / "wpt" / "harness").string();
  server_options.verbose = options.verbose;
  server_options.timeout_multiplier = options.timeout_multiplier;
  server_options.ports = {options.port,
                          static_cast<std::uint16_t>(options.port == 0 ? 0 : options.port + 1)};
  Server server{server_options};
  if (!server.Bind()) {
    std::fprintf(stderr, "could not start the test server: %s\n", server.Error().c_str());
    return 2;
  }
  const std::string origin = server.Origin(0);

  if (options.serve_only) {
    std::fprintf(stderr, "serving %s at %s (and %s)\n", server_options.wpt_root.c_str(),
                 origin.c_str(), server.Origin(1).c_str());
    server.Serve();
    return 0;
  }

  std::string error;
  std::vector<WptTest> tests =
      microbrowser::wpt::EnumerateTests(options.wpt_root, options.prefixes,
                                        options.refresh_manifest, &error);
  if (!error.empty()) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  if (options.testharness_only) {
    std::erase_if(tests, [](const WptTest& test) { return test.kind != TestKind::Testharness; });
  }
  if (options.reftests_only) {
    std::erase_if(tests, [](const WptTest& test) { return test.kind != TestKind::Reftest; });
  }
  if (options.shard_count > 1) {
    std::vector<WptTest> slice;
    for (std::size_t index = 0; index < tests.size(); ++index) {
      if (static_cast<int>(index % static_cast<std::size_t>(options.shard_count)) ==
          options.shard_index) {
        slice.push_back(std::move(tests[index]));
      }
    }
    tests = std::move(slice);
  }

  ExpectationStore expectations;
  expectations.Load(options.expectations_dir);

  if (options.list_only) {
    for (const WptTest& test : tests) {
      // The tolerance is printed under --verbose, because it is the only way to
      // check that a `<meta name=fuzzy>` was read at all without running the
      // test, and a tolerance silently dropped looks exactly like a browser
      // bug. **Under --verbose only**: `tools/wpt/firefox-gap.py` reads this
      // list as `kind<space>path` and takes everything after the first field as
      // the path, so an unconditional third column would silently rename 686
      // reftests and drop them out of the join with Firefox's results. Teaching
      // that reader to stop at the first space instead is *not* the fix -- 37
      // paths in the checkout contain one.
      const std::string fuzzy =
          options.verbose ? microbrowser::wpt::SerializeFuzzy(test.fuzzy) : std::string();
      std::printf("%s %s%s\n", test.kind == TestKind::Reftest ? "reftest    " : "testharness",
                  test.url_path.c_str(), fuzzy.empty() ? "" : ("  fuzzy=" + fuzzy).c_str());
    }
    std::fprintf(stderr, "%zu tests\n", tests.size());
    return 0;
  }
  if (tests.empty()) {
    std::fprintf(stderr, "no tests matched\n");
    return 1;
  }
  if (!options.reftest_artifacts_dir.empty()) {
    std::error_code code;
    std::filesystem::create_directories(options.reftest_artifacts_dir, code);
    if (code) {
      std::fprintf(stderr, "could not create %s: %s\n", options.reftest_artifacts_dir.c_str(),
                   code.message().c_str());
      return 2;
    }
  }

  // The server is forked before anything else is created, and before the font
  // catalog is scanned: it is a file server and has no business owning either.
  const pid_t server_pid = ::fork();
  if (server_pid == 0) {
    server.Serve();
    ::_exit(0);
  }
  if (server_pid < 0) {
    std::fprintf(stderr, "fork for the server failed: %s\n", strerror(errno));
    return 2;
  }

  // Scanned once, in the parent, and inherited by every test process. A scan
  // per test would be most of the run.
  microbrowser::gfx::FontLibrary font_library;
  microbrowser::platform::SystemFontProvider fonts{font_library};
  fonts.Scan();
  microbrowser::gfx::TextRenderer text{fonts};

  int jobs = options.jobs;
  if (jobs <= 0) {
    // **Half the cores, and the temptation to raise it is a trap with a
    // measurement attached.** A WPT run looks like it should scale far past the
    // core count: 6,930 of the suite's 23,146 tests are expected to TIMEOUT,
    // and a test that times out is a browser sitting in `poll`. On a
    // timeout-heavy area that is exactly true -- `websockets/` (702 tests) on a
    // 24-core machine went 461.9s wall at 12 jobs to 120.5s at 64, with CPU
    // flat at ~230s.
    //
    // On a CPU-bound area it is catastrophic, and quietly.
    // `encoding/legacy-mb-japanese/` (482 tests), same machine:
    //
    //     jobs=12   104.2s wall   605.0s cpu   441,614 subtests    18 timeouts
    //     jobs=64    60.6s wall   106.9s cpu    32,929 subtests   447 timeouts
    //
    // Twice as fast, and it reports **100.0% passing** against 97.8%. It is not
    // faster and it is not passing: 408,685 subtests were deleted from the
    // measurement. testharness.js gives up after ten seconds of *wall clock*,
    // so oversubscription turns a test that would report into one that dies
    // before `done()` -- and a test that dies early contributes zero subtests
    // to our denominator, which makes the pass rate go **up** as the run gets
    // worse. (The 441,614 matches the 447,722 docs/wpt-plan.md quotes for that
    // directory independently; 12 is the honest number.)
    //
    // This is the `--long-timeout` bug of 2026-08-16 in a new costume, and the
    // rule from it holds here: **compare the subtest count against the previous
    // record before believing a re-record.** `--jobs` is a flag; raise it by
    // hand for an interactive run over an area you know is timeout-bound, and
    // never for one that writes expectations.
    const long cores = ::sysconf(_SC_NPROCESSORS_ONLN);
    jobs = static_cast<int>(std::max<long>(1, cores / 2));
  }

  std::size_t next_test = 0;
  std::size_t completed = 0;
  std::size_t unexpected = 0;
  std::size_t disabled = 0;
  std::size_t crashes = 0;
  std::size_t timeouts = 0;
  std::size_t subtests_passed = 0;
  std::size_t subtests_total = 0;
  int artifacts_written = 0;
  std::vector<RunningTest> running;
  std::vector<std::string> failure_report;
  microbrowser::wpt::SummaryAccumulator summary;
  const auto started_at = std::chrono::steady_clock::now();

  // A result that disagrees with the expectation is re-run before it is
  // believed. 66 of 2,432 tests flipped between OK and TIMEOUT between two runs
  // of the *same binary* on this machine: twelve Debug-build engines on twelve
  // cores, and a test that legitimately finishes in nine seconds against a
  // ten-second in-page timeout is a coin toss. Retrying only a disagreement
  // costs nothing on a green run and is what makes the suite a gate rather than
  // a mood. It is not a way to hide a real intermittent -- `--retries 0` is how
  // you look for one.
  std::vector<std::size_t> retry_queue;
  std::vector<int> retries_left(tests.size(), options.retries);
  std::vector<bool> artifacts_counted(tests.size(), false);

  const auto finish = [&](std::size_t index, const Report& report) {
    const WptTest& test = tests[index];
    const TestExpectation* expected = expectations.Find(test.url_path);
    const Comparison first_look = Compare(test, report, expected);
    if (retries_left[index] > 0) {
      // Two reasons to run it again, and they are the same reason.
      const bool disagrees = first_look.outcome == Outcome::Unexpected &&
                             !options.update_expectations;
      // A TIMEOUT is the one status that is as much a property of the machine
      // as of the browser: the page's own harness gives up after ten seconds,
      // and a test that finishes in nine loses that race whenever the box is
      // busy. Recording one without a second look bakes the load average of the
      // afternoon into the expectations, and the next agent's first run is red
      // for a reason that has nothing to do with their change.
      const bool timed_out_while_recording =
          options.update_expectations && report.harness == "TIMEOUT";
      if (disagrees || timed_out_while_recording) {
        --retries_left[index];
        retry_queue.push_back(index);
        return;
      }
    }
    microbrowser::wpt::SummaryResult summary_result;
    summary_result.url_path = test.url_path;
    summary_result.harness = report.harness;
    summary_result.harness_message = report.harness_message;
    for (const auto& [name, status] : report.subtests) {
      ++subtests_total;
      ++summary_result.subtests_total;
      if (status == "PASS") {
        ++subtests_passed;
        ++summary_result.subtests_passed;
        continue;
      }
      // A subtest with no message is one the harness marked NOTRUN or TIMEOUT;
      // its status is the only thing it said, so that is what gets counted.
      const auto message = report.messages.find(name);
      summary_result.failure_messages.push_back(
          message == report.messages.end() || message->second.empty()
              ? status + " (no message)"
              : message->second);
    }
    if (!options.summary_path.empty()) {
      summary.Add(summary_result);
    }
    if (report.harness == "CRASH") {
      ++crashes;
    } else if (report.harness == "TIMEOUT") {
      ++timeouts;
    }
    const Comparison& comparison = first_look;
    if (options.update_expectations) {
      expectations.Set(test.url_path, comparison.observed);
    }
    if (comparison.outcome == Outcome::Unexpected && !options.update_expectations) {
      ++unexpected;
      if (!options.verbose) {
        // Under --verbose every result is already printed as it finishes;
        // collecting them a second time prints each failure twice.
        std::string block = test.url_path + "\n";
        for (const std::string& line : comparison.lines) {
          block += line + "\n";
        }
        failure_report.push_back(std::move(block));
      }
    }
    if (options.verbose) {
      std::printf("%-9s %s\n",
                  comparison.outcome == Outcome::Unexpected ? "UNEXPECTED" : "ok",
                  test.url_path.c_str());
      // A harness status that is not OK subsumes the subtests, so the only
      // thing that says *why* is its message -- and an expected TIMEOUT prints
      // no comparison lines at all. Without this, "the page never reported;
      // first script error: ..." is computed, escaped, sent down the pipe and
      // thrown away, and a session diagnosing 86 timeouts has to rebuild the
      // runner to see it.
      //
      // A message on an *OK* harness is printed too, and only a reftest ever
      // has one: it is how far inside its tolerance the pair landed, which is
      // the one number that says whether a passing reftest is passing by a
      // margin or by luck.
      if (!report.harness_message.empty()) {
        std::printf("  %s: %s\n", report.harness.c_str(), report.harness_message.c_str());
      }
      // And every subtest that did not pass, with the message that says why.
      //
      // The comparison below cannot show these, and it is the *expected*
      // failures that a session working an area needs: they are the work. Two
      // separate holes were behind that. A harness status that is not OK
      // subsumes its subtests in the expectation format, so a file that
      // reported 23 of 38 and then timed out printed one line saying TIMEOUT
      // and nothing about the fifteen. And a file whose harness is OK prints
      // only its *disagreements*, so a fully-expected 3-of-25 printed `ok` and
      // stopped -- which is a green light on the exact file you opened the
      // runner to read.
      //
      // The expectation file is still one line for a timeout, and still records
      // only failures (tests/wpt/expectations/README.md); this is the runner
      // being readable, not the format changing. Same argument the harness
      // message above won, and it is now three for three.
      for (const auto& [name, status] : report.subtests) {
        if (status == "PASS") {
          continue;
        }
        const auto message = report.messages.find(name);
        std::printf("  %s=%s%s%s\n", status.c_str(), name.c_str(),
                    message == report.messages.end() || message->second.empty() ? "" : " -- ",
                    message == report.messages.end() ? "" : message->second.c_str());
      }
      for (const std::string& line : comparison.lines) {
        std::printf("%s\n", line.c_str());
      }
    }
    ++completed;
    if (!options.verbose && (completed % 50 == 0 || completed == tests.size())) {
      std::fprintf(stderr, "\r%zu/%zu tests, %zu unexpected", completed, tests.size(), unexpected);
      std::fflush(stderr);
    }
  };

  while (next_test < tests.size() || !retry_queue.empty() || !running.empty()) {
    while ((next_test < tests.size() || !retry_queue.empty()) &&
           static_cast<int>(running.size()) < jobs) {
      std::size_t index = 0;
      if (!retry_queue.empty()) {
        index = retry_queue.back();
        retry_queue.pop_back();
      } else {
        index = next_test++;
      }
      const WptTest& test = tests[index];
      const TestExpectation* expected = expectations.Find(test.url_path);
      if (expected != nullptr && expected->disabled) {
        ++disabled;
        ++completed;
        continue;
      }
      int pipes[2];
      if (::pipe(pipes) != 0) {
        std::fprintf(stderr, "pipe: %s\n", strerror(errno));
        return 2;
      }
      // The read end only, and it is not optional. `poll` says a descriptor is
      // readable; it does not say how much. Reading in a loop until zero on a
      // *blocking* pipe parks the whole runner inside the second `read` of a
      // child that is still working -- eleven zombies, one live child, and a
      // parent that never reaps another. That is what the first full dom/ run
      // did. The write end stays blocking so a report larger than the pipe
      // buffer waits for the parent instead of being truncated.
      const int flags = ::fcntl(pipes[0], F_GETFL, 0);
      if (flags < 0 || ::fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        std::fprintf(stderr, "fcntl: %s\n", strerror(errno));
        return 2;
      }
      // The page's own harness deadline is what should fire, so the wall-clock
      // budget moves with the multiplier the page was given -- and it is that
      // deadline *plus a grace*, because the two are otherwise the same number
      // racing itself.
      //
      // testharness.js gives up after exactly `--timeout` milliseconds and then
      // reports: harness TIMEOUT, and every subtest with the status it actually
      // reached. Killing the page at the same instant threw all of that away and
      // recorded "the page never reported" instead -- which is a different
      // claim, and a false one. 86 of dom/nodes' 327 tests were in that state,
      // and one of them (Comment-constructor) had eleven passing subtests behind
      // a single async_test waiting on an iframe.
      //
      // The cost is bounded and paid only by a test that really does hang: the
      // grace is a few seconds on top of a deadline that already elapsed.
      constexpr int kReportGraceMs = 5000;
      const int budget_ms =
          (test.long_timeout ? options.long_timeout_ms : options.timeout_ms) *
              options.timeout_multiplier +
          kReportGraceMs;
      // Decided here, in the parent, because the children cannot see each
      // other's failures. A few over the limit is possible -- the children
      // already in flight when the budget runs out still have the directory --
      // and that is the right trade against a shared counter between processes.
      const bool may_write_artifacts = test.kind == TestKind::Reftest &&
                                       !options.reftest_artifacts_dir.empty() &&
                                       artifacts_written < options.reftest_artifacts_limit;
      const pid_t pid = ::fork();
      if (pid == 0) {
        ::close(pipes[0]);
        const std::string url = origin + "/" + test.url_path;
        std::string report;
        if (test.kind == TestKind::Reftest) {
          report = RunReftest(fonts, text, test, url, origin + "/" + test.reference,
                              may_write_artifacts ? options.reftest_artifacts_dir : std::string(),
                              budget_ms);
        } else {
          report = RunTestharness(fonts, url, budget_ms);
        }
        std::size_t written = 0;
        while (written < report.size()) {
          const ssize_t count =
              ::write(pipes[1], report.data() + written, report.size() - written);
          if (count <= 0) {
            break;
          }
          written += static_cast<std::size_t>(count);
        }
        ::close(pipes[1]);
        // `_exit` rather than `exit`: a child that ran atexit handlers would
        // flush the parent's buffers a second time and dump every counter the
        // engine collected, once per test.
        ::_exit(0);
      }
      ::close(pipes[1]);
      if (pid < 0) {
        std::fprintf(stderr, "fork: %s\n", strerror(errno));
        return 2;
      }
      RunningTest child;
      child.pid = pid;
      child.descriptor = pipes[0];
      child.index = index;
      // The wall-clock cap is generous against the in-page budget: the page's
      // own testharness timeout should be what fires, because it produces a
      // report and this produces a corpse.
      child.deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(budget_ms + 5000);
      child.may_write_artifacts = may_write_artifacts;
      running.push_back(std::move(child));
    }

    if (running.empty()) {
      continue;
    }
    std::vector<pollfd> descriptors;
    descriptors.reserve(running.size());
    for (const RunningTest& child : running) {
      descriptors.push_back(pollfd{child.descriptor, POLLIN, 0});
    }
    const int ready = ::poll(descriptors.data(), descriptors.size(), 100);
    if (ready < 0 && errno != EINTR) {
      std::fprintf(stderr, "poll: %s\n", strerror(errno));
      return 2;
    }

    const auto now = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < running.size();) {
      RunningTest& child = running[index];
      bool done = false;
      bool killed = false;
      if ((descriptors[index].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        char buffer[8192];
        while (true) {
          const ssize_t count = ::read(child.descriptor, buffer, sizeof(buffer));
          if (count > 0) {
            child.output.append(buffer, static_cast<std::size_t>(count));
            continue;
          }
          if (count == 0) {
            done = true;  // the child closed its end: it is finished
          }
          // count < 0 with EAGAIN means "nothing more right now", which is not
          // the same thing at all.
          break;
        }
      }
      if (!done && now >= child.deadline) {
        ::kill(child.pid, SIGKILL);
        killed = true;
        done = true;
      }
      if (!done) {
        ++index;
        continue;
      }
      int status = 0;
      ::waitpid(child.pid, &status, 0);
      ::close(child.descriptor);
      Report report;
      if (killed) {
        report.harness = "TIMEOUT";
        report.harness_message = "killed after the wall-clock budget";
      } else if (WIFSIGNALED(status)) {
        report.harness = "CRASH";
        report.harness_message = std::string("killed by signal ") + strsignal(WTERMSIG(status));
      } else if (child.output.empty()) {
        report.harness = "ERROR";
        report.harness_message = "the test process exited without a report";
      } else {
        report = ParseReport(child.output, server.Ports());
      }
      // Once per test rather than once per run of it: a disagreeing result is
      // re-run before it is believed, and the retry overwrites the same three
      // files, so counting both spends the budget at half the tests it names.
      if (child.may_write_artifacts && report.harness == "FAIL" &&
          !artifacts_counted[child.index]) {
        artifacts_counted[child.index] = true;
        ++artifacts_written;
        if (artifacts_written == options.reftest_artifacts_limit) {
          std::fprintf(stderr,
                       "\n%d reftest artifact sets written to %s; no more will be "
                       "(--reftest-artifacts-limit)\n",
                       artifacts_written, options.reftest_artifacts_dir.c_str());
        }
      }
      finish(child.index, report);
      running.erase(running.begin() + static_cast<std::ptrdiff_t>(index));
    }
  }

  ::kill(server_pid, SIGTERM);
  ::waitpid(server_pid, nullptr, 0);

  if (!options.verbose) {
    std::fprintf(stderr, "\n");
  }
  for (const std::string& block : failure_report) {
    std::printf("%s", block.c_str());
  }

  if (options.update_expectations) {
    std::string save_error;
    if (!expectations.Save(options.expectations_dir, &save_error)) {
      std::fprintf(stderr, "%s\n", save_error.c_str());
      return 2;
    }
    std::fprintf(stderr, "expectations updated in %s\n", options.expectations_dir.c_str());
  }

  // **`--summary-state` is honoured on its own**, without `--summary`, and it has to be: the flag
  // exists so a run that measures part of the suite can hand its counts to a later one, and the
  // *first* of a pair of shards has nothing to write a table from yet. It used to sit inside the
  // `--summary` branch, so a run given only the state path did the whole measurement and saved
  // none of it -- silently, with the flag accepted and the file never created. Three hours of
  // `dom/`, `fetch/`, `xhr/`, `encoding/` and thirty-four other areas came back and went nowhere.
  if (!options.summary_path.empty() || !options.summary_state_path.empty()) {
    // The revision the numbers came from, so a table in the repository can be
    // traced to the tests that produced it. A checkout somebody moved by hand
    // is why this is read from the pin rather than from the checkout.
    std::string revision = "unknown";
    std::ifstream revision_file(
        (std::filesystem::path(MICROBROWSER_SOURCE_ROOT) / "tools" / "wpt" / "REVISION").string());
    if (revision_file) {
      std::getline(revision_file, revision);
    }
    // The previous shards, if this is one. Loaded after the run so that every
    // area this run measured wins over whatever the file said about it.
    //
    // The state file is the summary document's *memory*: everything the
    // document says about an area this run did not touch comes from here and
    // from nowhere else. A run started without it therefore rewrites the
    // document down to the areas it ran, silently -- which is how a session
    // turned a 21,265-test table into a 1,959-test one. Refusing is wrong (the
    // first run of all has no state), so it says so, loudly, once.
    if (!options.summary_state_path.empty()) {
      if (!std::filesystem::exists(options.summary_state_path)) {
        std::fprintf(stderr,
                     "warning: %s does not exist, so the summary will describe only the areas "
                     "this run measured. Every other area's numbers are lost.\n",
                     options.summary_state_path.c_str());
      }
      summary.LoadState(options.summary_state_path);
    }
    std::string summary_error;
    if (!options.summary_state_path.empty() &&
        !summary.SaveState(options.summary_state_path, &summary_error)) {
      std::fprintf(stderr, "%s\n", summary_error.c_str());
      return 2;
    }
    if (!options.summary_path.empty()) {
      if (!summary.Write(options.summary_path, revision, &summary_error)) {
        std::fprintf(stderr, "%s\n", summary_error.c_str());
        return 2;
      }
      std::fprintf(stderr, "summary written to %s\n", options.summary_path.c_str());
    } else {
      std::fprintf(stderr, "summary state written to %s (no --summary, so no table)\n",
                   options.summary_state_path.c_str());
    }
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started_at)
                           .count();
  std::fprintf(stderr,
               "\n%zu tests in %lld ms: %zu subtests, %zu passed (%.1f%%), "
               "%zu crashes, %zu timeouts, %zu disabled\n",
               tests.size(), static_cast<long long>(elapsed), subtests_total, subtests_passed,
               subtests_total == 0 ? 0.0
                                   : 100.0 * static_cast<double>(subtests_passed) /
                                         static_cast<double>(subtests_total),
               crashes, timeouts, disabled);
  if (options.update_expectations) {
    return 0;
  }
  std::fprintf(stderr, "%zu unexpected results\n", unexpected);
  return unexpected == 0 ? 0 : 1;
}
