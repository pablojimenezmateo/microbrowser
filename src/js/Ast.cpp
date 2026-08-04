#include "js/Ast.h"

#include <algorithm>
#include <array>

namespace microbrowser::js {

namespace {

constexpr std::string_view kNames[] = {
    "Number", "BigInt", "String", "Template", "RegExp", "Boolean", "Null", "Id", "This", "Super",
    "Array", "Object", "Property", "Function", "Arrow", "ClassExpr", "Method",
    "Params", "Rest", "Default", "Yield", "Unary", "Update", "Binary", "Logical", "Assign",
    "Cond", "Call", "New", "NewTarget", "Member", "Seq", "Spread", "TaggedTemplate",
    "Program", "Block", "Var", "Declarator", "ExprStmt", "If", "For", "ForIn",
    "While", "DoWhile", "Return", "Break", "Continue", "Throw", "Try", "Switch",
    "Case", "Labeled", "FunctionDecl", "ClassDecl", "Empty", "Debugger",
    "Import", "ImportName", "Export", "ExportName", "ImportMeta", "ImportCall",
};

// The table is positional, so a kind inserted anywhere but the end renames
// every kind after it and a dump test starts asserting the wrong thing quietly.
// This is the only thing that would say so.
static_assert(std::size(kNames) == static_cast<std::size_t>(NodeKind::ImportCall) + 1,
              "kNames has one entry per NodeKind, in order");

void Dump(const Node& node, std::string& out) {
  out.push_back('(');
  const auto index = static_cast<std::size_t>(node.kind);
  out += index < std::size(kNames) ? kNames[index] : "?";
  if (!node.string.empty()) {
    out.push_back(' ');
    out.push_back('"');
    out += node.string;
    out.push_back('"');
  }
  for (const std::unique_ptr<Node>& child : node.children) {
    out.push_back(' ');
    if (child == nullptr) {
      // A null child is meaningful -- an array hole, a missing else -- so the
      // dump shows it rather than eliding it and making two different trees
      // print the same.
      out += "_";
    } else {
      Dump(*child, out);
    }
  }
  out.push_back(')');
}

}  // namespace

std::string DumpAst(const Node& node) {
  std::string out;
  Dump(node, out);
  return out;
}

namespace {

void CollectPatternNames(const Node& target, std::vector<std::string>& out) {
  switch (target.kind) {
    case NodeKind::Identifier:
      // Linear rather than a set: a function declares a handful of names, and
      // the order is the source's, which keeps a compiled prologue stable.
      if (std::find(out.begin(), out.end(), target.string) == out.end()) {
        out.push_back(target.string);
      }
      return;
    case NodeKind::ArrayLiteral:
      for (const NodePtr& element : target.children) {
        if (element != nullptr) {
          CollectPatternNames(*element, out);
        }
      }
      return;
    case NodeKind::ObjectLiteral:
      for (const NodePtr& property : target.children) {
        if (property != nullptr && property->kind == NodeKind::Property &&
            property->Child(0) != nullptr) {
          CollectPatternNames(*property->Child(0), out);
        }
      }
      return;
    case NodeKind::AssignmentPattern:
    case NodeKind::RestElement:
    case NodeKind::Spread:
      if (target.Child(0) != nullptr) {
        CollectPatternNames(*target.Child(0), out);
      }
      return;
    default:
      // A member expression as a target assigns through something that already
      // exists and declares nothing.
      return;
  }
}

void CollectVars(const Node& node, std::vector<std::string>& out) {
  switch (node.kind) {
    // A nested function's `var`s are its own. Its *name*, where it has one, is
    // hoisted by the caller as a function declaration rather than here.
    case NodeKind::FunctionDeclaration:
    case NodeKind::FunctionExpression:
    case NodeKind::ArrowFunction:
    case NodeKind::ClassDeclaration:
    case NodeKind::ClassExpression:
    case NodeKind::MethodDefinition:
      return;

    case NodeKind::VariableDeclaration:
      if (node.string != "var") {
        return;  // `let` and `const` belong to the block they are written in
      }
      for (const NodePtr& declarator : node.children) {
        if (declarator != nullptr && declarator->Child(0) != nullptr) {
          CollectPatternNames(*declarator->Child(0), out);
        }
      }
      return;

    default:
      break;
  }
  // Everything else is descended through. Deliberately every child of every
  // other kind rather than an enumerated list of the statements that can
  // contain a declaration: the list is long -- blocks, both `for` heads and
  // their bodies, `if` arms, `try`/`catch`/`finally`, `switch` cases, labels,
  // `with` -- and a missing entry is a `var` that silently does not exist.
  for (const NodePtr& child : node.children) {
    if (child != nullptr) {
      CollectVars(*child, out);
    }
  }
}

}  // namespace

void CollectVarNames(const Node& body, std::vector<std::string>& out) {
  // The body itself is a Block or a Program, so its children are walked rather
  // than the node -- entering through CollectVars would stop immediately if
  // `body` were ever a function node.
  for (const NodePtr& child : body.children) {
    if (child != nullptr) {
      CollectVars(*child, out);
    }
  }
}

}  // namespace microbrowser::js
