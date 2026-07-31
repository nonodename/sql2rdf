#include "sparql2sql/PatternFolder.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

#include "sparql-parser/ast/Expression.h"
#include "sparql2sql/ExprAnalysis.h"
#include "sparql2sql/ExpressionTranslator.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TermMapSql.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/TriplePatternTranslator.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

namespace {

std::set<std::string> setIntersect(const std::set<std::string> &a, const std::set<std::string> &b) {
	std::set<std::string> out;
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::inserter(out, out.begin()));
	return out;
}

bool referencesOptionalVar(const sparql::ast::Expression &expr, const std::set<std::string> &optionalVars) {
	std::vector<std::string> refs;
	collectVarRefs(expr, refs);
	for (const auto &v : refs) {
		if (optionalVars.count(v)) {
			return true;
		}
	}
	return false;
}

// Build the equi-join keys for the variables shared by two relations. Each
// key's null-safety is decided from the INPUT relations' optionality (matches
// the pre-IR buildJoinSql), and carries both sides' ColumnInfo (provenance)
// for the later native-typed-join-key pass.
std::vector<EquiKey> buildKeys(const RelNode &left, const RelNode &right) {
	std::set<std::string> shared = setIntersect(left.allVars(), right.allVars());
	std::set<std::string> leftOpt = left.optionalVars();
	std::set<std::string> rightOpt = right.optionalVars();
	std::vector<EquiKey> keys;
	for (const auto &v : shared) {
		EquiKey k;
		k.var = v;
		if (const ColumnInfo *lc = left.column(v)) {
			k.leftCol = *lc;
		}
		if (const ColumnInfo *rc = right.column(v)) {
			k.rightCol = *rc;
		}
		k.nullSafe = leftOpt.count(v) != 0 || rightOpt.count(v) != 0;
		keys.push_back(k);
	}
	return keys;
}

} // namespace

RelNodePtr identityRelation(TranslationContext &ctx) {
	(void)ctx;
	return RelNodePtr(new SingleRowNode());
}

RelNodePtr translateInlineData(const sparql::ast::InlineData &values, TranslationContext &ctx) {
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

	RelNodePtr node(new RawRelation());
	RawRelation &raw = static_cast<RawRelation &>(*node);
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
		raw.sql = sql;
	} else if (rowSqls.size() == 1) {
		raw.sql = rowSqls.front();
	} else {
		raw.sql = dialect.combineByName(/*all=*/true, rowSqls);
	}
	for (const auto &v : varNames) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = undefVars.count(v) == 0;
		raw.schema().push_back(col);
	}
	return node;
}

RelNodePtr innerJoin(RelNodePtr left, RelNodePtr right, TranslationContext &ctx) {
	(void)ctx;
	if (left->kind() == RelKind::SingleRow) {
		return right;
	}
	if (right->kind() == RelKind::SingleRow) {
		return left;
	}

	std::set<std::string> boundV = left->boundVars();
	std::set<std::string> rBound = right->boundVars();
	boundV.insert(rBound.begin(), rBound.end());
	std::set<std::string> allV = left->allVars();
	std::set<std::string> rAll = right->allVars();
	allV.insert(rAll.begin(), rAll.end());

	RelNodePtr node(new JoinNode());
	JoinNode &join = static_cast<JoinNode &>(*node);
	join.joinKind = JoinKind::Inner;
	join.keys = buildKeys(*left, *right);
	for (const auto &v : allV) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = boundV.count(v) != 0;
		join.schema().push_back(col);
	}
	join.left = std::move(left);
	join.right = std::move(right);
	return node;
}

RelNodePtr leftOuterJoin(RelNodePtr left, RelNodePtr right, TranslationContext &ctx) {
	(void)ctx;
	if (right->kind() == RelKind::SingleRow) {
		return left;
	}
	// Intentionally NOT short-circuiting a SingleRow left: `OPTIONAL { P }`
	// with nothing preceding still means "left-join the one-row identity with
	// P", yielding one all-NULL solution when P has zero matches.

	std::set<std::string> boundV = left->boundVars();
	std::set<std::string> allV = left->allVars();
	std::set<std::string> rAll = right->allVars();
	allV.insert(rAll.begin(), rAll.end());

	RelNodePtr node(new JoinNode());
	JoinNode &join = static_cast<JoinNode &>(*node);
	join.joinKind = JoinKind::LeftOuter;
	join.keys = buildKeys(*left, *right);
	for (const auto &v : allV) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = boundV.count(v) != 0;
		join.schema().push_back(col);
	}
	join.left = std::move(left);
	join.right = std::move(right);
	return node;
}

RelNodePtr antiJoin(RelNodePtr left, RelNodePtr right, TranslationContext &ctx) {
	(void)ctx;
	std::set<std::string> shared = setIntersect(left->allVars(), right->allVars());
	if (shared.empty()) {
		// SPARQL 1.1 Section 18.2: MINUS with zero shared variables can never
		// remove anything - return left unchanged.
		return left;
	}

	RelNodePtr node(new AntiJoinNode());
	AntiJoinNode &anti = static_cast<AntiJoinNode &>(*node);
	anti.keys = buildKeys(*left, *right);
	for (auto &k : anti.keys) {
		k.nullSafe = true; // MINUS compatibility is always null-tolerant.
	}
	anti.schema() = left->schema(); // MINUS preserves left's schema exactly.
	anti.left = std::move(left);
	anti.right = std::move(right);
	return node;
}

