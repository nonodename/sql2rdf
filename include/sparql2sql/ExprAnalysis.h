#pragma once

#include <string>
#include <vector>

namespace sparql {
namespace ast {
class Expression;
} // namespace ast
} // namespace sparql

namespace sparql2sql {

/// Recursively collect every VarRef name appearing anywhere in an expression
/// tree. Deliberately does NOT descend into an EXISTS/NOT-EXISTS
/// subexpression: an EXISTS's own pattern variables are locally scoped, so its
/// interior VarRefs are not dependencies of the enclosing expression in the
/// sense these callers need (BIND optional-var over-approximation; filter
/// pushdown, which never pushes EXISTS-bearing conjuncts - see containsExists).
void collectVarRefs(const sparql::ast::Expression &expr, std::vector<std::string> &out);

/// True if `expr` contains an EXISTS / NOT EXISTS anywhere in its tree.
/// Filter pushdown uses this to refuse to push a conjunct whose correlation
/// variables collectVarRefs intentionally does not surface.
bool containsExists(const sparql::ast::Expression &expr);

/// Split an expression into its top-level conjuncts, descending through
/// logical-AND (BinaryOp::And) only: `a && b && c` -> {&a, &b, &c}. Any
/// non-AND expression returns as a single-element list. The returned pointers
/// borrow into `expr`'s AST subtree (valid for that AST's lifetime).
std::vector<const sparql::ast::Expression *> splitConjuncts(const sparql::ast::Expression &expr);

} // namespace sparql2sql
