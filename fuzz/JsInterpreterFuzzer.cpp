#include <cstddef>
#include <cstdint>
#include <string_view>

#include "js/Interpreter.h"

// The whole JavaScript engine, fed arbitrary bytes.
//
// This is the surface where a hostile script meets a heap, and the properties
// that matter are the ones that keep a page from taking the browser with it:
//
//   1. Running terminates. `while (true) {}` is a step budget, unbounded
//      recursion is a RangeError, and neither is a hang -- the only difference
//      a user would notice is whether the browser comes back.
//   2. Every failure is a thrown value rather than a crash or a C++ exception
//      escaping into the host. Control flow is a value here precisely so that
//      there is nothing to escape.
//   3. A collection at the end frees everything the roots do not reach, and
//      then the interpreter is still usable -- a collector that frees a live
//      object is a use-after-free, and running afterwards is what would catch
//      it under ASan.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // A cap on the input, because the step budget is per-run and a megabyte of
  // distinct statements is slow without being interesting.
  if (size > 8192) {
    return 0;
  }
  const std::string_view source(reinterpret_cast<const char*>(data), size);

  microbrowser::js::Interpreter interpreter;
  const microbrowser::js::Result result = interpreter.Run(source);
  // Every completion has to be one of the five. A garbage value here would
  // mean a path returned an uninitialised Result.
  switch (result.completion) {
    case microbrowser::js::Completion::Normal:
    case microbrowser::js::Completion::Return:
    case microbrowser::js::Completion::Break:
    case microbrowser::js::Completion::Continue:
    case microbrowser::js::Completion::Throw:
      break;
    default:
      __builtin_trap();
  }

  // Converting the result must not crash whatever it is -- including an object
  // whose prototype chain is a cycle the script built.
  (void)microbrowser::js::ToString(result.value);
  (void)microbrowser::js::ToBoolean(result.value);

  interpreter.GetHeap().Collect({interpreter.Global()}, {interpreter.GlobalScope()});
  // Still usable afterwards. If the collector freed something reachable, this
  // is where ASan sees it.
  const microbrowser::js::Result again = interpreter.Run("1 + 1");
  if (again.completion == microbrowser::js::Completion::Normal &&
      microbrowser::js::ToString(again.value) != "2") {
    __builtin_trap();
  }
  return 0;
}
