#include "r2rml/R2RMLView.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"

#include <ostream>

namespace r2rml {

R2RMLView::R2RMLView(const std::string& query) : sqlQuery(query) {}
R2RMLView::~R2RMLView() = default;

std::unique_ptr<SQLResultSet> R2RMLView::getRows(SQLConnection& dbConnection) {
    effectiveSqlQuery = sqlQuery;
    return dbConnection.execute(sqlQuery);
}

std::vector<std::string> R2RMLView::getColumnNames() { return {}; }

std::ostream& R2RMLView::print(std::ostream& os) const {
    os << "R2RMLView { sqlQuery=\"" << sqlQuery << "\"";
    if (!sqlVersions.empty()) {
        os << " versions=[";
        for (std::size_t i = 0; i < sqlVersions.size(); ++i) {
            if (i) os << ", ";
            os << sqlVersions[i];
        }
        os << "]";
    }
    os << " }";
    return os;
}

} // namespace r2rml
