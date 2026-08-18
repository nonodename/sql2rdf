#include "sparql2sql/Translator.h"

#include <set>
#include <vector>

#include "sparql-parser/ast/Expression.h"
#include "sparql-parser/ast/Query.h"
#include "sparql2sql/ExpressionTranslator.h"
#include "sparql2sql/PatternFolder.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TagSql.h"
#include "sparql2sql/TagDemand.h"
#include "sparql2sql/TermInference.h"
#include "sparql2sql/TermInfo.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/ir/Optimizer.h"
#include "sparql2sql/ir/RelNode.h"
#include "sparql2sql/ir/SqlRenderer.h"

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
		TermSql key = translateTerm(*gc.expr, source, alias, ctx);
		if (gc.asVar) {
			std::string colAlias = mangleVar(gc.asVar->name, ctx.dialect());
			result.selectListPrefixCols.push_back("(" + key.value + ") AS " + colAlias);
			result.groupByKeys.push_back(colAlias);
			continue;
		}
		result.groupByKeys.push_back(key.value);
		// Group on the term, not just its text: two terms with the same lexical
		// form and different dimensions are different terms, so they are different
		// groups. Same rule as the DISTINCT key, and like it a no-op whenever the
		// mapping determines the dimension.
		if (!isFullyDetermined(key.staticInfo) && !key.tag.empty()) {
			result.groupByKeys.push_back(key.tag);
		}
	}
	return result;
}

std::string translateHaving(const Query &query, const TranslatedPattern &source, const std::string &alias,
                            TranslationContext &ctx) {
	std::vector<std::string> conds;
	for (const auto &h : query.solutionModifier.having) {
		conds.push_back(translateExpression(*h, source, alias, ctx));
	}
	return joinConditions(conds, ctx);
}

// The expression that *defines* a SELECT-list-only output alias: a
// `(expr AS ?var)` GROUP BY condition, or a computed SELECT item. Null when the
// name is not defined by an expression.
//
// ORDER BY over such a name is emitted as a bare alias reference, which by
// itself carries no type information - so without this lookup, `ORDER BY ?cnt`
// over `(COUNT(?x) AS ?cnt)` would sort the stringified count lexicographically
// and put "11" before "2". Resolving the alias back to its definition is what
// lets the sort key be typed.
const Expression *aliasDefinition(const Query &query, const std::string &name) {
	for (const auto &gc : query.solutionModifier.groupBy) {
		if (gc.asVar && gc.asVar->name == name) {
			return gc.expr.get();
		}
	}
	for (const auto &item : query.selectItems) {
		if (item.expr && item.var->name == name) {
			return item.expr.get();
		}
	}
	return nullptr;
}

// Wrap a sort key in a cast to its statically known type, so ordering is
// numeric or chronological rather than lexicographic. Only the *sort key* is
// cast - never the projected value, which keeps its lexical form.
//
// A value that contradicts its declared datatype fails the TRY_CAST and sorts
// as NULL (last, for ASC in DuckDB) rather than raising - consistent with the
// null-tolerant idiom used everywhere else here.
std::string typedSortKey(const std::string &exprSql, const TermInfo &info, const SqlDialect &dialect) {
	if (info.isIntegral()) {
		return dialect.tryCastToBigInt(exprSql);
	}
	if (info.isNumeric()) {
		return dialect.tryCastToDouble(exprSql);
	}
	if (info.datatypeIri == xsd::kDate) {
		return dialect.tryCastToDate(exprSql);
	}
	if (info.isTemporal()) {
		return dialect.tryCastToTimestamp(exprSql);
	}
	return exprSql;
}

