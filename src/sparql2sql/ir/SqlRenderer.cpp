#include "sparql2sql/ir/SqlRenderer.h"

#include <set>
#include <string>

#include "sparql2sql/ExpressionTranslator.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

namespace {

std::string renderNode(const RelNode &node, TranslationContext &ctx);

// A TranslatedPattern carrying only the bound/optional variable sets of a
// node's schema - enough for translateExpression's in-scope/optionality
// checks (it never reads scope.sql).
TranslatedPattern scopeOf(const RelNode &node) {
	TranslatedPattern tp;
	for (const auto &c : node.schema()) {
		if (c.nonNull) {
			tp.boundVars.insert(c.var);
		} else {
			tp.optionalVars.insert(c.var);
		}
	}
	return tp;
}

std::string renderSpj(const SpjRelation &rel, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string sql = "SELECT ";
	if (rel.distinct) {
		sql += "DISTINCT ";
	}
	if (rel.schema().empty()) {
		sql += "1 AS " + dialect.quoteIdentifier("_dummy");
	} else {
		for (std::size_t i = 0; i < rel.schema().size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			const ColumnInfo &c = rel.schema()[i];
			sql += c.renderedExpr + " AS " + mangleVar(c.var, dialect);
		}
	}
	sql += " FROM ";
	for (std::size_t i = 0; i < rel.sources.size(); ++i) {
		if (i > 0) {
			// Multi-source (post-flatten) spine: cross-join sources; the
			// inter-source join equalities live in whereConds.
			sql += ", ";
		}
		sql += rel.sources[i].sql;
	}
	if (!rel.whereConds.empty()) {
		sql += " WHERE ";
		for (std::size_t i = 0; i < rel.whereConds.size(); ++i) {
			if (i > 0) {
				sql += " AND ";
			}
			sql += "(" + rel.whereConds[i] + ")";
		}
	}
	return sql;
}

std::string renderJoin(const JoinNode &join, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string leftAlias = ctx.nextAlias();
	std::string rightAlias = ctx.nextAlias();
	std::string leftSql = renderNode(*join.left, ctx);
	std::string rightSql = renderNode(*join.right, ctx);

	std::set<std::string> shared;
	for (const auto &k : join.keys) {
		shared.insert(k.var);
	}

	std::vector<std::string> onConditions;
	std::vector<std::string> projectExprs;
	for (const auto &k : join.keys) {
		std::string lcol = leftAlias + "." + mangleVar(k.var, dialect);
		std::string rcol = rightAlias + "." + mangleVar(k.var, dialect);
		if (k.nullSafe) {
			onConditions.push_back("(" + lcol + " = " + rcol + " OR " + lcol + " IS NULL OR " + rcol + " IS NULL)");
			projectExprs.push_back("COALESCE(" + lcol + ", " + rcol + ") AS " + mangleVar(k.var, dialect));
		} else {
			onConditions.push_back(lcol + " = " + rcol);
			projectExprs.push_back(lcol + " AS " + mangleVar(k.var, dialect));
		}
	}
	for (const auto &v : join.left->allVars()) {
		if (shared.count(v)) {
			continue;
		}
		projectExprs.push_back(leftAlias + "." + mangleVar(v, dialect) + " AS " + mangleVar(v, dialect));
	}
	for (const auto &v : join.right->allVars()) {
		if (shared.count(v)) {
			continue;
		}
		projectExprs.push_back(rightAlias + "." + mangleVar(v, dialect) + " AS " + mangleVar(v, dialect));
	}

	std::string sql = "SELECT ";
	if (projectExprs.empty()) {
		sql += "1 AS " + dialect.quoteIdentifier("_dummy");
	} else {
		for (std::size_t i = 0; i < projectExprs.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			sql += projectExprs[i];
		}
	}
	const char *keyword = join.joinKind == JoinKind::LeftOuter ? "LEFT OUTER JOIN" : "INNER JOIN";
	sql += " FROM (" + leftSql + ") AS " + leftAlias + " " + keyword + " (" + rightSql + ") AS " + rightAlias + " ON ";
	if (onConditions.empty()) {
		sql += dialect.booleanLiteral(true);
	} else {
		for (std::size_t i = 0; i < onConditions.size(); ++i) {
			if (i > 0) {
				sql += " AND ";
			}
			sql += onConditions[i];
		}
	}
	return sql;
}

std::string renderAntiJoin(const AntiJoinNode &anti, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string leftAlias = ctx.nextAlias();
	std::string rightAlias = ctx.nextAlias();
	std::string leftSql = renderNode(*anti.left, ctx);
	std::string rightSql = renderNode(*anti.right, ctx);

	std::string cond;
	bool first = true;
	for (const auto &k : anti.keys) {
		std::string lcol = leftAlias + "." + mangleVar(k.var, dialect);
		std::string rcol = rightAlias + "." + mangleVar(k.var, dialect);
		if (!first) {
			cond += " AND ";
		}
		first = false;
		cond += "(" + lcol + " = " + rcol + " OR " + lcol + " IS NULL OR " + rcol + " IS NULL)";
	}
	return "SELECT * FROM (" + leftSql + ") AS " + leftAlias + " WHERE NOT EXISTS (SELECT 1 FROM (" + rightSql +
	       ") AS " + rightAlias + " WHERE " + cond + ")";
}

std::string renderUnion(const UnionByNameNode &un, TranslationContext &ctx) {
	std::vector<std::string> armSqls;
	armSqls.reserve(un.arms.size());
	for (const auto &arm : un.arms) {
		armSqls.push_back(renderNode(*arm, ctx));
	}
	return ctx.dialect().combineByName(un.all, armSqls);
}

std::string renderFilter(const FilterNode &f, TranslationContext &ctx) {
	std::string alias = ctx.nextAlias();
	std::string childSql = renderNode(*f.child, ctx);
	TranslatedPattern scope = scopeOf(*f.child);
	std::string cond = translateExpression(*f.predicate, scope, alias, ctx);
	return "SELECT * FROM (" + childSql + ") AS " + alias + " WHERE " + cond;
}

std::string renderBind(const BindNode &b, TranslationContext &ctx) {
	std::string alias = ctx.nextAlias();
	std::string childSql = renderNode(*b.child, ctx);
	TranslatedPattern scope = scopeOf(*b.child);
	std::string exprSql = translateExpression(*b.expr, scope, alias, ctx);
	return "SELECT *, (" + exprSql + ") AS " + mangleVar(b.outVar, ctx.dialect()) + " FROM (" + childSql + ") AS " +
	       alias;
}

std::string renderEmpty(const EmptyNode &e, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	if (e.schema().empty()) {
		return "SELECT 1 AS " + dialect.quoteIdentifier("_dummy") + " WHERE FALSE";
	}
	std::string sql = "SELECT ";
	for (std::size_t i = 0; i < e.schema().size(); ++i) {
		if (i > 0) {
			sql += ", ";
		}
		sql += "CAST(NULL AS VARCHAR) AS " + mangleVar(e.schema()[i].var, dialect);
	}
	sql += " WHERE FALSE";
	return sql;
}

std::string renderNode(const RelNode &node, TranslationContext &ctx) {
	switch (node.kind()) {
	case RelKind::Spj:
		return renderSpj(static_cast<const SpjRelation &>(node), ctx);
	case RelKind::Join:
		return renderJoin(static_cast<const JoinNode &>(node), ctx);
	case RelKind::AntiJoin:
		return renderAntiJoin(static_cast<const AntiJoinNode &>(node), ctx);
	case RelKind::UnionByName:
		return renderUnion(static_cast<const UnionByNameNode &>(node), ctx);
	case RelKind::Filter:
		return renderFilter(static_cast<const FilterNode &>(node), ctx);
	case RelKind::Bind:
		return renderBind(static_cast<const BindNode &>(node), ctx);
	case RelKind::Raw:
		return static_cast<const RawRelation &>(node).sql;
	case RelKind::SingleRow:
		return "SELECT 1 AS " + ctx.dialect().quoteIdentifier("_dummy");
	case RelKind::Empty:
		return renderEmpty(static_cast<const EmptyNode &>(node), ctx);
	}
	return std::string();
}

} // namespace

TranslatedPattern renderRelation(const RelNode &node, TranslationContext &ctx) {
	TranslatedPattern tp;
	tp.sql = renderNode(node, ctx);
	for (const auto &c : node.schema()) {
		if (c.nonNull) {
			tp.boundVars.insert(c.var);
		} else {
			tp.optionalVars.insert(c.var);
		}
	}
	tp.isIdentity = (node.kind() == RelKind::SingleRow);
	return tp;
}

} // namespace sparql2sql
