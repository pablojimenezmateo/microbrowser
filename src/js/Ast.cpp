#include "js/Ast.h"

#include <array>

namespace microbrowser::js {

namespace {

constexpr std::string_view kNames[] = {
    "Number", "String", "Template", "RegExp", "Boolean", "Null", "Id", "This", "Super",
    "Array", "Object", "Property", "Function", "Arrow", "ClassExpr", "Method",
    "Params", "Rest", "Default", "Unary", "Update", "Binary", "Logical", "Assign",
    "Cond", "Call", "New", "Member", "Seq", "Spread", "TaggedTemplate",
    "Program", "Block", "Var", "Declarator", "ExprStmt", "If", "For", "ForIn",
    "While", "DoWhile", "Return", "Break", "Continue", "Throw", "Try", "Switch",
    "Case", "Labeled", "FunctionDecl", "ClassDecl", "Empty", "Debugger",
};

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

}  // namespace microbrowser::js
