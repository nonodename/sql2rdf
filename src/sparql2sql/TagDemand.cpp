#include "sparql2sql/TagDemand.h"

#include <set>
#include <string>
#include <vector>

#include "sparql-parser/ast/Expression.h"
#include "sparql-parser/ast/GraphPattern.h"
#include "sparql-parser/ast/Query.h"
#include "sparql2sql/ExprAnalysis.h"
#include "sparql2sql/TermInfo.h"
#include "sparql2sql/ir/RelNode.h"
#include "sparql2sql/ir/SqlRenderer.h"

namespace sparql2sql {

namespace {

using sparql::ast::AggregateExpr;
using sparql::ast::AggregateKind;
using sparql::ast::BinaryExpr;
using sparql::ast::BinaryOp;
using sparql::ast::BuiltInCallExpr;
using sparql::ast::BuiltinFunction;
using sparql::ast::ElementKind;
using sparql::ast::Expression;
using sparql::ast::ExprKind;
using sparql::ast::FunctionCallExpr;
using sparql::ast::GraphGraphPattern;
using sparql::ast::GroupElement;
using sparql::ast::GroupGraphPattern;
using sparql::ast::InExpr;
using sparql::ast::MinusGraphPattern;
using sparql::ast::OptionalGraphPattern;
using sparql::ast::ServiceGraphPattern;
using sparql::ast::SubSelectElement;
using sparql::ast::UnaryExpr;
using sparql::ast::UnaryOp;
using sparql::ast::UnionGraphPattern;

// Mark every variable `expr` reads whose dimension the schema does not settle.
//
// Reached only from a node already known to inspect the dimension, so it does
// not re-check what the construct was: over-approximating *which* of that
// construct's variables matter costs an unused column, and the alternative
// (tracking argument positions per builtin) would be a fourth switch to keep in
// step with translateExpression's.
void markVarsIn(const Expression &expr, const TranslatedPattern &scope, TranslationContext &ctx) {
	std::vector<std::string> refs;
	collectVarRefs(expr, refs);
	for (const auto &name : refs) {
		if (!isFullyDetermined(scope.termInfoOf(name))) {
			ctx.markNeedsTag(name);
		}
	}
}

bool inspectsDimension(BinaryOp op) {
	switch (op) {
	case BinaryOp::Eq:
	case BinaryOp::Ne:
	case BinaryOp::Lt:
	case BinaryOp::Gt:
	case BinaryOp::Le:
	case BinaryOp::Ge:
	case BinaryOp::Add:
	case BinaryOp::Sub:
	case BinaryOp::Mul:
	case BinaryOp::Div:
		return true;
	case BinaryOp::Or:
	case BinaryOp::And:
		// Pure boolean plumbing: recursion handles the operands.
		return false;
	}
	return false;
}

bool inspectsDimension(BuiltinFunction fn) {
	switch (fn) {
	case BuiltinFunction::IsIri:
	case BuiltinFunction::IsUri:
	case BuiltinFunction::IsBlank:
	case BuiltinFunction::IsLiteral:
	case BuiltinFunction::IsNumeric:
	case BuiltinFunction::Lang:
	case BuiltinFunction::Datatype:
	case BuiltinFunction::SameTerm:
	case BuiltinFunction::Strdt:
	case BuiltinFunction::Strlang:
	case BuiltinFunction::Coalesce:
	case BuiltinFunction::If:
		return true;
	default:
		return false;
	}
}

bool inspectsDimension(AggregateKind kind) {
	// MIN/MAX/SAMPLE return one of their inputs verbatim, so the result's
	// dimension is an input's; SUM's integrality decides the output's lexical
	// form. COUNT/AVG/GROUP_CONCAT have fixed result types.
	return kind == AggregateKind::Min || kind == AggregateKind::Max || kind == AggregateKind::Sample ||
	       kind == AggregateKind::Sum;
}

void walkExpr(const Expression &expr, const TranslatedPattern &scope, TranslationContext &ctx);

// Recurse into an expression's children without treating this node itself as a
// dimension inspector.
void walkChildren(const Expression &expr, const TranslatedPattern &scope, TranslationContext &ctx) {
	switch (expr.kind()) {
	case ExprKind::Binary: {
		const auto &b = static_cast<const BinaryExpr &>(expr);
		walkExpr(*b.left, scope, ctx);
		walkExpr(*b.right, scope, ctx);
		return;
	}
	case ExprKind::Unary:
		walkExpr(*static_cast<const UnaryExpr &>(expr).operand, scope, ctx);
		return;
	case ExprKind::In: {
		const auto &in = static_cast<const InExpr &>(expr);
		walkExpr(*in.lhs, scope, ctx);
		for (const auto &item : in.list) {
			walkExpr(*item, scope, ctx);
		}
		return;
	}
	case ExprKind::BuiltInCall: {
		for (const auto &arg : static_cast<const BuiltInCallExpr &>(expr).args) {
			walkExpr(*arg, scope, ctx);
		}
		return;
	}
	case ExprKind::FunctionCall: {
		for (const auto &arg : static_cast<const FunctionCallExpr &>(expr).args) {
			walkExpr(*arg, scope, ctx);
		}
		return;
	}
	case ExprKind::Aggregate: {
		const auto &agg = static_cast<const AggregateExpr &>(expr);
		if (agg.arg) {
			walkExpr(*agg.arg, scope, ctx);
		}
		return;
	}
	case ExprKind::Literal:
	case ExprKind::VarRef:
	case ExprKind::IriRef:
	case ExprKind::Exists:
		// An EXISTS's pattern variables are locally scoped and its own FILTERs are
		// visited when that nested pattern is walked, so there is nothing to do
		// here - matching collectVarRefs, which also stops at EXISTS.
		return;
	}
}

void walkExpr(const Expression &expr, const TranslatedPattern &scope, TranslationContext &ctx) {
	bool inspects = false;
	switch (expr.kind()) {
	case ExprKind::Binary:
		inspects = inspectsDimension(static_cast<const BinaryExpr &>(expr).op);
		break;
	case ExprKind::Unary:
		// Arithmetic negation cares about integrality; logical NOT does not.
		inspects = static_cast<const UnaryExpr &>(expr).op != UnaryOp::Not;
		break;
	case ExprKind::In:
		// `?x IN (...)` is a disjunction of equalities.
		inspects = true;
		break;
	case ExprKind::BuiltInCall:
		inspects = inspectsDimension(static_cast<const BuiltInCallExpr &>(expr).fn);
		break;
	case ExprKind::Aggregate:
		inspects = inspectsDimension(static_cast<const AggregateExpr &>(expr).aggKind);
		break;
	default:
		break;
	}
	if (inspects) {
		markVarsIn(expr, scope, ctx);
	}
	walkChildren(expr, scope, ctx);
}

// An equi-join on a shared variable is RDF term equality, so two candidate
// sources whose dimensions can differ must compare tags as well as text. Only
// the un-merged join path needs a tag *column* for that: a merged inner join
// compares the two in-scope tag expressions directly (Optimizer's
// termDimensionEquality).
//
// Marked only when both sides supply a tag and those tags are textually
// different constants. Where either side's dimension is undetermined there is no
// tag to compare and nothing better than today's lexical-only comparison, and
// marking would only produce a column no producer can fill.
void markJoinKeys(const std::vector<EquiKey> &keys, TranslationContext &ctx) {
	for (const auto &k : keys) {
		if (k.leftCol.tagExpr.empty() || k.rightCol.tagExpr.empty()) {
			continue;
		}
		if (k.leftCol.tagExpr != k.rightCol.tagExpr) {
			ctx.markNeedsTag(k.var);
		}
	}
}

// Generic post-order walk over a relation tree, applying `visit` to each node.
template <typename Visitor>
void walkNodes(const RelNode &node, Visitor &visit) {
	visit(node);
	switch (node.kind()) {
	case RelKind::Join: {
		const auto &j = static_cast<const JoinNode &>(node);
		walkNodes(*j.left, visit);
		walkNodes(*j.right, visit);
		return;
	}
	case RelKind::AntiJoin: {
		const auto &a = static_cast<const AntiJoinNode &>(node);
		walkNodes(*a.left, visit);
		walkNodes(*a.right, visit);
		return;
	}
	case RelKind::UnionByName: {
		for (const auto &arm : static_cast<const UnionByNameNode &>(node).arms) {
			walkNodes(*arm, visit);
		}
		return;
	}
	case RelKind::Filter:
		walkNodes(*static_cast<const FilterNode &>(node).child, visit);
		return;
	case RelKind::Bind:
		walkNodes(*static_cast<const BindNode &>(node).child, visit);
		return;
	case RelKind::TransitiveClosure:
		walkNodes(*static_cast<const TransitiveClosureNode &>(node).step, visit);
		return;
	case RelKind::Spj:
	case RelKind::Raw:
	case RelKind::SingleRow:
	case RelKind::Empty:
		return;
	}
}

struct JoinKeyVisitor {
	TranslationContext *ctx;
	void operator()(const RelNode &node) const {
		if (node.kind() == RelKind::Join) {
			markJoinKeys(static_cast<const JoinNode &>(node).keys, *ctx);
		} else if (node.kind() == RelKind::AntiJoin) {
			markJoinKeys(static_cast<const AntiJoinNode &>(node).keys, *ctx);
		}
	}
};

struct ExpressionVisitor {
	TranslationContext *ctx;
	void operator()(const RelNode &node) const {
		if (node.kind() == RelKind::Filter) {
			const auto &f = static_cast<const FilterNode &>(node);
			// Against the child's schema: that is the scope the predicate is
			// rendered in, so it is the scope whose annotations decide sufficiency.
			TranslatedPattern scope;
			fillScopeFromSchema(scope, f.child->schema());
			if (f.predicate != nullptr) {
				walkExpr(*f.predicate, scope, *ctx);
			}
			return;
		}
		if (node.kind() != RelKind::Bind) {
			return;
		}
		const auto &b = static_cast<const BindNode &>(node);
		TranslatedPattern scope;
		fillScopeFromSchema(scope, b.child->schema());
		if (b.expr == nullptr) {
			return;
		}
		walkExpr(*b.expr, scope, *ctx);
		// Propagate demand *down* through the BIND. If something above needs this
		// node's output tag, renderBind has to build it from the defining
		// expression - so whichever of that expression's variables would supply it
		// needs a tag column of its own. The walk is parent-first for exactly this
		// reason: every consumer above has already had its say by now.
		if (ctx->needsTag(b.outVar)) {
			markVarsIn(*b.expr, scope, *ctx);
		}
	}
};

void markQueryLevelNeeds(const RelNode &root, const sparql::ast::Query *query, TranslationContext &ctx) {
	TranslatedPattern rootScope;
	fillScopeFromSchema(rootScope, root.schema());

	if (query == nullptr) {
		return;
	}

	for (const auto &item : query->selectItems) {
		if (item.expr) {
			walkExpr(*item.expr, rootScope, ctx);
		}
	}
	for (const auto &gc : query->solutionModifier.groupBy) {
		if (!gc.expr) {
			continue;
		}
		if (!gc.asVar) {
			// A bare GROUP BY key partitions by RDF term, so - exactly like the
			// DISTINCT key below - it inspects the dimension whether or not the
			// expression itself is an operator.
			markVarsIn(*gc.expr, rootScope, ctx);
		}
		walkExpr(*gc.expr, rootScope, ctx);
	}
	for (const auto &h : query->solutionModifier.having) {
		walkExpr(*h, rootScope, ctx);
	}
	for (const auto &oc : query->solutionModifier.orderBy) {
		if (!oc.expr) {
			continue;
		}
		// An ORDER BY key is compared against every other row's, so it inspects the
		// dimension whether or not the expression itself is an operator.
		markVarsIn(*oc.expr, rootScope, ctx);
		walkExpr(*oc.expr, rootScope, ctx);
	}

	// DISTINCT/REDUCED deduplicate whole solutions, and two terms with the same
	// lexical form but different dimensions are different solutions. Without the
	// tag in the key, "1"^^xsd:integer and "1"^^xsd:string would collapse into one
	// row.
	if (!query->distinct && !query->reduced) {
		return;
	}
	for (const auto &c : root.schema()) {
		if (ctx.isInternal(c.var)) {
			continue;
		}
		if (!query->selectStar) {
			bool projected = false;
			for (const auto &item : query->selectItems) {
				projected = projected || (!item.expr && item.var->name == c.var);
			}
			if (!projected) {
				continue;
			}
		}
		if (!isFullyDetermined(c.term)) {
			ctx.markNeedsTag(c.var);
		}
	}
}

// Mark every variable `expr` references that is in `eligible`, without the
// isFullyDetermined() gate markVarsIn applies - see markPreFoldTagNeeds's doc
// comment for why no scope is available yet to check that gate here, and why
// `eligible` exists to make up for it.
void markAllVarsIn(const Expression &expr, const std::set<std::string> &eligible, TranslationContext &ctx) {
	std::vector<std::string> refs;
	collectVarRefs(expr, refs);
	for (const auto &name : refs) {
		if (eligible.count(name) != 0) {
			ctx.markNeedsTag(name);
		}
	}
}

void collectSubSelectProjectedVars(const GroupGraphPattern &pattern, std::set<std::string> &out);

// Recurse through every combinator that can nest a `{ SELECT ... }` without
// crossing into a query boundary of its own (a further-nested sub-select's
// interior is that sub-select's own concern, handled when translateQueryPattern
// recurses into it and runs this same pre-scan again - see the header comment).
void collectSubSelectProjectedVars(const GroupElement &el, std::set<std::string> &out) {
	switch (el.kind()) {
	case ElementKind::GroupGraphPattern:
		collectSubSelectProjectedVars(static_cast<const GroupGraphPattern &>(el), out);
		return;
	case ElementKind::UnionGraphPattern:
		for (const auto &branch : static_cast<const UnionGraphPattern &>(el).branches) {
			collectSubSelectProjectedVars(*branch, out);
		}
		return;
	case ElementKind::OptionalGraphPattern:
		collectSubSelectProjectedVars(*static_cast<const OptionalGraphPattern &>(el).pattern, out);
		return;
	case ElementKind::MinusGraphPattern:
		collectSubSelectProjectedVars(*static_cast<const MinusGraphPattern &>(el).pattern, out);
		return;
	case ElementKind::GraphGraphPattern:
		collectSubSelectProjectedVars(*static_cast<const GraphGraphPattern &>(el).pattern, out);
		return;
	case ElementKind::ServiceGraphPattern:
		collectSubSelectProjectedVars(*static_cast<const ServiceGraphPattern &>(el).pattern, out);
		return;
	case ElementKind::SubSelect: {
		const auto &sub = static_cast<const SubSelectElement &>(el);
		// A `SELECT *` sub-select's projected variables are only known once it
		// folds, which is exactly what this pre-scan runs ahead of - left
		// unfixed for that shape, matching this pass's documented scope (bare
		// explicit projections only).
		if (sub.query != nullptr && !sub.query->selectStar) {
			for (const auto &item : sub.query->selectItems) {
				if (!item.expr) {
					out.insert(item.var->name);
				}
			}
		}
		return;
	}
	case ElementKind::BasicGraphPattern:
	case ElementKind::Filter:
	case ElementKind::Bind:
	case ElementKind::InlineData:
		return;
	}
}

void collectSubSelectProjectedVars(const GroupGraphPattern &pattern, std::set<std::string> &out) {
	for (const auto &el : pattern.elements) {
		collectSubSelectProjectedVars(*el, out);
	}
}

} // namespace

void markJoinKeyTagNeeds(const RelNode &root, TranslationContext &ctx) {
	JoinKeyVisitor visitor {&ctx};
	walkNodes(root, visitor);
}

void markExpressionTagNeeds(const RelNode &root, const sparql::ast::Query *query, TranslationContext &ctx) {
	// Query-level consumers first, then the tree. Order matters: the tree walk is
	// parent-first so that a BIND can see whether anything above it wants its
	// output tag and propagate that demand into its defining expression, and the
	// query's own SELECT/ORDER BY/DISTINCT sit above every node in the tree.
	markQueryLevelNeeds(root, query, ctx);
	ExpressionVisitor visitor {&ctx};
	walkNodes(root, visitor);
}

void markPreFoldTagNeeds(const sparql::ast::Query &query, TranslationContext &ctx) {
	if (query.where == nullptr) {
		return;
	}
	// Restrict marking to variables a nested sub-select could actually supply
	// bare: this pass runs before fold(), so unlike markQueryLevelNeeds it has
	// no schema to gate on isFullyDetermined(), and marking unconditionally
	// would materialise unwanted tag columns even on an ordinary top-level
	// query whose types the mapping pins down (no sub-select in sight) -
	// regressing the very "well-typed mapping generates the same SQL as
	// before" property markJoinKeyTagNeeds/markExpressionTagNeeds preserve.
	// Restricting to this set costs nothing when it's empty (no sub-select:
	// the whole pre-scan below becomes a no-op, unchanged from before this
	// pass existed) and is exactly the set this pass exists to help with when
	// it isn't.
	std::set<std::string> eligible;
	collectSubSelectProjectedVars(*query.where, eligible);
	if (eligible.empty()) {
		return;
	}

	for (const auto &item : query.selectItems) {
		if (item.expr) {
			markAllVarsIn(*item.expr, eligible, ctx);
		}
	}
	for (const auto &gc : query.solutionModifier.groupBy) {
		if (gc.expr) {
			markAllVarsIn(*gc.expr, eligible, ctx);
		}
	}
	for (const auto &h : query.solutionModifier.having) {
		markAllVarsIn(*h, eligible, ctx);
	}
	for (const auto &oc : query.solutionModifier.orderBy) {
		if (oc.expr) {
			markAllVarsIn(*oc.expr, eligible, ctx);
		}
	}
	// DISTINCT/REDUCED's dedup key is every projected variable - see
	// markQueryLevelNeeds's identical rationale. SELECT * is skipped: which
	// variables that projects is only known once query.where has folded, which
	// is exactly what this pass runs ahead of.
	if ((query.distinct || query.reduced) && !query.selectStar) {
		for (const auto &item : query.selectItems) {
			if (!item.expr && eligible.count(item.var->name) != 0) {
				ctx.markNeedsTag(item.var->name);
			}
		}
	}
}

} // namespace sparql2sql
