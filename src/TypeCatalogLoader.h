#pragma once

#include <string>

namespace r2rml {
class R2RMLMapping;
class SQLConnection;
} // namespace r2rml

namespace sparql2sql {
struct TypeCatalog;
} // namespace sparql2sql

namespace sql2rdf {

/// Fill a TypeCatalog from a live connection, so the SPARQL-to-SQL translator
/// can (a) emit native (uncast) join keys for type-comparable columns and
/// (b) apply R2RML Section 10.2's natural mapping to a bare rr:column literal,
/// which is what lets DATATYPE() answer for a mapping that declares no
/// rr:datatype.
///
/// Two sources of types, because they cover disjoint ground:
///   - every base table's columns, read in one sweep of information_schema;
///   - each rr:sqlQuery logical table's *result* columns, read by describing
///     the query. These appear in no information_schema at all - not the
///     computed ones, and not even the pass-through ones under the view's own
///     identity - so without this step every view-backed literal's datatype is
///     unknown, and one such candidate term map is enough to make DATATYPE()
///     refuse an answer for a predicate whose other candidates are typed.
///
/// Describing a query binds it without executing it, so this costs a plan per
/// view and reads no rows.
///
/// Deliberately lives in the CLI layer, not in sql2rdf_sparql2sql: it issues
/// SQL of its own (information_schema / DESCRIBE), which is backend-specific,
/// whereas the core library never touches a connection. The core's
/// contribution is sparql2sql::mappingViewSources(), which says *which*
/// queries to describe and under which catalog key to file the answers.
///
/// `mapping` may be null, in which case only base-table types are read.
/// Throws whatever the connection throws on the information_schema sweep (the
/// caller treats that as "no catalog", which is always correct, just slower and
/// less answerable); a view that fails to describe is skipped silently, since a
/// mapping may legitimately contain a view this database cannot bind.
void loadTypeCatalog(r2rml::SQLConnection &conn, const r2rml::R2RMLMapping *mapping, sparql2sql::TypeCatalog &catalog);

} // namespace sql2rdf
