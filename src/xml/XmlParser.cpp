#include "xml/XmlParser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microbrowser::xml {

namespace {

// Bounds. Every one of these is a refusal rather than a truncation: past any of
// them the parse fails and the caller gets the error document, which is the
// same thing a well-formedness error produces. Truncating would hand a page a
// tree that says something the input did not.
//
// Depth is 512 because `dom::Node`'s destructor recurses over its children:
// the *parse* is iterative and would happily build a million-deep tree, and
// then tearing it down would overflow the C++ stack in a destructor, where
// there is nowhere to report from. 512 nested elements is far past any document
// anyone writes and far short of where unwinding hurts.
constexpr std::size_t kMaxDepth = 512;
// A general entity table a page can drive from the internal subset. Small,
// because nothing legitimate declares thousands and the table is the only
// unbounded thing an internal subset can ask for.
constexpr std::size_t kMaxEntities = 256;
constexpr std::size_t kMaxEntityValueBytes = 1 << 16;

constexpr char32_t kReplacement = 0xFFFD;

// --- UTF-8, decoded before anything is classified ---------------------------
//
// A parser that looked at bytes would let the second byte of a truncated
// sequence stand in for a `<`. So every classification below goes through this,
// and anything ill-formed -- overlong, truncated, a surrogate code point, past
// U+10FFFF -- decodes as U+FFFD consuming exactly one byte, which is what makes
// the loop always advance.
struct Decoded {
  char32_t code = 0;
  std::size_t length = 1;
  bool valid = false;
};

Decoded DecodeUtf8(std::string_view text, std::size_t at) {
  Decoded out;
  if (at >= text.size()) {
    return out;
  }
  const auto byte = static_cast<unsigned char>(text[at]);
  if (byte < 0x80) {
    return Decoded{byte, 1, true};
  }
  std::size_t needed = 0;
  char32_t code = 0;
  if ((byte & 0xE0) == 0xC0) {
    needed = 1;
    code = byte & 0x1FU;
  } else if ((byte & 0xF0) == 0xE0) {
    needed = 2;
    code = byte & 0x0FU;
  } else if ((byte & 0xF8) == 0xF0) {
    needed = 3;
    code = byte & 0x07U;
  } else {
    return Decoded{kReplacement, 1, false};
  }
  if (at + needed >= text.size()) {
    return Decoded{kReplacement, 1, false};
  }
  for (std::size_t i = 1; i <= needed; ++i) {
    const auto continuation = static_cast<unsigned char>(text[at + i]);
    if ((continuation & 0xC0) != 0x80) {
      return Decoded{kReplacement, 1, false};
    }
    code = (code << 6) | (continuation & 0x3FU);
  }
  const bool overlong = (needed == 1 && code < 0x80) || (needed == 2 && code < 0x800) ||
                        (needed == 3 && code < 0x10000);
  if (overlong || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
    // A well-*formed* sequence carrying a value UTF-8 may not encode -- an
    // overlong form, a surrogate code point, anything past U+10FFFF -- is one
    // replacement character for the whole sequence, not one per byte. A lone
    // surrogate reaches here as three bytes, and every other engine turns it
    // into a single U+FFFD; per-byte would have produced three, which is a
    // string a page can tell apart.
    return Decoded{kReplacement, needed + 1, false};
  }
  return Decoded{code, needed + 1, true};
}

void AppendUtf8(std::string& out, char32_t code) {
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xC0 | (code >> 6));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xE0 | (code >> 12));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code >> 18));
    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

// XML 1.0 fifth edition, the three productions that matter. Written out rather
// than approximated by `isalpha`, because "which characters may start a name"
// is exactly the kind of thing an approximation gets wrong in the direction of
// accepting a document no other parser accepts.
bool IsXmlChar(char32_t c) {
  return c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) ||
         (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF);
}

bool IsNameStart(char32_t c) {
  return c == ':' || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) ||
         (c >= 0x370 && c <= 0x37D) || (c >= 0x37F && c <= 0x1FFF) ||
         (c >= 0x200C && c <= 0x200D) || (c >= 0x2070 && c <= 0x218F) ||
         (c >= 0x2C00 && c <= 0x2FEF) || (c >= 0x3001 && c <= 0xD7FF) ||
         (c >= 0xF900 && c <= 0xFDCF) || (c >= 0xFDF0 && c <= 0xFFFD) ||
         (c >= 0x10000 && c <= 0xEFFFF);
}

