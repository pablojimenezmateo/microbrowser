#include "xml/XmlSerializer.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dom/Node.h"

namespace microbrowser::xml {

namespace {

constexpr std::string_view kXmlNamespace = "http://www.w3.org/XML/1998/namespace";
constexpr std::string_view kXmlnsNamespace = "http://www.w3.org/2000/xmlns/";
constexpr std::string_view kHtmlNamespace = "http://www.w3.org/1999/xhtml";

// The bound. A serialization is a walk of a tree the parser already refused to
// build past 512 deep, but this function is also reachable from script over a
// tree script built node by node -- so the recursion needs its own limit, in
// the one place it can be checked.
constexpr std::size_t kMaxDepth = 512;

// The void elements, for the one place HTML's shape leaks into an XML
// serialization: an empty `<br>` in the HTML namespace writes ` />` and an
// empty `<div>` writes `></div>`.
bool IsVoidHtmlElement(std::string_view local_name) {
  static constexpr std::string_view kVoid[] = {
      "area", "base",  "basefont", "bgsound", "br",    "col",   "embed", "frame", "hr",
      "img",  "input", "keygen",   "link",    "menuitem", "meta", "param", "source", "track",
      "wbr"};
  return std::find(std::begin(kVoid), std::end(kVoid), local_name) != std::end(kVoid);
}

std::string EscapeText(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string EscapeAttributeValue(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '"': out += "&quot;"; break;
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      // The three whitespace escapes are what makes an attribute round-trip:
      // a literal tab or newline in an attribute value is normalized to a
      // space by every XML parser, so writing it literally loses it.
      case '\t': out += "&#x9;"; break;
      case '\n': out += "&#xA;"; break;
      case '\r': out += "&#xD;"; break;
      default: out += c; break;
    }
  }
  return out;
}

// The specification's "namespace prefix map": a namespace URI to the prefixes
// bound to it, in the order they were seen. Retrieval prefers an exact match
// and otherwise takes the **last** one added, which is what makes the prefix a
// nested element inherits the nearest one.
class PrefixMap {
 public:
  void Add(const std::string& uri, const std::string& prefix) {
    std::vector<std::string>& list = entries_[uri];
    if (std::find(list.begin(), list.end(), prefix) == list.end()) {
      list.push_back(prefix);
    }
  }

  std::optional<std::string> Retrieve(const std::string& uri,
                                      const std::string& preferred) const {
    const auto found = entries_.find(uri);
    if (found == entries_.end() || found->second.empty()) {
      return std::nullopt;
    }
    for (const std::string& candidate : found->second) {
      if (candidate == preferred) {
        return candidate;
      }
    }
    return found->second.back();
  }

