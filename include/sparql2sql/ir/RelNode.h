#pragma once

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "sparql2sql/GraphConstraint.h"
#include "sparql2sql/TermInfo.h"

namespace sparql {
namespace ast {
class Expression;
} // namespace ast
} // namespace sparql

namespace sparql2sql {

/// How a projected column's value was produced. Read by the optimizer's
/// native-typed-join-key and self-join passes; irrelevant to correctness of
/// plain rendering (which always uses ColumnInfo::renderedExpr).
///  - PureColumn: a single, uncast base column (from a ColumnTermMap). The
///    only provenance for which a native (non-VARCHAR-cast) join key is
///    considered.
///  - TemplateExpr: an rr:template string concatenation. A join on two
///    columns whose templates have the *same shape* (TemplateUtil::
///    sameTemplateShape - identical literal segments/placeholder positions,
///    placeholder names may differ) is equivalent to a join on the
///    placeholder columns (subject to TemplateUtil's inversion assumptions).
///  - ConstantExpr: a constant term map / literal.
///  - Coalesced: COALESCE(l, r) from a null-tolerant (OPTIONAL) join.
///  - Computed: a BIND/SELECT expression or anything else.
enum class Provenance { PureColumn, TemplateExpr, ConstantExpr, Coalesced, Computed };

/// One output column of a relation: the SPARQL variable it binds, the SQL
/// scalar expression that produces it, and enough structured provenance for
/// the optimizer passes.
struct ColumnInfo {
	std::string var; ///< SPARQL variable name (unmangled).

	/// A finished SQL scalar expression producing this column's value,
	/// evaluated in the context of the owning node's FROM sources. ALWAYS
	/// populated, so any node can be rendered standalone.
	std::string renderedExpr;

	Provenance prov = Provenance::Computed;
	std::string sourceAlias;     ///< FROM-source alias owning the column (PureColumn/TemplateExpr).
	std::string columnName;      ///< raw column name (PureColumn) - for native-key rewrite + catalog lookup.
	std::string nativeColumnRef; ///< alias-qualified, dialect-quoted uncast column ref (PureColumn), e.g. t1."ID".
	std::string tableIdentity;   ///< logical-table identity of the source (PureColumn/TemplateExpr).
	std::string templateString;  ///< original rr:template (TemplateExpr) - for same-template join detection.

	/// TemplateExpr only: the template's placeholder columns, in
	/// referencedColumns() order - raw names (for TypeCatalog lookup) and
	/// alias-qualified uncast refs (for the rewritten comparison).
	std::vector<std::string> templateColumnNames;
	std::vector<std::string> templateColumnRefs;

	/// TemplateExpr only: true when no two placeholders are textually adjacent,
	/// so the constructed term text determines the placeholder values uniquely
	/// (invertTemplate's PerColumnMatch condition). Required before equality of
	/// two same-template terms may be rewritten to equality of their
	/// placeholder columns.
	bool templateInvertible = false;

	bool nonNull = true; ///< guaranteed non-NULL in every row (bound) vs may be NULL (optional).

	/// What the R2RML mapping statically says about the RDF term this column
	/// produces. Purely descriptive - `renderedExpr` is unaffected by it - so a
	/// default (Unknown) annotation reproduces the translator's behaviour from
	/// before term tracking existed, byte for byte.
	///
	/// Written by termMapToSqlExpr at the producers, and combined by `meet` at
	/// every node fed by more than one input. Never hand-assemble the three
	/// fields elsewhere; go through meet/meetColumns so the lattice stays the
	/// single definition of how disagreement degrades.
	TermInfo term;

