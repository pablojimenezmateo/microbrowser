// `TextEncoder` / `TextDecoder` — the Encoding Standard's API — and
// `btoa` / `atob`, the Window binary-string Base64 pair.
//
// youtube's player builds PES keys and offline-cache blobs with
// `(new TextEncoder).encode(...)`. Without those names the path threw
// `Woffle: PES is undefined`. Separately, watch still throws
// `ReferenceError: btoa is not defined` dozens of times per load (measured
// under MICROBROWSER_JS_THROWS); the bytes already live in util::Base64.
//
// TextDecoder uses the encodings `html::EncodingFromLabel` already knows.
// Refusing every label but UTF-8 was honest while those decoders did not
// exist; they do now, and a second decoder here would be a second answer
// to "what does this label mean". Labels this browser does not have still
// throw RangeError — ADR 0012: a decoder that silently pretends every
// label is UTF-8 is worse than an absence.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "html/Encoding.h"
#include "js/StringUnits.h"
#include "util/Base64.h"
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
constexpr const char* kDecoderEncoding = "#textDecoderEncoding";
constexpr const char* kDecoderEncodingName = "#textDecoderEncodingName";
constexpr const char* kDecoderLeftover = "#textDecoderLeftover";
constexpr const char* kDecoderBomSeen = "#textDecoderBomSeen";
constexpr const char* kDecoderDoNotFlush = "#textDecoderDoNotFlush";

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

// Encoding Standard § encodeInto: walk UTF-16 code units, emit UTF-8, stop
// before a character that does not fit. `read` is code units, `written` is
// bytes. An unpaired surrogate becomes U+FFFD (three bytes, one unit).
void EncodeIntoUtf8(std::string_view text, std::uint8_t* dest, std::size_t dest_len,
                    std::size_t& read, std::size_t& written) {
  read = 0;
  written = 0;
  const std::size_t units = js::Utf16Length(text);
  std::size_t unit = 0;
  while (unit < units) {
    std::uint32_t code = js::CodePointAt(text, unit);
    std::size_t consumed = 1;
    if (code >= 0x10000u) {
      consumed = 2;
    } else if (code >= 0xD800u && code <= 0xDFFFu) {
      code = 0xFFFDu;
    }
    std::string encoded;
    util::AppendUtf8(encoded, code);
    if (written + encoded.size() > dest_len) {
      break;
    }
    if (dest != nullptr && !encoded.empty()) {
      std::memcpy(dest + written, encoded.data(), encoded.size());
    }
    written += encoded.size();
    unit += consumed;
  }
  read = unit;
}

