#include <vector>

#include "TestSupport.h"
#include "util/Env.h"
#include "util/TraceChannel.h"

namespace microbrowser::tests {

namespace {

constexpr const char* kVar = "MICROBROWSER_TEST_ENV_FLAG";

}  // namespace

void RegisterEnvTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Env/UnsetAndEmptyAreTheSameThing", [] {
    Expect(util::EnvValue(kVar) == nullptr, "an unset variable must read as absent");
    Expect(!util::EnvFlagEnabled(kVar), "an unset variable must not enable anything");

    const ScopedEnvVar empty(kVar, "");
    Expect(util::EnvValue(kVar) == nullptr,
           "an empty variable must read as absent: `FOO= ./microbrowser` is how a shell user "
           "turns something off, and set-but-blank would surprise them");
    Expect(!util::EnvFlagEnabled(kVar), "an empty variable must not enable anything");
  });

  AddTest(tests, "Env/FalseyTokensDoNotEnable", [] {
    for (const char* value : {"0", "false", "FALSE", "No", "off"}) {
      const ScopedEnvVar set(kVar, value);
      Expect(!util::EnvFlagEnabled(kVar),
             std::string("'") + value + "' must not enable; falsey tokens are case-insensitive");
      Expect(util::EnvValue(kVar) != nullptr,
             "a falsey flag is still *set*; EnvValue reports presence, not truth");
    }
  });

  AddTest(tests, "Env/AnythingElseEnables", [] {
    // Deliberately permissive: these are developer-facing debug switches, and
    // refusing to turn on because someone wrote `on` instead of `1` wastes more
    // time than it saves. Anything a user can set gets a stricter parse.
    for (const char* value : {"1", "yes", "on", "true", "please", " "}) {
      const ScopedEnvVar set(kVar, value);
      Expect(util::EnvFlagEnabled(kVar), std::string("'") + value + "' must enable");
    }
  });

  AddTest(tests, "Env/NullAndEmptyNamesAreRejected", [] {
    // Reached when a channel is constructed with no env name for one of its
    // modes -- a nullptr here must be a false, not a getenv(nullptr).
    Expect(util::EnvValue(nullptr) == nullptr, "a null name must read as absent");
    Expect(util::EnvValue("") == nullptr, "an empty name must read as absent");
    Expect(!util::EnvFlagEnabled(nullptr), "a null name must not enable anything");
    Expect(!util::EnvFlagEnabled(""), "an empty name must not enable anything");
  });

  AddTest(tests, "TraceChannel/MinimumDurationUsesStrictLocaleIndependentParse", [] {
    {
      const ScopedEnvVar set(kVar, "1.5");
      util::TraceChannel channel("test", nullptr, nullptr, kVar);
      Expect(channel.MinimumDurationMs() == 1.5,
             "a valid duration is read as milliseconds");
    }
    {
      const ScopedEnvVar set(kVar, "1.5ms");
      util::TraceChannel channel("test", nullptr, nullptr, kVar);
      Expect(channel.MinimumDurationMs() == 0.0,
             "trailing garbage is rejected rather than parsed as a prefix");
    }
    {
      const ScopedEnvVar set(kVar, "-2");
      util::TraceChannel channel("test", nullptr, nullptr, kVar);
      Expect(channel.MinimumDurationMs() == 0.0, "negative durations clamp to zero");
    }
  });
}

}  // namespace microbrowser::tests
