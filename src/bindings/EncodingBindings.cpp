// `TextEncoder` / `TextDecoder` — the Encoding Standard's UTF-8 half.
//
// youtube's player builds PES keys and offline-cache blobs with
// `(new TextEncoder).encode(...)`. Without these names the path throws
// `Woffle: PES is undefined` (and Gal's setmediasrc catch records
// `fmt.unplayable`) while MSE still buffers. The survey counted 35 uses
// across the Gate targets; this is the platform surface, not a youtube shim.
//
// Only UTF-8 is implemented. Other labels refuse with RangeError — the
// Encoding Standard's answer, and ADR 0012's: a decoder that silently
// pretends every label is UTF-8 is worse than an absence.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstring>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

constexpr const char* kEncoderMarker = "#isTextEncoder";
constexpr const char* kDecoderMarker = "#isTextDecoder";
constexpr const char* kDecoderFatal = "#textDecoderFatal";
constexpr const char* kDecoderIgnoreBom = "#textDecoderIgnoreBom";

bool IsEncoder(const Value& value) {
  if (!value.IsObject()) {
    return false;
  }
  const Value* marker = value.object->GetOwn(kEncoderMarker);
  return marker != nullptr && marker->type == js::ValueType::Boolean && marker->boolean;
}

bool IsDecoder(const Value& value) {
  if (!value.IsObject()) {
    return false;
  }
  const Value* marker = value.object->GetOwn(kDecoderMarker);
  return marker != nullptr && marker->type == js::ValueType::Boolean && marker->boolean;
}

// Encoding Standard § encode: unpaired UTF-16 surrogates become U+FFFD.
// Walks the engine's UTF-8/WTF-8 storage with util::DecodeUtf8 so bindings
// never reaches js/StringUnits (module-private).
std::string EncodeJsString(std::string_view text) {
  bool ascii = true;
  for (char ch : text) {
    if (static_cast<unsigned char>(ch) >= 0x80u) {
      ascii = false;
      break;
    }
  }
  if (ascii) {
    return std::string(text);
  }
  std::string out;
  out.reserve(text.size());
  std::size_t at = 0;
  while (at < text.size()) {
    std::uint32_t code = 0;
    if (util::DecodeUtf8(text, at, code)) {
      if (code >= 0xD800u && code <= 0xDFFFu) {
        util::AppendUtf8(out, 0xFFFDu);
      } else {
        util::AppendUtf8(out, code);
      }
      continue;
    }
    ++at;
    util::AppendUtf8(out, 0xFFFDu);
  }
  return out;
}

Value BytesToUint8Array(js::Interpreter& interpreter, std::string_view body) {
  const Value* buffer_ctor = interpreter.GlobalScope()->Lookup("ArrayBuffer");
  const Value* view_ctor = interpreter.GlobalScope()->Lookup("Uint8Array");
  if (buffer_ctor == nullptr || view_ctor == nullptr) {
    return interpreter.MakeError("TypeError", "Uint8Array is unavailable");
  }
  const js::Result buffer =
      interpreter.ConstructValue(*buffer_ctor, {Value::Number(static_cast<double>(body.size()))});
  if (buffer.IsAbrupt() || !buffer.value.IsObject()) {
    return buffer.value;
  }
  const js::BufferView* view = buffer.value.object->View();
  if (view != nullptr && view->bytes != nullptr && view->bytes->size() >= body.size()) {
    if (!body.empty()) {
      std::memcpy(view->bytes->data(), body.data(), body.size());
    }
  }
  const js::Result array = interpreter.ConstructValue(*view_ctor, {buffer.value});
  if (array.IsAbrupt() || !array.value.IsObject()) {
    return array.value;
  }
  return array.value;
}

