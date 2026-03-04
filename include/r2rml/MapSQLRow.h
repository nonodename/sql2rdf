#pragma once

#include "SQLRow.h"
#include "SQLValue.h"
#include <map>
#include <memory>
#include <string>

namespace r2rml {

/**
 * Concrete SQLRow implementation backed by a std::map of column names to
 * SQLValues.  This is the default row type produced by DuckDBConnection and
 * used by the test helpers.
 */
class MapSQLRow : public SQLRow {
public:
	MapSQLRow() = default;
	explicit MapSQLRow(std::map<std::string, std::unique_ptr<SQLValue>> columns);

	MapSQLRow(const MapSQLRow &other);
	MapSQLRow &operator=(const MapSQLRow &other);

	MapSQLRow(MapSQLRow &&) = default;
	MapSQLRow &operator=(MapSQLRow &&) = default;

	std::unique_ptr<SQLValue> getValue(const std::string &columnName) const override;
	bool isNull(const std::string &columnName) const override;
	std::unique_ptr<SQLRow> clone() const override;

private:
	std::map<std::string, std::unique_ptr<SQLValue>> columns_;
};

} // namespace r2rml
