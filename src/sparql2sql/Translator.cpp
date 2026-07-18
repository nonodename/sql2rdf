#include "sparql2sql/Translator.h"

#include <set>
#include <vector>

#include "sparql-parser/ast/Expression.h"
#include "sparql-parser/ast/Query.h"
#include "sparql2sql/ExpressionTranslator.h"
#include "sparql2sql/PatternFolder.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TranslationError.h"

namespace sparql2sql {

namespace {

using sparql::ast::BinaryExpr;
using sparql::ast::BuiltInCallExpr;
using sparql::ast::Expression;
using sparql::ast::ExprKind;
using sparql::ast::FunctionCallExpr;
using sparql::ast::InExpr;
using sparql::ast::OrderCondition;
using sparql::ast::OrderDirection;
using sparql::ast::Query;
using sparql::ast::QueryForm;
using sparql::ast::UnaryExpr;
using sparql::ast::VarExpr;

bool containsAggregate(const Expression &expr) {
	switch (expr.kind()) {
	case ExprKind::Aggregate:
		return true;
	case ExprKind::Literal:
	case ExprKind::VarRef:
	case ExprKind::IriRef:
	case ExprKind::Exists:
		return false;
	case ExprKind::Unary:
		return containsAggregate(*static_cast<const UnaryExpr &>(expr).operand);
	case ExprKind::Binary: {
		const auto &b = static_cast<const BinaryExpr &>(expr);
		return containsAggregate(*b.left) || containsAggregate(*b.right);
	}
	case ExprKind::In: {
		const auto &in = static_cast<const InExpr &>(expr);
		if (containsAggregate(*in.lhs)) {
			return true;
		}
		for (const auto &e : in.list) {
			if (containsAggregate(*e)) {
				return true;
			}
		}
		return false;
	}
	case ExprKind::FunctionCall: {
		const auto &fc = static_cast<const FunctionCallExpr &>(expr);
		for (const auto &a : fc.args) {
			if (containsAggregate(*a)) {
				return true;
			}
		}
		return false;
	}
	case ExprKind::BuiltInCall: {
		const auto &bc = static_cast<const BuiltInCallExpr &>(expr);
		for (const auto &a : bc.args) {
			if (containsAggregate(*a)) {
				return true;
			}
		}
		return false;
	}
	}
	return false;
}

bool queryHasAggregate(const Query &query) {
	for (const auto &item : query.selectItems) {
		if (item.expr && containsAggregate(*item.expr)) {
			return true;
		}
	}
	for (const auto &h : query.solutionModifier.having) {
		if (containsAggregate(*h)) {
			return true;
		}
	}
	return false;
}

// Names introduced purely by a `(expr AS ?var)` GROUP BY condition (not
// present in the pre-aggregation `source` relation at all - only defined as
// a SELECT-list output alias). ORDER BY commonly references these (e.g.
// `GROUP BY (?year+1 AS ?y) ... ORDER BY ?y`); since DuckDB allows ORDER BY
// to reference a SELECT-list alias directly, those references are emitted
// bare instead of going through the normal scope-qualified path.
std::set<std::string> groupByAliasNames(const Query &query) {
	std::set<std::string> names;
	for (const auto &gc : query.solutionModifier.groupBy) {
		if (gc.asVar) {
			names.insert(gc.asVar->name);
		}
	}
	return names;
}

struct GroupByBuild {
	std::vector<std::string> selectListPrefixCols; // "(expr) AS v_x" entries for asVar conditions, emitted first
	std::vector<std::string> groupByKeys;          // SQL text for each GROUP BY key (alias ref or raw expr)
};

GroupByBuild buildGroupBy(const Query &query, const TranslatedPattern &source, const std::string &alias,
                          TranslationContext &ctx) {
	GroupByBuild result;
	for (const auto &gc : query.solutionModifier.groupBy) {
		std::string exprSql = translateExpression(*gc.expr, source, alias, ctx);
		if (gc.asVar) {
			std::string colAlias = mangleVar(gc.asVar->name, ctx.dialect());
			result.selectListPrefixCols.push_back("(" + exprSql + ") AS " + colAlias);
			result.groupByKeys.push_back(colAlias);
		} else {
			result.groupByKeys.push_back(exprSql);
		}
	}
	return result;
}

std::string translateHaving(const Query &query, const TranslatedPattern &source, const std::string &alias,
                            TranslationContext &ctx) {
	std::string havingSql;
	for (const auto &h : query.solutionModifier.having) {
		std::string cond = translateExpression(*h, source, alias, ctx);
		if (!havingSql.empty()) {
			havingSql += " AND ";
		}
		havingSql += cond;
	}
	return havingSql;
}

std::string translateOrderBy(const Query &query, const TranslatedPattern &source, const std::string &alias,
                             const std::set<std::string> &selectListAliasNames, TranslationContext &ctx) {
	if (query.solutionModifier.orderBy.empty()) {
		return std::string();
	}
	std::string sql = " ORDER BY ";
	for (std::size_t i = 0; i < query.solutionModifier.orderBy.size(); ++i) {
		if (i > 0) {
			sql += ", ";
		}
		const OrderCondition &oc = query.solutionModifier.orderBy[i];
		if (oc.expr->kind() == ExprKind::VarRef) {
			const std::string &name = static_cast<const VarExpr &>(*oc.expr).var->name;
			if (selectListAliasNames.count(name) && !source.allVars().count(name)) {
				// Only defined as a SELECT-list output alias (e.g. a GROUP
				// BY (expr AS ?var) or an aggregate SELECT item) - reference
				// it bare, relying on DuckDB's support for ORDER BY
				// referencing SELECT-list aliases directly.
				sql += mangleVar(name, ctx.dialect());
				sql += (oc.direction == OrderDirection::Desc) ? " DESC" : " ASC";
				continue;
			}
		}
		sql += translateExpression(*oc.expr, source, alias, ctx);
		sql += (oc.direction == OrderDirection::Desc) ? " DESC" : " ASC";
	}
	return sql;
}

} // namespace

TranslatedPattern translateQueryPattern(const sparql::ast::Query &query, TranslationContext &ctx) {
	if (query.form != QueryForm::Select) {
		throw TranslationError(
		    "translateQueryPattern: only SELECT queries produce a projected relation (subqueries and the top-level "
		    "SELECT path both require the SELECT form)");
	}

	const SqlDialect &dialect = ctx.dialect();

	TranslatedPattern source = query.where ? fold(*query.where, ctx) : identityRelation(ctx);
	if (query.valuesClause) {
		source = innerJoin(source, translateInlineData(*query.valuesClause, ctx), ctx);
	}

	std::string alias = ctx.nextAlias();

	bool grouping = !query.solutionModifier.groupBy.empty() || queryHasAggregate(query);
	GroupByBuild groupBy = buildGroupBy(query, source, alias, ctx);

	std::set<std::string> groupByAliases = groupByAliasNames(query);

	// Names visible as SELECT-list output aliases: GROUP BY (expr AS ?var)
	// conditions and every projected SELECT item (bare or computed). Used
	// only to let ORDER BY reference them directly (see translateOrderBy).
	std::set<std::string> selectListAliasNames = groupByAliases;
	for (const auto &item : query.selectItems) {
		selectListAliasNames.insert(item.var->name);
	}

	std::vector<std::string> selectCols = groupBy.selectListPrefixCols;
	if (query.selectStar) {
		std::set<std::string> vars = source.allVars();
		for (const auto &v : vars) {
			selectCols.push_back(alias + "." + mangleVar(v, dialect) + " AS " + mangleVar(v, dialect));
		}
	} else {
		for (const auto &item : query.selectItems) {
			std::string exprSql;
			if (item.expr) {
				exprSql = translateExpression(*item.expr, source, alias, ctx);
			} else if (groupByAliases.count(item.var->name) && !source.allVars().count(item.var->name)) {
				// Bare-projecting a GROUP BY (expr AS ?var) alias.
				exprSql = mangleVar(item.var->name, dialect);
			} else {
				exprSql = alias + "." + mangleVar(item.var->name, dialect);
			}
			selectCols.push_back("(" + exprSql + ") AS " + mangleVar(item.var->name, dialect));
		}
	}

	std::string havingSql = translateHaving(query, source, alias, ctx);
	std::string orderBySql = translateOrderBy(query, source, alias, selectListAliasNames, ctx);

	std::string sql = "SELECT ";
	if (query.distinct || query.reduced) {
		sql += "DISTINCT ";
	}
	if (selectCols.empty()) {
		sql += "1 AS " + dialect.quoteIdentifier("_dummy");
	} else {
		for (std::size_t i = 0; i < selectCols.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			sql += selectCols[i];
		}
	}
	sql += " FROM (" + source.sql + ") AS " + alias;
	if (grouping && !groupBy.groupByKeys.empty()) {
		sql += " GROUP BY ";
		for (std::size_t i = 0; i < groupBy.groupByKeys.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			sql += groupBy.groupByKeys[i];
		}
	}
	if (!havingSql.empty()) {
		sql += " HAVING " + havingSql;
	}
	sql += orderBySql;
	sql += dialect.limitOffsetClause(query.solutionModifier.hasLimit, query.solutionModifier.limit,
	                                 query.solutionModifier.hasOffset, query.solutionModifier.offset);

	TranslatedPattern result;
	result.sql = sql;
	if (query.selectStar) {
		result.boundVars = source.boundVars;
		result.optionalVars = source.optionalVars;
	} else {
		for (const auto &item : query.selectItems) {
			result.boundVars.insert(item.var->name);
		}
	}
	return result;
}

std::string translateQuery(const sparql::ast::Query &query, const r2rml::R2RMLMapping &mapping,
                           const SqlDialect &dialect) {
	TranslationContext ctx(mapping, dialect);

	if (query.form == QueryForm::Ask) {
		TranslatedPattern source = query.where ? fold(*query.where, ctx) : identityRelation(ctx);
		if (query.valuesClause) {
			source = innerJoin(source, translateInlineData(*query.valuesClause, ctx), ctx);
		}

		std::string alias = ctx.nextAlias();
		bool grouping = !query.solutionModifier.groupBy.empty() || queryHasAggregate(query);
		GroupByBuild groupBy = buildGroupBy(query, source, alias, ctx);
		std::string havingSql = translateHaving(query, source, alias, ctx);

		std::string innerSql = "SELECT 1";
		for (const auto &c : groupBy.selectListPrefixCols) {
			innerSql += ", " + c;
		}
		innerSql += " FROM (" + source.sql + ") AS " + alias;
		if (grouping && !groupBy.groupByKeys.empty()) {
			innerSql += " GROUP BY ";
			for (std::size_t i = 0; i < groupBy.groupByKeys.size(); ++i) {
				if (i > 0) {
					innerSql += ", ";
				}
				innerSql += groupBy.groupByKeys[i];
			}
		}
		if (!havingSql.empty()) {
			innerSql += " HAVING " + havingSql;
		}
		// ORDER BY/LIMIT/OFFSET are intentionally ignored for ASK: they are
		// semantically inert for an existence check.
		return "SELECT " + dialect.existsClause(false, innerSql) + " AS " + dialect.quoteIdentifier("ask");
	}

	if (query.form != QueryForm::Select) {
		throw TranslationError(
		    "query form not supported (only SELECT and ASK are implemented; CONSTRUCT/DESCRIBE are not)");
	}

	TranslatedPattern result = translateQueryPattern(query, ctx);
	return result.sql;
}

} // namespace sparql2sql
