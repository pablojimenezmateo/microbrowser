#include <chrono>
#include <vector>

#include "TestSupport.h"
#include "platform/DescriptorWait.h"
#include "app/EventDrainBudget.h"
#include "app/IdleWaitStrategy.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::tests {

using app::ChooseIdleWait;
using app::IdleWaitMode;
using app::IdleWaitState;

void RegisterIdleWaitStrategyTests(std::vector<TestCase>& tests) {
  // This is the test that guards the project's headline property. If it ever
  // starts failing in the "must block" direction, idle CPU has stopped being
  // zero and something is spinning.
  AddTest(tests, "IdleWait/FullyIdleBlocks", [] {
    const IdleWaitState state;
    const auto decision = ChooseIdleWait(state);
    Expect(decision.mode == IdleWaitMode::Wait,
           "with nothing pending and nothing scheduled the loop must block indefinitely");
  });

  AddTest(tests, "IdleWait/PendingRepaintPolls", [] {
    IdleWaitState state;
    state.repaint_pending = true;
    Expect(ChooseIdleWait(state).mode == IdleWaitMode::Poll,
           "a composed-but-unpresented frame must not wait for an input event");
  });

  AddTest(tests, "IdleWait/PendingMessagesPoll", [] {
    IdleWaitState state;
    state.messages_pending = true;
    Expect(ChooseIdleWait(state).mode == IdleWaitMode::Poll,
           "sleeping with a non-empty message queue is how a browser appears to hang");
  });

  AddTest(tests, "IdleWait/ScheduledWorkUsesATimedWait", [] {
    IdleWaitState state;
    state.next_deadline_ms = 250;
    const auto decision = ChooseIdleWait(state);
    Expect(decision.mode == IdleWaitMode::WaitTimeout, "a scheduled deadline needs a timed wait");
    ExpectEqInt(decision.timeout_ms, 250, "the timeout must be the deadline");
  });

  AddTest(tests, "IdleWait/ZeroDeadlineDoesNotBusySpin", [] {
    IdleWaitState state;
    state.next_deadline_ms = 0;
    const auto decision = ChooseIdleWait(state);
    Expect(decision.mode == IdleWaitMode::WaitTimeout, "an elapsed deadline still waits");
    ExpectEqInt(decision.timeout_ms, app::kMinimumTimeoutMs,
                "a zero timeout would turn the timed wait into a spin");
  });

  AddTest(tests, "IdleWait/PendingWorkOutranksASchedule", [] {
    IdleWaitState state;
    state.repaint_pending = true;
    state.next_deadline_ms = 500;
    Expect(ChooseIdleWait(state).mode == IdleWaitMode::Poll,
           "work that is ready now must not wait on a future deadline");
  });

  AddTest(tests, "IdleWait/HugeDeadlineDoesNotOverflow", [] {
    IdleWaitState state;
    state.next_deadline_ms = 0xFFFFFFFFu;
    const auto decision = ChooseIdleWait(state);
    Expect(decision.timeout_ms > 0, "a huge deadline must not wrap to a negative timeout");
  });

  // ADR 0011: a request in flight is something the loop sleeps *on*. These four
  // are the whole of the policy half of that decision -- the rest is the
  // platform actually waiting on what it is handed.
  AddTest(tests, "IdleWait/NoRequestsMeansNoDescriptorsToWatch", [] {
    const IdleWaitState state;
    Expect(!ChooseIdleWait(state).watch_descriptors,
           "an idle browser must not be handed anything to watch, or the wait it does is "
           "no longer a wait on input alone");
  });

  AddTest(tests, "IdleWait/OutstandingRequestStillBlocksIndefinitely", [] {
    const util::WaitDescriptor sockets[] = {{7, /*readable=*/true, /*writable=*/false}};
    IdleWaitState state;
    state.descriptors = sockets;
    const auto decision = ChooseIdleWait(state);
    Expect(decision.mode == IdleWaitMode::Wait,
           "a request with no deadline behind it must not turn the wait into a timed one: "
           "that is the shape polling arrives in");
    Expect(decision.watch_descriptors, "and the socket must be in the wait");
  });

  AddTest(tests, "IdleWait/ADeadlineAndASocketAreBothWaitedOn", [] {
    const util::WaitDescriptor sockets[] = {{7, /*readable=*/true, /*writable=*/false}};
    IdleWaitState state;
    state.descriptors = sockets;
    state.next_deadline_ms = 40;
    const auto decision = ChooseIdleWait(state);
    Expect(decision.mode == IdleWaitMode::WaitTimeout, "the timer still bounds the wait");
    ExpectEqInt(decision.timeout_ms, 40, "the timeout must be the deadline");
    Expect(decision.watch_descriptors,
           "a pending timer must not make the loop stop watching the socket -- that is the "
           "bug where a page with a setInterval loads at one resource per tick");
  });

  AddTest(tests, "IdleWait/PendingWorkOutranksASocket", [] {
    const util::WaitDescriptor sockets[] = {{7, /*readable=*/true, /*writable=*/false}};
    IdleWaitState state;
    state.descriptors = sockets;
    state.repaint_pending = true;
    const auto decision = ChooseIdleWait(state);
    Expect(decision.mode == IdleWaitMode::Poll, "a composed frame goes out first");
    Expect(!decision.watch_descriptors, "and a poll waits for nothing at all");
  });

  // The other half of the headline property, and the half that had no test.
  // `ChooseIdleWait` deciding to block is worth nothing if the thing it blocks
  // in returns immediately -- which is exactly what `WaitOnDescriptors` did
  // with an empty descriptor list until 2026-08-17. It ignored its timeout and
  // answered false, so a caller with no sockets open and a timer pending span.
  // Under the WPT runner that was 16.6 million turns of the pump loop in ten
  // seconds, one core at 100%, on a page doing nothing at all -- and roughly
  // 7,000 of the suite's tests are in exactly that state.
  //
  // Asserted as a *lower* bound on elapsed time, because sleeping longer than
  // asked is a loaded machine and sleeping not at all is the bug.
  AddTest(tests, "IdleWait/AnEmptyWaitStillHonoursItsTimeout", [] {
    const auto started = std::chrono::steady_clock::now();
    const bool ready = platform::WaitOnDescriptors({}, 50);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    Expect(!ready, "nothing was watched, so nothing became ready");
    Expect(elapsed >= 40,
           "a wait with nothing to watch must still wait: returning immediately is how a "
           "page with a pending timer and no sockets burns a core");
  });

  AddTest(tests, "IdleWait/AnEmptyWaitWithNoDeadlineDoesNotHang", [] {
    // A negative timeout means "no deadline", and with nothing to watch that is
    // a wait nothing could ever end. Answering at once is the only safe
    // reading; blocking would turn a caller's bug into a hung browser.
    const auto started = std::chrono::steady_clock::now();
    Expect(!platform::WaitOnDescriptors({}, -1), "nothing to watch, nothing ready");
    Expect(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
                   .count() < 1000,
           "and it must return rather than block forever");
  });

  AddTest(tests, "EventDrainBudget/YieldsOnlyWithAPendingRepaint", [] {
    Expect(!app::ShouldYieldEventDrain(10000, /*repaint_pending=*/false),
           "with nothing to show, draining the queue fully is correct");
    Expect(app::ShouldYieldEventDrain(app::kMaxEventsPerDrain, /*repaint_pending=*/true),
           "past the budget with a frame waiting, yield so the user sees it");
    Expect(!app::ShouldYieldEventDrain(1, /*repaint_pending=*/true),
           "a single event must not cost a round trip through render");
  });
}

}  // namespace microbrowser::tests
