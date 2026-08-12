#pragma once

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

} // namespace sparql2sql
