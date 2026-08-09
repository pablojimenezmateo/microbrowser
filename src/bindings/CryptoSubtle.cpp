// `crypto.subtle` — the Web Crypto subset youtube's PES path needs.
//
// Player `au()` requires `importKey`, `sign` and `encrypt` on `window.crypto.subtle`
// before it hands AES-CTR / HMAC to the offline encoder. Without them Woffle
// reports `PES is undefined` (handleError prefix + missing encoder) while MSE
// still buffers. Survey: 2 uses; ADR 0029 already owns `getRandomValues`.
//
// Surface here is deliberately narrow: raw `importKey` for AES-CTR (128-bit
// keys) and HMAC-SHA-256, `encrypt` (AES-CTR), `sign` (HMAC-SHA-256). Everything
// else stays absent — ADR 0012.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/FetchSupport.h"
#include "util/AesCtr.h"
#include "util/Hmac.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

constexpr const char* kKeyMarker = "#isCryptoKey";
constexpr const char* kKeyAlg = "#cryptoKeyAlg";
constexpr const char* kKeyRaw = "#cryptoKeyRaw";

bool CopyBufferBytes(const Value& value, std::string& out) {
  if (!value.IsObject()) {
    return false;
  }
  const js::BufferView* view = value.object->View();
  if (view == nullptr || view->bytes == nullptr) {
    return false;
  }
  const std::size_t element_size = js::ElementSize(view->kind);
  const std::size_t byte_length = view->length * element_size;
  if (view->offset > view->bytes->size() || byte_length > view->bytes->size() - view->offset) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(view->bytes->data() + view->offset), byte_length);
  return true;
}

Value BytesToArrayBuffer(js::Interpreter& interpreter, std::string_view body) {
  const Value* buffer_ctor = interpreter.GlobalScope()->Lookup("ArrayBuffer");
  if (buffer_ctor == nullptr) {
    return interpreter.MakeError("TypeError", "ArrayBuffer is unavailable");
  }
  const js::Result buffer =
      interpreter.ConstructValue(*buffer_ctor, {Value::Number(static_cast<double>(body.size()))});
  if (buffer.IsAbrupt() || !buffer.value.IsObject()) {
    return buffer.value;
  }
  const js::BufferView* view = buffer.value.object->View();
  if (view != nullptr && view->bytes != nullptr && view->bytes->size() >= body.size() &&
      !body.empty()) {
    std::memcpy(view->bytes->data(), body.data(), body.size());
  }
  return buffer.value;
}

bool IsCryptoKey(const Value& value) {
  if (!value.IsObject()) {
    return false;
  }
  const Value* marker = value.object->GetOwn(kKeyMarker);
  return marker != nullptr && marker->type == js::ValueType::Boolean && marker->boolean;
}

std::string AlgorithmName(const Value& algorithm) {
  if (algorithm.IsString()) {
    return algorithm.AsString();
  }
  if (algorithm.IsObject()) {
    const Value* name = algorithm.object->Get("name");
    if (name != nullptr) {
      return js::ToString(*name);
    }
  }
  return {};
}

std::string NormalizeAlg(std::string name) {
  for (char& c : name) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 32);
    }
  }
  return name;
}

Value Reject(js::Interpreter& interpreter, std::string_view kind, std::string message) {
  return SettledPromise(interpreter, interpreter.MakeError(kind, std::move(message)), true);
}

Value Fulfill(js::Interpreter& interpreter, const Value& value) {
  return SettledPromise(interpreter, value, false);
}

}  // namespace

