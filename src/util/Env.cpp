#include "util/Env.h"

#include <cstdlib>

#include "util/StringUtil.h"

namespace microbrowser::util {

const char* EnvValue(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    return nullptr;
  }
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return nullptr;
  }
  return value;
}

bool EnvFlagEnabled(const char* name) {
  const char* value = EnvValue(name);
  return value != nullptr && !IsFalseyToken(value);
}

}  // namespace microbrowser::util