RelNodePtr unionAll(std::vector<RelNodePtr> branches, TranslationContext &ctx) {
	if (branches.empty()) {
		return identityRelation(ctx);
	}
	if (branches.size() == 1) {
		return std::move(branches.front());
	}

	std::set<std::string> allV;
	std::set<std::string> boundV = branches.front()->boundVars();
	for (std::size_t i = 0; i < branches.size(); ++i) {
		std::set<std::string> bv = branches[i]->allVars();
		allV.insert(bv.begin(), bv.end());
		if (i > 0) {
			boundV = setIntersect(boundV, branches[i]->boundVars());
		}
	}

	RelNodePtr node(new UnionByNameNode());
	UnionByNameNode &un = static_cast<UnionByNameNode &>(*node);
	un.all = true; // UNION algebra is bag-preserving.
	for (const auto &v : allV) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = boundV.count(v) != 0;
		un.schema().push_back(col);
	}
	un.arms = std::move(branches);
	return node;
}

RelNodePtr fold(const sparql::ast::GroupGraphPattern &pattern, TranslationContext &ctx) {
	using sparql::ast::BasicGraphPattern;
	using sparql::ast::Bind;
	using sparql::ast::ElementKind;
	using sparql::ast::Filter;
	using sparql::ast::GroupGraphPattern;
	using sparql::ast::InlineData;
	using sparql::ast::MinusGraphPattern;
	using sparql::ast::OptionalGraphPattern;
	using sparql::ast::SubSelectElement;
	using sparql::ast::UnionGraphPattern;

	RelNodePtr acc = identityRelation(ctx);

	for (const auto &elPtr : pattern.elements) {
		const auto &el = *elPtr;
		switch (el.kind()) {
		case ElementKind::BasicGraphPattern: {
			const auto &bgp = static_cast<const BasicGraphPattern &>(el);
			for (const auto &tp : bgp.triples) {
				acc = innerJoin(std::move(acc), translateTriplePattern(tp, ctx), ctx);
			}
			break;
		}
		case ElementKind::GroupGraphPattern: {
			const auto &nested = static_cast<const GroupGraphPattern &>(el);
			acc = innerJoin(std::move(acc), fold(nested, ctx), ctx);
			break;
		}
		case ElementKind::OptionalGraphPattern: {
			const auto &opt = static_cast<const OptionalGraphPattern &>(el);
			acc = leftOuterJoin(std::move(acc), fold(*opt.pattern, ctx), ctx);
			break;
		}
		case ElementKind::UnionGraphPattern: {
			const auto &un = static_cast<const UnionGraphPattern &>(el);
			std::vector<RelNodePtr> branches;
			branches.reserve(un.branches.size());
			for (const auto &b : un.branches) {
				branches.push_back(fold(*b, ctx));
			}
			acc = innerJoin(std::move(acc), unionAll(std::move(branches), ctx), ctx);
			break;
		}
		case ElementKind::MinusGraphPattern: {
			const auto &mn = static_cast<const MinusGraphPattern &>(el);
			acc = antiJoin(std::move(acc), fold(*mn.pattern, ctx), ctx);
			break;
		}
		case ElementKind::Filter: {
			const auto &f = static_cast<const Filter &>(el);
			RelNodePtr node(new FilterNode());
			FilterNode &fn = static_cast<FilterNode &>(*node);
			fn.schema() = acc->schema();
			fn.predicate = f.constraint.get();
			fn.child = std::move(acc);
			acc = std::move(node);
			break;
		}
		case ElementKind::Bind: {
			const auto &b = static_cast<const Bind &>(el);
			bool optional = referencesOptionalVar(*b.expr, acc->optionalVars());
			RelNodePtr node(new BindNode());
			BindNode &bn = static_cast<BindNode &>(*node);
			bn.schema() = acc->schema();
			ColumnInfo col;
			col.var = b.var->name;
			col.nonNull = !optional;
			bn.schema().push_back(col);
			bn.outVar = b.var->name;
			bn.expr = b.expr.get();
			bn.child = std::move(acc);
			acc = std::move(node);
			break;
		}
		case ElementKind::InlineData: {
			const auto &values = static_cast<const InlineData &>(el);
			acc = innerJoin(std::move(acc), translateInlineData(values, ctx), ctx);
			break;
		}
		case ElementKind::SubSelect: {
			const auto &sub = static_cast<const SubSelectElement &>(el);
			TranslatedPattern nested = translateQueryPattern(*sub.query, ctx);
			// Conservatively mark every projected variable as optional (proving
			// non-nullability through an arbitrary nested query's modifiers is
			// disproportionate; over-marking optional is always safe).
			RelNodePtr node(new RawRelation());
			RawRelation &raw = static_cast<RawRelation &>(*node);
			raw.sql = nested.sql;
			for (const auto &v : nested.allVars()) {
				ColumnInfo col;
				col.var = v;
				col.nonNull = false;
				raw.schema().push_back(col);
			}
			acc = innerJoin(std::move(acc), std::move(node), ctx);
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
