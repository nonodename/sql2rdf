#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

namespace r2rml {

SQLRow::SQLRow() = default;
SQLRow::~SQLRow() = default;
SQLValue SQLRow::getValue(const std::string&) const { return SQLValue(); }
bool SQLRow::isNull(const std::string&) const { return true; }

} // namespace r2rml
