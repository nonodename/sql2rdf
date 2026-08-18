#include "sparql2sql/ir/SqlRenderer.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sparql2sql/ExpressionTranslator.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TagSql.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/ir/NativeKey.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

namespace {

std::string renderNode(const RelNode &node, TranslationContext &ctx);

// A TranslatedPattern carrying a node's schema *description* but no SQL -
// enough for translateExpression's in-scope/optionality checks and its static
// term-dimension lookups (it never reads scope.sql).
TranslatedPattern scopeOf(const RelNode &node) {
	TranslatedPattern tp;
	fillScopeFromSchema(tp, node.schema());
	return tp;
}

// The SQL expression producing `col`'s runtime type tag from within the owning
// node's own FROM scope - i.e. the constant the producer minted.
//
// Throws rather than substituting NULL when there is no such constant, because
// NULL is this representation's spelling of *unbound*: emitting one for a term
// that is bound would make isLITERAL() answer "error" for a perfectly good
// literal. The only shapes that reach here without a constant are a transitive
// closure whose step relation's endpoint term maps disagree, and a Raw leaf
// whose producer had no constant either - both of which threw at translation
// time before tags existed too, so this is not a new refusal, only a
// better-explained one.
std::string producedTag(const ColumnInfo &col, const char *nodeDescription) {
	if (col.tagExpr.empty()) {
		throw TranslationError(
		    std::string("unsupported: the RDF term dimension of ?") + col.var +
		    " is needed at run "
		    "time, but " +
		    nodeDescription +
		    " cannot supply one - its contributing term maps "
		    "disagree and it is not re-rendered late enough to carry a per-row tag column. Declare a "
		    "consistent rr:datatype/rr:termType across the contributing term maps, or avoid asking "
		    "about this variable's type.");
	}
	return col.tagExpr;
}

// ", <tag> AS d_<var>" for one transitive-closure endpoint column, or "" when
// no tag is wanted.
//
// The closure's own recursive CTE deliberately carries only the endpoint node
// text: threading a tag column through it would change what the terminating
// UNION deduplicates on. That is sound because the step relation's endpoint term
// maps determine the tag statically in every mapping shape that reaches here -
// and where they disagree, producedTag says so rather than guessing.
std::string closureTagProjection(const ColumnInfo &col, TranslationContext &ctx) {
	if (!ctx.needsTag(col.var)) {
		return std::string();
	}
	return ", " + producedTag(col, "a transitive-closure (`+`/`*`) path") + " AS " +
	       mangleVarTag(col.var, ctx.dialect());
}

// Render one equi-key comparison across a derived-table boundary (where only
// the mangled `v_<name>` columns are in scope, so no native-column rewrite is
// possible here - see nativeKeyProjections).
//
// A null-tolerant key only needs the IS NULL disjunct for the side that can
// actually be NULL, and needs neither when both sides are guaranteed non-NULL.
// This matters far more than the redundant text suggests: an OR'd comparison is
// a non-equi predicate, which stops the engine from decorrelating a
// correlated/anti-join into a hash semi-join and leaves it a nested loop over
// the inner relation.
std::string keyComparison(const EquiKey &k, const std::string &lcol, const std::string &rcol) {
	std::string cond = lcol + " = " + rcol;
	if (!k.nullSafe) {
		return cond;
	}
	if (!k.leftCol.nonNull) {
		cond += " OR " + lcol + " IS NULL";
	}
	if (!k.rightCol.nonNull) {
		cond += " OR " + rcol + " IS NULL";
	}
	if (cond.find(" OR ") == std::string::npos) {
		return cond; // Flagged null-safe, but provably neither side is nullable.
	}
	return "(" + cond + ")";
}

// Extra projected columns to append to an SpjRelation's SELECT list: the
// (mangled name, SQL expression) pairs a join renderer needs exposed through
// the derived-table boundary so it can compare native key columns.
using ExtraProjections = std::vector<std::pair<std::string, std::string>>;

std::string renderSpj(const SpjRelation &rel, TranslationContext &ctx, const ExtraProjections *extra) {
	const SqlDialect &dialect = ctx.dialect();
	std::string sql = "SELECT";
	if (rel.distinct) {
		sql += " DISTINCT";
	}
	if (rel.schema().empty() && (extra == nullptr || extra->empty())) {
		sql += joinColumnList({"1 AS " + dialect.quoteIdentifier("_dummy")}, ctx);
	} else {
		std::vector<std::string> cols;
		for (const auto &c : rel.schema()) {
			cols.push_back(c.renderedExpr + " AS " + mangleVar(c.var, dialect));
			if (ctx.needsTag(c.var)) {
				cols.push_back(producedTag(c, "this select-project-join block") + " AS " + mangleVarTag(c.var, dialect));
			}
		}
		if (extra != nullptr) {
			for (const auto &e : *extra) {
				cols.push_back(e.second + " AS " + e.first);
			}
		}
		sql += joinColumnList(cols, ctx);
	}
	sql += ctx.clauseSep() + "FROM ";
	for (std::size_t i = 0; i < rel.sources.size(); ++i) {
		if (i > 0) {
			// Multi-source (post-flatten) spine: cross-join sources; the
			// inter-source join equalities live in whereConds.
			sql += ", ";
		}
		sql += rel.sources[i].sql;
	}
	if (!rel.whereConds.empty()) {
		std::vector<std::string> conds;
		for (const auto &c : rel.whereConds) {
			conds.push_back("(" + c + ")");
		}
		sql += ctx.clauseSep() + "WHERE " + joinConditions(conds, ctx);
	}
	return sql;
}

// Plan native-column comparisons for a join across the derived-table boundary.
// Only the projected term text is in scope out there, so a native comparison
// needs each side's base columns exposed as extra hidden projections, which is
// only possible when the child renders its own SELECT list - i.e. when it is
// directly an SpjRelation (the shape flattening produces). Anything else, and
// any null-tolerant key, keeps the term-text comparison.
//
// This is the rewrite an engine cannot perform for itself: R2RML subjects are
// nearly always rr:template IRIs, so without it every subject join hashes
// constructed IRI strings instead of the underlying key columns.
void planNativeKeys(const std::vector<EquiKey> &keys, const RelNode &left, const RelNode &right,
                    TranslationContext &ctx, const std::string &leftAlias, const std::string &rightAlias,
                    ExtraProjections &leftExtra, ExtraProjections &rightExtra, std::vector<std::string> &conds,
                    std::vector<bool> &rewritten) {
	rewritten.assign(keys.size(), false);
	if (left.kind() != RelKind::Spj || right.kind() != RelKind::Spj) {
		return;
	}
	const SqlDialect &dialect = ctx.dialect();
	for (std::size_t i = 0; i < keys.size(); ++i) {
		if (keys[i].nullSafe) {
			continue;
		}
		std::vector<NativeKeyPair> pairs = nativeKeyPairs(keys[i].leftCol, keys[i].rightCol, ctx.catalog());
		if (pairs.empty()) {
			continue;
		}
		for (const auto &p : pairs) {
			std::string hidden = dialect.quoteIdentifier("k_" + std::to_string(leftExtra.size()));
			leftExtra.emplace_back(hidden, p.leftRef);
			rightExtra.emplace_back(hidden, p.rightRef);
			conds.push_back(leftAlias + "." + hidden + " = " + rightAlias + "." + hidden);
		}
		rewritten[i] = true;
	}
}

// Render a join/anti-join child, appending `extra` hidden projections when the
// native-key plan asked for them (only ever non-empty for a direct Spj child).
std::string renderChild(const RelNode &node, TranslationContext &ctx, const ExtraProjections &extra) {
	if (!extra.empty() && node.kind() == RelKind::Spj) {
		return renderSpj(static_cast<const SpjRelation &>(node), ctx, &extra);
	}
	return renderNode(node, ctx);
}

std::string renderJoin(const JoinNode &join, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string leftAlias = ctx.nextAlias();
	std::string rightAlias = ctx.nextAlias();

	ExtraProjections leftExtra;
	ExtraProjections rightExtra;
	std::vector<std::string> nativeConds;
	std::vector<bool> rewritten;
	planNativeKeys(join.keys, *join.left, *join.right, ctx, leftAlias, rightAlias, leftExtra, rightExtra, nativeConds,
	               rewritten);

	std::string leftSql;
	std::string rightSql;
	{
		TranslationContext::SubqueryDepthGuard depthGuard(ctx);
		leftSql = renderChild(*join.left, ctx, leftExtra);
		rightSql = renderChild(*join.right, ctx, rightExtra);
	}

	std::set<std::string> shared;
	for (const auto &k : join.keys) {
		shared.insert(k.var);
	}

	std::vector<std::string> onConditions;
	std::vector<std::string> projectExprs;
	for (std::size_t i = 0; i < join.keys.size(); ++i) {
		const EquiKey &k = join.keys[i];
		std::string lcol = leftAlias + "." + mangleVar(k.var, dialect);
		std::string rcol = rightAlias + "." + mangleVar(k.var, dialect);
		if (!rewritten[i]) {
			onConditions.push_back(keyComparison(k, lcol, rcol));
		}
		if (k.nullSafe) {
			projectExprs.push_back("COALESCE(" + lcol + ", " + rcol + ") AS " + mangleVar(k.var, dialect));
		} else {
			projectExprs.push_back(lcol + " AS " + mangleVar(k.var, dialect));
		}
		if (ctx.needsTag(k.var)) {
			const std::string ltag = leftAlias + "." + mangleVarTag(k.var, dialect);
			const std::string rtag = rightAlias + "." + mangleVarTag(k.var, dialect);
			// A plain COALESCE of the two *columns* is exactly right here, unlike
			// the constant-tag case in mergeInner: a child's tag column is NULL
			// precisely when its value column is, so COALESCE picks the tag from
			// the same side the value came from.
			projectExprs.push_back((k.nullSafe ? ("COALESCE(" + ltag + ", " + rtag + ")") : ltag) + " AS " +
			                       mangleVarTag(k.var, dialect));
		}
	}
	onConditions.insert(onConditions.end(), nativeConds.begin(), nativeConds.end());
	for (const auto &v : join.left->allVars()) {
		if (shared.count(v)) {
			continue;
		}
		projectExprs.push_back(leftAlias + "." + mangleVar(v, dialect) + " AS " + mangleVar(v, dialect));
		if (ctx.needsTag(v)) {
			projectExprs.push_back(leftAlias + "." + mangleVarTag(v, dialect) + " AS " + mangleVarTag(v, dialect));
		}
	}
	for (const auto &v : join.right->allVars()) {
		if (shared.count(v)) {
			continue;
		}
		projectExprs.push_back(rightAlias + "." + mangleVar(v, dialect) + " AS " + mangleVar(v, dialect));
		if (ctx.needsTag(v)) {
			projectExprs.push_back(rightAlias + "." + mangleVarTag(v, dialect) + " AS " + mangleVarTag(v, dialect));
		}
	}

	std::string sql = "SELECT";
	if (projectExprs.empty()) {
		sql += joinColumnList({"1 AS " + dialect.quoteIdentifier("_dummy")}, ctx);
	} else {
		sql += joinColumnList(projectExprs, ctx);
	}
	const char *keyword = join.joinKind == JoinKind::LeftOuter ? "LEFT OUTER JOIN" : "INNER JOIN";
	sql += ctx.clauseSep() + "FROM (" + leftSql + ") AS " + leftAlias;
	sql += ctx.clauseSep() + keyword + " (" + rightSql + ") AS " + rightAlias;
	sql += ctx.onSep() + "ON ";
	if (onConditions.empty()) {
		sql += dialect.booleanLiteral(true);
	} else {
		sql += joinConditions(onConditions, ctx);
	}
	return sql;
}

std::string renderAntiJoin(const AntiJoinNode &anti, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string leftAlias = ctx.nextAlias();
	std::string rightAlias = ctx.nextAlias();
	ExtraProjections leftExtra;
	ExtraProjections rightExtra;
	std::vector<std::string> nativeConds;
	std::vector<bool> rewritten;
	planNativeKeys(anti.keys, *anti.left, *anti.right, ctx, leftAlias, rightAlias, leftExtra, rightExtra, nativeConds,
	               rewritten);

	std::string leftSql;
	std::string rightSql;
	{
		TranslationContext::SubqueryDepthGuard depthGuard(ctx);
		leftSql = renderChild(*anti.left, ctx, leftExtra);
		rightSql = renderChild(*anti.right, ctx, rightExtra);
	}

	std::vector<std::string> conds;
	for (std::size_t i = 0; i < anti.keys.size(); ++i) {
		if (rewritten[i]) {
			continue;
		}
		const EquiKey &k = anti.keys[i];
		conds.push_back(keyComparison(k, leftAlias + "." + mangleVar(k.var, dialect),
		                              rightAlias + "." + mangleVar(k.var, dialect)));
	}
	conds.insert(conds.end(), nativeConds.begin(), nativeConds.end());
	std::string cond = joinConditions(conds, ctx);

	// Project MINUS's schema (which is the left operand's) explicitly rather
	// than SELECT *: the left side may carry hidden native-key columns that must
	// not escape into an enclosing UNION BY NAME.
	std::vector<std::string> projectionCols;
	for (const auto &c : anti.schema()) {
		projectionCols.push_back(leftAlias + "." + mangleVar(c.var, dialect) + " AS " + mangleVar(c.var, dialect));
		if (ctx.needsTag(c.var)) {
			projectionCols.push_back(leftAlias + "." + mangleVarTag(c.var, dialect) + " AS " +
			                         mangleVarTag(c.var, dialect));
		}
	}
	std::string sql = "SELECT";
	sql += projectionCols.empty() ? joinColumnList({"1 AS " + dialect.quoteIdentifier("_dummy")}, ctx)
	                              : joinColumnList(projectionCols, ctx);
	sql += ctx.clauseSep() + "FROM (" + leftSql + ") AS " + leftAlias + " WHERE NOT EXISTS (SELECT 1 FROM (" + rightSql +
	       ") AS " + rightAlias + " WHERE " + cond + ")";
	return sql;
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
	std::string childSql;
	{
		TranslationContext::SubqueryDepthGuard depthGuard(ctx);
		childSql = renderNode(*f.child, ctx);
	}
	TranslatedPattern scope = scopeOf(*f.child);
	std::string cond = translateExpression(*f.predicate, scope, alias, ctx);
	return "SELECT *" + ctx.clauseSep() + "FROM (" + childSql + ") AS " + alias + ctx.clauseSep() + "WHERE " + cond;
}

std::string renderBind(const BindNode &b, TranslationContext &ctx) {
	std::string alias = ctx.nextAlias();
	std::string childSql;
	{
		TranslationContext::SubqueryDepthGuard depthGuard(ctx);
		childSql = renderNode(*b.child, ctx);
	}
	TranslatedPattern scope = scopeOf(*b.child);
	TermSql bound = translateTerm(*b.expr, scope, alias, ctx);
	std::string extra;
	if (ctx.needsTag(b.outVar)) {
		if (bound.tag.empty()) {
			throw TranslationError("unsupported: the RDF term dimension of the BIND-ed variable ?" + b.outVar +
			                       " is needed at run time, but neither the mapping nor this translator can derive it "
			                       "from the defining expression.");
		}
		extra = ", (" + bound.tag + ") AS " + mangleVarTag(b.outVar, ctx.dialect());
	}
	return "SELECT *, (" + bound.value + ") AS " + mangleVar(b.outVar, ctx.dialect()) + extra + ctx.clauseSep() +
	       "FROM (" + childSql + ") AS " + alias;
}

// E+ closure rendering. Registers two CTEs on `ctx` (the one-hop step
// relation, then the recursive closure over it - never inlining step's SQL
// twice, since both the seed and the recursive term reference it) and
// returns a bare "SELECT ... FROM <closureCte> ..." reference, analogous to
// how RawRelation returns its pre-rendered sql verbatim.
//
// Two shapes, chosen by tc.mode:
//  - BothVars: a full (from, to) pairs closure - the seed is every one-hop
//    edge, the recursive term extends a known pair by one more hop. This is
//    the only shape that needs both endpoint columns; the other three anchor
//    at a bound literal and only need a unary "reachable node" column, which
//    is far cheaper.
//  - ForwardFromSubject/BackwardFromObject/BothBound: seed from the bound
//    endpoint's literal and walk the step relation forward or backward one
//    hop at a time. BothBound projects nothing and tests membership via
//    EXISTS instead, mirroring zeroLengthPath's both-bound-unequal Empty
//    shape (0 columns, existence only).
//
// Every recursive term is combined with its seed via a deduplicating UNION
// (never UNION ALL): this is what makes the closure terminate on cyclic edge
// data, which real graphs frequently have.
std::string renderTransitiveClosure(const TransitiveClosureNode &tc, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string fromCol = mangleVar(tc.fromVar, dialect);
	std::string toCol = mangleVar(tc.toVar, dialect);

	std::string stepCte = ctx.nextCteName();
	ctx.addCte(stepCte, renderNode(*tc.step, ctx));
	std::string closureCte = ctx.nextCteName();

	if (tc.mode == TransitiveClosureNode::Mode::BothVars) {
		std::string cteFrom = dialect.quoteIdentifier("cte_from");
		std::string cteTo = dialect.quoteIdentifier("cte_to");
		std::string s1 = ctx.nextAlias();
		std::string s2 = ctx.nextAlias();
		std::string r = ctx.nextAlias();
		std::string seed = "SELECT " + s1 + "." + fromCol + " AS " + cteFrom + ", " + s1 + "." + toCol + " AS " +
		                   cteTo + " FROM " + stepCte + " AS " + s1;
		std::string recursive = "SELECT " + r + "." + cteFrom + ", " + s2 + "." + toCol + " FROM " + closureCte +
		                        " AS " + r + " JOIN " + stepCte + " AS " + s2 + " ON " + r + "." + cteTo + " = " + s2 +
		                        "." + fromCol;
		ctx.addCte(closureCte, seed + " UNION " + recursive);

		std::string c = ctx.nextAlias();
		if (tc.schema().size() == 1) {
			// Subject and object share one variable (`?x p+ ?x`): only the
			// diagonal of the pairs closure satisfies the pattern.
			return "SELECT " + c + "." + cteFrom + " AS " + mangleVar(tc.schema()[0].var, dialect) +
			       closureTagProjection(tc.schema()[0], ctx) + " FROM " + closureCte + " AS " + c + " WHERE " + c +
			       "." + cteFrom + " = " + c + "." + cteTo;
		}
		return "SELECT " + c + "." + cteFrom + " AS " + mangleVar(tc.schema()[0].var, dialect) +
		       closureTagProjection(tc.schema()[0], ctx) + ", " + c + "." + cteTo + " AS " +
		       mangleVar(tc.schema()[1].var, dialect) + closureTagProjection(tc.schema()[1], ctx) + " FROM " +
		       closureCte + " AS " + c;
	}

	// Unary reachable-set: ForwardFromSubject/BackwardFromObject walk the
	// step relation in the direction that starts at the bound endpoint;
	// BothBound reuses the forward walk (seeded from the subject) and only
	// tests membership.
	std::string cteNode = dialect.quoteIdentifier("cte_node");
	bool forward = tc.mode != TransitiveClosureNode::Mode::BackwardFromObject;
	const std::string &walkCol = forward ? fromCol : toCol;
	const std::string &landCol = forward ? toCol : fromCol;

	std::string seedAlias = ctx.nextAlias();
	std::string s = ctx.nextAlias();
	std::string r = ctx.nextAlias();
	// One hop from the anchor, not the anchor itself: this node's minimum
	// cardinality is always 1 (see class comment), so seeding with the anchor
	// verbatim would wrongly include it as a zero-hop "reachable" result.
	std::string seed = "SELECT " + seedAlias + "." + landCol + " AS " + cteNode + " FROM " + stepCte + " AS " +
	                   seedAlias + " WHERE " + seedAlias + "." + walkCol + " = " + tc.anchorLiteral;
	std::string recursive = "SELECT " + s + "." + landCol + " FROM " + closureCte + " AS " + r + " JOIN " + stepCte +
	                        " AS " + s + " ON " + r + "." + cteNode + " = " + s + "." + walkCol;
	ctx.addCte(closureCte, seed + " UNION " + recursive);

	if (tc.mode == TransitiveClosureNode::Mode::BothBound) {
		return "SELECT 1 AS " + dialect.quoteIdentifier("_dummy") + " WHERE " +
		       dialect.existsClause(false,
		                            "SELECT 1 FROM " + closureCte + " WHERE " + cteNode + " = " + tc.targetLiteral);
	}

	std::string c = ctx.nextAlias();
	return "SELECT " + c + "." + cteNode + " AS " + mangleVar(tc.schema()[0].var, dialect) +
	       closureTagProjection(tc.schema()[0], ctx) + " FROM " + closureCte + " AS " + c;
}

// A Raw leaf's SQL was built during folding, before any tag was demanded, so it
// only carries the tag columns its producer could not have reconstructed later
// (providedTagVars). Every other demanded tag is a constant, so it is added here
// by wrapping - which costs nothing when nothing is demanded, the overwhelmingly
// common case, where this returns the pre-rendered SQL verbatim as it always
// did.
std::string renderRaw(const RawRelation &raw, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string added;
	for (const auto &c : raw.schema()) {
		if (!ctx.needsTag(c.var) || raw.providedTagVars.count(c.var) != 0) {
			continue;
		}
		added += ", " + producedTag(c, "this inline-data or subquery relation") + " AS " + mangleVarTag(c.var, dialect);
	}
	if (added.empty()) {
		return raw.sql;
	}
	return "SELECT *" + added + ctx.clauseSep() + "FROM (" + raw.sql + ") AS " + ctx.nextAlias();
}

std::string renderEmpty(const EmptyNode &e, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	if (e.schema().empty()) {
		return "SELECT" + joinColumnList({"1 AS " + dialect.quoteIdentifier("_dummy")}, ctx) + ctx.clauseSep() +
		       "WHERE FALSE";
	}
	std::vector<std::string> cols;
	for (const auto &c : e.schema()) {
		cols.push_back("CAST(NULL AS VARCHAR) AS " + mangleVar(c.var, dialect));
		if (ctx.needsTag(c.var)) {
			// No rows, so no term: a NULL tag beside the NULL value is both the
			// only honest answer and the one the invariant requires.
			cols.push_back("CAST(NULL AS VARCHAR) AS " + mangleVarTag(c.var, dialect));
		}
	}
	return "SELECT" + joinColumnList(cols, ctx) + ctx.clauseSep() + "WHERE FALSE";
}

std::string renderNode(const RelNode &node, TranslationContext &ctx) {
	switch (node.kind()) {
	case RelKind::Spj:
		return renderSpj(static_cast<const SpjRelation &>(node), ctx, nullptr);
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
		return renderRaw(static_cast<const RawRelation &>(node), ctx);
	case RelKind::SingleRow:
		return "SELECT 1 AS " + ctx.dialect().quoteIdentifier("_dummy");
	case RelKind::Empty:
		return renderEmpty(static_cast<const EmptyNode &>(node), ctx);
	case RelKind::TransitiveClosure:
		return renderTransitiveClosure(static_cast<const TransitiveClosureNode &>(node), ctx);
	}
	return std::string();
}

} // namespace

void fillScopeFromSchema(TranslatedPattern &out, const std::vector<ColumnInfo> &schema) {
	for (const auto &c : schema) {
		if (c.nonNull) {
			out.boundVars.insert(c.var);
		} else {
			out.optionalVars.insert(c.var);
		}
		if (c.term.kindKnown()) {
			// Only record what is actually known: an absent entry already reads
			// back as Unknown, so storing one saves nothing and invites a
			// consumer to distinguish "absent" from "present but Unknown".
			out.termInfo[c.var] = c.term;
		}
	}
}

TranslatedPattern renderRelation(const RelNode &node, TranslationContext &ctx) {
	TranslatedPattern tp;
	tp.sql = renderNode(node, ctx);
	fillScopeFromSchema(tp, node.schema());
	tp.isIdentity = (node.kind() == RelKind::SingleRow);
	return tp;
}

} // namespace sparql2sql
