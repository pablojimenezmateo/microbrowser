#pragma once

namespace microbrowser::util {

// The only place in the process that reads the environment.
//
// It is centralized for two reasons, and neither is tidiness.
//
// The first is that the environment is *input*. It arrives from whoever
// launched the process, it survives into every child, and code that reads it
// ad hoc is code that has quietly grown a configuration surface nobody
// reviewed. A browser that changes behavior based on an undocumented variable
// has a behavior an attacker who controls a launcher can select. Funnelling
// every read through one translation unit makes the whole surface greppable,
// and the architecture lint (`EnvironmentReadsAreCentralized`) keeps it that
// way.
//
// The second is that this was three implementations before it was one:
// PerformanceTrace, TraceChannel, and PerformanceCounters each had their own
// "is this flag on" that agreed by coincidence rather than by construction.
//
// Nothing here caches. Callers that read a flag on a hot path hold the result
// in a function-local static, so the cost is paid once and the decision about
// *when* it is paid stays visible at the call site.

// The variable's value, or nullptr when it is unset or empty. An empty variable
// means "unset" everywhere in this codebase: `FOO= ./microbrowser` is how a
// shell user turns something off, and treating it as set-but-blank surprises
// them.
const char* EnvValue(const char* name);

// True when the variable is set to anything that is not an explicitly falsey
// token (`0`, `false`, `no`, `off`, any case). So `FOO=1`, `FOO=yes`, and
// `FOO=please` all enable; `FOO=0` and `FOO=` do not.
//
// Deliberately not `IsTruthyToken`: these are developer-facing debug switches,
// and refusing to turn on because someone wrote `FOO=on` when the parser wanted
// `FOO=1` wastes more time than it prevents. Anything a *user* can set gets a
// stricter parse than this.
bool EnvFlagEnabled(const char* name);

}  // namespace microbrowser::util