// One ORDER BY key, as the comma-separated list of SQL sort keys it needs.
//
// When the mapping determines the term's dimension this is a single key, exactly
// as before: typedSortKey casts it to the right SQL type so numbers sort
// numerically and timestamps chronologically.
//
// When the dimension varies per row, one key cannot express SPARQL 1.1 Section
// 15.1's ordering, which is defined *across* term kinds and value spaces before
// it is defined within one. So the key expands into the section's own layered
// order: term kind first (unbound < blank node < IRI < literal), then value
// space so that comparable literals sort together, then the value itself read as
// each of the two orderable types, and finally the raw lexical form - which is
// both the answer for xsd:string and a stable tie-break everywhere else.
//
// A TRY_CAST failure sorts as NULL (last for ASC in DuckDB) rather than raising,
// consistent with the null-tolerant idiom used throughout this translator.
std::string sortKeys(const TermSql &key, const SqlDialect &dialect, bool descending) {
	const char *direction = descending ? " DESC" : " ASC";
	if (isFullyDetermined(key.staticInfo) || key.tag.empty()) {
		return typedSortKey(key.value, key.staticInfo, dialect) + direction;
	}
	const std::vector<std::string> keys = {tagKindRank(key.tag, dialect), tagValueSpace(key.tag, dialect),
	                                       dialect.tryCastToDouble(key.value), dialect.tryCastToTimestamp(key.value),
	                                       key.value};
	std::string sql;
	for (std::size_t i = 0; i < keys.size(); ++i) {
		sql += (i > 0 ? ", " : "");
		sql += keys[i] + direction;
	}
	return sql;
}

std::string translateOrderBy(const Query &query, const TranslatedPattern &source, const std::string &alias,
                             const std::set<std::string> &selectListAliasNames, TranslationContext &ctx) {
	if (query.solutionModifier.orderBy.empty()) {
		return std::string();
	}
	std::vector<std::string> keys;
	for (std::size_t i = 0; i < query.solutionModifier.orderBy.size(); ++i) {
		const OrderCondition &oc = query.solutionModifier.orderBy[i];
		const bool descending = (oc.direction == OrderDirection::Desc);
		if (oc.expr->kind() == ExprKind::VarRef) {
			const std::string &name = static_cast<const VarExpr &>(*oc.expr).var->name;
			if (selectListAliasNames.count(name) && !source.allVars().count(name)) {
				// Only defined as a SELECT-list output alias (e.g. a GROUP
				// BY (expr AS ?var) or an aggregate SELECT item) - reference
				// it bare, relying on DuckDB's support for ORDER BY
				// referencing SELECT-list aliases directly. Its type comes from
				// the expression that defines it, not from the bare reference.
				const Expression *definition = aliasDefinition(query, name);
				TermSql key;
				key.value = mangleVar(name, ctx.dialect());
				if (definition != nullptr) {
					key.staticInfo = inferExprTermInfo(*definition, source);
				}
				// No tag is reachable through a bare alias reference, so this stays
				// on the single-key path whatever the definition's dimension is.
				keys.push_back(sortKeys(key, ctx.dialect(), descending));
				continue;
			}
		}
		keys.push_back(sortKeys(translateTerm(*oc.expr, source, alias, ctx), ctx.dialect(), descending));
	}
	return ctx.clauseSep() + "ORDER BY" + joinColumnList(keys, ctx);
}

// Prepend the query's collected WITH-clause entries (registered by
// TransitiveClosureNode rendering) ahead of the final top-level statement.
// Called only at the two actual outermost sites (the ASK branch and the
// SELECT-form return below), never inside translateQueryPattern itself,
// which is reentrant for SubSelectElement subqueries sharing the same ctx -
// a CTE registered while rendering a nested query still belongs to the one
// outermost WITH clause, not a WITH clause of its own.
//
// One "WITH RECURSIVE ..." prefix covers every entry even though not all of
// them self-reference (e.g. a closure's non-recursive step CTE): RECURSIVE
// is a clause-level flag enabling recursive self-reference for whichever
// CTEs in the list use it, not a per-entry requirement, in both Postgres and
// DuckDB.
std::string prependCtes(const TranslationContext &ctx, const std::string &body) {
	if (ctx.pendingCtes().empty()) {
		return body;
	}
	std::vector<std::string> cteDefs;
	for (const auto &cte : ctx.pendingCtes()) {
		cteDefs.push_back(cte.name + " AS (" + cte.bodySql + ")");
	}
	return "WITH RECURSIVE" + joinColumnList(cteDefs, ctx) + ctx.clauseSep() + body;
}

} // namespace

