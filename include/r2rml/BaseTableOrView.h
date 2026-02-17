#pragma once

#include "LogicalTable.h"

namespace r2rml {

/**
 * Logical table representing a direct reference to a base table or view
 * (rr:tableName).
 */
class BaseTableOrView : public LogicalTable {
public:
    BaseTableOrView() = default;
    explicit BaseTableOrView(const std::string& table);
    ~BaseTableOrView() override;

    std::unique_ptr<SQLResultSet> getRows(SQLConnection& dbConnection) override;

    std::vector<std::string> getColumnNames() override;

    std::string tableName;
};

} // namespace r2rml