bool CopyBufferBytes(const Value& value, std::string& out) {
  if (value.IsUndefined() || value.IsNull()) {
    out.clear();
    return true;
  }
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

// WHATWG UTF-8 decode with replacement (or fatal). Returns false on fatal error.
bool DecodeUtf8Bytes(std::string_view bytes, bool fatal, bool ignore_bom, std::string& out) {
  out.clear();
  out.reserve(bytes.size());
  std::size_t at = 0;
  if (!ignore_bom && bytes.size() >= 3 &&
      static_cast<unsigned char>(bytes[0]) == 0xEFu &&
      static_cast<unsigned char>(bytes[1]) == 0xBBu &&
      static_cast<unsigned char>(bytes[2]) == 0xBFu) {
    at = 3;
  }
  while (at < bytes.size()) {
    std::uint32_t code = 0;
    const std::size_t before = at;
    if (util::DecodeUtf8(bytes, at, code) &&
        !(code >= 0xD800u && code <= 0xDFFFu) && code <= 0x10FFFFu) {
      // Reject overlong forms DecodeUtf8 does not already reject by walking
      // the shortest encoding: util::DecodeUtf8 accepts any structurally
      // valid sequence, including overlong. Re-check the width.
      const std::size_t width = at - before;
      const std::size_t expected = code < 0x80u ? 1 : code < 0x800u ? 2 : code < 0x10000u ? 3 : 4;
      if (width == expected) {
        util::AppendUtf8(out, code);
        continue;
      }
      at = before;
    }
    if (fatal) {
      return false;
    }
    // Replacement: consume one byte and emit U+FFFD.
    ++at;
    util::AppendUtf8(out, 0xFFFDu);
  }
  return true;
}

std::string NormalizeLabel(std::string_view label) {
  std::string out;
  out.reserve(label.size());
  // Trim ASCII whitespace, lowercase.
  std::size_t begin = 0;
  while (begin < label.size() &&
         (label[begin] == ' ' || label[begin] == '\t' || label[begin] == '\n' ||
          label[begin] == '\r' || label[begin] == '\f')) {
    ++begin;
  }
  std::size_t end = label.size();
  while (end > begin && (label[end - 1] == ' ' || label[end - 1] == '\t' || label[end - 1] == '\n' ||
                         label[end - 1] == '\r' || label[end - 1] == '\f')) {
    --end;
  }
  for (std::size_t i = begin; i < end; ++i) {
    const unsigned char c = static_cast<unsigned char>(label[i]);
    out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  }
  if (out == "utf8" || out == "unicode-1-1-utf-8" || out == "unicode11utf8" ||
      out == "unicode20utf8" || out == "x-unicode20utf8") {
    return "utf-8";
  }
  return out;
}

}  // namespace