TranslatedPattern translateQueryPattern(const sparql::ast::Query &query, TranslationContext &ctx, bool nested) {
	if (query.form != QueryForm::Select) {
		throw TranslationError(
		    "translateQueryPattern: only SELECT queries produce a projected relation (subqueries and the top-level "
		    "SELECT path both require the SELECT form)");
	}

	const SqlDialect &dialect = ctx.dialect();

	// Must run before fold(): a nested `{ SELECT ... }` sub-select inside
	// query.where folds (and renders its final SQL text) as one step of
	// building this query's own tree, so this query's own ORDER BY/HAVING/
	// SELECT-list/DISTINCT demand for a tag has to be visible to it *before*
	// that happens - see markPreFoldTagNeeds's doc comment.
	markPreFoldTagNeeds(query, ctx);

	RelNodePtr rootNode = query.where ? fold(*query.where, ctx) : identityRelation(ctx);
	if (query.valuesClause) {
		rootNode = innerJoin(std::move(rootNode), translateInlineData(*query.valuesClause, ctx), ctx);
	}
	// Join keys before optimize (mergeInner fixes their comparison text there),
	// expressions after it (so filter pushdown gets first refusal at resolving a
	// predicate per union arm, which needs no tag at all). See TagDemand.h.
	markJoinKeyTagNeeds(*rootNode, ctx);

	OptimizerOptions opts;
	opts.topLevelDistinct =
	    (query.distinct || query.reduced) && query.solutionModifier.groupBy.empty() && !queryHasAggregate(query);
	opts.catalog = ctx.catalog();
	opts.ctx = &ctx;
	rootNode = optimize(std::move(rootNode), opts);
	markExpressionTagNeeds(*rootNode, &query, ctx);
	TranslatedPattern source;
	{
		TranslationContext::SubqueryDepthGuard depthGuard(ctx);
		source = renderRelation(*rootNode, ctx);
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

	// A projected variable's tag rides along in two situations, both of which
	// need it to survive into this statement's own result columns:
	//
	//  - a nested sub-select, whose SQL is spliced into the enclosing tree as
	//    text and never re-rendered, so an undetermined tag could not be
	//    reconstructed later (see Translator.h);
	//  - DISTINCT/REDUCED, where the tag is part of the dedup key: SQL dedups on
	//    the select list, so without it "1"^^xsd:integer and "1"^^xsd:string -
	//    two different RDF terms, and two different solutions - would collapse
	//    into one row.
	//
	// Both are gated on the dimension not being statically determined, so an
	// ordinary query's result columns stay exactly the variables it projected.
	const bool dedup = query.distinct || query.reduced;
	const bool wantTagCols = nested || dedup;
	std::set<std::string> tagCols;
	std::vector<std::string> selectCols = groupBy.selectListPrefixCols;
	if (query.selectStar) {
		// Internal variables - property path intermediates and blank-node
		// positions - are bound and joinable but are not query variables, so
		// `SELECT *` must not project them.
		std::set<std::string> vars = source.allVars();
		for (const auto &v : vars) {
			if (ctx.isInternal(v)) {
				continue;
			}
			selectCols.push_back(alias + "." + mangleVar(v, dialect) + " AS " + mangleVar(v, dialect));
			if (wantTagCols && ctx.needsTag(v) && !isFullyDetermined(source.termInfoOf(v))) {
				selectCols.push_back(alias + "." + mangleVarTag(v, dialect) + " AS " + mangleVarTag(v, dialect));
				tagCols.insert(v);
			}
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
			const std::string &name = item.var->name;
			if (wantTagCols && !item.expr && source.allVars().count(name) && ctx.needsTag(name) &&
			    !isFullyDetermined(source.termInfoOf(name))) {
				selectCols.push_back(alias + "." + mangleVarTag(name, dialect) + " AS " + mangleVarTag(name, dialect));
				tagCols.insert(name);
			}
		}
	}

	std::string havingSql = translateHaving(query, source, alias, ctx);
	std::string orderBySql = translateOrderBy(query, source, alias, selectListAliasNames, ctx);

	std::string sql = "SELECT";
	if (query.distinct || query.reduced) {
		sql += " DISTINCT";
	}
	if (selectCols.empty()) {
		sql += joinColumnList({"1 AS " + dialect.quoteIdentifier("_dummy")}, ctx);
	} else {
		sql += joinColumnList(selectCols, ctx);
	}
	sql += ctx.clauseSep() + "FROM (" + source.sql + ") AS " + alias;
	if (grouping && !groupBy.groupByKeys.empty()) {
		sql += ctx.clauseSep() + "GROUP BY" + joinColumnList(groupBy.groupByKeys, ctx);
	}
	if (!havingSql.empty()) {
		sql += ctx.clauseSep() + "HAVING " + havingSql;
	}
	sql += orderBySql;
	sql += dialect.limitOffsetClause(query.solutionModifier.hasLimit, query.solutionModifier.limit,
	                                 query.solutionModifier.hasOffset, query.solutionModifier.offset);

	TranslatedPattern result;
	result.sql = sql;
	result.providedTagVars = tagCols;
	if (query.selectStar) {
		for (const auto &v : source.boundVars) {
			if (!ctx.isInternal(v)) {
				result.boundVars.insert(v);
			}
		}
		for (const auto &v : source.optionalVars) {
			if (!ctx.isInternal(v)) {
				result.optionalVars.insert(v);
			}
		}
		// A projected variable carries its term annotation straight through:
		// `SELECT *` renames nothing and computes nothing.
		for (const auto &v : result.allVars()) {
			TermInfo term = source.termInfoOf(v);
			if (term.kindKnown()) {
				result.termInfo[v] = term;
			}
		}
	} else {
		for (const auto &item : query.selectItems) {
			result.boundVars.insert(item.var->name);
			// A computed item's annotation comes from the expression; a bare one
			// passes the source variable's through. Either may be Unknown, in
			// which case it is simply left out of the map.
			TermInfo term = item.expr ? inferExprTermInfo(*item.expr, source) : source.termInfoOf(item.var->name);
			if (term.kindKnown()) {
				result.termInfo[item.var->name] = term;
			}
		}
	}
	return result;
}

std::string translateQuery(const sparql::ast::Query &query, const r2rml::R2RMLMapping &mapping,
                           const SqlDialect &dialect, const TypeCatalog *catalog, bool prettyPrint) {
	TranslationContext ctx(mapping, dialect, catalog, prettyPrint);

	if (!query.datasetClauses.empty()) {
		throw TranslationError(
		    "FROM / FROM NAMED dataset clauses are not supported; every query is translated against the entire "
		    "R2RML mapping");
	}

	if (query.form == QueryForm::Ask) {
		RelNodePtr rootNode = query.where ? fold(*query.where, ctx) : identityRelation(ctx);
		if (query.valuesClause) {
			rootNode = innerJoin(std::move(rootNode), translateInlineData(*query.valuesClause, ctx), ctx);
		}
		markJoinKeyTagNeeds(*rootNode, ctx);
		OptimizerOptions askOpts;
		askOpts.topLevelDistinct = true; // ASK is an existence check: per-pattern DISTINCT is redundant.
		askOpts.catalog = ctx.catalog();
		askOpts.ctx = &ctx;
		rootNode = optimize(std::move(rootNode), askOpts);
		markExpressionTagNeeds(*rootNode, &query, ctx);
		TranslatedPattern source;
		{
			TranslationContext::SubqueryDepthGuard depthGuard(ctx);
			source = renderRelation(*rootNode, ctx);
		}

		std::string alias = ctx.nextAlias();
		bool grouping = !query.solutionModifier.groupBy.empty() || queryHasAggregate(query);
		GroupByBuild groupBy = buildGroupBy(query, source, alias, ctx);
		std::string havingSql = translateHaving(query, source, alias, ctx);

		std::string innerSql = "SELECT 1";
		for (const auto &c : groupBy.selectListPrefixCols) {
			innerSql += ", " + c;
		}
		innerSql += ctx.clauseSep() + "FROM (" + source.sql + ") AS " + alias;
		if (grouping && !groupBy.groupByKeys.empty()) {
			innerSql += ctx.clauseSep() + "GROUP BY" + joinColumnList(groupBy.groupByKeys, ctx);
		}
		if (!havingSql.empty()) {
			innerSql += ctx.clauseSep() + "HAVING " + havingSql;
		}
		// ORDER BY/LIMIT/OFFSET are intentionally ignored for ASK: they are
		// semantically inert for an existence check.
		return prependCtes(ctx,
		                   "SELECT " + dialect.existsClause(false, innerSql) + " AS " + dialect.quoteIdentifier("ask"));
	}

	if (query.form != QueryForm::Select) {
		throw TranslationError(
		    "query form not supported (only SELECT and ASK are implemented; CONSTRUCT/DESCRIBE are not)");
	}

	TranslatedPattern result = translateQueryPattern(query, ctx);
	return prependCtes(ctx, result.sql);
}

} // namespace sparql2sql
