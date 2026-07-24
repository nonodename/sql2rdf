#pragma once

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

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
///    columns with the *identical* template is equivalent to a join on the
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
	std::string tableIdentity;   ///< logical-table identity of the source (PureColumn/TemplateExpr).
	std::string templateString;  ///< original rr:template (TemplateExpr) - for same-template join detection.

	bool nonNull = true; ///< guaranteed non-NULL in every row (bound) vs may be NULL (optional).
};

enum class RelKind {
	Spj,         ///< fused Select-Project-Join block (base of flattening/self-join).
	Join,        ///< binary Inner/LeftOuter join of two arbitrary relations.
	AntiJoin,    ///< MINUS (NOT EXISTS).
	UnionByName, ///< schema-extending union of >=1 relations.
	Values,      ///< inline VALUES data.
	Project,     ///< projection/compute over a (composite) child (final SELECT, BIND, subselect).
	Aggregate,   ///< GROUP BY / aggregate.
	Limit,       ///< LIMIT/OFFSET.
	SingleRow,   ///< the fold's identity relation (SELECT 1).
	Empty        ///< a provably empty relation (WHERE FALSE) with a declared schema.
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

/// One FROM source of an SpjRelation: a table/view/inline-join SQL fragment
/// already suffixed with its alias ("TABLE" AS t1 / (view) AS t1 / child JOIN
/// parent ON ...), plus the driving alias and a logical-table identity key
/// used by self-join elimination.
struct SpjSource {
	std::string sql;           ///< e.g. `"company" AS t1` or `(<view sql>) AS t1`.
	std::string alias;         ///< the primary alias introduced by this source.
	std::string tableIdentity; ///< table name, or a stable key for a view/join source.
};

/// The fused Select-Project-Join block. A single triple-pattern candidate is
/// one SpjSource + its whereConds + distinct=true; flattening merges several
/// inner-joined SpjRelations into one (all sources, all conds, join equalities
/// folded into whereConds, projections unioned by variable).
class SpjRelation : public RelNode {
public:
	SpjRelation() : RelNode(RelKind::Spj) {
	}

	std::vector<SpjSource> sources;         ///< >=1; joined as an inner-join spine.
	std::vector<std::string> whereConds;    ///< conjuncts: inversion eq, self-join eq, IS NOT NULL, join keys.
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
	std::vector<EquiKey> keys; ///< always null-tolerant, per SPARQL MINUS semantics.
};

class UnionByNameNode : public RelNode {
public:
	UnionByNameNode() : RelNode(RelKind::UnionByName) {
	}
	bool all = true; ///< false => also dedup (SQL UNION semantics).
	std::vector<RelNodePtr> arms;
};

class ValuesNode : public RelNode {
public:
	ValuesNode() : RelNode(RelKind::Values) {
	}
	/// Each arm is a finished "SELECT <lit> AS v_x, ..." row; combined by the
	/// renderer (single row => used directly; empty => WHERE FALSE relation).
	std::vector<std::string> rowSqls;
};

/// One output column of a Project: either a straight pass-through of a child
/// variable, or a computed expression (BIND / SELECT `(expr AS ?v)` / group
/// alias). Computed columns hold a borrowed AST expression rendered late
/// (deferred-expression design), so filter/bind pushdown is re-parenting.
struct ProjectItem {
	std::string outVar;                              ///< output SPARQL variable name.
	bool passthrough = true;                         ///< true => copy child column `sourceVar`.
	std::string sourceVar;                           ///< child var to copy (passthrough).
	const sparql::ast::Expression *expr = nullptr;   ///< computed expression (!passthrough).
	std::string groupAliasName;                      ///< non-empty => a bare GROUP BY alias reference.
	bool nonNull = true;
};

class ProjectNode : public RelNode {
public:
	ProjectNode() : RelNode(RelKind::Project) {
	}
	RelNodePtr child;
	std::vector<ProjectItem> items;
	bool distinct = false;
};

class LimitNode : public RelNode {
public:
	LimitNode() : RelNode(RelKind::Limit) {
	}
	RelNodePtr child;
	bool hasLimit = false;
	long long limit = 0;
	bool hasOffset = false;
	long long offset = 0;
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

} // namespace sparql2sql