	/// A SQL scalar expression producing this column's runtime **type tag** (the
	/// encodeTag() encoding), evaluated in the same scope as `renderedExpr`.
	///
	/// Set once, at the producer, from that single term map's own
	/// fully-determined annotation - so it is always a constant string literal -
	/// and then deliberately **not** degraded by `meet`. Where two term maps feed
	/// one column of a single SPJ block their values are forced equal by a WHERE
	/// conjunct, so either contributor's tag describes the row; picking the first
	/// is honest, whereas degrading to "no tag" would throw away a fact the
	/// mapping does supply.
	///
	/// Empty on any column a *node* combines from children (a union's arms, a
	/// join's COALESCE): there the tag is projected from the child's own tag
	/// column rather than recomputed, exactly as the value column is.
	std::string tagExpr;
};

enum class RelKind {
	Spj,              ///< fused Select-Project-Join block (base of flattening/self-join).
	Join,             ///< binary Inner/LeftOuter join of two arbitrary relations.
	AntiJoin,         ///< MINUS (NOT EXISTS).
	UnionByName,      ///< schema-extending union of >=1 relations.
	Filter,           ///< a FILTER over a (composite) child: deferred predicate expression.
	Bind,             ///< a BIND: child columns plus one computed column.
	Raw,              ///< a pre-rendered SELECT string (VALUES, subselect) with a declared schema.
	SingleRow,        ///< the fold's identity relation (SELECT 1).
	Empty,            ///< a provably empty relation (WHERE FALSE) with a declared schema.
	TransitiveClosure ///< E+ (one-or-more property path): a WITH RECURSIVE closure of `step`.
};

class RelNode {
public:
	explicit RelNode(RelKind kind) : kind_(kind) {
	}
	virtual ~RelNode();

	RelKind kind() const {
		return kind_;
	}

	const std::vector<ColumnInfo> &schema() const {
		return schema_;
	}
	std::vector<ColumnInfo> &schema() {
		return schema_;
	}

	/// Variables guaranteed non-NULL (nonNull == true).
	std::set<std::string> boundVars() const;
	/// Variables that may be NULL (nonNull == false).
	std::set<std::string> optionalVars() const;
	/// Every variable in the schema.
	std::set<std::string> allVars() const;

	/// Locate a schema column by variable name; nullptr if absent.
	const ColumnInfo *column(const std::string &var) const;

protected:
	RelKind kind_;
	std::vector<ColumnInfo> schema_;
};

using RelNodePtr = std::unique_ptr<RelNode>;

/// Meet the term annotations of several inputs that feed one output column.
///
/// A **null** entry contributes nothing and is skipped rather than treated as
/// Unknown: a variable that an arm (or one side of an outer join) does not bind
/// is SQL NULL in those rows and denotes no RDF term at all, so it must not
/// poison the arms that do bind it. Without that rule every schema-extending
/// UNION would collapse to Unknown and the annotation would be worthless.
///
/// With no non-null contributions at all the result is Unknown.
TermInfo meetColumns(const std::vector<const ColumnInfo *> &sources);

/// meetColumns over one variable across a union node's arms - the common case,
/// factored out so the gather loop isn't repeated at each union site.
///
/// Call this while the arms are still owned by the caller: after
/// `node.arms = std::move(branches)` the moved-from pointers are null.
TermInfo meetAcrossArms(const std::string &var, const std::vector<RelNodePtr> &arms);

/// One FROM source of an SpjRelation: a table/view/inline-join SQL fragment
/// already suffixed with its alias ("TABLE" AS t1 / (view) AS t1 / child JOIN
/// parent ON ...), plus the driving alias and a logical-table identity key
/// used by self-join elimination.
struct SpjSource {
	std::string sql;           ///< e.g. `"company" AS t1` or `(<view sql>) AS t1`.
	std::string alias;         ///< the primary alias introduced by this source.
	std::string tableIdentity; ///< table name, or a stable key for a view/join source.

	/// Self-join-elimination metadata. `subjectVar` is the SPARQL variable
	/// bound to this source's subject position; `subjectKeySig` is an
	/// alias-independent signature of its subject term map (e.g.
	/// "tmpl:<template>" or "col:<column>"). Two sources with the same
	/// tableIdentity, the same non-empty subjectVar, and the same non-empty
	/// subjectKeySig denote the same table row and may be merged.
	///
	/// For a referencing-object-map source (`sql` is "child JOIN parent ON
	/// ..."), `subjectKeySig` additionally encodes the parent table identity
	/// and join columns (see TriplePatternTranslator.cpp), so two such
	/// sources only compare equal when their *entire* FROM clause - child
	/// table, parent table, and join condition - is identical; a plain
	/// single-table source and a referencing-object-map source over the same
	/// child table always compare unequal, since only one of them carries
	/// that suffix.
	std::string subjectVar;
	std::string subjectKeySig;

	/// The second alias a referencing-object-map source introduces (the
	/// parent side of "child JOIN parent"), so self-join elimination can
	/// rewrite references to it too when two such sources merge. Empty for a
	/// plain single-table source.
	std::string parentAlias;
};

/// The fused Select-Project-Join block. A single triple-pattern candidate is
/// one SpjSource + its whereConds + distinct=true; flattening merges several
/// inner-joined SpjRelations into one (all sources, all conds, join equalities
/// folded into whereConds, projections unioned by variable).
class SpjRelation : public RelNode {
public:
	SpjRelation() : RelNode(RelKind::Spj) {
	}

