#pragma once

#include "LogicalTable.h"
#include <memory>

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

    bool isValid() const override { return !tableName.empty(); }

    std::ostream& print(std::ostream& os) const override;

    std::string tableName;
};

} // namespace r2rml
