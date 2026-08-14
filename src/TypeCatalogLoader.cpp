#include "sql2rdf/TypeCatalogLoader.h"

#include <cctype>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <string>
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

void loadBaseTableTypes(r2rml::SQLConnection &conn, sparql2sql::TypeCatalog &catalog) {
	std::unique_ptr<r2rml::SQLResultSet> rs =
	    conn.execute("SELECT table_name, column_name, data_type FROM information_schema.columns");
	while (rs->next()) {
		const r2rml::SQLRow &row = rs->getCurrentRow();
		std::vector<std::string> names = row.columnNames();
		const std::string *tableCol = findResultColumn(names, "table_name");
		if (tableCol == nullptr) {
			continue;
		}
		std::unique_ptr<r2rml::SQLValue> table = row.getValue(*tableCol);
		std::string column;
		std::string type;
		if (table->isNull() || !readPair(row, "column_name", "data_type", column, type)) {
			continue;
		}
		catalog.columnTypes[table->asString()][column] = type;
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
