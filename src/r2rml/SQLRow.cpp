#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

namespace r2rml {

SQLRow::SQLRow() = default;

SQLRow::SQLRow(std::map<std::string, SQLValue> columns) : columns_(std::move(columns)) {
}

SQLValue SQLRow::getValue(const std::string &columnName) const {
	auto it = columns_.find(columnName);
	if (it == columns_.end()) {
		return SQLValue();
	}
	return it->second;
}

bool SQLRow::isNull(const std::string &columnName) const {
	auto it = columns_.find(columnName);
	if (it == columns_.end()) {
		return true;
	}
	return it->second.isNull();
}

} // namespace r2rml