	std::vector<SpjSource> sources;      ///< >=1; joined as an inner-join spine.
	std::vector<std::string> whereConds; ///< conjuncts: inversion eq, self-join eq, IS NOT NULL, join keys.
	bool distinct = false;
	// schema_ holds the projected columns (each renderedExpr evaluated over `sources`).
};

/// A structured equi-join key on a shared variable. Carries both sides'
/// ColumnInfo (with provenance) so the native-typed-join-key pass can rewrite
/// the comparison; `nullSafe` reproduces the OPTIONAL null-tolerant form.
struct EquiKey {
	std::string var;
	ColumnInfo leftCol;
	ColumnInfo rightCol;
	bool nullSafe = false; ///< (l = r OR l IS NULL OR r IS NULL) + COALESCE projection.
};

enum class JoinKind { Inner, LeftOuter };

class JoinNode : public RelNode {
public:
	JoinNode() : RelNode(RelKind::Join) {
	}
	JoinKind joinKind = JoinKind::Inner;
	RelNodePtr left;
	RelNodePtr right;
	std::vector<EquiKey> keys;
};

class AntiJoinNode : public RelNode {
public:
	AntiJoinNode() : RelNode(RelKind::AntiJoin) {
	}
	RelNodePtr left;
	RelNodePtr right;

	/// MINUS compatibility is null-tolerant in principle (an unbound variable
	/// is not in the solution's domain, so it doesn't constrain compatibility),
	/// but the tolerance is *vacuous* for a key whose columns are both
	/// guaranteed non-NULL. So `nullSafe` is set from the operands' actual
	/// optionality (as for a join), not blanket-true: an OR'd null-tolerant
	/// comparison is a non-equi predicate that stops the engine from
	/// decorrelating the anti-join into a hash semi-join.
	std::vector<EquiKey> keys;
};

class UnionByNameNode : public RelNode {
public:
	UnionByNameNode() : RelNode(RelKind::UnionByName) {
	}
	bool all = true; ///< false => also dedup (SQL UNION semantics).
	std::vector<RelNodePtr> arms;
};

/// A FILTER: passes through the child's rows that satisfy `predicate`. The
/// predicate is a borrowed AST expression rendered late against the child's
/// schema (deferred-expression design), so filter pushdown is re-parenting.
class FilterNode : public RelNode {
public:
	FilterNode() : RelNode(RelKind::Filter) {
	}
	RelNodePtr child;
	const sparql::ast::Expression *predicate = nullptr;

	/// The active graph in force where this filter was written.
	///
	/// `predicate` is a borrowed AST pointer translated lazily, at render time -
	/// long after fold() returned and any ActiveGraphGuard was destroyed. An
	/// EXISTS inside it folds a whole graph pattern of its own at that point, so
	/// without this the pattern would be matched against the default graph even
	/// when the FILTER sits inside a GRAPH block. Captured here at construction
	/// and reinstated by renderFilter. (A parameter threaded through fold()
	/// would not help: the deferred node would not carry it either.)
	GraphConstraint activeGraph;
};

/// A BIND: the child's columns plus one computed column `outVar`.
class BindNode : public RelNode {
public:
	BindNode() : RelNode(RelKind::Bind) {
	}
	RelNodePtr child;
	std::string outVar;
	const sparql::ast::Expression *expr = nullptr;

	/// The active graph in force where this BIND was written; see
	/// FilterNode::activeGraph, which this mirrors exactly (a BIND's defining
	/// expression may equally contain an EXISTS).
	GraphConstraint activeGraph;
};

/// A leaf holding a pre-rendered, self-contained "SELECT ..." string (used for
/// inline VALUES and for `{ SELECT ... }` subqueries, whose SQL is produced by
/// the query-level path). Its schema is declared explicitly.
class RawRelation : public RelNode {
public:
	RawRelation() : RelNode(RelKind::Raw) {
	}
	std::string sql;

