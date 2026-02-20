#pragma once

#include "LogicalTable.h"

#include <vector>
#include <string>

namespace r2rml {

/**
 * Logical table backed by an arbitrary SQL query (rr:sqlQuery) optionally
 * annotated with SQL version identifiers.
 */
class R2RMLView : public LogicalTable {
public:
    R2RMLView() = default;
    explicit R2RMLView(const std::string& query);
    ~R2RMLView() override;

    std::unique_ptr<SQLResultSet> getRows(SQLConnection& dbConnection) override;
    std::vector<std::string> getColumnNames() override;

    bool isValid() const override { return !sqlQuery.empty(); }

    std::string sqlQuery;
    std::vector<std::string> sqlVersions;
};

} // namespace r2rml
