#include "r2rml/MapSQLRow.h"
#include "r2rml/StringSQLValue.h"

namespace r2rml {

MapSQLRow::MapSQLRow(std::map<std::string, std::unique_ptr<SQLValue>> columns) : columns_(std::move(columns)) {
}

MapSQLRow::MapSQLRow(const MapSQLRow &other) {
	for (const auto &[name, value] : other.columns_) {
		columns_[name] = value->clone();
	}
}

MapSQLRow &MapSQLRow::operator=(const MapSQLRow &other) {
	columns_.clear();
	for (const auto &[name, value] : other.columns_) {
		columns_[name] = value->clone();
	}
	return *this;
}

std::unique_ptr<SQLValue> MapSQLRow::getValue(const std::string &columnName) const {
	if (auto it = columns_.find(columnName); it == columns_.end()) {
		return std::unique_ptr<SQLValue>(new StringSQLValue());
	} else {
		return it->second->clone();
	}
}

bool MapSQLRow::isNull(const std::string &columnName) const {
	if (auto it = columns_.find(columnName); it == columns_.end()) {
		return true;
	} else {
		return it->second->isNull();
	}
}

std::vector<std::string> MapSQLRow::columnNames() const {
	std::vector<std::string> names;
	names.reserve(columns_.size());
	for (const auto &[name, value] : columns_) {
		names.push_back(name);
	}
	return names;
}

std::unique_ptr<SQLRow> MapSQLRow::clone() const {
	std::map<std::string, std::unique_ptr<SQLValue>> cloned;
	for (const auto &[name, value] : columns_) {
		cloned[name] = value->clone();
	}
	return std::unique_ptr<SQLRow>(new MapSQLRow(std::move(cloned)));
}

} // namespace r2rml
