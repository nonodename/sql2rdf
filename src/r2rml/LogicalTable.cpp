#include "r2rml/LogicalTable.h"

#include <ostream>

namespace r2rml {

LogicalTable::~LogicalTable() = default;

std::ostream& LogicalTable::print(std::ostream& os) const {
    return os << "LogicalTable { effectiveSqlQuery=\"" << effectiveSqlQuery << "\" }";
}

std::ostream& operator<<(std::ostream& os, const LogicalTable& lt) {
    return lt.print(os);
}

} // namespace r2rml
