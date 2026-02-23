#pragma once

#include <ostream>
#include <string>
#include <memory>

namespace r2rml {

/**
 * Represents a join condition between a child and parent logical table.
 */
class JoinCondition {
public:
    JoinCondition() = default;
    JoinCondition(const std::string& childCol, const std::string& parentCol);

    bool isValid() const { return !childColumn.empty() && !parentColumn.empty(); }

    friend std::ostream& operator<<(std::ostream& os, const JoinCondition& jc);

    std::string childColumn;
    std::string parentColumn;
};

} // namespace r2rml