void DomBindings::InstallSubtleCrypto(const js::Value& crypto) {
  if (interpreter_ == nullptr || !crypto.IsObject()) {
    return;
  }
  const Value subtle = interpreter_->NewObjectValue();
  if (!subtle.IsObject()) {
    return;
  }

  const Value key_prototype = interpreter_->NewObjectValue();

  const Value import_key = interpreter_->NewNativeValue(
      "importKey", [key_prototype](NativeCall& call) -> Value {
        const std::string format = js::ToString(Argument(call.arguments, 0));
        if (format != "raw") {
          return Reject(call.interpreter, "Error",
                        "NotSupportedError: only raw importKey is implemented");
        }
        std::string key_bytes;
        if (!CopyBufferBytes(Argument(call.arguments, 1), key_bytes)) {
          return Reject(call.interpreter, "TypeError",
                        "importKey keyData must be a BufferSource");
        }
        const std::string alg = NormalizeAlg(AlgorithmName(Argument(call.arguments, 2)));
        if (alg != "AES-CTR" && alg != "HMAC") {
          return Reject(call.interpreter, "Error",
                        "NotSupportedError: algorithm is not supported");
        }
        if (alg == "AES-CTR" && key_bytes.size() != 16) {
          return Reject(call.interpreter, "Error",
                        "DataError: AES-CTR raw keys must be 128 bits");
        }
        if (alg == "HMAC") {
          const Value algorithm = Argument(call.arguments, 2);
          std::string hash_name = "SHA-256";
          if (algorithm.IsObject()) {
            if (const Value* hash = algorithm.object->Get("hash"); hash != nullptr) {
              if (hash->IsString()) {
                hash_name = NormalizeAlg(hash->AsString());
              } else {
                hash_name = NormalizeAlg(AlgorithmName(*hash));
              }
            }
          }
          if (hash_name != "SHA-256") {
            return Reject(call.interpreter, "Error",
                          "NotSupportedError: only HMAC SHA-256 is implemented");
          }
        }
        const Value key = call.interpreter.NewObjectValue();
        if (!key.IsObject()) {
          return Reject(call.interpreter, "Error", "OperationError: out of memory");
        }
        if (key_prototype.IsObject()) {
          key.object->SetPrototype(key_prototype.object);
        }
        key.object->SetHidden(kKeyMarker, Value::Bool(true));
        key.object->SetHidden(kKeyAlg, Value::String(alg == "AES-CTR" ? "AES-CTR" : "HMAC"));
        key.object->SetHidden(kKeyRaw, Value::String(std::move(key_bytes)));
        key.object->Set("type", Value::String(std::string("secret")));
        key.object->Set("extractable", Value::Bool(js::ToBoolean(Argument(call.arguments, 3))));
        util::AddPerformanceCounter(util::PerfCounterId::CryptoSubtleImportKey);
        return Fulfill(call.interpreter, key);
      });
  if (import_key.IsObject()) {
    subtle.object->Set("importKey", import_key);
  }

  const Value encrypt = interpreter_->NewNativeValue("encrypt", [](NativeCall& call) -> Value {
    const std::string alg = NormalizeAlg(AlgorithmName(Argument(call.arguments, 0)));
    if (alg != "AES-CTR") {
      return Reject(call.interpreter, "Error",
                    "NotSupportedError: only AES-CTR encrypt is implemented");
    }
    const Value key = Argument(call.arguments, 1);
    if (!IsCryptoKey(key)) {
      return Reject(call.interpreter, "TypeError", "encrypt requires a CryptoKey");
    }
    const Value* key_alg = key.object->GetOwn(kKeyAlg);
    if (key_alg == nullptr || !key_alg->IsString() || key_alg->AsString() != "AES-CTR") {
      return Reject(call.interpreter, "Error", "InvalidAccessError: key algorithm mismatch");
    }
    const Value* raw = key.object->GetOwn(kKeyRaw);
    if (raw == nullptr || !raw->IsString() || raw->AsString().size() != 16) {
      return Reject(call.interpreter, "Error", "DataError: invalid AES key");
    }
    const Value algorithm = Argument(call.arguments, 0);
    if (!algorithm.IsObject()) {
      return Reject(call.interpreter, "TypeError", "AES-CTR encrypt needs an algorithm object");
    }
    const Value* counter_value = algorithm.object->Get("counter");
    std::string counter_bytes;
    if (counter_value == nullptr || !CopyBufferBytes(*counter_value, counter_bytes) ||
        counter_bytes.size() != 16) {
      return Reject(call.interpreter, "Error",
                    "DataError: AES-CTR counter must be 16 bytes");
    }
    // Web Crypto's `length` is the number of bits that increment. youtube passes 128.
    double length_bits = 128.0;
    if (const Value* length = algorithm.object->Get("length"); length != nullptr) {
      length_bits = js::ToNumber(*length);
    }
    if (length_bits != 128.0) {
      return Reject(call.interpreter, "Error",
                    "NotSupportedError: only AES-CTR length 128 is implemented");
    }
    std::string data;
    if (!CopyBufferBytes(Argument(call.arguments, 2), data)) {
      return Reject(call.interpreter, "TypeError", "encrypt data must be a BufferSource");
    }
    std::array<std::uint8_t, 16> key_arr{};
    std::array<std::uint8_t, 16> ctr_arr{};
    std::memcpy(key_arr.data(), raw->AsString().data(), 16);
    std::memcpy(ctr_arr.data(), counter_bytes.data(), 16);
    std::string out(data.size(), '\0');
    if (!util::Aes128CtrXor(
            std::span<const std::uint8_t, 16>(key_arr),
            std::span<const std::uint8_t, 16>(ctr_arr),
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()),
                                          data.size()),
            std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(out.data()), out.size()))) {
      return Reject(call.interpreter, "Error", "OperationError: AES-CTR failed");
    }
    util::AddPerformanceCounter(util::PerfCounterId::CryptoSubtleEncrypt);
    util::AddPerformanceCounter(util::PerfCounterId::CryptoSubtleEncryptBytes, out.size());
    return Fulfill(call.interpreter, BytesToArrayBuffer(call.interpreter, out));
  });
  if (encrypt.IsObject()) {
    subtle.object->Set("encrypt", encrypt);
  }

  const Value sign = interpreter_->NewNativeValue("sign", [](NativeCall& call) -> Value {
    const std::string alg = NormalizeAlg(AlgorithmName(Argument(call.arguments, 0)));
    if (alg != "HMAC") {
      return Reject(call.interpreter, "Error",
                    "NotSupportedError: only HMAC sign is implemented");
    }
    const Value key = Argument(call.arguments, 1);
    if (!IsCryptoKey(key)) {
      return Reject(call.interpreter, "TypeError", "sign requires a CryptoKey");
    }
    const Value* key_alg = key.object->GetOwn(kKeyAlg);
    if (key_alg == nullptr || !key_alg->IsString() || key_alg->AsString() != "HMAC") {
      return Reject(call.interpreter, "Error", "InvalidAccessError: key algorithm mismatch");
    }
    const Value* raw = key.object->GetOwn(kKeyRaw);
    if (raw == nullptr || !raw->IsString()) {
      return Reject(call.interpreter, "Error", "DataError: invalid HMAC key");
    }
    std::string data;
    if (!CopyBufferBytes(Argument(call.arguments, 2), data)) {
      return Reject(call.interpreter, "TypeError", "sign data must be a BufferSource");
    }
    const std::string mac = util::HmacSha256(raw->AsString(), data);
    util::AddPerformanceCounter(util::PerfCounterId::CryptoSubtleSign);
    return Fulfill(call.interpreter, BytesToArrayBuffer(call.interpreter, mac));
  });
  if (sign.IsObject()) {
    subtle.object->Set("sign", sign);
  }

  crypto.object->Set("subtle", subtle);
  util::AddPerformanceCounter(util::PerfCounterId::CryptoSubtleInstalled);
}

}  // namespace microbrowser::bindings
