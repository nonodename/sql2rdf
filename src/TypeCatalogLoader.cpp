#include "sql2rdf/TypeCatalogLoader.h"

#include <cctype>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "r2rml/R2RMLMapping.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"
#include "sparql2sql/LogicalTableSource.h"
#include "sparql2sql/TypeCatalog.h"

namespace sql2rdf {

namespace {

// Resolve a result column by name, case-insensitively: backends report result
// column names in their own case (DuckDB upper-cases them). Returns nullptr if
// absent.
const std::string *findResultColumn(const std::vector<std::string> &names, const char *wanted) {
	for (const auto &n : names) {
		if (n.size() != std::strlen(wanted)) {
			continue;
		}
		bool same = true;
		for (std::size_t i = 0; i < n.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(n[i])) != std::tolower(static_cast<unsigned char>(wanted[i]))) {
				same = false;
				break;
			}
		}
		if (same) {
			return &n;
		}
	}
	return nullptr;
}

// Read a (name, type) pair out of one catalog/DESCRIBE row, resolving both
// columns by NAME rather than position: SQLRow holds its columns in a
// std::map, so columnNames() comes back sorted alphabetically rather than in
// SELECT order. Returns false if either is missing or NULL.
bool readPair(const r2rml::SQLRow &row, const char *nameColumn, const char *typeColumn, std::string &nameOut,
              std::string &typeOut) {
	std::vector<std::string> names = row.columnNames();
	const std::string *nameCol = findResultColumn(names, nameColumn);
	const std::string *typeCol = findResultColumn(names, typeColumn);
	if (nameCol == nullptr || typeCol == nullptr) {
		return false;
	}
	std::unique_ptr<r2rml::SQLValue> name = row.getValue(*nameCol);
	std::unique_ptr<r2rml::SQLValue> type = row.getValue(*typeCol);
	if (name->isNull() || type->isNull()) {
		return false;
	}
	nameOut = name->asString();
	typeOut = type->asString();
	return true;
}

// Read one named result column as a string. Resolved by name,
// case-insensitively, for the same reason readPair is. Returns false when the
// column is absent from the result or NULL in this row.
bool readColumn(const r2rml::SQLRow &row, const char *wanted, std::string &out) {
	std::vector<std::string> names = row.columnNames();
	const std::string *col = findResultColumn(names, wanted);
	if (col == nullptr) {
		return false;
	}
	std::unique_ptr<r2rml::SQLValue> value = row.getValue(*col);
	if (value->isNull()) {
		return false;
	}
	out = value->asString();
	return true;
}

