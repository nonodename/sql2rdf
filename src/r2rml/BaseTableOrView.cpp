#include "r2rml/BaseTableOrView.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"

namespace r2rml {

BaseTableOrView::BaseTableOrView(const std::string& table)
    : tableName(table) {}

BaseTableOrView::~BaseTableOrView() = default;

std::unique_ptr<SQLResultSet> BaseTableOrView::getRows(SQLConnection& dbConnection) {
    // Construct the effective SQL query for a base table or view.
    std::string query = "SELECT * FROM \"" + tableName + "\"";
    effectiveSqlQuery = query;
    return dbConnection.execute(query);
}

std::vector<std::string> BaseTableOrView::getColumnNames() { return {}; }

} // namespace r2rml
