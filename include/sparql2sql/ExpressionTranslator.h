#pragma once

#include <string>

#include "sparql-parser/ast/Expression.h"
#include "sparql2sql/TermInfo.h"
#include "sparql2sql/TranslatedPattern.h"

namespace sparql2sql {

/// An expression translated as a full RDF *term*: its lexical form and its
/// runtime type dimension, together.
///
/// `tag` is a SQL scalar in encodeTag()'s encoding, and is empty when this
/// translator cannot produce one - either because the expression's dimension is
/// not statically determined *and* its inputs carry no tag column (nothing
/// demanded one, see TagDemand.h), or because the construct's dimension is
/// genuinely not derivable. Every consumer must cope with an empty tag by
/// degrading to what it did before tags existed; none may substitute NULL, which
/// this representation reserves for *unbound*.
///
/// INVARIANT: `tag` is SQL NULL exactly when `value` is. Constant tags over
/// computed values are null-guarded against the value to keep it (a TRY_CAST
/// failure inside `value` must not leave a non-NULL tag beside a NULL term).
struct TermSql {
	std::string value;
	std::string tag;
	TermInfo staticInfo;
};

/// Translate a FILTER/BIND/ORDER BY/HAVING/GROUP BY/SELECT expression into a
/// SQL scalar (or boolean) expression together with its type tag.
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
TermSql translateTerm(const sparql::ast::Expression &expr, const TranslatedPattern &scope,
                      const std::string &scopeAlias, TranslationContext &ctx);

/// translateTerm()'s lexical-form half. The spelling every boolean context
/// (FILTER, HAVING, a join's ON clause) wants, since a boolean has no term
/// dimension worth carrying.
std::string translateExpression(const sparql::ast::Expression &expr, const TranslatedPattern &scope,
                                const std::string &scopeAlias, TranslationContext &ctx);

} // namespace sparql2sql
