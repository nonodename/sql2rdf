#pragma once

#include "SQLValue.h"
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
    ~SQLRow();

    SQLValue getValue(const std::string& columnName) const;
    bool isNull(const std::string& columnName) const;
};

} // namespace r2rml