void DomBindings::InstallTextEncoding() {
  if (interpreter_ == nullptr) {
    return;
  }
  if (interpreter_->GlobalScope()->Lookup("TextEncoder") != nullptr) {
    return;
  }

  const Value encoder_prototype = interpreter_->NewObjectValue();
  const Value encoder_ctor = interpreter_->NewNativeValue("TextEncoder", [encoder_prototype](NativeCall& call) -> Value {
    const Value object = call.interpreter.NewObjectValue();
    if (!object.IsObject()) {
      return Value::Undefined();
    }
    if (encoder_prototype.IsObject()) {
      object.object->SetPrototype(encoder_prototype.object);
    }
    object.object->SetHidden(kEncoderMarker, Value::Bool(true));
    util::AddPerformanceCounter(util::PerfCounterId::EncodingTextEncoderConstructed);
    return object;
  });
  if (!encoder_ctor.IsObject() || !encoder_prototype.IsObject()) {
    return;
  }
  encoder_ctor.object->Set("prototype", encoder_prototype);
  encoder_prototype.object->Set("constructor", encoder_ctor);

  const Value encoding_getter = interpreter_->NewNativeValue("get encoding", [](NativeCall& call) -> Value {
    if (!IsEncoder(call.self) && !IsDecoder(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    return Value::String(std::string("utf-8"));
  });
  if (encoding_getter.IsObject()) {
    encoder_prototype.object->DefineAccessor("encoding", encoding_getter.object, nullptr);
  }

  const Value encode = interpreter_->NewNativeValue("encode", [](NativeCall& call) -> Value {
    if (!IsEncoder(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    std::string input;
    if (!Argument(call.arguments, 0).IsUndefined()) {
      input = js::ToString(Argument(call.arguments, 0));
    }
    const std::string bytes = EncodeJsString(input);
    util::AddPerformanceCounter(util::PerfCounterId::EncodingTextEncoderEncode);
    util::AddPerformanceCounter(util::PerfCounterId::EncodingTextEncoderBytes, bytes.size());
    return BytesToUint8Array(call.interpreter, bytes);
  });
  if (encode.IsObject()) {
    encoder_prototype.object->Set("encode", encode);
  }

  interpreter_->Global()->Set("TextEncoder", encoder_ctor);
  interpreter_->GlobalScope()->Declare("TextEncoder", encoder_ctor, false);

  const Value decoder_prototype = interpreter_->NewObjectValue();
  const Value decoder_ctor = interpreter_->NewNativeValue("TextDecoder", [decoder_prototype](NativeCall& call) -> Value {
    std::string label = "utf-8";
    if (!Argument(call.arguments, 0).IsUndefined()) {
      label = NormalizeLabel(js::ToString(Argument(call.arguments, 0)));
    }
    if (label != "utf-8") {
      return call.Throw("RangeError", "The encoding label provided ('" + label + "') is invalid.");
    }
    bool fatal = false;
    bool ignore_bom = false;
    if (Argument(call.arguments, 1).IsObject()) {
      const Value* fatal_value = Argument(call.arguments, 1).object->Get("fatal");
      if (fatal_value != nullptr) {
        fatal = js::ToBoolean(*fatal_value);
      }
      const Value* ignore = Argument(call.arguments, 1).object->Get("ignoreBOM");
      if (ignore != nullptr) {
        ignore_bom = js::ToBoolean(*ignore);
      }
    }
    const Value object = call.interpreter.NewObjectValue();
    if (!object.IsObject()) {
      return Value::Undefined();
    }
    if (decoder_prototype.IsObject()) {
      object.object->SetPrototype(decoder_prototype.object);
    }
    object.object->SetHidden(kDecoderMarker, Value::Bool(true));
    object.object->SetHidden(kDecoderFatal, Value::Bool(fatal));
    object.object->SetHidden(kDecoderIgnoreBom, Value::Bool(ignore_bom));
    util::AddPerformanceCounter(util::PerfCounterId::EncodingTextDecoderConstructed);
    return object;
  });
  if (!decoder_ctor.IsObject() || !decoder_prototype.IsObject()) {
    return;
  }
  decoder_ctor.object->Set("prototype", decoder_prototype);
  decoder_prototype.object->Set("constructor", decoder_ctor);

  if (encoding_getter.IsObject()) {
    decoder_prototype.object->DefineAccessor("encoding", encoding_getter.object, nullptr);
  }
  const Value fatal_getter = interpreter_->NewNativeValue("get fatal", [](NativeCall& call) -> Value {
    if (!IsDecoder(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    const Value* flag = call.self.object->GetOwn(kDecoderFatal);
    return flag == nullptr ? Value::Bool(false) : *flag;
  });
  if (fatal_getter.IsObject()) {
    decoder_prototype.object->DefineAccessor("fatal", fatal_getter.object, nullptr);
  }
  const Value ignore_getter = interpreter_->NewNativeValue("get ignoreBOM", [](NativeCall& call) -> Value {
    if (!IsDecoder(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    const Value* flag = call.self.object->GetOwn(kDecoderIgnoreBom);
    return flag == nullptr ? Value::Bool(false) : *flag;
  });
  if (ignore_getter.IsObject()) {
    decoder_prototype.object->DefineAccessor("ignoreBOM", ignore_getter.object, nullptr);
  }

  const Value decode = interpreter_->NewNativeValue("decode", [](NativeCall& call) -> Value {
    if (!IsDecoder(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    std::string bytes;
    if (!CopyBufferBytes(Argument(call.arguments, 0), bytes)) {
      return call.Throw("TypeError", "The provided value is not of type BufferSource");
    }
    const Value* fatal_flag = call.self.object->GetOwn(kDecoderFatal);
    const Value* ignore_flag = call.self.object->GetOwn(kDecoderIgnoreBom);
    const bool fatal = fatal_flag != nullptr && fatal_flag->type == js::ValueType::Boolean &&
                       fatal_flag->boolean;
    const bool ignore_bom = ignore_flag != nullptr && ignore_flag->type == js::ValueType::Boolean &&
                            ignore_flag->boolean;
    std::string text;
    if (!DecodeUtf8Bytes(bytes, fatal, ignore_bom, text)) {
      return call.Throw("TypeError", "The encoded data was not valid.");
    }
    util::AddPerformanceCounter(util::PerfCounterId::EncodingTextDecoderDecode);
    util::AddPerformanceCounter(util::PerfCounterId::EncodingTextDecoderBytes, bytes.size());
    return Value::String(std::move(text));
  });
  if (decode.IsObject()) {
    decoder_prototype.object->Set("decode", decode);
  }

  interpreter_->Global()->Set("TextDecoder", decoder_ctor);
  interpreter_->GlobalScope()->Declare("TextDecoder", decoder_ctor, false);
}

}  // namespace microbrowser::bindings