bool IsNameChar(char32_t c) {
  return IsNameStart(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7 ||
         (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

constexpr std::string_view kXmlNamespace = "http://www.w3.org/XML/1998/namespace";
constexpr std::string_view kXmlnsNamespace = "http://www.w3.org/2000/xmlns/";

struct RawAttribute {
  std::string name;
  std::string value;
  std::size_t colon = std::string::npos;  // first colon in `name`, or npos
};

struct Binding {
  std::string prefix;  // empty for the default namespace
  std::string uri;
};

// One pass, one sticky error.
//
// "Sticky" is the design: in XML the first well-formedness error is the only
// one that matters, because everything after it is being read under an
// assumption the document has already broken. Every step checks `Failed()` and
// returns, so there is exactly one place that decides the parse is over.
class Parser {
 public:
  explicit Parser(std::string_view input) : in_(input) {}

  std::unique_ptr<dom::Document> Run();

  bool Failed() const { return !error_.empty(); }
  const std::string& Error() const { return error_; }

 private:
  void Fail(std::string message) {
    if (error_.empty()) {
      error_ = std::move(message);
    }
  }

  bool AtEnd() const { return pos_ >= in_.size(); }
  char Peek(std::size_t ahead = 0) const {
    return pos_ + ahead < in_.size() ? in_[pos_ + ahead] : '\0';
  }
  bool Starts(std::string_view text) const { return in_.compare(pos_, text.size(), text) == 0; }
  void SkipSpace() {
    while (!AtEnd() && IsSpace(in_[pos_])) {
      ++pos_;
    }
  }

  std::string ScanName();
  bool ScanQuoted(std::string& out);
  void ScanMisc(dom::Node& parent, bool allow_doctype);
  void ScanDoctype();
  void ScanInternalSubset();
  void ScanComment(dom::Node& parent);
  void ScanProcessingInstruction(dom::Node& parent);
  void ScanCdata(dom::Node& parent);
  void ScanCharData(dom::Node& parent);
  void ScanElement();
  void ScanReference(std::string& out, bool in_attribute);
  std::string ScanAttributeValue();

  // Appends `text` to `parent`'s last child if it is a text node, and makes one
  // otherwise. Adjacent character data, a reference and a CDATA section are one
  // text node in the DOM, which is what `normalize` already having been done
  // means -- and what the round-trip tests read.
  void AppendText(dom::Node& parent, std::string_view text);

  // Resolves a prefix in the current scope. Null means undeclared, which is a
  // namespace-well-formedness error rather than "no namespace".
  const std::string* LookupPrefix(std::string_view prefix) const;

  void PushBindings(const std::vector<RawAttribute>& attributes);
  void ApplyAttributes(dom::Element& element, const std::vector<RawAttribute>& attributes);

  std::string_view in_;
  std::size_t pos_ = 0;
  std::string error_;
  std::unique_ptr<dom::Document> document_ = std::make_unique<dom::Document>();
  // The open elements. Borrowed pointers into `document_`'s tree; the document
  // owns every one of them and outlives this.
  std::vector<dom::Element*> open_;
  // The namespace scope, flattened: `scope_marks_[i]` is how many bindings were
  // in scope when element `i` opened, so closing one truncates back to it.
  std::vector<Binding> bindings_;
  std::vector<std::size_t> scope_marks_;
  std::vector<std::pair<std::string, std::string>> entities_;
  bool saw_root_ = false;
};

std::string Parser::ScanName() {
  const std::size_t begin = pos_;
  const Decoded first = DecodeUtf8(in_, pos_);
  if (first.length == 0 || !IsNameStart(first.code)) {
    return {};
  }
  pos_ += first.length;
  while (!AtEnd()) {
    const Decoded next = DecodeUtf8(in_, pos_);
    if (!IsNameChar(next.code)) {
      break;
    }
    pos_ += next.length;
  }
  return std::string(in_.substr(begin, pos_ - begin));
}

bool Parser::ScanQuoted(std::string& out) {
  const char quote = Peek();
  if (quote != '"' && quote != '\'') {
    return false;
  }
  ++pos_;
  const std::size_t begin = pos_;
  while (!AtEnd() && in_[pos_] != quote) {
    ++pos_;
  }
  if (AtEnd()) {
    return false;
  }
  out.assign(in_.substr(begin, pos_ - begin));
  ++pos_;
  return true;
}

void Parser::ScanReference(std::string& out, bool in_attribute) {
  ++pos_;  // '&'
  if (Peek() == '#') {
    ++pos_;
    const bool hex = Peek() == 'x';
    if (hex) {
      ++pos_;
    }
    std::uint64_t code = 0;
    bool any = false;
    while (!AtEnd() && in_[pos_] != ';') {
      const char c = in_[pos_];
      int digit = -1;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (hex && c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (hex && c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      }
      if (digit < 0) {
        Fail("malformed character reference");
        return;
      }
      code = code * (hex ? 16 : 10) + static_cast<std::uint64_t>(digit);
      if (code > 0x110000) {
        code = 0x110000;  // saturated: the value is refused below either way
      }
      any = true;
      ++pos_;
    }
    if (!any || AtEnd()) {
      Fail("unterminated character reference");
      return;
    }
    ++pos_;  // ';'
    const auto value = static_cast<char32_t>(code);
    if (!IsXmlChar(value)) {
      Fail("character reference to a character XML does not allow");
      return;
    }
    AppendUtf8(out, value);
    return;
  }
  const std::string name = ScanName();
  if (name.empty() || Peek() != ';') {
    Fail("malformed entity reference");
    return;
  }
  ++pos_;
  if (name == "amp") {
    out += '&';
    return;
  }
  if (name == "lt") {
    out += '<';
    return;
  }
  if (name == "gt") {
    out += '>';
    return;
  }
  if (name == "quot") {
    out += '"';
    return;
  }
  if (name == "apos") {
    out += '\'';
    return;
  }
  for (const auto& [declared, value] : entities_) {
    if (declared != name) {
      continue;
    }
    // Substituted **once**, with only character references expanded inside it.
    // That is not laziness: recursive expansion is "billion laughs", and a
    // parser with no recursion in it cannot have that bug at all. A page that
    // needs nested entities gets a wrong -- but bounded -- answer, and no page
    // outside a test suite has ever needed one.
    for (std::size_t i = 0; i < value.size();) {
      if (value[i] == '&' && i + 1 < value.size() && value[i + 1] == '#') {
        Parser inner(std::string_view(value).substr(i));
        inner.ScanReference(out, in_attribute);
        if (inner.Failed()) {
          Fail(inner.Error());
          return;
        }
        i += inner.pos_;
        continue;
      }
      out += value[i];
      ++i;
    }
    return;
  }
  Fail("reference to undeclared entity '" + name + "'");
}

std::string Parser::ScanAttributeValue() {
  const char quote = Peek();
  if (quote != '"' && quote != '\'') {
    Fail("attribute value is not quoted");
    return {};
  }
  ++pos_;
  std::string out;
  while (!AtEnd() && in_[pos_] != quote) {
    const char c = in_[pos_];
    if (c == '<') {
      Fail("'<' in an attribute value");
      return out;
    }
    if (c == '&') {
      ScanReference(out, true);
      if (Failed()) {
        return out;
      }
      continue;
    }
    // Attribute-value normalization: every literal whitespace character becomes
    // a space, which is what makes `title="a\nb"` and `title="a b"` the same
    // attribute to everything downstream.
    if (c == '\t' || c == '\n') {
      out += ' ';
      ++pos_;
      continue;
    }
    if (c == '\r') {
      out += ' ';
      ++pos_;
      if (Peek() == '\n') {
        ++pos_;
      }
      continue;
    }
    const Decoded decoded = DecodeUtf8(in_, pos_);
    if (!decoded.valid || !IsXmlChar(decoded.code)) {
      AppendUtf8(out, kReplacement);
    } else {
      out.append(in_.substr(pos_, decoded.length));
    }
    pos_ += decoded.length;
  }
  if (AtEnd()) {
    Fail("unterminated attribute value");
  } else {
    ++pos_;
  }
  return out;
}

void Parser::AppendText(dom::Node& parent, std::string_view text) {
  if (text.empty()) {
    return;
  }
  dom::Node* last = parent.LastChild();
  if (last != nullptr && last->GetKind() == dom::Node::Kind::Text) {
    static_cast<dom::Text*>(last)->Append(text);
    return;
  }
  parent.Append(std::make_unique<dom::Text>(std::string(text)));
}

void Parser::ScanComment(dom::Node& parent) {
  pos_ += 4;  // "<!--"
  const std::size_t begin = pos_;
  while (pos_ + 1 < in_.size() && !(in_[pos_] == '-' && in_[pos_ + 1] == '-')) {
    ++pos_;
  }
  if (pos_ + 1 >= in_.size()) {
    Fail("unterminated comment");
    return;
  }
  const std::string data(in_.substr(begin, pos_ - begin));
  pos_ += 2;
  if (Peek() != '>') {
    Fail("'--' inside a comment");
    return;
  }
  ++pos_;
  parent.Append(std::make_unique<dom::Comment>(data));
}

void Parser::ScanProcessingInstruction(dom::Node& parent) {
  pos_ += 2;  // "<?"
  const std::string target = ScanName();
  if (target.empty()) {
    Fail("processing instruction with no target");
    return;
  }
  const std::size_t begin = pos_;
  while (pos_ + 1 < in_.size() && !(in_[pos_] == '?' && in_[pos_ + 1] == '>')) {
    ++pos_;
  }
  if (pos_ + 1 >= in_.size()) {
    Fail("unterminated processing instruction");
    return;
  }
  std::string data(in_.substr(begin, pos_ - begin));
  pos_ += 2;
  const std::size_t first = data.find_first_not_of(" \t\n\r");
  data = first == std::string::npos ? std::string() : data.substr(first);
  parent.Append(std::make_unique<dom::ProcessingInstruction>(target, std::move(data)));
}

void Parser::ScanCdata(dom::Node& parent) {
  pos_ += 9;  // "<![CDATA["
  const std::size_t begin = pos_;
  while (pos_ + 2 < in_.size() &&
         !(in_[pos_] == ']' && in_[pos_ + 1] == ']' && in_[pos_ + 2] == '>')) {
    ++pos_;
  }
  if (pos_ + 2 >= in_.size()) {
    Fail("unterminated CDATA section");
    return;
  }
  // Decoded and re-encoded rather than copied: a CDATA section is the one place
  // a page can put arbitrary bytes straight into a text node, so this is where
  // an ill-formed sequence has to become U+FFFD.
  std::string text;
  for (std::size_t i = begin; i < pos_;) {
    const Decoded decoded = DecodeUtf8(in_, i);
    if (!decoded.valid || !IsXmlChar(decoded.code)) {
      AppendUtf8(text, kReplacement);
    } else {
      text.append(in_.substr(i, decoded.length));
    }
    i += decoded.length;
  }
  pos_ += 3;
  AppendText(parent, text);
}

void Parser::ScanCharData(dom::Node& parent) {
  std::string text;
  while (!AtEnd() && in_[pos_] != '<') {
    const char c = in_[pos_];
    if (c == '&') {
      ScanReference(text, false);
      if (Failed()) {
        return;
      }
      continue;
    }
    if (c == ']' && Starts("]]>")) {
      Fail("']]>' in character data");
      return;
    }
    // Line-ending normalization, Infra/XML both: CRLF and a lone CR are LF.
    if (c == '\r') {
      text += '\n';
      ++pos_;
      if (Peek() == '\n') {
        ++pos_;
      }
      continue;
    }
    const Decoded decoded = DecodeUtf8(in_, pos_);
    if (!decoded.valid || !IsXmlChar(decoded.code)) {
      AppendUtf8(text, kReplacement);
    } else {
      text.append(in_.substr(pos_, decoded.length));
    }
    pos_ += decoded.length;
  }
  AppendText(parent, text);
}

void Parser::ScanInternalSubset() {
  ++pos_;  // '['
  while (!AtEnd() && in_[pos_] != ']') {
    if (Starts("<!ENTITY")) {
      pos_ += 8;
      SkipSpace();
      if (Peek() == '%') {
        // A parameter entity. Skipped rather than declared, which is the same
        // refusal external entities get: expanding one is how a DTD reaches off
        // the machine.
        while (!AtEnd() && in_[pos_] != '>') {
          ++pos_;
        }
        continue;
      }
      const std::string name = ScanName();
      SkipSpace();
      std::string value;
      if (!name.empty() && (Peek() == '"' || Peek() == '\'') && ScanQuoted(value) &&
          entities_.size() < kMaxEntities && value.size() <= kMaxEntityValueBytes) {
        entities_.emplace_back(name, std::move(value));
      }
      while (!AtEnd() && in_[pos_] != '>') {
        ++pos_;
      }
      continue;
    }
    if (Peek() == '"' || Peek() == '\'') {
      std::string ignored;
      if (!ScanQuoted(ignored)) {
        Fail("unterminated literal in the internal subset");
        return;
      }
      continue;
    }
    ++pos_;
  }
  if (AtEnd()) {
    Fail("unterminated internal subset");
    return;
  }
  ++pos_;  // ']'
}

void Parser::ScanDoctype() {
  pos_ += 9;  // "<!DOCTYPE"
  if (!IsSpace(Peek())) {
    Fail("doctype with no name");
    return;
  }
  SkipSpace();
  const std::string name = ScanName();
  if (name.empty()) {
    Fail("doctype with no name");
    return;
  }
  std::string public_id;
  std::string system_id;
  SkipSpace();
  if (Starts("PUBLIC")) {
    pos_ += 6;
    SkipSpace();
    if (!ScanQuoted(public_id)) {
      Fail("doctype PUBLIC identifier is not a quoted literal");
      return;
    }
    SkipSpace();
    // The system identifier is **required** after a public one, and this is
    // the one place the difference is observable from script:
    // `<!DOCTYPE html PUBLIC "…">` is an error document and
    // `<!DOCTYPE html PUBLIC "…" "">` is not.
    if (!ScanQuoted(system_id)) {
      Fail("doctype PUBLIC identifier with no system identifier");
      return;
    }
    SkipSpace();
  } else if (Starts("SYSTEM")) {
    pos_ += 6;
    SkipSpace();
    if (!ScanQuoted(system_id)) {
      Fail("doctype SYSTEM identifier is not a quoted literal");
      return;
    }
    SkipSpace();
  }
  if (Peek() == '[') {
    ScanInternalSubset();
    if (Failed()) {
      return;
    }
    SkipSpace();
  }
  if (Peek() != '>') {
    Fail("malformed doctype");
    return;
  }
  ++pos_;
  document_->Append(std::make_unique<dom::DocumentType>(name, public_id, system_id));
}

const std::string* Parser::LookupPrefix(std::string_view prefix) const {
  for (std::size_t i = bindings_.size(); i > 0; --i) {
    if (bindings_[i - 1].prefix == prefix) {
      return &bindings_[i - 1].uri;
    }
  }
  if (prefix == "xml") {
    static const std::string kXml(kXmlNamespace);
    return &kXml;
  }
  if (prefix == "xmlns") {
    static const std::string kXmlns(kXmlnsNamespace);
    return &kXmlns;
  }
  if (prefix.empty()) {
    static const std::string kEmpty;
    return &kEmpty;
  }
  return nullptr;
}

void Parser::PushBindings(const std::vector<RawAttribute>& attributes) {
  for (const RawAttribute& attribute : attributes) {
    if (attribute.name == "xmlns") {
      bindings_.push_back(Binding{std::string(), attribute.value});
      continue;
    }
    if (attribute.name.compare(0, 6, "xmlns:") != 0) {
      continue;
    }
    const std::string prefix = attribute.name.substr(6);
    if (prefix.empty()) {
      Fail("'xmlns:' with no prefix");
      return;
    }
    if (prefix == "xmlns") {
      Fail("the prefix 'xmlns' may not be declared");
      return;
    }
    if (prefix == "xml" && attribute.value != kXmlNamespace) {
      Fail("the prefix 'xml' may only be bound to the XML namespace");
      return;
    }
    if (prefix != "xml" && attribute.value == kXmlNamespace) {
      Fail("the XML namespace may only be bound to the prefix 'xml'");
      return;
    }
    if (attribute.value == kXmlnsNamespace) {
      Fail("the xmlns namespace may not be bound to a prefix");
      return;
    }
    if (attribute.value.empty()) {
      // XML 1.0: only the *default* namespace may be undeclared. `xmlns:p=""`
      // is a namespace-well-formedness error, which is what
      // `<span xmlns:xmlns="">` is testing two rules of at once.
      Fail("a prefix may not be bound to the empty namespace");
      return;
    }
    bindings_.push_back(Binding{prefix, attribute.value});
  }
}

void Parser::ApplyAttributes(dom::Element& element,
                             const std::vector<RawAttribute>& attributes) {
  for (const RawAttribute& attribute : attributes) {
    dom::NamespaceRef name_space;
    std::uint32_t prefix_length = 0;
    if (attribute.name == "xmlns" || attribute.name.compare(0, 6, "xmlns:") == 0) {
      name_space = dom::NamespaceRef(kXmlnsNamespace);
      prefix_length = attribute.name == "xmlns" ? 0 : 5;
    } else if (attribute.colon != std::string::npos) {
      const std::string prefix = attribute.name.substr(0, attribute.colon);
      const std::string* uri = LookupPrefix(prefix);
      if (uri == nullptr) {
        Fail("undeclared namespace prefix '" + prefix + "'");
        return;
      }
      if (uri->empty()) {
        Fail("prefix '" + prefix + "' is bound to no namespace");
        return;
      }
      name_space = dom::NamespaceRef(*uri);
      prefix_length = static_cast<std::uint32_t>(attribute.colon);
    }
    // Duplicate detection is on (namespace, local name) as well as on the
    // qualified name, because `p:a` and `q:a` with `p` and `q` on one URI are
    // the same attribute and are a well-formedness error.
    if (element.GetAttributeNS(name_space, std::string_view(attribute.name)
                                               .substr(prefix_length == 0 ? 0
                                                                          : prefix_length + 1)) !=
        nullptr) {
      Fail("duplicate attribute '" + attribute.name + "'");
      return;
    }
    element.SetAttributeNS(name_space, attribute.name, prefix_length, attribute.value);
  }
}

void Parser::ScanElement() {
  ++pos_;  // '<'
  const std::string name = ScanName();
  if (name.empty()) {
    Fail("'<' not followed by a name");
    return;
  }
  const std::size_t colon = name.find(':');
  if (colon != std::string::npos &&
      (colon == 0 || colon + 1 == name.size() || name.find(':', colon + 1) != std::string::npos)) {
    Fail("'" + name + "' is not a qualified name");
    return;
  }

  std::vector<RawAttribute> attributes;
  bool empty_element = false;
  while (true) {
    const bool had_space = IsSpace(Peek());
    SkipSpace();
    if (Peek() == '>') {
      ++pos_;
      break;
    }
    if (Starts("/>")) {
      pos_ += 2;
      empty_element = true;
      break;
    }
    if (AtEnd()) {
      Fail("unterminated start tag '" + name + "'");
      return;
    }
    if (!had_space) {
      Fail("attributes must be separated by whitespace");
      return;
    }
    RawAttribute attribute;
    attribute.name = ScanName();
    if (attribute.name.empty()) {
      Fail("malformed attribute name in '" + name + "'");
      return;
    }
    attribute.colon = attribute.name.find(':');
    if (attribute.colon != std::string::npos &&
        (attribute.colon == 0 || attribute.colon + 1 == attribute.name.size() ||
         attribute.name.find(':', attribute.colon + 1) != std::string::npos)) {
      Fail("'" + attribute.name + "' is not a qualified name");
      return;
    }
    SkipSpace();
    if (Peek() != '=') {
      Fail("attribute '" + attribute.name + "' has no value");
      return;
    }
    ++pos_;
    SkipSpace();
    attribute.value = ScanAttributeValue();
    if (Failed()) {
      return;
    }
    for (const RawAttribute& earlier : attributes) {
      if (earlier.name == attribute.name) {
        Fail("duplicate attribute '" + attribute.name + "'");
        return;
      }
    }
    attributes.push_back(std::move(attribute));
  }

  if (open_.size() >= kMaxDepth) {
    Fail("elements nested deeper than this parser will build");
    return;
  }

  scope_marks_.push_back(bindings_.size());
  PushBindings(attributes);
  if (Failed()) {
    return;
  }

  dom::NamespaceRef element_namespace;
  std::uint32_t prefix_length = 0;
  if (colon == std::string::npos) {
    const std::string* uri = LookupPrefix(std::string_view());
    if (uri != nullptr && !uri->empty()) {
      element_namespace = dom::NamespaceRef(*uri);
    }
  } else {
    const std::string prefix = name.substr(0, colon);
    const std::string* uri = LookupPrefix(prefix);
    if (uri == nullptr || uri->empty()) {
      Fail("undeclared namespace prefix '" + prefix + "'");
      return;
    }
    element_namespace = dom::NamespaceRef(*uri);
    prefix_length = static_cast<std::uint32_t>(colon);
  }

  auto made = std::make_unique<dom::Element>(element_namespace, name, prefix_length);
  dom::Element* element = made.get();
  ApplyAttributes(*element, attributes);
  if (Failed()) {
    return;
  }
  dom::Node& parent = open_.empty() ? static_cast<dom::Node&>(*document_) : *open_.back();
  parent.Append(std::move(made));
  if (empty_element) {
    bindings_.resize(scope_marks_.back());
    scope_marks_.pop_back();
    return;
  }
  open_.push_back(element);
}

std::unique_ptr<dom::Document> Parser::Run() {
  // Prolog. An XML declaration is legal only as the very first thing in the
  // document, which is why it is peeled off here rather than treated as a
  // processing instruction like every other `<?…?>`.
  if (Starts("<?xml") && (pos_ + 5 >= in_.size() || IsSpace(in_[pos_ + 5]))) {
    while (pos_ + 1 < in_.size() && !(in_[pos_] == '?' && in_[pos_ + 1] == '>')) {
      ++pos_;
    }
    if (pos_ + 1 >= in_.size()) {
      Fail("unterminated XML declaration");
      return nullptr;
    }
    pos_ += 2;
  }

  bool saw_doctype = false;
  while (!AtEnd() && !Failed()) {
    // Whichever node a comment, a PI or an element is going into: the innermost
    // open element, and the document itself in the prolog and the epilog. One
    // expression rather than a branch at each of the three, because the bug it
    // hides is silent -- a comment inside an element ends up beside the root.
    dom::Node& into = open_.empty() ? static_cast<dom::Node&>(*document_) : *open_.back();
    // Whitespace is only *ignorable* outside the root element. Inside one it is
    // character data like any other, and skipping it here dropped the leading
    // space of every mixed-content element -- invisible in a tree dump and
    // visible the moment anything reads `textContent`.
    if (open_.empty() && IsSpace(in_[pos_])) {
      ++pos_;
      continue;
    }
    if (Starts("<!--")) {
      ScanComment(into);
      continue;
    }
    if (Starts("<?")) {
      ScanProcessingInstruction(into);
      continue;
    }
    if (Starts("<!DOCTYPE")) {
      if (saw_doctype || saw_root_) {
        Fail("a second doctype");
        break;
      }
      saw_doctype = true;
      ScanDoctype();
      continue;
    }
    if (Starts("</")) {
      if (open_.empty()) {
        Fail("end tag with no open element");
        break;
      }
      pos_ += 2;
      const std::string name = ScanName();
      SkipSpace();
      if (Peek() != '>') {
        Fail("malformed end tag");
        break;
      }
      ++pos_;
      if (name != open_.back()->TagName()) {
        Fail("end tag '" + name + "' does not match '" + open_.back()->TagName() + "'");
        break;
      }
      open_.pop_back();
      bindings_.resize(scope_marks_.back());
      scope_marks_.pop_back();
      continue;
    }
    if (Starts("<![CDATA[")) {
      if (open_.empty()) {
        Fail("CDATA section outside the root element");
        break;
      }
      ScanCdata(*open_.back());
      continue;
    }
    if (Peek() == '<') {
      if (open_.empty() && saw_root_) {
        Fail("a second root element");
        break;
      }
      saw_root_ = true;
      ScanElement();
      continue;
    }
    if (open_.empty()) {
      Fail("character data outside the root element");
      break;
    }
    ScanCharData(*open_.back());
  }

  if (!Failed() && !open_.empty()) {
    Fail("unclosed element '" + open_.back()->TagName() + "'");
  }
  if (!Failed() && !saw_root_) {
    Fail("no root element");
  }
  return Failed() ? nullptr : std::move(document_);
}

std::unique_ptr<dom::Document> ErrorDocument(const std::string& message) {
  auto document = std::make_unique<dom::Document>();
  auto error = std::make_unique<dom::Element>(dom::NamespaceRef(kParserErrorNamespace),
                                              "parsererror", 0);
  // No `xmlns` attribute: the element's *namespace* is what a page reads, and
  // the serializer writes the declaration out from that. An attribute would be
  // a second copy of the same fact, free to disagree with the first.
  error->Append(std::make_unique<dom::Text>(message));
  document->Append(std::move(error));
  return document;
}

}  // namespace

XmlParseResult ParseXml(std::string_view input) {
  Parser parser(input);
  XmlParseResult result;
  std::unique_ptr<dom::Document> document = parser.Run();
  if (document == nullptr) {
    result.error = parser.Error().empty() ? std::string("XML parse error") : parser.Error();
    result.document = ErrorDocument(result.error);
    return result;
  }
  result.document = std::move(document);
  result.ok = true;
  return result;
}

}  // namespace microbrowser::xml
