#include "util/StartupTrace.h"

namespace microbrowser::util {

TraceChannel& StartupTrace::Channel() {
  static TraceChannel channel("startup", "MICROBROWSER_STARTUP_TRACE", "MICROBROWSER_STARTUP_SUMMARY",
                              /*min_duration_env=*/nullptr);
  return channel;
}

}  // namespace microbrowser::util
