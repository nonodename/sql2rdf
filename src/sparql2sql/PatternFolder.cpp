#include "sparql2sql/PatternFolder.h"

#include <algorithm>
#include <iterator>
#include <set>

#include "sparql-parser/ast/Expression.h"
#include "sparql2sql/ExpressionTranslator.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TermMapSql.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/TriplePatternTranslator.h"

namespace sparql2sql {

namespace {

std::set<std::string> setIntersect(const std::set<std::string> &a, const std::set<std::string> &b) {
	std::set<std::string> out;
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::inserter(out, out.begin()));
	return out;
}

// Recursively collect every VarRef appearing anywhere inside an expression
// tree (used only to decide BIND's conservative optional-var
// over-approximation - EXISTS's own internal pattern variables are locally
// scoped and intentionally not collected here).
void collectVarRefs(const sparql::ast::Expression &expr, std::vector<std::string> &out) {
	using sparql::ast::AggregateExpr;
	using sparql::ast::BinaryExpr;
	using sparql::ast::BuiltInCallExpr;
	using sparql::ast::ExprKind;
	using sparql::ast::FunctionCallExpr;
	using sparql::ast::InExpr;
	using sparql::ast::UnaryExpr;
	using sparql::ast::VarExpr;
	switch (expr.kind()) {
	case ExprKind::VarRef:
		out.push_back(static_cast<const VarExpr &>(expr).var->name);
		return;
	case ExprKind::Literal:
	case ExprKind::IriRef:
	case ExprKind::Exists:
		return;
	case ExprKind::Unary:
		collectVarRefs(*static_cast<const UnaryExpr &>(expr).operand, out);
		return;
	case ExprKind::Binary: {
		const auto &b = static_cast<const BinaryExpr &>(expr);
		collectVarRefs(*b.left, out);
		collectVarRefs(*b.right, out);
		return;
	}
	case ExprKind::In: {
		const auto &in = static_cast<const InExpr &>(expr);
		collectVarRefs(*in.lhs, out);
		for (const auto &e : in.list) {
			collectVarRefs(*e, out);
		}
		return;
	}
	case ExprKind::FunctionCall: {
		const auto &fc = static_cast<const FunctionCallExpr &>(expr);
		for (const auto &a : fc.args) {
			collectVarRefs(*a, out);
		}
		return;
	}
	case ExprKind::BuiltInCall: {
		const auto &bc = static_cast<const BuiltInCallExpr &>(expr);
		for (const auto &a : bc.args) {
			collectVarRefs(*a, out);
		}
		return;
	}
	case ExprKind::Aggregate: {
		const auto &agg = static_cast<const AggregateExpr &>(expr);
		if (agg.arg) {
			collectVarRefs(*agg.arg, out);
		}
		return;
	}
	}
}

bool referencesOptionalVar(const sparql::ast::Expression &expr, const TranslatedPattern &scope) {
	std::vector<std::string> refs;
	collectVarRefs(expr, refs);
	for (const auto &v : refs) {
		if (scope.optionalVars.count(v)) {
			return true;
		}
	}
	return false;
}

struct JoinBuild {
	std::string sql;
	std::set<std::string> allVars;
};

JoinBuild buildJoinSql(const TranslatedPattern &left, const TranslatedPattern &right, TranslationContext &ctx,
                       const std::string &joinKeyword) {
	const SqlDialect &dialect = ctx.dialect();
	std::string leftAlias = ctx.nextAlias();
	std::string rightAlias = ctx.nextAlias();

	std::set<std::string> leftVars = left.allVars();
	std::set<std::string> rightVars = right.allVars();
	std::set<std::string> shared = setIntersect(leftVars, rightVars);

	std::vector<std::string> onConditions;
	std::vector<std::string> projectExprs;

	for (const auto &v : shared) {
		std::string lcol = leftAlias + "." + mangleVar(v, dialect);
		std::string rcol = rightAlias + "." + mangleVar(v, dialect);
		bool optionalEither = left.optionalVars.count(v) != 0 || right.optionalVars.count(v) != 0;
		if (optionalEither) {
			onConditions.push_back("(" + lcol + " = " + rcol + " OR " + lcol + " IS NULL OR " + rcol + " IS NULL)");
			projectExprs.push_back("COALESCE(" + lcol + ", " + rcol + ") AS " + mangleVar(v, dialect));
		} else {
			onConditions.push_back(lcol + " = " + rcol);
			projectExprs.push_back(lcol + " AS " + mangleVar(v, dialect));
		}
	}
	for (const auto &v : leftVars) {
		if (shared.count(v)) {
			continue;
		}
		projectExprs.push_back(leftAlias + "." + mangleVar(v, dialect) + " AS " + mangleVar(v, dialect));
	}
	for (const auto &v : rightVars) {
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
	sql += " FROM (" + left.sql + ") AS " + leftAlias + " " + joinKeyword + " (" + right.sql + ") AS " + rightAlias +
	       " ON ";
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

	JoinBuild jb;
	jb.sql = sql;
	jb.allVars = leftVars;
	jb.allVars.insert(rightVars.begin(), rightVars.end());
	return jb;
}

} // namespace

TranslatedPattern translateInlineData(const sparql::ast::InlineData &values, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::vector<std::string> varNames;
	varNames.reserve(values.vars.size());
	for (const auto &v : values.vars) {
		varNames.push_back(v->name);
	}

	std::set<std::string> undefVars;
	std::vector<std::string> rowSqls;
	for (const auto &row : values.rows) {
		std::string sql = "SELECT ";
		for (std::size_t i = 0; i < row.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			if (!row[i]) {
				sql += "CAST(NULL AS VARCHAR) AS " + mangleVar(varNames[i], dialect);
				undefVars.insert(varNames[i]);
			} else {
				sql += dialect.stringLiteral(termLexicalForm(*row[i])) + " AS " + mangleVar(varNames[i], dialect);
			}
		}
		rowSqls.push_back(sql);
	}

	TranslatedPattern result;
	if (rowSqls.empty()) {
		std::string sql = "SELECT ";
		for (std::size_t i = 0; i < varNames.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			sql += "CAST(NULL AS VARCHAR) AS " + mangleVar(varNames[i], dialect);
		}
		sql += (varNames.empty() ? std::string("1 AS ") + dialect.quoteIdentifier("_dummy") : std::string());
		sql += " WHERE FALSE";
		result.sql = sql;
	} else if (rowSqls.size() == 1) {
		result.sql = rowSqls.front();
	} else {
		result.sql = dialect.combineByName(/*all=*/true, rowSqls);
	}
	for (const auto &v : varNames) {
		if (undefVars.count(v)) {
			result.optionalVars.insert(v);
		} else {
			result.boundVars.insert(v);
		}
	}
	return result;
}

TranslatedPattern identityRelation(TranslationContext &ctx) {
	TranslatedPattern p;
	p.sql = "SELECT 1 AS " + ctx.dialect().quoteIdentifier("_dummy");
	p.isIdentity = true;
	return p;
}

TranslatedPattern innerJoin(const TranslatedPattern &left, const TranslatedPattern &right, TranslationContext &ctx) {
	if (left.isIdentity) {
		return right;
	}
	if (right.isIdentity) {
		return left;
	}
	JoinBuild jb = buildJoinSql(left, right, ctx, "INNER JOIN");
	TranslatedPattern result;
	result.sql = jb.sql;
	result.boundVars = left.boundVars;
	result.boundVars.insert(right.boundVars.begin(), right.boundVars.end());
	for (const auto &v : jb.allVars) {
		if (!result.boundVars.count(v)) {
			result.optionalVars.insert(v);
		}
	}
	return result;
}

TranslatedPattern leftOuterJoin(const TranslatedPattern &left, const TranslatedPattern &right,
                                TranslationContext &ctx) {
	if (right.isIdentity) {
		return left;
	}
	// Intentionally NOT short-circuiting left.isIdentity => return right:
	// `OPTIONAL { P }` with nothing preceding it still means "left-join the
	// one-row identity with P", which correctly yields one all-NULL
	// solution when P itself has zero matches, rather than an empty result.
	JoinBuild jb = buildJoinSql(left, right, ctx, "LEFT OUTER JOIN");
	TranslatedPattern result;
	result.sql = jb.sql;
	result.boundVars = left.boundVars;
	for (const auto &v : jb.allVars) {
		if (!result.boundVars.count(v)) {
			result.optionalVars.insert(v);
		}
	}
	return result;
}

TranslatedPattern antiJoin(const TranslatedPattern &left, const TranslatedPattern &right, TranslationContext &ctx) {
	std::set<std::string> shared = setIntersect(left.allVars(), right.allVars());
	if (shared.empty()) {
		// SPARQL 1.1 Section 18.2: MINUS only removes rows sharing >= 1
		// variable with (and compatible with) a right-hand row. With zero
		// shared variables it can never remove anything - emit nothing.
		return left;
	}
	const SqlDialect &dialect = ctx.dialect();
	std::string leftAlias = ctx.nextAlias();
	std::string rightAlias = ctx.nextAlias();
	std::string cond;
	bool first = true;
	for (const auto &v : shared) {
		std::string lcol = leftAlias + "." + mangleVar(v, dialect);
		std::string rcol = rightAlias + "." + mangleVar(v, dialect);
		if (!first) {
			cond += " AND ";
		}
		first = false;
		cond += "(" + lcol + " = " + rcol + " OR " + lcol + " IS NULL OR " + rcol + " IS NULL)";
	}
	TranslatedPattern result;
	result.sql = "SELECT * FROM (" + left.sql + ") AS " + leftAlias + " WHERE NOT EXISTS (SELECT 1 FROM (" + right.sql +
	             ") AS " + rightAlias + " WHERE " + cond + ")";
	result.boundVars = left.boundVars;
	result.optionalVars = left.optionalVars;
	return result;
}

TranslatedPattern unionAll(const std::vector<TranslatedPattern> &branches, TranslationContext &ctx) {
	if (branches.empty()) {
		return identityRelation(ctx);
	}
	if (branches.size() == 1) {
		return branches.front();
	}

	std::set<std::string> allV;
	for (const auto &b : branches) {
		std::set<std::string> bv = b.allVars();
		allV.insert(bv.begin(), bv.end());
	}
	std::set<std::string> boundV = branches.front().boundVars;
	for (std::size_t i = 1; i < branches.size(); ++i) {
		boundV = setIntersect(boundV, branches[i].boundVars);
	}

	std::vector<std::string> armSqls;
	armSqls.reserve(branches.size());
	for (const auto &b : branches) {
		armSqls.push_back(b.sql);
	}

	TranslatedPattern result;
	result.sql = ctx.dialect().combineByName(/*all=*/true, armSqls);
	result.boundVars = boundV;
	for (const auto &v : allV) {
		if (!boundV.count(v)) {
			result.optionalVars.insert(v);
		}
	}
	return result;
}

TranslatedPattern fold(const sparql::ast::GroupGraphPattern &pattern, TranslationContext &ctx) {
	using sparql::ast::BasicGraphPattern;
	using sparql::ast::Bind;
	using sparql::ast::ElementKind;
	using sparql::ast::Filter;
	using sparql::ast::GroupElement;
	using sparql::ast::GroupGraphPattern;
	using sparql::ast::InlineData;
	using sparql::ast::MinusGraphPattern;
	using sparql::ast::OptionalGraphPattern;
	using sparql::ast::SubSelectElement;
	using sparql::ast::UnionGraphPattern;

	TranslatedPattern acc = identityRelation(ctx);

	for (const auto &elPtr : pattern.elements) {
		const GroupElement &el = *elPtr;
		switch (el.kind()) {
		case ElementKind::BasicGraphPattern: {
			const auto &bgp = static_cast<const BasicGraphPattern &>(el);
			for (const auto &tp : bgp.triples) {
				acc = innerJoin(acc, translateTriplePattern(tp, ctx), ctx);
			}
			break;
		}
		case ElementKind::GroupGraphPattern: {
			const auto &nested = static_cast<const GroupGraphPattern &>(el);
			acc = innerJoin(acc, fold(nested, ctx), ctx);
			break;
		}
		case ElementKind::OptionalGraphPattern: {
			const auto &opt = static_cast<const OptionalGraphPattern &>(el);
			acc = leftOuterJoin(acc, fold(*opt.pattern, ctx), ctx);
			break;
		}
		case ElementKind::UnionGraphPattern: {
			const auto &un = static_cast<const UnionGraphPattern &>(el);
			std::vector<TranslatedPattern> branches;
			branches.reserve(un.branches.size());
			for (const auto &b : un.branches) {
				branches.push_back(fold(*b, ctx));
			}
			acc = innerJoin(acc, unionAll(branches, ctx), ctx);
			break;
		}
		case ElementKind::MinusGraphPattern: {
			const auto &mn = static_cast<const MinusGraphPattern &>(el);
			acc = antiJoin(acc, fold(*mn.pattern, ctx), ctx);
			break;
		}
		case ElementKind::Filter: {
			const auto &f = static_cast<const Filter &>(el);
			std::string alias = ctx.nextAlias();
			std::string cond = translateExpression(*f.constraint, acc, alias, ctx);
			TranslatedPattern result;
			result.sql = "SELECT * FROM (" + acc.sql + ") AS " + alias + " WHERE " + cond;
			result.boundVars = acc.boundVars;
			result.optionalVars = acc.optionalVars;
			acc = result;
			break;
		}
		case ElementKind::Bind: {
			const auto &b = static_cast<const Bind &>(el);
			std::string alias = ctx.nextAlias();
			std::string exprSql = translateExpression(*b.expr, acc, alias, ctx);
			bool optional = referencesOptionalVar(*b.expr, acc);
			TranslatedPattern result;
			result.sql = "SELECT *, (" + exprSql + ") AS " + mangleVar(b.var->name, ctx.dialect()) + " FROM (" +
			             acc.sql + ") AS " + alias;
			result.boundVars = acc.boundVars;
			result.optionalVars = acc.optionalVars;
			if (optional) {
				result.optionalVars.insert(b.var->name);
			} else {
				result.boundVars.insert(b.var->name);
			}
			acc = result;
			break;
		}
		case ElementKind::InlineData: {
			const auto &values = static_cast<const InlineData &>(el);
			acc = innerJoin(acc, translateInlineData(values, ctx), ctx);
			break;
		}
		case ElementKind::SubSelect: {
			const auto &sub = static_cast<const SubSelectElement &>(el);
			TranslatedPattern nested = translateQueryPattern(*sub.query, ctx);
			// Conservatively mark every projected variable as optional:
			// proving non-nullability through an arbitrary nested query's
			// own modifiers is disproportionate effort here, and
			// over-marking optional is always safe (just costs a little
			// unnecessary null-safety machinery downstream), never wrong.
			TranslatedPattern conservative;
			conservative.sql = nested.sql;
			conservative.optionalVars = nested.allVars();
			acc = innerJoin(acc, conservative, ctx);
			break;
		}
		case ElementKind::GraphGraphPattern:
			throw TranslationError(
			    "unsupported: GRAPH patterns require named-graph support, which this R2RML mapping model does not "
			    "provide (rr:graph/rr:graphMap are never populated by the mapping parser)");
		case ElementKind::ServiceGraphPattern:
			throw TranslationError("unsupported: SERVICE (federated query) is not supported");
		}
	}

	return acc;
}

} // namespace sparql2sql
