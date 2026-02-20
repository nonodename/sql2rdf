#include "r2rml/JoinCondition.h"

namespace r2rml {

JoinCondition::JoinCondition(const std::string& childCol,
                             const std::string& parentCol)
    : childColumn(childCol), parentColumn(parentCol) {}

} // namespace r2rml
