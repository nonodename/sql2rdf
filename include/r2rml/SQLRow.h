#pragma once

#include "SQLValue.h"
#include <memory>
#include <string>
#include <vector>

namespace r2rml {

/**
 * Abstract interface for a single row of SQL results.  Values may be of
 * various types and are accessed by column name.
 *
 * Implement this interface to proxy rows from a custom backend without going
 * through the map-based MapSQLRow.
 */
class SQLRow {
public:
	virtual ~SQLRow() = default;

	virtual std::unique_ptr<SQLValue> getValue(const std::string &columnName) const = 0;
	virtual bool isNull(const std::string &columnName) const = 0;

	/** Column names present on this row, e.g. for printing headers. */
	virtual std::vector<std::string> columnNames() const = 0;

	/** Deep-copy this row.  Used internally when rows must be cached (e.g.
	 *  join evaluation). */
	virtual std::unique_ptr<SQLRow> clone() const = 0;
};

} // namespace r2rml
