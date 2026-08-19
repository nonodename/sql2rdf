#include "sparql2sql/ir/Optimizer.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sparql2sql/ExprAnalysis.h"
#include "sparql2sql/ExpressionTranslator.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TagSql.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/TypeCatalog.h"
#include "sparql2sql/ir/NativeKey.h"
#include "sparql2sql/ir/RelNode.h"
#include "sparql2sql/ir/SqlRenderer.h"

namespace sparql2sql {

namespace {

// An equi-join on a shared variable is RDF *term* equality, so two terms with
// the same lexical form but different dimensions - `"1"^^xsd:integer` from one
// triples map and `"1"^^xsd:string` from another - must not join.
//
// Inside a merged SPJ block both sides' tags are ordinary in-scope expressions
// (a producer's constant, or mergeInner's null-tolerant CASE), so the check
// costs nothing to add and needs no tag *column*: textually identical tags are
// trivially equal and contribute no conjunct at all, which is why an ordinary
// single-mapping self-join is unaffected. An empty tag means the mapping does
// not determine the dimension here, and there is nothing better to do than
// today's lexical-only comparison.
std::string termDimensionEquality(const ColumnInfo &lc, const ColumnInfo &rc) {
	if (lc.tagExpr.empty() || rc.tagExpr.empty() || lc.tagExpr == rc.tagExpr) {
		return std::string();
	}
	return "(" + lc.tagExpr + ") = (" + rc.tagExpr + ")";
}

// The SQL for one shared-variable equi-join key, folded into a flattened
// SpjRelation's WHERE clause.
//
// When the key isn't null-tolerant and nativeKeyPairs can prove a native
// rewrite (both sides a pure uncast base column, or both the same invertible
// rr:template), join on the NATIVE columns (`t1."x" = t2."y"`) - index-friendly
// and the actual perf win. Otherwise fall back to the VARCHAR-cast comparison
// (always correct): a null-tolerant disjunction for OPTIONAL-lineage keys, or
// plain equality.
std::string joinEquality(const ColumnInfo &lc, const ColumnInfo &rc, bool nullSafe, const TypeCatalog *catalog) {
	if (!nullSafe) {
		std::vector<NativeKeyPair> pairs = nativeKeyPairs(lc, rc, catalog);
		if (!pairs.empty()) {
			std::string cond;
			for (std::size_t i = 0; i < pairs.size(); ++i) {
				cond += (i > 0 ? " AND " : "");
				cond += pairs[i].leftRef + " = " + pairs[i].rightRef;
			}
			return cond;
		}
	}
	if (nullSafe) {
		return "(" + lc.renderedExpr + " = " + rc.renderedExpr + " OR " + lc.renderedExpr + " IS NULL OR " +
		       rc.renderedExpr + " IS NULL)";
	}
	return lc.renderedExpr + " = " + rc.renderedExpr;
}

// Merge an inner JoinNode whose two children are already-flattened
// SpjRelations into a single SpjRelation: the sources are concatenated
// (cross-join spine), all WHERE conjuncts plus the join equalities are pooled,
// and the projection is rebuilt from the children's columns (which carry the
// original base-column provenance, so native-key rewrite still works through
// arbitrarily deep left-deep chains). A per-child DISTINCT is lifted to one
// DISTINCT on the merged block: an inner join of duplicate-free relations is
// itself duplicate-free, so this is set-equivalent.
RelNodePtr mergeInner(JoinNode &join, const TypeCatalog *catalog) {
	SpjRelation &left = static_cast<SpjRelation &>(*join.left);
	SpjRelation &right = static_cast<SpjRelation &>(*join.right);

	RelNodePtr node(new SpjRelation());
	SpjRelation &out = static_cast<SpjRelation &>(*node);

	for (const auto &s : left.sources) {
		out.sources.push_back(s);
	}
	for (const auto &s : right.sources) {
		out.sources.push_back(s);
	}
	out.whereConds = left.whereConds;
	for (const auto &c : right.whereConds) {
		out.whereConds.push_back(c);
	}
	out.distinct = left.distinct || right.distinct;

	std::set<std::string> shared;
	for (const auto &k : join.keys) {
		shared.insert(k.var);
	}

	for (const auto &k : join.keys) {
		const ColumnInfo *lc = left.column(k.var);
		const ColumnInfo *rc = right.column(k.var);
		if (lc != nullptr && rc != nullptr) {
			out.whereConds.push_back(joinEquality(*lc, *rc, k.nullSafe, catalog));
			// Not folded into joinEquality itself: that function has a native-key
			// fast path returning early, and the dimension check applies to every
			// form of the key equally.
			const std::string dimension = termDimensionEquality(*lc, *rc);
			if (!dimension.empty() && !k.nullSafe) {
				// A null-tolerant key is vacuous when either side is unbound, so the
				// dimension check would have to be guarded the same way; leaving it
				// off keeps OPTIONAL's compatibility semantics exactly as they were.
				out.whereConds.push_back(dimension);
			}
		}
		const ColumnInfo *jc = join.column(k.var);
		ColumnInfo col;
		if (k.nullSafe && lc != nullptr && rc != nullptr) {
			col.var = k.var;
			col.renderedExpr = "COALESCE(" + lc->renderedExpr + ", " + rc->renderedExpr + ")";
			col.prov = Provenance::Coalesced;
			col.nonNull = jc != nullptr ? jc->nonNull : false;
			// The COALESCE really can return either operand, so the column can
			// only claim what both sides agree on. (The non-COALESCE branches
			// below copy one side wholesale, which carries its annotation with
			// it - only that side's value ever reaches the output.)
			col.term = meetColumns({lc, rc});
			if (!lc->tagExpr.empty() && !rc->tagExpr.empty()) {
				col.tagExpr = coalescedTag(lc->renderedExpr, lc->tagExpr, rc->renderedExpr, rc->tagExpr);
			}
		} else if (lc != nullptr) {
			col = *lc;
			col.nonNull = jc != nullptr ? jc->nonNull : lc->nonNull;
		} else if (rc != nullptr) {
			col = *rc;
			col.nonNull = jc != nullptr ? jc->nonNull : rc->nonNull;
		} else {
			col.var = k.var;
		}
		out.schema().push_back(col);
	}
	for (const auto &c : left.schema()) {
		if (shared.count(c.var)) {
			continue;
		}
		ColumnInfo col = c;
		const ColumnInfo *jc = join.column(c.var);
		if (jc != nullptr) {
			col.nonNull = jc->nonNull;
		}
		out.schema().push_back(col);
	}
	for (const auto &c : right.schema()) {
		if (shared.count(c.var)) {
			continue;
		}
		ColumnInfo col = c;
		const ColumnInfo *jc = join.column(c.var);
		if (jc != nullptr) {
			col.nonNull = jc->nonNull;
		}
		out.schema().push_back(col);
	}
	return node;
}

// SPJ flattening. Bottom-up: after flattening both children of an inner join,
// if both are SpjRelations, merge them. Outer joins, anti-joins, unions,
// filters and binds are boundaries: their children flatten internally, but the
// node itself is preserved (never hoisted across).
RelNodePtr flatten(RelNodePtr node, const TypeCatalog *catalog) {
	switch (node->kind()) {
	case RelKind::Join: {
		JoinNode &j = static_cast<JoinNode &>(*node);
		j.left = flatten(std::move(j.left), catalog);
		j.right = flatten(std::move(j.right), catalog);
		if (j.joinKind == JoinKind::Inner && j.left->kind() == RelKind::Spj && j.right->kind() == RelKind::Spj) {
			return mergeInner(j, catalog);
		}
		return node;
	}
	case RelKind::AntiJoin: {
		AntiJoinNode &a = static_cast<AntiJoinNode &>(*node);
		a.left = flatten(std::move(a.left), catalog);
		a.right = flatten(std::move(a.right), catalog);
		return node;
	}
	case RelKind::UnionByName: {
		UnionByNameNode &u = static_cast<UnionByNameNode &>(*node);
		for (auto &arm : u.arms) {
			arm = flatten(std::move(arm), catalog);
		}
		return node;
	}
	case RelKind::Filter: {
		FilterNode &f = static_cast<FilterNode &>(*node);
		f.child = flatten(std::move(f.child), catalog);
		return node;
	}
	case RelKind::Bind: {
		BindNode &b = static_cast<BindNode &>(*node);
		b.child = flatten(std::move(b.child), catalog);
		return node;
	}
	case RelKind::TransitiveClosure: {
		TransitiveClosureNode &t = static_cast<TransitiveClosureNode &>(*node);
		t.step = flatten(std::move(t.step), catalog);
		return node; // non-mergeable: the node itself is never hoisted/merged.
	}
	case RelKind::Spj:
	case RelKind::Raw:
	case RelKind::SingleRow:
	case RelKind::Empty:
		return node;
	}
	return node;
}

std::string replaceAll(std::string s, const std::string &from, const std::string &to) {
	if (from.empty()) {
		return s;
	}
	std::size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}

// Rewrite alias-qualified column references in a SQL fragment per `rename`
// (dropped alias -> kept alias). Aliases only ever appear as `alias."col"`, so
// matching on the trailing dot is exact: "t2." never matches inside "t20.".
std::string applyRename(const std::string &sql, const std::map<std::string, std::string> &rename) {
	std::string out;
	out.reserve(sql.size() + rename.size() * 8); // rough guess to avoid too many reallocs
	out = sql;
	for (const auto &r : rename) {
		out = replaceAll(out, r.first + ".", r.second + ".");
	}
	return out;
}

std::string renameAlias(const std::string &alias, const std::map<std::string, std::string> &rename) {
	auto it = rename.find(alias);
	return it == rename.end() ? alias : it->second;
}

// A WHERE conjunct that became trivially true after alias rewriting: a single
// top-level "X = X" (the subject-key equality between two merged scans). The
// null-tolerant OPTIONAL form has additional operators, so it never matches.
bool isTrivialEquality(const std::string &cond) {
	std::size_t eq = cond.find(" = ");
	if (eq == std::string::npos) {
		return false;
	}
	if (cond.find(" = ", eq + 3) != std::string::npos) {
		return false; // more than one "=" -> not a plain equality
	}
	return cond.substr(0, eq) == cond.substr(eq + 3);
}

// Self-join elimination. Two sources of the same logical table bound to the
// same subject variable via the same subject term map denote the same row
// (the subject map is assumed row-unique - a standard well-formed-R2RML
// assumption). Keep the first, drop the rest, and rewrite every reference to a
// dropped alias onto the kept one; the subject-key equalities then collapse to
// trivially-true and are removed, and duplicate conjuncts are deduplicated.
void eliminateSelfJoinsInSpj(SpjRelation &spj) {
	std::map<std::string, std::string> keptByGroup; // group key -> kept alias
	std::map<std::string, std::string> rename;      // dropped alias -> kept alias
	for (const auto &s : spj.sources) {
		if (s.subjectVar.empty() || s.subjectKeySig.empty()) {
			continue;
		}
		std::string group = s.tableIdentity + "\x1f" + s.subjectKeySig + "\x1f" + s.subjectVar;
		auto it = keptByGroup.find(group);
		if (it == keptByGroup.end()) {
			keptByGroup[group] = s.alias;
		} else {
			rename[s.alias] = it->second;
		}
	}
	if (rename.empty()) {
		return;
	}

	std::vector<SpjSource> keptSources;
	keptSources.reserve(spj.sources.size() - rename.size());
	for (const auto &s : spj.sources) {
		if (rename.count(s.alias) == 0) {
			keptSources.push_back(s);
		}
	}
	spj.sources = std::move(keptSources);

	for (auto &c : spj.schema()) {
		c.sourceAlias = renameAlias(c.sourceAlias, rename);
		c.renderedExpr = applyRename(c.renderedExpr, rename);
		// Usually a constant string literal with no alias in it at all, but
		// mergeInner's null-tolerant form embeds both sides' renderedExpr to pick
		// the tag COALESCE would have picked - so it has to be renamed too.
		c.tagExpr = applyRename(c.tagExpr, rename);
		c.nativeColumnRef = applyRename(c.nativeColumnRef, rename);
		for (auto &ref : c.templateColumnRefs) {
			ref = applyRename(ref, rename);
		}
	}

	std::vector<std::string> newConds;
	std::set<std::string> seen;
	for (const auto &cond : spj.whereConds) {
		std::string r = applyRename(cond, rename);
		if (isTrivialEquality(r)) {
			continue;
		}
		if (seen.insert(r).second) {
			newConds.push_back(r);
		}
	}
	spj.whereConds = std::move(newConds);
}

void selfJoinWalk(RelNode &node) {
	switch (node.kind()) {
	case RelKind::Spj:
		eliminateSelfJoinsInSpj(static_cast<SpjRelation &>(node));
		break;
	case RelKind::Join: {
		JoinNode &j = static_cast<JoinNode &>(node);
		selfJoinWalk(*j.left);
		selfJoinWalk(*j.right);
		break;
	}
	case RelKind::AntiJoin: {
		AntiJoinNode &a = static_cast<AntiJoinNode &>(node);
		selfJoinWalk(*a.left);
		selfJoinWalk(*a.right);
		break;
	}
	case RelKind::UnionByName: {
		UnionByNameNode &u = static_cast<UnionByNameNode &>(node);
		for (auto &arm : u.arms) {
			selfJoinWalk(*arm);
		}
		break;
	}
	case RelKind::Filter:
		selfJoinWalk(*static_cast<FilterNode &>(node).child);
		break;
	case RelKind::Bind:
		selfJoinWalk(*static_cast<BindNode &>(node).child);
		break;
	case RelKind::TransitiveClosure:
		selfJoinWalk(*static_cast<TransitiveClosureNode &>(node).step);
		break;
	case RelKind::Raw:
	case RelKind::SingleRow:
	case RelKind::Empty:
		break;
	}
}

// Always-safe DISTINCT elimination: when the enclosing query dedups its result
// (SELECT DISTINCT) or is an ASK existence check, every per-relation DISTINCT
// is redundant - the outer dedup/existence subsumes it. Also downgrades the
// candidate UNION's dedup (UNION BY NAME) to UNION ALL BY NAME for the same
// reason.
void stripDistinct(RelNode &node) {
	switch (node.kind()) {
	case RelKind::Spj:
		static_cast<SpjRelation &>(node).distinct = false;
		break;
	case RelKind::UnionByName: {
		UnionByNameNode &u = static_cast<UnionByNameNode &>(node);
		u.all = true;
		for (auto &arm : u.arms) {
			stripDistinct(*arm);
		}
		break;
	}
	case RelKind::Join: {
		JoinNode &j = static_cast<JoinNode &>(node);
		stripDistinct(*j.left);
		stripDistinct(*j.right);
		break;
	}
	case RelKind::AntiJoin: {
		AntiJoinNode &a = static_cast<AntiJoinNode &>(node);
		stripDistinct(*a.left);
		stripDistinct(*a.right); // inside NOT EXISTS: dedup is irrelevant to existence.
		break;
	}
	case RelKind::Filter:
		stripDistinct(*static_cast<FilterNode &>(node).child);
		break;
	case RelKind::Bind:
		stripDistinct(*static_cast<BindNode &>(node).child);
		break;
	case RelKind::TransitiveClosure:
		stripDistinct(*static_cast<TransitiveClosureNode &>(node).step);
		break;
	case RelKind::Raw:
	case RelKind::SingleRow:
	case RelKind::Empty:
		break;
	}
}

// --- Filter pushdown -----------------------------------------------------
//
// Re-parent FILTERs downward so each lands on the smallest relation supplying
// its variables. A FilterNode's predicate is a borrowed AST pointer rendered
// late against its child's schema, so moving one is pure re-parenting: no
// re-render, no AST synthesis. A conjunction is decomposed into single
// conjuncts, and each is routed independently.
//
// Safe targets: inner-join sides (predicate over one side only), every arm of
// a UNION (WHERE distributes over UNION when every arm supplies the vars), and
// an anti-join's left side (its output schema is exactly the left's, so a
// left-only predicate commutes). LeftOuter joins, BINDs, and anything else are
// boundaries - a conjunct that reaches one stops there. Conjuncts containing
// EXISTS/NOT EXISTS are never pushed (their correlation variables are
// deliberately invisible to collectVarRefs), so they too stop at the boundary.
//
// A conjunct that reaches an SpjRelation is FOLDED INTO that block's WHERE list
// rather than wrapping it in a FilterNode (when a TranslationContext is
// available - see foldConjunctIntoSpj). That is what makes this pass worth
// running at all: this pass runs BEFORE flattening, so a folded predicate
// leaves the block an SpjRelation and mergeInner can still fuse it with its
// inner-join partners. Left as a FilterNode it would instead be a flattening
// boundary, splitting one merged block into two derived tables and losing the
// native-typed-join-key rewrite across the split. (Simply re-parenting a
// FilterNode lower buys nothing against an engine that does its own predicate
// pushdown through derived tables; keeping blocks mergeable does.)

std::set<std::string> varRefSet(const sparql::ast::Expression &expr) {
	std::vector<std::string> refs;
	collectVarRefs(expr, refs);
	return std::set<std::string>(refs.begin(), refs.end());
}

bool isSubset(const std::set<std::string> &small, const std::set<std::string> &big) {
	for (const auto &v : small) {
		if (big.count(v) == 0) {
			return false;
		}
	}
	return true;
}

bool intersects(const std::set<std::string> &a, const std::set<std::string> &b) {
	for (const auto &v : a) {
		if (b.count(v)) {
			return true;
		}
	}
	return false;
}

// Fold one FILTER conjunct into an SpjRelation's WHERE list.
//
// The predicate is rendered against a fresh sentinel alias, so every variable
// reference comes out as `sentinel."v_x"`; each of those is then textually
// replaced by that column's own SQL expression, evaluated over the block's own
// FROM sources. This is the same alias-substitution technique the self-join pass
// uses (applyRename), and relies on the same invariant: an alias only ever
// appears as `alias."col"`, so matching on the trailing dot is exact.
//
// Returns false (leaving the caller to wrap a FilterNode instead) if any
// reference to the sentinel survives substitution, which would mean a variable
// the block's schema doesn't supply.
//
// Filtering before the block's DISTINCT is equivalent to filtering after it: the
// predicate reads only projected columns, so it cannot distinguish two rows that
// DISTINCT would collapse.
bool foldConjunctIntoSpj(SpjRelation &spj, const sparql::ast::Expression &pred, TranslationContext &ctx) {
	// Share the renderer's scope builder rather than rebuilding it here: a
	// folded predicate must see exactly the same variable scope *and* the same
	// static term annotations as an un-folded one, or whether a filter was
	// pushed down would change the SQL it translates to.
	TranslatedPattern scope;
	fillScopeFromSchema(scope, spj.schema());

	std::string sentinel = ctx.nextAlias();
	std::string sql;
	try {
		sql = translateExpression(pred, scope, sentinel, ctx);
	} catch (const TranslationError &) {
		// This block cannot render the predicate - most often because the term
		// dimension it needs is not statically known here and no tag column has
		// been materialised yet (tag demand for expressions is decided *after* this
		// pass, precisely so pushdown gets first refusal - see TagDemand.h).
		// Declining to push leaves the FilterNode in place, where the late renderer
		// will translate it with the tag available, or raise the same error there.
		return false;
	}
	for (const auto &c : spj.schema()) {
		sql = replaceAll(sql, sentinel + "." + mangleVar(c.var, ctx.dialect()), "(" + c.renderedExpr + ")");
	}
	if (sql.find(sentinel + ".") != std::string::npos) {
		return false;
	}
	spj.whereConds.push_back(sql);
	return true;
}

// Wrap `child` in a chain of single-conjunct FilterNodes (no further pushdown -
// used at boundaries and for conjuncts that stay above a node). Each new
// FilterNode copies the child's schema, exactly as PatternFolder builds them.
RelNodePtr wrapFilters(RelNodePtr child, const std::vector<const sparql::ast::Expression *> &preds) {
	for (const auto *p : preds) {
		RelNodePtr node(new FilterNode());
		FilterNode &f = static_cast<FilterNode &>(*node);
		f.schema() = child->schema();
		f.predicate = p;
		f.child = std::move(child);
		child = std::move(node);
	}
	return child;
}

// Apply `conjuncts` as filters on top of `child`, pushing each as deep as it
// can safely go. Recurses on strictly smaller subtrees, so it terminates.
RelNodePtr pushConjuncts(RelNodePtr child, const std::vector<const sparql::ast::Expression *> &conjuncts,
                         TranslationContext *ctx) {
	if (conjuncts.empty()) {
		return child;
	}

	switch (child->kind()) {
	case RelKind::Join: {
		JoinNode &j = static_cast<JoinNode &>(*child);
		if (j.joinKind != JoinKind::Inner) {
			return wrapFilters(std::move(child), conjuncts); // LeftOuter: boundary.
		}
		std::set<std::string> leftVars = j.left->allVars();
		std::set<std::string> rightVars = j.right->allVars();
		// A nullSafe key's merged value is COALESCE(left, right): pushing a
		// predicate onto a side where the variable is still nullable there
		// would evaluate against a NULL that the join's null-tolerant ON
		// condition (and the COALESCE projection) are specifically there to
		// look past, wrongly dropping rows the other side would have rescued.
		// Only push a nullSafe key's variable onto a side where it's
		// guaranteed non-NULL.
		std::set<std::string> leftUnsafe;
		std::set<std::string> rightUnsafe;
		for (const auto &k : j.keys) {
			if (!k.leftCol.nonNull) {
				leftUnsafe.insert(k.var);
			}
			if (!k.rightCol.nonNull) {
				rightUnsafe.insert(k.var);
			}
		}
		std::vector<const sparql::ast::Expression *> leftConj;
		std::vector<const sparql::ast::Expression *> rightConj;
		std::vector<const sparql::ast::Expression *> keep;
		for (const auto *c : conjuncts) {
			if (containsExists(*c)) {
				keep.push_back(c);
				continue;
			}
			std::set<std::string> refs = varRefSet(*c);
			if (isSubset(refs, leftVars) && !intersects(refs, leftUnsafe)) {
				leftConj.push_back(c);
			} else if (isSubset(refs, rightVars) && !intersects(refs, rightUnsafe)) {
				rightConj.push_back(c);
			} else {
				keep.push_back(c);
			}
		}
		j.left = pushConjuncts(std::move(j.left), leftConj, ctx);
		j.right = pushConjuncts(std::move(j.right), rightConj, ctx);
		return wrapFilters(std::move(child), keep);
	}
	case RelKind::AntiJoin: {
		AntiJoinNode &a = static_cast<AntiJoinNode &>(*child);
		std::set<std::string> leftVars = a.left->allVars();
		std::vector<const sparql::ast::Expression *> leftConj;
		std::vector<const sparql::ast::Expression *> keep;
		for (const auto *c : conjuncts) {
			if (!containsExists(*c) && isSubset(varRefSet(*c), leftVars)) {
				leftConj.push_back(c);
			} else {
				keep.push_back(c);
			}
		}
		a.left = pushConjuncts(std::move(a.left), leftConj, ctx);
		return wrapFilters(std::move(child), keep);
	}
	case RelKind::UnionByName: {
		UnionByNameNode &u = static_cast<UnionByNameNode &>(*child);
		std::vector<const sparql::ast::Expression *> distribute;
		std::vector<const sparql::ast::Expression *> keep;
		for (const auto *c : conjuncts) {
			if (containsExists(*c)) {
				keep.push_back(c);
				continue;
			}
			std::set<std::string> refs = varRefSet(*c);
			bool everyArmHasVars = true;
			for (const auto &arm : u.arms) {
				if (!isSubset(refs, arm->allVars())) {
					everyArmHasVars = false;
					break;
				}
			}
			if (everyArmHasVars) {
				distribute.push_back(c);
			} else {
				keep.push_back(c);
			}
		}
		if (!distribute.empty()) {
			for (auto &arm : u.arms) {
				arm = pushConjuncts(std::move(arm), distribute, ctx);
			}
		}
		return wrapFilters(std::move(child), keep);
	}
	case RelKind::Spj: {
		// Fold what we can straight into the block's WHERE; anything that can't
		// be folded (no context, an EXISTS conjunct, or a reference the schema
		// doesn't supply) stays a FilterNode above it.
		SpjRelation &spj = static_cast<SpjRelation &>(*child);
		std::vector<const sparql::ast::Expression *> keep;
		keep.reserve(conjuncts.size());
		for (const auto *c : conjuncts) {
			if (ctx == nullptr || containsExists(*c) || !foldConjunctIntoSpj(spj, *c, *ctx)) {
				keep.push_back(c);
			}
		}
		return wrapFilters(std::move(child), keep);
	}
	case RelKind::TransitiveClosure:
		// The closure's own output vars (subject/object) are unrelated to
		// step's internal from/to vars, so a filter over the output cannot
		// be re-expressed over step; boundary, same as Bind/Raw/SingleRow/Empty.
		return wrapFilters(std::move(child), conjuncts);
	default:
		// Bind, Raw, SingleRow, Empty: boundary.
		return wrapFilters(std::move(child), conjuncts);
	}
}

// Structural walk: recurse everywhere, and at each FilterNode split its
// predicate into conjuncts and push them into the (already-optimized) child.
RelNodePtr pushFilters(RelNodePtr node, TranslationContext *ctx) {
	switch (node->kind()) {
	case RelKind::Filter: {
		FilterNode &f = static_cast<FilterNode &>(*node);
		f.child = pushFilters(std::move(f.child), ctx);
		std::vector<const sparql::ast::Expression *> conjuncts = splitConjuncts(*f.predicate);
		return pushConjuncts(std::move(f.child), conjuncts, ctx);
	}
	case RelKind::Join: {
		JoinNode &j = static_cast<JoinNode &>(*node);
		j.left = pushFilters(std::move(j.left), ctx);
		j.right = pushFilters(std::move(j.right), ctx);
		return node;
	}
	case RelKind::AntiJoin: {
		AntiJoinNode &a = static_cast<AntiJoinNode &>(*node);
		a.left = pushFilters(std::move(a.left), ctx);
		a.right = pushFilters(std::move(a.right), ctx);
		return node;
	}
	case RelKind::UnionByName: {
		UnionByNameNode &u = static_cast<UnionByNameNode &>(*node);
		for (auto &arm : u.arms) {
			arm = pushFilters(std::move(arm), ctx);
		}
		return node;
	}
	case RelKind::Bind: {
		BindNode &b = static_cast<BindNode &>(*node);
		b.child = pushFilters(std::move(b.child), ctx);
		return node;
	}
	case RelKind::Spj:
	case RelKind::Raw:
	case RelKind::SingleRow:
	case RelKind::Empty:
	case RelKind::TransitiveClosure:
		// TransitiveClosure: nothing above it needs to push a filter into
		// step (that's pushConjuncts's job, handled above); step itself was
		// never wrapped by an unpushed FilterNode from the closure's own
		// construction.
		return node;
	}
	return node;
}

} // namespace

RelNodePtr optimize(RelNodePtr root, const OptimizerOptions &opts) {
	// Pushdown first: a conjunct folded into an SpjRelation keeps that block
	// mergeable, so flattening can still fuse it with its inner-join partners.
	root = pushFilters(std::move(root), opts.ctx);
	root = flatten(std::move(root), opts.catalog);
	selfJoinWalk(*root);
	if (opts.topLevelDistinct) {
		stripDistinct(*root);
	}
	return root;
}

} // namespace sparql2sql
