#include "sparql2sql/TranslatedPattern.h"

#include "sparql2sql/SqlDialect.h"

namespace sparql2sql {

std::string mangleVar(const std::string &sparqlVarName, const SqlDialect &dialect) {
	return dialect.quoteIdentifier("v_" + sparqlVarName);
}

std::string mangleVarTag(const std::string &sparqlVarName, const SqlDialect &dialect) {
	return dialect.quoteIdentifier("d_" + sparqlVarName);
}

std::string joinColumnList(const std::vector<std::string> &items, const TranslationContext &ctx) {
	if (items.empty()) {
		return std::string();
	}
	std::string sql;
	for (std::size_t i = 0; i < items.size(); ++i) {
		sql += i == 0 ? (ctx.pretty() ? (ctx.nl() + ctx.indent(1)) : std::string(" "))
		              : (ctx.pretty() ? (ctx.nl() + ctx.indent(1) + ", ") : std::string(", "));
		sql += items[i];
	}
	return sql;
}

std::string joinConditions(const std::vector<std::string> &items, const TranslationContext &ctx) {
	if (items.empty()) {
		return std::string();
	}
	std::string sep = ctx.pretty() ? (ctx.nl() + ctx.indent(1) + "AND ") : std::string(" AND ");
	std::string sql = items[0];
	for (std::size_t i = 1; i < items.size(); ++i) {
		sql += sep + items[i];
	}
	return sql;
}

} // namespace sparql2sql
