#pragma once

#include <string>

namespace r2rml {

/**
 * Represents a join condition between a child and parent logical table.
 */
class JoinCondition {
public:
    JoinCondition() = default;
    JoinCondition(const std::string& childCol, const std::string& parentCol);

    std::string childColumn;
    std::string parentColumn;
};

} // namespace r2rml
