#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace sparql2sql {

/// Optional, plain-data column-type catalog supplied to the translator so the
/// native-typed-join-key optimization can prove two join columns are
/// type-comparable (and thus safe to compare without the VARCHAR cast), plus
/// the DDL constraint facts (NOT NULL, PRIMARY KEY / UNIQUE) that let it drop
/// vacuous null guards and provably-redundant per-candidate DISTINCTs.
///
/// Everything here is a *schema* (DDL) fact, never a data statistic: a rewrite
/// proved against it stays valid as rows come and go. Deliberately so - row
/// counts and other cardinality estimates are the target engine's job, and
/// baking a snapshot of them into generated SQL would go stale.
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

	/// Logical-table identity -> the columns the DDL declares NOT NULL. Same key
	/// space as `columnTypes`. A column absent from this set is simply "not known
	/// to be non-nullable"; absence never asserts nullability.
	std::map<std::string, std::set<std::string>> notNullColumns;

	/// Logical-table identity -> one column set per declared PRIMARY KEY / UNIQUE
	/// constraint. Same key space as `columnTypes`. Only base tables ever appear:
	/// an rr:sqlQuery view has no constraint metadata in any backend, so every
	/// key-dependent rewrite declines on one.
	std::map<std::string, std::vector<std::set<std::string>>> uniqueKeys;

	/// Look up a column's SQL type; empty string if unknown.
	std::string typeOf(const std::string &table, const std::string &column) const;

	/// True iff the DDL declares this column NOT NULL. Conservative: an unknown
	/// table or column returns false, so a missing/partial catalog only ever
	/// keeps a guard that was already there.
	bool isNotNull(const std::string &table, const std::string &column) const;

	/// True iff some declared PRIMARY KEY / UNIQUE constraint of `table` has all
	/// its columns present in `columns` - i.e. `columns` functionally determines
	/// the row, so a tuple built injectively from them is already duplicate-free.
	///
	/// A UNIQUE key over a nullable column is still accepted: the caller only
	/// ever asks about columns a candidate arm projects, and every such column is
	/// either declared NOT NULL or carries the arm's own "IS NOT NULL" guard
	/// (R2RML's null-column-drops-the-term rule), so no row reaching the
	/// projection can hold a NULL in a key position.
	///
	/// Conservative: an unknown table, or no key contained in `columns`, returns
	/// false.
	bool coversUniqueKey(const std::string &table, const std::set<std::string> &columns) const;

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