// True for information_schema's 'NO' spelling of is_nullable, case-insensitively
// and ignoring surrounding whitespace. Anything else - 'YES', an unexpected
// spelling, an absent column - reads as "not known to be non-nullable", which is
// the conservative direction: a guard we would have emitted anyway stays.
bool isNoNullable(const std::string &raw) {
	std::string trimmed;
	for (char c : raw) {
		if (c != ' ' && c != '\t') {
			trimmed += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	return trimmed == "no";
}

void loadBaseTableTypes(r2rml::SQLConnection &conn, sparql2sql::TypeCatalog &catalog) {
	// is_nullable rides along on the sweep we already run - the NOT NULL facts
	// cost no extra round trip and no rows read.
	std::unique_ptr<r2rml::SQLResultSet> rs =
	    conn.execute("SELECT table_name, column_name, data_type, is_nullable FROM information_schema.columns");
	while (rs->next()) {
		const r2rml::SQLRow &row = rs->getCurrentRow();
		std::string table;
		std::string column;
		std::string type;
		if (!readColumn(row, "table_name", table) || !readPair(row, "column_name", "data_type", column, type)) {
			continue;
		}
		catalog.columnTypes[table][column] = type;
		std::string nullable;
		if (readColumn(row, "is_nullable", nullable) && isNoNullable(nullable)) {
			catalog.notNullColumns[table].insert(column);
		}
	}
}

// Read the PRIMARY KEY / UNIQUE constraints of every base table, grouping
// key_column_usage's per-column rows by constraint name. Best-effort: the caller
// swallows any failure, since these two information_schema views are less
// universally implemented than `columns` and a backend without them simply
// leaves every key-dependent rewrite declining.
void loadUniqueKeys(r2rml::SQLConnection &conn, sparql2sql::TypeCatalog &catalog) {
	std::unique_ptr<r2rml::SQLResultSet> rs = conn.execute(
	    "SELECT tc.table_name AS table_name, kcu.constraint_name AS constraint_name, kcu.column_name AS column_name "
	    "FROM information_schema.table_constraints AS tc "
	    "JOIN information_schema.key_column_usage AS kcu ON tc.constraint_name = kcu.constraint_name "
	    "WHERE tc.constraint_type IN ('PRIMARY KEY', 'UNIQUE')");

	// (table, constraint) -> its column set. Grouped here rather than trusting
	// row order: a composite key arrives as several rows, and nothing guarantees
	// they are adjacent.
	std::map<std::pair<std::string, std::string>, std::set<std::string>> keys;
	while (rs->next()) {
		const r2rml::SQLRow &row = rs->getCurrentRow();
		std::string table;
		std::string constraint;
		std::string column;
		if (!readColumn(row, "table_name", table) || !readColumn(row, "constraint_name", constraint) ||
		    !readColumn(row, "column_name", column)) {
			continue;
		}
		keys[std::make_pair(table, constraint)].insert(column);
	}
	for (const auto &entry : keys) {
		catalog.uniqueKeys[entry.first.first].push_back(entry.second);
	}
}

// Describe one rr:sqlQuery view's result schema and file its column types
// under the view's catalog identity. Wrapping the view in `SELECT * FROM
// (<sql>) AS v` mirrors exactly how the translator embeds it, so the described
// columns are the ones the generated SQL will actually project.
void loadViewTypes(r2rml::SQLConnection &conn, const sparql2sql::ViewSource &view, sparql2sql::TypeCatalog &catalog) {
	std::unique_ptr<r2rml::SQLResultSet> rs = conn.execute("DESCRIBE SELECT * FROM (" + view.sql + ") AS sql2rdf_view");
	std::map<std::string, std::string> columnTypes;
	while (rs->next()) {
		std::string column;
		std::string type;
		if (!readPair(rs->getCurrentRow(), "column_name", "column_type", column, type)) {
			continue;
		}
		columnTypes[column] = type;
	}
	if (columnTypes.empty()) {
		return;
	}
	// Merge rather than assign: a description that came back partial must not
	// wipe types an earlier pass already established for the same identity.
	for (const auto &entry : columnTypes) {
		catalog.columnTypes[view.identity][entry.first] = entry.second;
	}
}

} // namespace

void loadTypeCatalog(r2rml::SQLConnection &conn, const r2rml::R2RMLMapping *mapping, sparql2sql::TypeCatalog &catalog) {
	loadBaseTableTypes(conn, catalog);
	try {
		loadUniqueKeys(conn, catalog);
	} catch (const std::exception &) {
		// A backend without information_schema.table_constraints /
		// key_column_usage leaves `uniqueKeys` empty, which every consumer reads
		// as "no key is known" and declines on. Non-fatal for the same reason a
		// view that won't describe is: correct, just less optimizable.
	}
	if (mapping == nullptr) {
		return;
	}
	for (const auto &view : sparql2sql::mappingViewSources(*mapping)) {
		try {
			loadViewTypes(conn, view, catalog);
		} catch (const std::exception &) {
			// A view this database cannot bind (a table the mapping expects but
			// this database lacks, a dialect quirk) leaves its columns untyped -
			// the same position as before this loader existed, which is correct,
			// just less answerable. Never fatal: the rest of the mapping still
			// translates.
			continue;
		}
	}
}

} // namespace sql2rdf
