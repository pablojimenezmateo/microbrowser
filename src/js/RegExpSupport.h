#pragma once

#include <string>

#include "js/Heap.h"
#include "js/Value.h"

// The regex forms of the String methods that take a pattern.
//
// Module-private. `split`, `replace` and `replaceAll` are String methods and
// live with the rest of String.prototype, but their regex behaviour is the
// pattern engine's rather than the string library's -- so the branch is here
// and StringBuiltins delegates to it. Keeping it the other way round would put
// capture-group substitution in the file whose defining property is that it
// has none.

namespace microbrowser::js {

// Each of these is only called once the caller has established that `pattern`
// is a RegExp object, which `Interpreter::RegExpOf` is the question for.
Value RegExpSplit(NativeCall& call, const Value& pattern, const std::string& text,
                  const Value& limit);
// `all` forces every match to be replaced, which is what `replaceAll` means
// for a pattern that does not carry `g`.
Value RegExpReplace(NativeCall& call, const Value& pattern, const std::string& text,
                    const Value& replacement, bool all);

}  // namespace microbrowser::js
