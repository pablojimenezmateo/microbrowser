#include <cstdio>

#include "app/AppStartupOptions.h"
#include "app/Application.h"
#include "util/TraceChannel.h"

int main(int argc, char** argv) {
  // Identify the thread whose latency the user feels, before anything else can
  // record a trace scope. Without this every ranked performance summary is
  // misleading in the same direction: background work outranks the stalls that
  // actually drop frames.
  microbrowser::util::MarkTracingMainThread();

  const microbrowser::app::AppStartupOptions options =
      microbrowser::app::ParseStartupOptions(argc, argv);
  if (options.should_exit) {
    std::fputs(options.message.c_str(), options.exit_code == 0 ? stdout : stderr);
    return options.exit_code;
  }

  microbrowser::app::Application application;
  return application.Run(options);
}