 private:
  std::map<std::string, std::vector<std::string>> entries_;
};

std::string GeneratePrefix(PrefixMap& map, const std::string& uri, int& index) {
  const std::string prefix = "ns" + std::to_string(index);
  ++index;
  map.Add(uri, prefix);
  return prefix;
}

// The attribute's prefix as the DOM means it: absent rather than empty.
std::optional<std::string> PrefixOf(const dom::Attribute& attribute) {
  if (attribute.prefix_length == 0) {
    return std::nullopt;
  }
  return std::string(attribute.Prefix());
}

// Whether this attribute is a default-namespace declaration. See the header:
// local name plus "no prefix" rather than the xmlns namespace, because
// `setAttribute("xmlns", …)` puts the attribute in no namespace and every
// engine still treats it as one.
bool IsDefaultNamespaceDeclaration(const dom::Attribute& attribute) {
  return attribute.prefix_length == 0 && attribute.name == "xmlns";
}

bool IsPrefixDeclaration(const dom::Attribute& attribute) {
  return attribute.prefix_length == 5 && attribute.Prefix() == "xmlns";
}

std::string Serialize(const dom::Node& node, const std::string& context_namespace,
                      PrefixMap map, int& prefix_index, std::size_t depth);

// "Recording the namespace information": what this element's own xmlns
// attributes add to the map, and what its default namespace declaration says.
std::optional<std::string> RecordNamespaces(
    const dom::Element& element, PrefixMap& map,
    std::map<std::string, std::string>& local_prefixes) {
  std::optional<std::string> default_namespace;
  for (const dom::Attribute& attribute : element.Attributes()) {
    if (IsDefaultNamespaceDeclaration(attribute)) {
      default_namespace = attribute.value;
      continue;
    }
    if (!IsPrefixDeclaration(attribute)) {
      continue;
    }
    const std::string prefix(attribute.LocalName());
    if (attribute.value == kXmlNamespace) {
      continue;
    }
    const auto existing = local_prefixes.find(prefix);
    if (existing != local_prefixes.end() && existing->second == attribute.value) {
      continue;
    }
    local_prefixes[prefix] = attribute.value;
    if (!attribute.value.empty()) {
      map.Add(attribute.value, prefix);
    }
  }
  return default_namespace;
}

std::string SerializeAttributes(const dom::Element& element, PrefixMap& map, int& prefix_index,
                                const std::map<std::string, std::string>& local_prefixes,
                                bool ignore_default_declaration) {
  std::string out;
  for (const dom::Attribute& attribute : element.Attributes()) {
    const std::string attribute_namespace(attribute.name_space.Uri());
    std::optional<std::string> candidate_prefix;
    if (!attribute_namespace.empty()) {
      candidate_prefix =
          map.Retrieve(attribute_namespace, PrefixOf(attribute).value_or(std::string()));
    }
    const bool is_declaration =
        IsDefaultNamespaceDeclaration(attribute) || IsPrefixDeclaration(attribute);
    if (is_declaration) {
      if (attribute.value == kXmlNamespace) {
        continue;
      }
      if (IsDefaultNamespaceDeclaration(attribute) && ignore_default_declaration) {
        continue;
      }
      if (IsPrefixDeclaration(attribute)) {
        const std::string prefix(attribute.LocalName());
        const auto found = local_prefixes.find(prefix);
        const bool inconsistent = found == local_prefixes.end() || found->second != attribute.value;
        if (inconsistent && map.Retrieve(attribute.value, prefix).value_or(std::string()) ==
                                prefix) {
          continue;
        }
        candidate_prefix = std::string("xmlns");
      }
    } else if (!attribute_namespace.empty()) {
      if (!candidate_prefix.has_value()) {
        candidate_prefix = GeneratePrefix(map, attribute_namespace, prefix_index);
        out += " xmlns:";
        out += *candidate_prefix;
        out += "=\"";
        out += EscapeAttributeValue(attribute_namespace);
        out += "\"";
      }
    }
    out += ' ';
    if (candidate_prefix.has_value() && !candidate_prefix->empty()) {
      out += *candidate_prefix;
      out += ':';
    }
    out += attribute.LocalName();
    out += "=\"";
    out += EscapeAttributeValue(attribute.value);
    out += "\"";
  }
  return out;
}

std::string SerializeElement(const dom::Element& element, const std::string& context_namespace,
                             PrefixMap map, int& prefix_index, std::size_t depth) {
  std::map<std::string, std::string> local_prefixes;
  const std::optional<std::string> local_default = RecordNamespaces(element, map, local_prefixes);
  const std::string element_namespace(element.Namespace().Uri());
  const std::string prefix(element.Prefix());
  const bool has_prefix = element.Prefix().size() > 0;
  const std::string local_name(element.LocalName());

  std::string markup = "<";
  std::string qualified_name;
  std::string inherited = context_namespace;
  bool ignore_default_declaration = false;

  if (context_namespace == element_namespace) {
    if (local_default.has_value()) {
      ignore_default_declaration = true;
    }
    qualified_name = element_namespace == kXmlNamespace ? "xml:" + local_name : local_name;
    markup += qualified_name;
  } else {
    // A prefix already bound to this namespace wins over declaring a default
    // one, *even when this element declares exactly that default*. That is the
    // specification, and it is the case nobody expects:
    // `<root xmlns:x="uri1"><table xmlns="uri1"/></root>` serializes the child
    // as `<x:table xmlns="uri1"/>`, prefix and redundant declaration together.
    std::optional<std::string> candidate = map.Retrieve(element_namespace, prefix);
    if (prefix == "xmlns") {
      candidate = prefix;
    }
    if (candidate.has_value()) {
      qualified_name = *candidate + ":" + local_name;
      if (local_default.has_value() && *local_default != kXmlNamespace) {
        inherited = *local_default;
      }
      markup += qualified_name;
    } else if (has_prefix) {
      std::string chosen = prefix;
      if (local_prefixes.find(prefix) != local_prefixes.end()) {
        chosen = GeneratePrefix(map, element_namespace, prefix_index);
      } else {
        map.Add(element_namespace, prefix);
      }
      qualified_name = chosen + ":" + local_name;
      markup += qualified_name;
      markup += " xmlns:";
      markup += chosen;
      markup += "=\"";
      markup += EscapeAttributeValue(element_namespace);
      markup += "\"";
    } else if (!local_default.has_value() || *local_default != element_namespace) {
      ignore_default_declaration = true;
      qualified_name = local_name;
      inherited = element_namespace;
      markup += qualified_name;
      markup += " xmlns=\"";
      markup += EscapeAttributeValue(element_namespace);
      markup += "\"";
    } else {
      qualified_name = local_name;
      inherited = element_namespace;
      markup += qualified_name;
    }
  }

  markup += SerializeAttributes(element, map, prefix_index, local_prefixes,
                                ignore_default_declaration);

  const bool childless = element.Children().empty();
  bool skip_end_tag = false;
  if (element_namespace == kHtmlNamespace && childless && IsVoidHtmlElement(local_name)) {
    markup += " /";
    skip_end_tag = true;
  } else if (element_namespace != kHtmlNamespace && childless) {
    markup += "/";
    skip_end_tag = true;
  }
  markup += ">";
  if (skip_end_tag) {
    return markup;
  }
  const dom::DocumentFragment* content = element.Content();
  if (content != nullptr) {
    for (const auto& child : content->Children()) {
      markup += Serialize(*child, inherited, map, prefix_index, depth + 1);
    }
  } else {
    for (const auto& child : element.Children()) {
      markup += Serialize(*child, inherited, map, prefix_index, depth + 1);
    }
  }
  markup += "</";
  markup += qualified_name;
  markup += ">";
  return markup;
}

std::string Serialize(const dom::Node& node, const std::string& context_namespace, PrefixMap map,
                      int& prefix_index, std::size_t depth) {
  if (depth > kMaxDepth) {
    return {};
  }
  switch (node.GetKind()) {
    case dom::Node::Kind::Element:
      return SerializeElement(static_cast<const dom::Element&>(node), context_namespace, map,
                              prefix_index, depth);
    case dom::Node::Kind::Text:
      return EscapeText(static_cast<const dom::Text&>(node).Data());
    case dom::Node::Kind::Comment:
      return "<!--" + static_cast<const dom::Comment&>(node).Data() + "-->";
    case dom::Node::Kind::ProcessingInstruction: {
      const auto& instruction = static_cast<const dom::ProcessingInstruction&>(node);
      return "<?" + instruction.Target() + " " + instruction.Data() + "?>";
    }
    case dom::Node::Kind::DocumentType: {
      const auto& doctype = static_cast<const dom::DocumentType&>(node);
      // Quoted with `"` unless the identifier contains one, in which case `'`.
      // The specification says `"` unconditionally and *throws* for the other
      // case under its well-formed flag; this serializer does not throw, so
      // writing `"a"b"` would be an output path that produces markup no parser
      // reads back. Switching the delimiter is what XML's own SystemLiteral
      // production allows and is what makes the round trip total.
      const auto quoted = [](const std::string& value) {
        const char quote = value.find('"') == std::string::npos ? '"' : '\'';
        return std::string(1, quote) + value + std::string(1, quote);
      };
      std::string out = "<!DOCTYPE " + doctype.Name();
      if (!doctype.PublicId().empty()) {
        // Both literals, always. A public identifier with no system identifier
        // after it is not XML -- `<!DOCTYPE a PUBLIC "p">` does not parse -- so
        // omitting an *empty* system identifier here writes markup this
        // module's own parser refuses. The fuzz target found exactly that on
        // its first run, which is the round-trip property doing its job.
        out += " PUBLIC " + quoted(doctype.PublicId()) + " " + quoted(doctype.SystemId());
      } else if (!doctype.SystemId().empty()) {
        out += " SYSTEM " + quoted(doctype.SystemId());
      }
      // The internal subset is deliberately absent: the DocumentType interface
      // does not carry one, and inventing one here would be a round trip that
      // produced markup the DOM never held.
      return out + ">";
    }
    case dom::Node::Kind::Document:
    case dom::Node::Kind::DocumentFragment: {
      std::string out;
      for (const auto& child : node.Children()) {
        out += Serialize(*child, context_namespace, map, prefix_index, depth + 1);
      }
      return out;
    }
  }
  return {};
}

}  // namespace

std::string SerializeXml(const dom::Node& node) {
  PrefixMap map;
  map.Add(std::string(kXmlNamespace), "xml");
  int prefix_index = 1;
  return Serialize(node, std::string(), map, prefix_index, 0);
}

}  // namespace microbrowser::xml
