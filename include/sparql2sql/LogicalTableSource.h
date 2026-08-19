#pragma once

#include <map>
#include <string>
#include <vector>

namespace r2rml {
class LogicalTable;
class R2RMLMapping;
} // namespace r2rml

namespace sparql2sql {

/// A stable identity for a logical table, and the key under which the
/// translator looks its column types up in a TypeCatalog: a base table's
/// declared name (rr:tableName), or `"view:" + <the rr:sqlQuery text>` for an
/// R2RML view. Empty for an unrecognized LogicalTable subtype.
///
/// Public because a catalog *populator* has to agree with the translator on
/// this key exactly - it is the only way to give a view's columns declared
/// types (see mappingViewSources below). The view form deliberately keys on
/// the raw, un-stripped SQL text: two views are the same source iff their
/// query text is identical.
std::string logicalTableIdentity(const r2rml::LogicalTable &logicalTable);

/// Strip trailing whitespace and at most one trailing ';' from a SQL string,
/// yielding text safe to embed as a derived table (`(<sql>) AS t1`) or to hand
/// to a backend's schema-description statement. Idempotent.
std::string stripTrailingSemicolon(std::string sql);

/// One rr:sqlQuery-backed logical table of a mapping, paired with the catalog
/// key its columns must be filed under.
struct ViewSource {
	/// The TypeCatalog::columnTypes key for this view ("view:<raw sql>").
	std::string identity;
	/// The view's SQL, semicolon-stripped and ready to embed or describe.
	std::string sql;
};

/// Every distinct rr:sqlQuery logical table the mapping reads from, in
/// first-appearance order and de-duplicated by identity.
///
/// A view's columns appear in no information_schema, so a catalog built only
/// from one leaves them untyped - and R2RML Section 10.2's natural mapping
/// then cannot supply a datatype for a bare rr:column literal over a view,
/// which is what makes DATATYPE() refuse an answer. A caller that holds a
/// database connection walks this list, asks the backend for each query's
/// result schema (in DuckDB: `DESCRIBE SELECT * FROM (<sql>) AS v`, which
/// binds the query without executing it), and files the answers under
/// `identity`. The core library stays connection-free: it only tells the
/// caller what to ask about.
std::vector<ViewSource> mappingViewSources(const r2rml::R2RMLMapping &mapping);

/// Unescape a single-quoted SQL string literal, e.g. `'ab''c'` -> `ab'c`.
/// Returned unchanged if it doesn't look like one (missing/mismatched
/// surrounding quotes).
std::string unquoteSqlStringLiteral(const std::string &quoted);

/// Column name -> the VARCHAR-cast rendering every row of `sql`'s result
/// would produce for that column, for every column in `sql`'s top-level
/// SELECT list that is a bare literal (a single-quoted string, the keyword
/// TRUE/FALSE, or a bare integer) rather than a real per-row column
/// reference - e.g. `SELECT true AS FLAG, 'x' AS KIND FROM ...` yields
/// {"FLAG": "true", "KIND": "x"}.
///
/// Detected by a lightweight textual scan of the query's top-level SELECT
/// list (this project's dependency-free core has no SQL parser to build on).
/// A column outside these three literal forms, or a query shape this scan
/// doesn't confidently recognize (no top-level "SELECT ... FROM ..."), is
/// simply absent from the result - always the conservative, safe answer, and
/// what makes the optimization built on this data provably correct: it only
/// ever removes a comparison this scan is certain evaluates the same way for
/// every row.
std::map<std::string, std::string> detectConstantSelectColumns(const std::string &sql);

} // namespace sparql2sql