html::Encoding EncodingOfDecoder(const Value& self) {
  const Value* stored = self.IsObject() ? self.object->GetOwn(kDecoderEncoding) : nullptr;
  if (stored == nullptr || stored->type != js::ValueType::Number) {
    return html::Encoding::Utf8;
  }
  return static_cast<html::Encoding>(static_cast<std::uint8_t>(stored->number));
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

bool HiddenBool(const Value& self, const char* key) {
  if (!self.IsObject()) {
    return false;
  }
  const Value* flag = self.object->GetOwn(key);
  return flag != nullptr && flag->type == js::ValueType::Boolean && flag->boolean;
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
    if (IsDecoder(call.self)) {
      const Value* name = call.self.object->GetOwn(kDecoderEncodingName);
      if (name != nullptr && name->IsString()) {
        return *name;
      }
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

  const Value encode_into = interpreter_->NewNativeValue("encodeInto", [](NativeCall& call) -> Value {
    if (!IsEncoder(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    std::string input;
    if (!Argument(call.arguments, 0).IsUndefined()) {
      input = js::ToString(Argument(call.arguments, 0));
    }
    const Value dest = Argument(call.arguments, 1);
    if (!dest.IsObject() || dest.object->GetKind() != js::Object::Kind::TypedArray) {
      return call.Throw("TypeError", "The provided value is not of type Uint8Array");
    }
    const js::BufferView* view = dest.object->View();
    if (view == nullptr || view->kind != js::ElementKind::Uint8) {
      return call.Throw("TypeError", "The provided value is not of type Uint8Array");
    }
    std::size_t read = 0;
    std::size_t written = 0;
    if (view->bytes != nullptr) {
      const std::size_t dest_len = view->length;
      std::uint8_t* dest_bytes = view->bytes->data() + view->offset;
      if (view->offset > view->bytes->size() ||
          dest_len > view->bytes->size() - view->offset) {
        dest_bytes = nullptr;
      } else {
        EncodeIntoUtf8(input, dest_bytes, dest_len, read, written);
      }
    }
    const Value result = call.interpreter.NewObjectValue();
    if (result.IsObject()) {
      result.object->Set("read", Value::Number(static_cast<double>(read)));
      result.object->Set("written", Value::Number(static_cast<double>(written)));
    }
    util::AddPerformanceCounter(util::PerfCounterId::EncodingTextEncoderEncode);
    util::AddPerformanceCounter(util::PerfCounterId::EncodingTextEncoderBytes, written);
    return result;
  });
  if (encode_into.IsObject()) {
    encoder_prototype.object->Set("encodeInto", encode_into);
  }

  interpreter_->Global()->Set("TextEncoder", encoder_ctor);
  interpreter_->GlobalScope()->Declare("TextEncoder", encoder_ctor, false);

  const Value decoder_prototype = interpreter_->NewObjectValue();
  const Value decoder_ctor = interpreter_->NewNativeValue("TextDecoder", [decoder_prototype](NativeCall& call) -> Value {
    html::Encoding encoding = html::Encoding::Utf8;
    if (!Argument(call.arguments, 0).IsUndefined()) {
      const std::string label = js::ToString(Argument(call.arguments, 0));
      const std::optional<html::Encoding> found = html::EncodingFromLabel(label);
      if (!found.has_value() || *found == html::Encoding::Replacement) {
        return call.Throw("RangeError",
                          "The encoding label provided ('" + label + "') is invalid.");
      }
      encoding = *found;
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
    object.object->SetHidden(kDecoderEncoding,
                             Value::Number(static_cast<double>(static_cast<std::uint8_t>(encoding))));
    object.object->SetHidden(kDecoderEncodingName,
                             Value::String(util::AsciiLowerCase(html::EncodingName(encoding))));
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
    const bool fatal = HiddenBool(call.self, kDecoderFatal);
    const bool ignore_bom = HiddenBool(call.self, kDecoderIgnoreBom);
    const html::Encoding encoding = EncodingOfDecoder(call.self);

    // Encoding Standard: if the previous call was a flush, this one starts a new decoder
    // instance -- leftover and BOM-seen go. A stream:true call keeps both.
    std::string leftover;
    if (HiddenBool(call.self, kDecoderDoNotFlush)) {
      const Value* stored = call.self.object->GetOwn(kDecoderLeftover);
      if (stored != nullptr) {
        (void)CopyBufferBytes(*stored, leftover);
      }
    } else {
      call.self.object->SetHidden(kDecoderBomSeen, Value::Bool(false));
    }
    bool stream = false;
    if (Argument(call.arguments, 1).IsObject()) {
      const Value* stream_flag = Argument(call.arguments, 1).object->Get("stream");
      if (stream_flag != nullptr) {
        stream = js::ToBoolean(*stream_flag);
      }
    }
    call.self.object->SetHidden(kDecoderDoNotFlush, Value::Bool(stream));

    std::string text;
    if (!html::DecodeBytesStreaming(leftover, bytes, encoding, text, stream, fatal)) {
      // ISO-2022-JP is the one decoder whose state survives a streamed error -- Roman after
      // `ESC ( J` plus a bad byte is still Roman on the next call. The other encodings have
      // nothing to keep: a hole consumes its bytes and leftover is empty.
      if (stream && !leftover.empty()) {
        const Value held = BytesToUint8Array(call.interpreter, leftover);
        if (held.IsObject()) {
          call.self.object->SetHidden(kDecoderLeftover, held);
        }
      } else {
        call.self.object->SetHidden(kDecoderLeftover, Value::Undefined());
      }
      return call.Throw("TypeError", "The encoded data was not valid.");
    }
    if (leftover.empty()) {
      call.self.object->SetHidden(kDecoderLeftover, Value::Undefined());
    } else {
      const Value held = BytesToUint8Array(call.interpreter, leftover);
      if (held.IsObject()) {
        call.self.object->SetHidden(kDecoderLeftover, held);
      }
    }

    // serialize I/O queue: skip a leading U+FEFF once, for UTF-8 and UTF-16, unless ignoreBOM.
    if (!ignore_bom &&
        (encoding == html::Encoding::Utf8 || encoding == html::Encoding::Utf16Le ||
         encoding == html::Encoding::Utf16Be) &&
        !HiddenBool(call.self, kDecoderBomSeen) && !text.empty()) {
      call.self.object->SetHidden(kDecoderBomSeen, Value::Bool(true));
      if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEFu &&
          static_cast<unsigned char>(text[1]) == 0xBBu &&
          static_cast<unsigned char>(text[2]) == 0xBFu) {
        text.erase(0, 3);
      }
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

  // `btoa` / `atob`: Window's Latin-1 ↔ Base64 pair. They take and return
  // *binary strings* (one byte per UTF-16 code unit in 0..255), not UTF-8 —
  // which is why they are not TextEncoder aliases and why a code unit above
  // 255 is InvalidCharacterError rather than a multi-byte encoding.
  const Value btoa = interpreter_->NewNativeValue("btoa", [](NativeCall& call) -> Value {
    const std::string input = js::ToString(Argument(call.arguments, 0));
    std::string bytes;
    bytes.reserve(input.size());
    // Walk Unicode scalar values. `btoa` accepts U+0000..U+00FF only; our
    // strings are UTF-8, so U+00FF is two bytes on the wire and must still
    // round-trip as a single Latin-1 byte.
    for (std::size_t i = 0; i < input.size();) {
      const unsigned char lead = static_cast<unsigned char>(input[i]);
      std::uint32_t cp = 0;
      std::size_t width = 1;
      if (lead < 0x80) {
        cp = lead;
      } else if ((lead & 0xE0) == 0xC0 && i + 1 < input.size()) {
        cp = (lead & 0x1F) << 6 | (static_cast<unsigned char>(input[i + 1]) & 0x3F);
        width = 2;
      } else if ((lead & 0xF0) == 0xE0 && i + 2 < input.size()) {
        cp = static_cast<std::uint32_t>(lead & 0x0F) << 12 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]) & 0x3F) << 6 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]) & 0x3F);
        width = 3;
      } else if ((lead & 0xF8) == 0xF0 && i + 3 < input.size()) {
        cp = static_cast<std::uint32_t>(lead & 0x07) << 18 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]) & 0x3F) << 12 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]) & 0x3F) << 6 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 3]) & 0x3F);
        width = 4;
      } else {
        return ThrowDom(call, "InvalidCharacterError",
                          "InvalidCharacterError: btoa requires a Latin1 string");
      }
      if (cp > 0xFF) {
        return ThrowDom(call, "InvalidCharacterError",
                          "InvalidCharacterError: btoa requires a Latin1 string");
      }
      bytes.push_back(static_cast<char>(cp));
      i += width;
    }
    util::AddPerformanceCounter(util::PerfCounterId::EncodingBtoa);
    return Value::String(util::Base64Encode(bytes));
  });
  if (btoa.IsObject()) {
    interpreter_->Global()->Set("btoa", btoa);
    interpreter_->GlobalScope()->Declare("btoa", btoa, false);
  }

  const Value atob = interpreter_->NewNativeValue("atob", [](NativeCall& call) -> Value {
    const std::string input = js::ToString(Argument(call.arguments, 0));
    // HTML strips ASCII whitespace before decoding.
    std::string stripped;
    stripped.reserve(input.size());
    for (char c : input) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
        continue;
      }
      stripped.push_back(c);
    }
    const std::optional<std::string> decoded = util::Base64Decode(stripped);
    if (!decoded.has_value()) {
      return ThrowDom(call, "InvalidCharacterError",
                        "InvalidCharacterError: atob received an invalid character");
    }
    // Re-encode each byte as a Latin-1 code unit in a UTF-8 string (bytes
    // 0..127 stay one byte; 128..255 become two-byte UTF-8 sequences). Our
    // string type is UTF-8, so this is the portable form of a binary string.
    std::string latin1;
    latin1.reserve(decoded->size());
    for (char ch : *decoded) {
      const unsigned char byte = static_cast<unsigned char>(ch);
      if (byte < 0x80) {
        latin1.push_back(static_cast<char>(byte));
      } else {
        latin1.push_back(static_cast<char>(0xC0 | (byte >> 6)));
        latin1.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
      }
    }
    util::AddPerformanceCounter(util::PerfCounterId::EncodingAtob);
    return Value::String(std::move(latin1));
  });
  if (atob.IsObject()) {
    interpreter_->Global()->Set("atob", atob);
    interpreter_->GlobalScope()->Declare("atob", atob, false);
  }
}

}  // namespace microbrowser::bindings
