#include "html/TreeBuilder.h"

#include "html/TreeBuilderInternal.h"
#include "util/StringUtil.h"

// The table insertion modes, WHATWG HTML §13.2.6.4.9 through §13.2.6.4.17.
//
// Their own translation unit rather than a section of TreeBuilder.cpp because
// they are half the tree builder by line count and are read against their own
// half of the spec. Everything they share with the rest of the builder is in
// TreeBuilderInternal.h; everything else is a member of TreeBuilder.
namespace microbrowser::html {

// §13.2.6.4.9 "in table".
//
// The table modes exist because a table's children are constrained in a way no
// other element's are: text and stray elements cannot live between a `<table>`
// and its `<tr>`, so the parser moves them out (foster parenting) rather than
// building a tree no layout engine could interpret. Every clause below is the
// spec's; the one that is absent is template.
void TreeBuilder::ProcessInTable(const Token& token) {
  switch (token.kind) {
    case Token::Kind::Character:
      if (!open_elements_.empty() && IsFosterParent(open_elements_.back()->TagName())) {
        pending_table_text_.clear();
        original_mode_ = mode_;
        mode_ = InsertionMode::InTableText;
        Process(token);
        return;
      }
      break;  // text somewhere a table cannot hold it: the anything-else clause

    case Token::Kind::Comment:
      InsertComment(token.data, nullptr);
      return;

    case Token::Kind::Doctype:
      ++errors_;
      return;

    case Token::Kind::StartTag: {
      if (token.data == "caption") {
        ClearStackToContext({"table"});
        InsertElement(token);
        mode_ = InsertionMode::InCaption;
        return;
      }
      if (token.data == "colgroup") {
        ClearStackToContext({"table"});
        InsertElement(token);
        mode_ = InsertionMode::InColumnGroup;
        return;
      }
      if (token.data == "col") {
        ClearStackToContext({"table"});
        InsertImplied("colgroup");
        mode_ = InsertionMode::InColumnGroup;
        Process(token);
        return;
      }
      if (token.data == "tbody" || token.data == "tfoot" || token.data == "thead") {
        ClearStackToContext({"table"});
        InsertElement(token);
        mode_ = InsertionMode::InTableBody;
        return;
      }
      if (token.data == "td" || token.data == "th" || token.data == "tr") {
        // A row that skipped its section gets one. Pages write this constantly.
        ClearStackToContext({"table"});
        InsertImplied("tbody");
        mode_ = InsertionMode::InTableBody;
        Process(token);
        return;
      }
      if (token.data == "table") {
        // A table inside a table's own structure closes the outer one, which is
        // not what the markup says and is what every browser does.
        ++errors_;
        if (!HasInScope("table", Scope::Table)) {
          return;
        }
        PopUntil("table");
        ResetInsertionMode();
        Process(token);
        return;
      }
      if (token.data == "style") {
        SwitchToRawText(token, TokenizerState::RawText);
        return;
      }
      if (token.data == "script") {
        SwitchToRawText(token, TokenizerState::ScriptData);
        return;
      }
      if (token.data == "input" && token.AttributeValue("type") != nullptr &&
          util::EqualsAsciiCaseInsensitive(*token.AttributeValue("type"), "hidden")) {
        // The one element allowed to sit in a table without being fostered: it
        // renders nothing, so it cannot disturb the table's boxes.
        ++errors_;
        InsertElement(token);
        return;
      }
      break;
    }

    case Token::Kind::EndTag: {
      if (token.data == "table") {
        if (!HasInScope("table", Scope::Table)) {
          ++errors_;
          return;
        }
        PopUntil("table");
        ResetInsertionMode();
        return;
      }
      if (token.data == "body" || token.data == "html" ||
          Contains(kTableStructureTags, token.data)) {
        ++errors_;
        return;
      }
      break;
    }

    case Token::Kind::EndOfFile:
      ProcessInBody(token);
      return;
  }

  // Anything else. Foster parenting is the whole point: the token is processed
  // by the ordinary body rules, but whatever it inserts lands before the table
  // instead of inside it.
  ++errors_;
  foster_parenting_ = true;
  ProcessInBody(token);
  foster_parenting_ = false;
}

// §13.2.6.4.10 "in table text". Character tokens are held because where they go
// depends on whether the whole run is whitespace, and that is not known until
// the run ends.
void TreeBuilder::ProcessInTableText(const Token& token) {
  if (token.kind == Token::Kind::Character) {
    pending_table_text_ += token.data;
    return;
  }
  const std::string pending = std::move(pending_table_text_);
  pending_table_text_.clear();
  mode_ = original_mode_;
  if (!pending.empty()) {
    if (IsWhitespaceOnly(pending)) {
      InsertText(pending);
    } else {
      // Non-whitespace in a table is the anything-else clause of "in table",
      // one character token's worth at a time — here, the whole run at once,
      // which produces the same tree.
      ++errors_;
      foster_parenting_ = true;
      InsertText(pending);
      foster_parenting_ = false;
      frameset_ok_ = false;
    }
  }
  Process(token);
}

// §13.2.6.4.11 "in caption".
void TreeBuilder::ProcessInCaption(const Token& token) {
  const bool closes_caption =
      (token.kind == Token::Kind::StartTag && Contains(kTableStructureTags, token.data)) ||
      (token.kind == Token::Kind::EndTag && token.data == "table");
  if (closes_caption || (token.kind == Token::Kind::EndTag && token.data == "caption")) {
    if (!HasInScope("caption", Scope::Table)) {
      ++errors_;
      return;
    }
    GenerateImpliedEndTags();
    if (CurrentNode().IsElement() &&
        static_cast<dom::Element&>(CurrentNode()).TagName() != "caption") {
      ++errors_;
    }
    PopUntil("caption");
    mode_ = InsertionMode::InTable;
    if (closes_caption) {
      Process(token);  // the token that forced the close still has to be handled
    }
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "body" || token.data == "col" || token.data == "colgroup" ||
       token.data == "html" || token.data == "tbody" || token.data == "td" ||
       token.data == "tfoot" || token.data == "th" || token.data == "thead" ||
       token.data == "tr")) {
    ++errors_;
    return;
  }
  ProcessInBody(token);
}

