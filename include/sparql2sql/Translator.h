#pragma once

#include <string>

#include "sparql2sql/TranslatedPattern.h"

namespace sparql {
namespace ast {
class Query;
} // namespace ast
} // namespace sparql

namespace r2rml {
class R2RMLMapping;
} // namespace r2rml

namespace sparql2sql {

class SqlDialect;
struct TypeCatalog;

/// Translate a full, already-parsed SPARQL query against an already-parsed
/// R2RML mapping, returning a single SQL statement for the given dialect.
/// Only SELECT and ASK query forms are supported; CONSTRUCT/DESCRIBE throw
/// TranslationError naming the unsupported form.
///
/// `catalog` is optional: when supplied, join keys over type-comparable base
/// columns are emitted natively (uncast) rather than as VARCHAR comparisons -
/// index-friendly and materially faster on large tables. When null, all joins
/// fall back to the always-correct VARCHAR-cast form.
///
/// `prettyPrint` is a debug/readability aid only: when true, the returned SQL
/// is laid out with newlines, indentation and one-column-per-line formatting
/// instead of the default single-line form. It has no effect on the SQL's
/// meaning or on execution performance once handed to the engine.
std::string translateQuery(const sparql::ast::Query &query, const r2rml::R2RMLMapping &mapping,
                           const SqlDialect &dialect, const TypeCatalog *catalog = nullptr, bool prettyPrint = false);

/// Translate a Query (top-level or a `{ SELECT ... }` subquery) against an
/// existing TranslationContext, applying its own SELECT projection/
/// DISTINCT/GROUP BY/HAVING/ORDER BY/LIMIT/OFFSET, and return it as a
/// TranslatedPattern (a "SELECT ..." whose columns are exactly the query's
/// own projected variables) rather than a final wrapped string. Exposed
/// here (not kept file-local to Translator.cpp) because PatternFolder's
/// SubSelectElement handling must invoke it recursively.
///
/// `nested` distinguishes the recursive SubSelectElement call from the one
/// top-level call, and controls one thing: a nested query additionally projects
/// a runtime type-tag column for each projected variable whose annotation is
/// **not** fully determined, recording them in the result's providedTagVars.
/// Those tags cannot be reconstructed by the enclosing query - this SQL is
/// spliced in as literal text and never re-rendered - whereas a determined one
/// is a constant the outer renderer can synthesise on demand. The top-level call
/// leaves them off so an ordinary query's result columns stay exactly the
/// variables it projected.
TranslatedPattern translateQueryPattern(const sparql::ast::Query &query, TranslationContext &ctx, bool nested = false);

/// Close `body` - a rendered relation, valid apart from its CTE references -
/// over the WITH-clause entries `ctx` accumulated while producing it: the
/// hoisted rr:sqlQuery views first, then any property-path closure CTEs,
/// dropping entries nothing references and adding RECURSIVE only if a closure
/// needs it. Returns `body` unchanged when there is nothing to prepend.
///
/// translateQuery() applies this to its own result, so callers of that need not.
/// It is exposed for the other direction: code that drives translateTriplePattern
/// or renderRelation itself gets SQL referring to CTEs that only this function
/// emits, and so must call it to obtain a runnable statement.
std::string prependCtes(const TranslationContext &ctx, const std::string &body);

} // namespace sparql2sql
