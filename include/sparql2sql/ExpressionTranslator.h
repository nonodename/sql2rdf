#pragma once

#include <string>

#include "sparql-parser/ast/Expression.h"
#include "sparql2sql/TranslatedPattern.h"

namespace sparql2sql {

/// Translate a FILTER/BIND/ORDER BY/HAVING/GROUP BY/SELECT expression into
/// a SQL scalar (or boolean) expression.
///
/// `scope` is the relation the expression is evaluated against (its
/// `v_<name>` columns are what VarExpr resolves to); `scopeAlias` is the
/// SQL alias the *caller* has bound (or is about to bind) `scope`'s rows to
/// in its own FROM clause - every variable reference is emitted fully
/// qualified as `scopeAlias.v_<name>` rather than bare. This is not just a
/// style choice: EXISTS/NOT EXISTS introduces its own nested subquery with
/// its own FROM alias, and an unqualified reference to a variable that
/// happens to also be projected by that nested pattern would incorrectly
/// bind to the nested subquery's own column instead of correlating out to
/// `scope` - always qualifying eliminates that ambiguity everywhere, not
/// just inside EXISTS.
///
/// Throws TranslationError for any variable reference not in
/// scope.allVars() (SPARQL's precise per-row unbound-variable/type-error
/// semantics are not emulated - out-of-scope variables are rejected at
/// translation time instead), and for any deferred/unsupported builtin
/// function.
std::string translateExpression(const sparql::ast::Expression &expr, const TranslatedPattern &scope,
                                const std::string &scopeAlias, TranslationContext &ctx);

} // namespace sparql2sql