// §13.2.6.4.12 "in column group".
void TreeBuilder::ProcessInColumnGroup(const Token& token) {
  switch (token.kind) {
    case Token::Kind::Character:
      if (IsWhitespaceOnly(token.data)) {
        InsertText(token.data);
        return;
      }
      break;

    case Token::Kind::Comment:
      InsertComment(token.data, nullptr);
      return;

    case Token::Kind::Doctype:
      ++errors_;
      return;

    case Token::Kind::StartTag:
      if (token.data == "html") {
        ProcessInBody(token);
        return;
      }
      if (token.data == "col") {
        InsertElement(token);  // void: never becomes the current node
        return;
      }
      break;

    case Token::Kind::EndTag:
      if (token.data == "colgroup") {
        if (open_elements_.empty() || open_elements_.back()->TagName() != "colgroup") {
          ++errors_;
          return;
        }
        PopCurrent();
        mode_ = InsertionMode::InTable;
        return;
      }
      if (token.data == "col") {
        ++errors_;
        return;
      }
      break;

    case Token::Kind::EndOfFile:
      ProcessInBody(token);
      return;
  }

  if (open_elements_.empty() || open_elements_.back()->TagName() != "colgroup") {
    ++errors_;
    return;
  }
  PopCurrent();
  mode_ = InsertionMode::InTable;
  Process(token);
}

