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

/// Translate a full, already-parsed SPARQL query against an already-parsed
/// R2RML mapping, returning a single SQL statement for the given dialect.
/// Only SELECT and ASK query forms are supported; CONSTRUCT/DESCRIBE throw
/// TranslationError naming the unsupported form.
std::string translateQuery(const sparql::ast::Query &query, const r2rml::R2RMLMapping &mapping,
                           const SqlDialect &dialect);

/// Translate a Query (top-level or a `{ SELECT ... }` subquery) against an
/// existing TranslationContext, applying its own SELECT projection/
/// DISTINCT/GROUP BY/HAVING/ORDER BY/LIMIT/OFFSET, and return it as a
/// TranslatedPattern (a "SELECT ..." whose columns are exactly the query's
/// own projected variables) rather than a final wrapped string. Exposed
/// here (not kept file-local to Translator.cpp) because PatternFolder's
/// SubSelectElement handling must invoke it recursively.
TranslatedPattern translateQueryPattern(const sparql::ast::Query &query, TranslationContext &ctx);

} // namespace sparql2sql
