#include "r2rml/MapSQLRow.h"

namespace r2rml {

MapSQLRow::MapSQLRow(std::map<std::string, SQLValue> columns) : columns_(std::move(columns)) {
}

SQLValue MapSQLRow::getValue(const std::string &columnName) const {
	auto it = columns_.find(columnName);
	if (it == columns_.end()) {
		return SQLValue();
	}
	return it->second;
}

bool MapSQLRow::isNull(const std::string &columnName) const {
	auto it = columns_.find(columnName);
	if (it == columns_.end()) {
		return true;
	}
	return it->second.isNull();
}

std::unique_ptr<SQLRow> MapSQLRow::clone() const {
	return std::unique_ptr<SQLRow>(new MapSQLRow(columns_));
}

} // namespace r2rml
