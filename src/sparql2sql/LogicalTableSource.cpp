#include "sparql2sql/LogicalTableSource.h"

#include <set>

#include "r2rml/BaseTableOrView.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLView.h"
#include "r2rml/TriplesMap.h"

namespace sparql2sql {

std::string logicalTableIdentity(const r2rml::LogicalTable &logicalTable) {
	if (const auto *base = dynamic_cast<const r2rml::BaseTableOrView *>(&logicalTable)) {
		return base->tableName;
	}
	if (const auto *view = dynamic_cast<const r2rml::R2RMLView *>(&logicalTable)) {
		return "view:" + view->sqlQuery;
	}
	return std::string();
}

std::string stripTrailingSemicolon(std::string sql) {
	std::size_t end = sql.find_last_not_of(" \t\r\n");
	if (end == std::string::npos) {
		return std::string();
	}
	sql.erase(end + 1);
	if (!sql.empty() && sql.back() == ';') {
		sql.pop_back();
		// Trim again: "SELECT 1 ;" would otherwise keep the space the
		// terminator was hiding.
		end = sql.find_last_not_of(" \t\r\n");
		sql.erase(end == std::string::npos ? 0 : end + 1);
	}
	return sql;
}

std::vector<ViewSource> mappingViewSources(const r2rml::R2RMLMapping &mapping) {
	std::vector<ViewSource> out;
	std::set<std::string> seen;
	for (const auto &tmPtr : mapping.triplesMaps) {
		if (!tmPtr || !tmPtr->logicalTable) {
			continue;
		}
		const auto *view = dynamic_cast<const r2rml::R2RMLView *>(tmPtr->logicalTable.get());
		if (view == nullptr) {
			continue;
		}
		ViewSource source;
		source.identity = logicalTableIdentity(*tmPtr->logicalTable);
		source.sql = stripTrailingSemicolon(view->sqlQuery);
		if (source.sql.empty() || !seen.insert(source.identity).second) {
			continue;
		}
		out.push_back(source);
	}
	return out;
}

} // namespace sparql2sql
