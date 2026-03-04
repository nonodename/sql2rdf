#include "r2rml/MapSQLRow.h"
#include "r2rml/StringSQLValue.h"

namespace r2rml {

MapSQLRow::MapSQLRow(std::map<std::string, std::unique_ptr<SQLValue>> columns) : columns_(std::move(columns)) {
}

MapSQLRow::MapSQLRow(const MapSQLRow &other) {
	for (const auto &p : other.columns_) {
		columns_[p.first] = p.second->clone();
	}
}

MapSQLRow &MapSQLRow::operator=(const MapSQLRow &other) {
	columns_.clear();
	for (const auto &p : other.columns_) {
		columns_[p.first] = p.second->clone();
	}
	return *this;
}

std::unique_ptr<SQLValue> MapSQLRow::getValue(const std::string &columnName) const {
	auto it = columns_.find(columnName);
	if (it == columns_.end()) {
		return std::unique_ptr<SQLValue>(new StringSQLValue());
	}
	return it->second->clone();
}

bool MapSQLRow::isNull(const std::string &columnName) const {
	auto it = columns_.find(columnName);
	if (it == columns_.end()) {
		return true;
	}
	return it->second->isNull();
}

std::unique_ptr<SQLRow> MapSQLRow::clone() const {
	std::map<std::string, std::unique_ptr<SQLValue>> cloned;
	for (const auto &p : columns_) {
		cloned[p.first] = p.second->clone();
	}
	return std::unique_ptr<SQLRow>(new MapSQLRow(std::move(cloned)));
}

} // namespace r2rml
