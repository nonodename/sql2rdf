#include "r2rml/JoinCondition.h"

#include <ostream>

namespace r2rml {

JoinCondition::JoinCondition(const std::string &childCol, const std::string &parentCol)
    : childColumn(childCol), parentColumn(parentCol) {
}

std::ostream &operator<<(std::ostream &os, const JoinCondition &jc) {
	return os << "join child=\"" << jc.childColumn << "\" parent=\"" << jc.parentColumn << '"';
}

} // namespace r2rml
