#include "sparql2sql/ir/Optimizer.h"

#include <set>
#include <string>
#include <utility>

#include "sparql2sql/TypeCatalog.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

namespace {

// The SQL for one shared-variable equi-join key, folded into a flattened
// SpjRelation's WHERE clause.
//
// When both sides are a pure (uncast) base column, the types are known
// comparable via the catalog, and the key isn't null-tolerant, join on the
// NATIVE columns (`t1."x" = t2."y"`) - index-friendly and the actual perf win.
// Otherwise fall back to the VARCHAR-cast comparison (always correct): a
// null-tolerant disjunction for OPTIONAL-lineage keys, or plain equality.
std::string joinEquality(const ColumnInfo &lc, const ColumnInfo &rc, bool nullSafe, const TypeCatalog *catalog) {
	if (!nullSafe && lc.prov == Provenance::PureColumn && rc.prov == Provenance::PureColumn &&
	    !lc.nativeColumnRef.empty() && !rc.nativeColumnRef.empty() && catalog != nullptr &&
	    catalog->comparable(lc.tableIdentity, lc.columnName, rc.tableIdentity, rc.columnName)) {
		return lc.nativeColumnRef + " = " + rc.nativeColumnRef;
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
		}
		const ColumnInfo *jc = join.column(k.var);
		ColumnInfo col;
		if (k.nullSafe && lc != nullptr && rc != nullptr) {
			col.var = k.var;
			col.renderedExpr = "COALESCE(" + lc->renderedExpr + ", " + rc->renderedExpr + ")";
			col.prov = Provenance::Coalesced;
			col.nonNull = jc != nullptr ? jc->nonNull : false;
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
	case RelKind::Spj:
	case RelKind::Raw:
	case RelKind::SingleRow:
	case RelKind::Empty:
		return node;
	}
	return node;
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
	case RelKind::Raw:
	case RelKind::SingleRow:
	case RelKind::Empty:
		break;
	}
}

} // namespace

RelNodePtr optimize(RelNodePtr root, const OptimizerOptions &opts) {
	root = flatten(std::move(root), opts.catalog);
	if (opts.topLevelDistinct) {
		stripDistinct(*root);
	}
	return root;
}

} // namespace sparql2sql