	/// Variables whose runtime type-tag column `sql` already projects.
	///
	/// A Raw leaf's SQL is built during folding, before the tag-demand pass has
	/// run, so it cannot consult needsTag(). Its producers instead unconditionally
	/// emit a tag column for every variable whose static annotation is **not**
	/// fully determined - a VALUES column mixing an IRI with a literal, or a
	/// subquery projecting a variable its own arms disagree about - because those
	/// are the only tags that cannot be reconstructed later. Every other tag is a
	/// constant the renderer synthesises on demand by wrapping this relation.
	std::set<std::string> providedTagVars;
};

class SingleRowNode : public RelNode {
public:
	SingleRowNode() : RelNode(RelKind::SingleRow) {
	}
};

class EmptyNode : public RelNode {
public:
	EmptyNode() : RelNode(RelKind::Empty) {
	}
};

/// E+ (SPARQL 1.1 Section 9.3/18.1.7 rule closure): the transitive closure of
/// a one-hop "step" relation between two internal endpoint variables
/// (`fromVar`/`toVar`, both minted via TranslationContext::nextInternalVar()),
/// seeded and projected according to which of the pattern's *real*
/// subject/object endpoints are bound. `E*` is not represented here at all -
/// PropertyPathTranslator composes a TransitiveClosureNode with
/// zeroLengthPath via unionAll(dedup=true), exactly as ZeroOrOne composes the
/// child path with zeroLengthPath - so this node's minimum cardinality is
/// always 1 and there is no separate zero-or-more mode to encode.
///
/// `step` is a live child relation (typically the UnionByName-of-Spj-arms
/// shape translateAtomicPattern produces for the path's underlying
/// predicate), not pre-rendered SQL: this lets it participate in the outer
/// tree's flatten/selfJoinWalk before this node's own renderer ever sees it.
/// This node itself is an optimizer boundary - never merged/rewritten - but
/// `step` still optimizes normally, exactly like FilterNode/BindNode's child.
class TransitiveClosureNode : public RelNode {
public:
	enum class Mode {
		ForwardFromSubject, ///< subject bound, object variable: unary reachable-set seeded forward.
		BackwardFromObject, ///< object bound, subject variable: unary reachable-set seeded backward.
		BothBound,          ///< both bound (and unequal): forward reachable-set + EXISTS membership test.
		BothVars            ///< both variable: full (from, to) pairs closure.
	};

	TransitiveClosureNode() : RelNode(RelKind::TransitiveClosure) {
	}

	RelNodePtr step;
	std::string fromVar; ///< step's internal "from" endpoint variable name.
	std::string toVar;   ///< step's internal "to" endpoint variable name.
	Mode mode = Mode::BothVars;

	/// The bound endpoint's already-dialect-quoted SQL string literal
	/// (dialect.stringLiteral(termLexicalForm(...))). Used by every mode
	/// except BothVars, where there is no anchor.
	std::string anchorLiteral;

	/// BothBound only: the *other* bound endpoint's literal, tested for
	/// closure membership via EXISTS rather than projected.
	std::string targetLiteral;

	/// BothVars only: subject and object are the *same* variable (`?x :p+ ?x`),
	/// so only the diagonal of the pairs closure satisfies the pattern.
	///
	/// Recorded explicitly rather than inferred from schema().size(), which the
	/// renderer used to do. That test was only ever correct because this node's
	/// projected width is fully determined by `mode` - so any future feature
	/// that adds a column to a closure (a carried invariant, say) would silently
	/// turn `?x :p+ ?x` into the two-endpoint form and return wrong rows rather
	/// than fail. The producer already knows the answer; ask it.
	bool sameEndpointVar = false;

	/// Variables the step relation binds that must be held **constant across
	/// every hop** of the closure, rather than walked like the endpoints.
	///
	/// In practice this is 0 or 1 entries: the graph name of an enclosing
	/// `GRAPH ?g` block. A property path inside a GRAPH block is evaluated
	/// within that one graph, so a two-hop match may not cross from one graph
	/// into another - which is exactly a column carried through the recursion
	/// and equated between the accumulated row and the next step.
	///
	/// Each entry must also appear in schema() (appended after the endpoints, so
	/// the endpoint columns stay at indices 0/1), and is projected out of the
	/// closure like any other column.
	std::vector<std::string> invariantVars;

	// schema_ (inherited) declares exactly what this node projects:
	//  - BothBound:            0 columns.
	//  - ForwardFromSubject:   1 column, var = object's real SPARQL var name.
	//  - BackwardFromObject:   1 column, var = subject's real SPARQL var name.
	//  - BothVars, subject != object var: 2 columns, order [subjectVar, objectVar].
	//  - BothVars, subject == object var (`?x p+ ?x`): 1 column, that shared name.
};

} // namespace sparql2sql
