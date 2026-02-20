#include "r2rml/BaseTableOrView.h"
#include "r2rml/SQLResultSet.h"

namespace r2rml {

BaseTableOrView::BaseTableOrView(const std::string& table)
    : tableName(table) {}
BaseTableOrView::~BaseTableOrView() = default;
std::unique_ptr<SQLResultSet> BaseTableOrView::getRows(SQLConnection& dbConnection) {
    return nullptr;
}
std::vector<std::string> BaseTableOrView::getColumnNames() { return {}; }

} // namespace r2rml
