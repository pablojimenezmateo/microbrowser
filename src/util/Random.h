#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace microbrowser::util {

// Cryptographically strong random bytes, from the operating system.
//
// ADR 0029 §6, session 37. **The one entry on that ADR's answer table that is not reduced**, and the
// reason is that randomness carries no information *about* the machine: a page handed 32 random bytes
// learns nothing, and a page handed predictable ones has its session tokens guessed. Weakening this
// would be trading a privacy property for a security hole.
//
// So: `getrandom(2)` where it exists, `/dev/urandom` otherwise, and **never a fallback to a
// pseudo-random generator**. A failure fills nothing and says so, because the alternative -- quietly
// producing weak bytes -- is the failure mode that ships and is never noticed. Every caller checks.
//
// Not seeded, not stateful, and not a class: the kernel owns the pool, which is the whole reason to ask
// it rather than to keep a generator here. A userspace generator would need to be seeded (from what?),
// would need to be forked-safe, and would be one more thing holding entropy across a navigation.
[[nodiscard]] bool FillRandomBytes(std::span<std::uint8_t> out);

}  // namespace microbrowser::util