// §13.2.6.4.13 "in table body".
void TreeBuilder::ProcessInTableBody(const Token& token) {
  if (token.kind == Token::Kind::StartTag) {
    if (token.data == "tr") {
      ClearStackToContext({"tbody", "tfoot", "thead"});
      InsertElement(token);
      mode_ = InsertionMode::InRow;
      return;
    }
    if (token.data == "th" || token.data == "td") {
      ++errors_;  // a cell that skipped its row gets one
      ClearStackToContext({"tbody", "tfoot", "thead"});
      InsertImplied("tr");
      mode_ = InsertionMode::InRow;
      Process(token);
      return;
    }
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "tbody" || token.data == "tfoot" || token.data == "thead")) {
    if (!HasInScope(token.data, Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tbody", "tfoot", "thead"});
    PopCurrent();
    mode_ = InsertionMode::InTable;
    return;
  }

  const bool leaves_section =
      (token.kind == Token::Kind::StartTag &&
       (token.data == "caption" || token.data == "col" || token.data == "colgroup" ||
        token.data == "tbody" || token.data == "tfoot" || token.data == "thead")) ||
      (token.kind == Token::Kind::EndTag && token.data == "table");
  if (leaves_section) {
    if (!HasInScope("tbody", Scope::Table) && !HasInScope("thead", Scope::Table) &&
        !HasInScope("tfoot", Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tbody", "tfoot", "thead"});
    PopCurrent();
    mode_ = InsertionMode::InTable;
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "body" || token.data == "caption" || token.data == "col" ||
       token.data == "colgroup" || token.data == "html" || token.data == "td" ||
       token.data == "th" || token.data == "tr")) {
    ++errors_;
    return;
  }
  ProcessInTable(token);
}

// §13.2.6.4.14 "in row".
void TreeBuilder::ProcessInRow(const Token& token) {
  if (token.kind == Token::Kind::StartTag && (token.data == "th" || token.data == "td")) {
    ClearStackToContext({"tr"});
    InsertElement(token);
    mode_ = InsertionMode::InCell;
    return;
  }
  if (token.kind == Token::Kind::EndTag && token.data == "tr") {
    if (!HasInScope("tr", Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tr"});
    PopCurrent();
    mode_ = InsertionMode::InTableBody;
    return;
  }

  const bool leaves_row =
      (token.kind == Token::Kind::StartTag &&
       (token.data == "caption" || token.data == "col" || token.data == "colgroup" ||
        token.data == "tbody" || token.data == "tfoot" || token.data == "thead" ||
        token.data == "tr")) ||
      (token.kind == Token::Kind::EndTag && token.data == "table");
  if (leaves_row) {
    if (!HasInScope("tr", Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tr"});
    PopCurrent();
    mode_ = InsertionMode::InTableBody;
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "tbody" || token.data == "tfoot" || token.data == "thead")) {
    if (!HasInScope(token.data, Scope::Table) || !HasInScope("tr", Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tr"});
    PopCurrent();
    mode_ = InsertionMode::InTableBody;
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "body" || token.data == "caption" || token.data == "col" ||
       token.data == "colgroup" || token.data == "html" || token.data == "td" ||
       token.data == "th")) {
    ++errors_;
    return;
  }
  ProcessInTable(token);
}

void TreeBuilder::CloseCell() {
  const std::string_view name = HasInScope("td", Scope::Table) ? "td" : "th";
  GenerateImpliedEndTags();
  if (CurrentNode().IsElement() && static_cast<dom::Element&>(CurrentNode()).TagName() != name) {
    ++errors_;
  }
  PopUntil(name);
  mode_ = InsertionMode::InRow;
}

// §13.2.6.4.15 "in cell". A cell is the one place inside a table where ordinary
// content is ordinary again, so most tokens go straight to the body rules.
void TreeBuilder::ProcessInCell(const Token& token) {
  if (token.kind == Token::Kind::EndTag && (token.data == "td" || token.data == "th")) {
    if (!HasInScope(token.data, Scope::Table)) {
      ++errors_;
      return;
    }
    GenerateImpliedEndTags();
    if (CurrentNode().IsElement() &&
        static_cast<dom::Element&>(CurrentNode()).TagName() != token.data) {
      ++errors_;
    }
    PopUntil(token.data);
    mode_ = InsertionMode::InRow;
    return;
  }
  if (token.kind == Token::Kind::StartTag && Contains(kTableStructureTags, token.data)) {
    // A new cell or row ends this one: `<td>a<td>b` is two cells, not nesting.
    if (!HasInScope("td", Scope::Table) && !HasInScope("th", Scope::Table)) {
      ++errors_;
      return;
    }
    CloseCell();
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "table" || token.data == "tbody" || token.data == "tfoot" ||
       token.data == "thead" || token.data == "tr")) {
    if (!HasInScope(token.data, Scope::Table)) {
      ++errors_;
      return;
    }
    CloseCell();
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "body" || token.data == "caption" || token.data == "col" ||
       token.data == "colgroup" || token.data == "html")) {
    ++errors_;
    return;
  }
  ProcessInBody(token);
}

// §13.2.6.4.16 "in select".
void TreeBuilder::ProcessInSelect(const Token& token) {
  const auto current_is = [this](std::string_view tag_name) {
    return CurrentNode().IsElement() &&
           static_cast<dom::Element&>(CurrentNode()).TagName() == tag_name;
  };
  const auto pop_current_if = [&](std::string_view tag_name) {
    if (current_is(tag_name)) {
      PopCurrent();
      return true;
    }
    return false;
  };

  switch (token.kind) {
    case Token::Kind::Character:
      InsertText(token.data);
      return;

    case Token::Kind::Comment:
      InsertComment(token.data, nullptr);
      return;

    case Token::Kind::Doctype:
      ++errors_;
      return;

    case Token::Kind::StartTag:
      if (token.data == "html") {
        ProcessInBody(token);
        return;
      }
      if (token.data == "option") {
        pop_current_if("option");
        InsertElement(token);
        return;
      }
      if (token.data == "optgroup") {
        pop_current_if("option");
        pop_current_if("optgroup");
        InsertElement(token);
        return;
      }
      if (token.data == "hr") {
        pop_current_if("option");
        pop_current_if("optgroup");
        InsertElement(token);
        return;
      }
      if (token.data == "select") {
        ++errors_;
        if (!HasInScope("select", Scope::Select)) {
          return;
        }
        PopUntil("select");
        ResetInsertionMode();
        return;
      }
      if (token.data == "input" || token.data == "textarea") {
        ++errors_;
        if (!HasInScope("select", Scope::Select)) {
          return;
        }
        PopUntil("select");
        ResetInsertionMode();
        Process(token);
        return;
      }
      if (token.data == "script") {
        SwitchToRawText(token, TokenizerState::ScriptData);
        return;
      }
      break;

    case Token::Kind::EndTag:
      if (token.data == "option") {
        if (!pop_current_if("option")) {
          ++errors_;
        }
        return;
      }
      if (token.data == "optgroup") {
        pop_current_if("option");
        if (!pop_current_if("optgroup")) {
          ++errors_;
        }
        return;
      }
      if (token.data == "select") {
        if (!HasInScope("select", Scope::Select)) {
          ++errors_;
          return;
        }
        PopUntil("select");
        ResetInsertionMode();
        return;
      }
      break;

    case Token::Kind::EndOfFile:
      ProcessInBody(token);
      return;
  }

  ++errors_;
}

// §13.2.6.4.17 "in select in table".
void TreeBuilder::ProcessInSelectInTable(const Token& token) {
  const bool table_start = token.kind == Token::Kind::StartTag &&
                           Contains(kSelectTableTags, token.data);
  const bool table_end = token.kind == Token::Kind::EndTag && Contains(kSelectTableTags, token.data);
  if (table_start || table_end) {
    ++errors_;
    if (table_start || HasInScope(token.data, Scope::Table)) {
      PopUntil("select");
      ResetInsertionMode();
      Process(token);
    }
    return;
  }
  ProcessInSelect(token);
}

}  // namespace microbrowser::html
