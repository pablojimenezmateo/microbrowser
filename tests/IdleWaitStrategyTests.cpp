#include <vector>

#include "TestSupport.h"
#include "app/EventDrainBudget.h"
#include "app/IdleWaitStrategy.h"

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
