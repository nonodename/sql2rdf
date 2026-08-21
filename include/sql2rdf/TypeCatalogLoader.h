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
/// can (a) emit native (uncast) join keys for type-comparable columns,
/// (b) apply R2RML Section 10.2's natural mapping to a bare rr:column literal,
/// which is what lets DATATYPE() answer for a mapping that declares no
/// rr:datatype, (c) skip the "IS NOT NULL" guard on a column the DDL declares
/// NOT NULL, and (d) drop a candidate arm's DISTINCT once a declared PRIMARY KEY
/// / UNIQUE constraint proves its projected rows are already distinct.
///
/// Everything read here is a schema fact, never a data statistic - no row
/// counts, no cardinality estimates. That is what makes a rewrite proved against
/// this catalog stay valid as rows change; cardinality is deliberately left to
/// the target engine, which has better numbers than this sweep could get.
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
/// Constraint facts come from base tables only: `is_nullable` rides along on the
/// information_schema.columns sweep at no extra cost, and keys come from one
/// extra query over information_schema.table_constraints / key_column_usage.
/// An rr:sqlQuery view gets neither - no backend reports constraints for an
/// arbitrary query's result columns - so key- and NOT NULL-dependent rewrites
/// always decline on a view-backed source. The key query is best-effort: those
/// two views are less universally implemented than `columns`, and a backend
/// lacking them just leaves `uniqueKeys` empty.
///
/// Lives in its own target (sql2rdf_type_catalog_loader), separate from
/// sql2rdf_sparql2sql: it issues SQL of its own (information_schema /
/// DESCRIBE) against a live r2rml::SQLConnection, whereas the core library
/// never touches a connection. It has no DuckDB dependency of its own -
/// `conn` can be any r2rml::SQLConnection implementation - which is what lets
/// FetchContent consumers with their own backend link it directly. The
/// core's contribution is sparql2sql::mappingViewSources(), which says
/// *which* queries to describe and under which catalog key to file the
/// answers.
///
/// `mapping` may be null, in which case only base-table types are read.
/// Throws whatever the connection throws on the information_schema sweep (the
/// caller treats that as "no catalog", which is always correct, just slower and
/// less answerable); a view that fails to describe is skipped silently, since a
/// mapping may legitimately contain a view this database cannot bind.
void loadTypeCatalog(r2rml::SQLConnection &conn, const r2rml::R2RMLMapping *mapping, sparql2sql::TypeCatalog &catalog);

} // namespace sql2rdf
