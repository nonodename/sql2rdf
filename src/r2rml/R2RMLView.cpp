#include "r2rml/R2RMLView.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"

namespace r2rml {

R2RMLView::R2RMLView(const std::string& query) : sqlQuery(query) {}
R2RMLView::~R2RMLView() = default;

std::unique_ptr<SQLResultSet> R2RMLView::getRows(SQLConnection& dbConnection) {
    effectiveSqlQuery = sqlQuery;
    return dbConnection.execute(sqlQuery);
}

std::vector<std::string> R2RMLView::getColumnNames() { return {}; }

} // namespace r2rml
