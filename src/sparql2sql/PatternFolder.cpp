#include "sparql2sql/PatternFolder.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

#include "sparql-parser/ast/Expression.h"
#include "sparql2sql/ExprAnalysis.h"
#include "sparql2sql/ExpressionTranslator.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TagSql.h"
#include "sparql2sql/TermInference.h"
#include "sparql2sql/TermMapSql.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/TriplePatternTranslator.h"
#include "sparql2sql/ir/RelNode.h"
#include "sparql2sql/ir/SqlRenderer.h"

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
	keys.reserve(shared.size());
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
	// A VALUES column's terms live per *cell*, so its annotation is the meet
	// down the column: one row may give an IRI and another a literal. An UNDEF
	// cell contributes nothing at all (it is not a term), so it is skipped
	// rather than met in as Unknown - which would wipe out every other row.
	std::vector<TermInfo> columnTerms(varNames.size());
	std::vector<bool> columnSeeded(varNames.size(), false);
	for (const auto &row : values.rows) {
		for (std::size_t i = 0; i < row.size(); ++i) {
			if (!row[i]) {
				undefVars.insert(varNames[i]);
				continue;
			}
			TermInfo cell = termInfoOfTerm(*row[i]);
			columnTerms[i] = columnSeeded[i] ? meet(columnTerms[i], cell) : cell;
			columnSeeded[i] = true;
		}
	}

	// A column whose cells disagree about their term dimension is one of the two
	// places a not-statically-determined tag cannot be reconstructed later (the
	// other being a subquery's projection), because this relation's SQL is built
	// here and never re-rendered. So emit its tag per cell, unconditionally -
	// there is no needsTag() to consult yet. Every other column's tag is a single
	// constant the renderer synthesises on demand instead, which is why an
	// ordinary VALUES clause still generates exactly the SQL it always did.
	std::vector<bool> perCellTag(varNames.size(), false);
	for (std::size_t i = 0; i < varNames.size(); ++i) {
		perCellTag[i] = columnSeeded[i] && !isFullyDetermined(columnTerms[i]);
	}

	std::vector<std::string> rowSqls;
	rowSqls.reserve(values.rows.size());
	for (const auto &row : values.rows) {
		std::string sql;
		sql.reserve(256); // rough guess to avoid too many reallocs
		sql = "SELECT ";
		for (std::size_t i = 0; i < row.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			if (!row[i]) {
				sql += "CAST(NULL AS VARCHAR) AS " + mangleVar(varNames[i], dialect);
				if (perCellTag[i]) {
					// NULL value, NULL tag: the two columns must agree on
					// unboundedness (see mangleVarTag).
					sql += ", CAST(NULL AS VARCHAR) AS " + mangleVarTag(varNames[i], dialect);
				}
				continue;
			}
			sql += dialect.stringLiteral(termLexicalForm(*row[i])) + " AS " + mangleVar(varNames[i], dialect);
			if (perCellTag[i]) {
				sql +=
				    ", " + tagLiteral(termInfoOfTerm(*row[i]), dialect) + " AS " + mangleVarTag(varNames[i], dialect);
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
	for (std::size_t i = 0; i < varNames.size(); ++i) {
		ColumnInfo col;
		col.var = varNames[i];
		col.nonNull = undefVars.count(varNames[i]) == 0;
		col.term = columnTerms[i];
		if (perCellTag[i]) {
			raw.providedTagVars.insert(varNames[i]);
		} else if (columnSeeded[i]) {
			col.tagExpr = tagLiteral(columnTerms[i], dialect);
		} else {
			// An all-UNDEF column (or a VALUES with no rows at all) binds no term
			// in any row, so its tag is unbound in every row too - a constant, and
			// one that keeps the tag-NULL-iff-value-NULL invariant.
			col.tagExpr = "CAST(NULL AS VARCHAR)";
		}
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
	// Meet before the children are moved below - after the move, left->column()
	// would dereference a null unique_ptr. A side that doesn't bind the variable
	// yields nullptr, which meetColumns skips rather than treating as Unknown.
	for (const auto &v : allV) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = boundV.count(v) != 0;
		col.term = meetColumns({left->column(v), right->column(v)});
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
	// Meet both sides, as for an inner join, and for the same reason: a shared
	// variable is either equal on both sides (matched rows) or NULL on the right
	// (unmatched), and NULL denotes no term. Must run before the moves below.
	for (const auto &v : allV) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = boundV.count(v) != 0;
		col.term = meetColumns({left->column(v), right->column(v)});
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
	// buildKeys already derives null-safety from each operand's optionality,
	// which is exactly what MINUS needs: null tolerance only ever matters for a
	// key that can actually be NULL (see AntiJoinNode::keys).
	anti.keys = buildKeys(*left, *right);
	// MINUS preserves left's schema exactly - including its term annotations,
	// since no value from the right side ever reaches the output.
	anti.schema() = left->schema();
	anti.left = std::move(left);
	anti.right = std::move(right);
	return node;
}

RelNodePtr unionAll(std::vector<RelNodePtr> branches, TranslationContext &ctx, bool dedup) {
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
	un.all = !dedup; // UNION algebra is bag-preserving unless a caller asks otherwise.
	// Meet each variable across the arms before they are moved below.
	for (const auto &v : allV) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = boundV.count(v) != 0;
		col.term = meetAcrossArms(v, branches);
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
			// Infer from the expression while `acc` is still alive - it supplies
			// the annotations of the variables the expression reads.
			{
				TranslatedPattern bindScope;
				fillScopeFromSchema(bindScope, acc->schema());
				col.term = inferExprTermInfo(*b.expr, bindScope);
			}
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
			TranslatedPattern nested = translateQueryPattern(*sub.query, ctx, /*nested=*/true);
			RelNodePtr node(new RawRelation());
			RawRelation &raw = static_cast<RawRelation &>(*node);
			raw.sql = nested.sql;
			raw.providedTagVars = nested.providedTagVars;
			for (const auto &v : nested.allVars()) {
				ColumnInfo col;
				col.var = v;
				// translateQueryPattern already tracks which of the sub-select's
				// projected variables are guaranteed bound (boundVars) vs. only
				// sometimes bound (optionalVars, from OPTIONAL/UNION inside the
				// sub-select) — reuse that instead of over-approximating every
				// variable as optional, which would force a null-safe join below.
				col.nonNull = nested.boundVars.count(v) != 0;
				col.term = nested.termInfoOf(v);
				// A fully-determined variable's tag is a constant the renderer can
				// synthesise on demand later, exactly like every other producer
				// (translateInlineData above, TriplePatternTranslator, ...) - so a
				// consumer outside the sub-select (e.g. an enclosing ORDER BY) that
				// discovers its need for the tag only after this SQL text was
				// already frozen can still be satisfied. Only a genuinely
				// undetermined variable is irrecoverable here, which is exactly what
				// providedTagVars/producedTag's error is for.
				if (raw.providedTagVars.count(v) == 0 && isFullyDetermined(col.term)) {
					col.tagExpr = tagLiteral(col.term, ctx.dialect());
				}
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
