#pragma once

#include "SQLValue.h"
#include <map>
#include <string>
#include <memory>

namespace r2rml {

/**
 * Represents a single row of SQL results.  Values may be of various types and
 * are accessed by column name.
 */
class SQLRow {
public:
	SQLRow();
	explicit SQLRow(std::map<std::string, SQLValue> columns);
	~SQLRow() = default;

	SQLValue getValue(const std::string &columnName) const;
	bool isNull(const std::string &columnName) const;

private:
	std::map<std::string, SQLValue> columns_;
};

} // namespace r2rml
