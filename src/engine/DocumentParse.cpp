#include "engine/DocumentParse.h"

#include <string>
#include <utility>

#include "html/TreeBuilder.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "xml/XmlParser.h"

namespace microbrowser::engine {

// Is this document XML? The MIME types HTML's "navigate" step routes to the XML parser, and no
// others: `text/html` is the HTML tree builder whatever the file is called.
bool IsXmlContentType(std::string_view content_type) {
  std::string type = util::AsciiLowerCase(content_type);
  const std::size_t semicolon = type.find(';');
  if (semicolon != std::string::npos) {
    type.erase(semicolon);
  }
  const std::string_view essence = util::TrimAscii(type);
  return essence == "application/xhtml+xml" || essence == "application/xml" ||
         essence == "text/xml" || essence == "image/svg+xml" ||
         (essence.size() > 4 && essence.compare(essence.size() - 4, 4, "+xml") == 0);
}

// **The two parsers are not interchangeable and the difference is visible on ordinary pages.** XML
// has no RAWTEXT: `<style><![CDATA[ … ]]></style>` is a stylesheet in XHTML and, read by the HTML
// tokenizer, is a stylesheet whose first rule has `<![CDATA[` glued onto its selector -- so the
// rule is dropped and the element it styled renders unstyled. The CSS 2.1 test suite is written in
// XHTML and puts almost every reference's stylesheet in a CDATA section, which is how this was
// found: the *references* were rendering blank.
//
// **A well-formedness error falls back to the HTML tree builder, and that is a deliberate deviation
// from the specification**, which says the document becomes a `parsererror` element and nothing
// else. The reason is one missing table rather than a disagreement: XML predefines five entities
// and XHTML's DTD adds the other 2,226, which `src/html` has and `src/xml` -- which may see only
// `util` and `dom` -- does not. Until that table is somewhere both modules can reach, refusing
// would turn 236 files of the CSS 2.1 suite from "rendered" into "blank", and a blank page is not
// a better wrong answer than a recovered one. `engine.document_xml_fell_back_to_html` counts it,
// so the day the table moves this comment has a number to be checked against.
std::unique_ptr<dom::Document> ParseDocumentFor(std::string_view source,
                                                std::string_view content_type) {
  if (!IsXmlContentType(content_type)) {
    return html::ParseDocument(source);
  }
  util::AddPerformanceCounter(util::PerfCounterId::DocumentParsedAsXml);
  xml::XmlParseResult result = xml::ParseXml(source);
  if (result.ok) {
    return std::move(result.document);
  }
  util::AddPerformanceCounter(util::PerfCounterId::DocumentXmlFellBackToHtml);
  return html::ParseDocument(source);
}

}  // namespace microbrowser::engine
