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
	/// logical table name -> (column name -> SQL type name, e.g. "BIGINT").
	std::map<std::string, std::map<std::string, std::string>> columnTypes;

	/// Look up a column's SQL type; empty string if unknown.
	std::string typeOf(const std::string &table, const std::string &column) const;

	/// True iff both columns' types are known and comparable without a lexical
	/// (VARCHAR) cast changing equality semantics. Conservative: unknown types
	/// return false, so a missing/partial catalog never enables an unsafe
	/// rewrite.
	bool comparable(const std::string &tableA, const std::string &columnA, const std::string &tableB,
	                const std::string &columnB) const;
};

} // namespace sparql2sql
