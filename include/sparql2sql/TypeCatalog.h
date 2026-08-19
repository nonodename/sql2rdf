#pragma once

#include <map>
#include <string>

namespace sparql2sql {

/// Optional, plain-data column-type catalog supplied to the translator so the
/// native-typed-join-key optimization can prove two join columns are
/// type-comparable (and thus safe to compare without the VARCHAR cast).
///
/// Deliberately dependency-free: the core `sql2rdf_sparql2sql` library never
/// opens a database. The CLI (which links DuckDB) populates this from
/// information_schema/DESCRIBE and passes it in; when absent, the translator
/// falls back to VARCHAR-cast joins (always correct, just not index-friendly).
struct TypeCatalog {
	/// Logical-table identity -> (column name -> SQL type name, e.g. "BIGINT").
	/// The key is what sparql2sql::logicalTableIdentity() returns for the
	/// source: a base table's declared name, or "view:<rr:sqlQuery text>" for
	/// an R2RML view (see sparql2sql/LogicalTableSource.h - a populator that
	/// only reads information_schema types no view columns, which is what makes
	/// DATATYPE() over a view-backed literal unanswerable).
	std::map<std::string, std::map<std::string, std::string>> columnTypes;

	/// Look up a column's SQL type; empty string if unknown.
	std::string typeOf(const std::string &table, const std::string &column) const;

	/// True iff both columns' types are known and comparable without a lexical
	/// (VARCHAR) cast changing equality semantics. Conservative: unknown types
	/// return false, so a missing/partial catalog never enables an unsafe
	/// rewrite.
	bool comparable(const std::string &tableA, const std::string &columnA, const std::string &tableB,
	                const std::string &columnB) const;

	/// True iff the column's declared SQL type is already a VARCHAR-family
	/// string type (VARCHAR/CHAR/TEXT/STRING/BPCHAR, any width/precision), so a
	/// `CAST(... AS VARCHAR)` around it is a no-op that can be dropped. False -
	/// the conservative default - when the type is unknown or anything else.
	bool isStringType(const std::string &table, const std::string &column) const;
};

/// R2RML Section 10.2's natural mapping from a SQL datatype to an XSD datatype
/// IRI, keyed on a SQL *type name* as a catalog reports it. Case-insensitive
/// and precision/width-insensitive ("DECIMAL(18,2)" == "decimal"). Returns the
/// empty string for a type with no natural mapping, or an unrecognised one,
/// meaning "no datatype can be inferred" - never a guess.
///
/// Deliberately distinct from - and not derivable from -
/// r2rml::SQLValue::datatypeIRI(), which classifies a *runtime value* through a
/// five-way enum with no notion of SQL type names. This one is static, needed
/// before any row is read, and covers the full table.
std::string naturalXsdDatatype(const std::string &sqlTypeName);

} // namespace sparql2sql
