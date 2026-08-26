#include "sparql2sql/TriplePatternTranslator.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

#include "r2rml/BaseTableOrView.h"
#include "r2rml/ColumnTermMap.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/GraphMap.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/MapSQLRow.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLView.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/TemplateTermMap.h"
#include "r2rml/TermMap.h"
#include "r2rml/TriplesMap.h"
#include "sparql-parser/ast/Term.h"
#include "sparql2sql/GraphConstraint.h"
#include "sparql2sql/LogicalTableSource.h"
#include "sparql2sql/PropertyPathTranslator.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TagSql.h"
#include "sparql2sql/TemplateUtil.h"
#include "sparql2sql/TermMapSql.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

namespace {

const char *const kRdfTypeIri = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";

// One of the three positions' SQL source: either a fixed constant string, or
// a real R2RML TermMap evaluated against a given source-table alias (with the
// logical-table identity of that alias, for provenance).
struct TermSource {
	bool isConstant = false;
	std::string constantValue;               // valid iff isConstant
	const r2rml::TermMap *termMap = nullptr; // valid iff !isConstant
	std::string alias;                       // valid iff !isConstant
	std::string tableIdentity;               // valid iff !isConstant
};

// A resolved position: its SQL scalar expression plus the structured
// provenance the optimizer passes need.
struct Resolved {
	std::string expr;
	std::vector<std::string> requiredNonNull;
	Provenance prov = Provenance::Computed;
	std::string columnName;
	std::string templateString;
	std::string sourceAlias;
	std::string tableIdentity;
	TermInfo term;
};

Provenance provenanceOf(const r2rml::TermMap &termMap, std::string &columnName, std::string &templateString) {
	if (const auto *col = dynamic_cast<const r2rml::ColumnTermMap *>(&termMap)) {
		columnName = col->columnName;
		return Provenance::PureColumn;
	}
	if (const auto *tmpl = dynamic_cast<const r2rml::TemplateTermMap *>(&termMap)) {
		templateString = tmpl->templateString;
		return Provenance::TemplateExpr;
	}
	if (dynamic_cast<const r2rml::ConstantTermMap *>(&termMap)) {
		return Provenance::ConstantExpr;
	}
	return Provenance::Computed;
}

Resolved resolveSource(const TermSource &src, const SqlDialect &dialect, const TypeCatalog *catalog) {
	Resolved r;
	if (src.isConstant) {
		r.expr = dialect.stringLiteral(src.constantValue);
		r.prov = Provenance::ConstantExpr;
		// Every constant TermSource this file constructs is an IRI: either
		// rdf:type for a synthetic rr:class triple, or the rr:class IRI itself.
		r.term.kind = RdfTermKind::Iri;
		return r;
	}
	SqlExpr e = termMapToSqlExpr(*src.termMap, src.alias, dialect, catalog, src.tableIdentity);
	r.expr = e.expr;
	r.requiredNonNull = e.requiredNonNullColumns;
	r.prov = provenanceOf(*src.termMap, r.columnName, r.templateString);
	r.sourceAlias = src.alias;
	r.tableIdentity = src.tableIdentity;
	r.term = e.term;
	return r;
}

InversionResult resolveInversion(const TermSource &src, const sparql::ast::Term &boundTerm, const SqlDialect &dialect,
                                 const TypeCatalog *catalog) {
	if (src.isConstant) {
		InversionResult r;
		r.possible = (src.constantValue == termLexicalForm(boundTerm));
		return r;
	}
	return invertTermMapAgainstBoundTerm(*src.termMap, boundTerm, src.alias, dialect, catalog, src.tableIdentity);
}

// A base table is already a bare name, so it is referenced directly. An
// rr:sqlQuery view is instead hoisted into a top-level CTE and referenced by
// name: this function is reached once per use site, and a single view commonly
// backs many of them (every predicate-object map of its TriplesMap, and every
// arm of a variable-predicate expansion), so inlining the text here duplicated
// it across the whole statement and left the engine no way to see that the
// occurrences were one relation.
std::string logicalTableFromSql(const r2rml::LogicalTable &lt, const std::string &alias, TranslationContext &ctx) {
	if (const auto *base = dynamic_cast<const r2rml::BaseTableOrView *>(&lt)) {
		return ctx.dialect().quoteIdentifier(base->tableName) + " AS " + alias;
	}
	if (const auto *view = dynamic_cast<const r2rml::R2RMLView *>(&lt)) {
		return ctx.viewCteName(logicalTableIdentity(lt), stripTrailingSemicolon(view->sqlQuery)) + " AS " + alias;
	}
	throw std::logic_error("logicalTableFromSql: unrecognized LogicalTable subtype");
}

// Fill in a TemplateExpr column's placeholder metadata: the raw placeholder
// column names, their alias-qualified uncast refs, and whether the template is
// invertible (no two placeholders textually adjacent, so equal generated text
// implies equal placeholder values). Read by the native-typed-join-key rewrite,
// which turns an equality between two same-template terms into an equality
// between their placeholder columns.
void fillTemplateKeyInfo(ColumnInfo &col, const SqlDialect &dialect) {
	std::vector<TemplateSegment> segments = parseTemplate(col.templateString);
	col.templateColumnNames = referencedColumns(segments);
	if (col.templateColumnNames.empty()) {
		return;
	}
	for (const auto &name : col.templateColumnNames) {
		col.templateColumnRefs.push_back(col.sourceAlias + "." + dialect.quoteIdentifier(name));
	}
	bool adjacent = false;
	for (std::size_t i = 1; i < segments.size(); ++i) {
		if (segments[i].isPlaceholder && segments[i - 1].isPlaceholder) {
			adjacent = true;
			break;
		}
	}
	// A repeated placeholder ({A}/{A}) still splits unambiguously, but the
	// forward projection then constrains the column twice; pairwise equality of
	// the deduplicated columns remains exactly equivalent, so it needs no
	// special handling here.
	col.templateInvertible = !adjacent;
}

// Replace every occurrence of `from` with `to`. Local to this file; Optimizer.cpp
// has its own copy for the same reason (both are one-liners used to normalise
// alias-bearing SQL text, not a shared abstraction worth a header).
std::string replaceAllText(std::string text, const std::string &from, const std::string &to) {
	if (from.empty()) {
		return text;
	}
	std::size_t pos = 0;
	while ((pos = text.find(from, pos)) != std::string::npos) {
		text.replace(pos, from.size(), to);
		pos += to.size();
	}
	return text;
}

void addUnique(std::vector<std::string> &out, const std::vector<std::string> &more) {
	out.reserve(more.size());
	for (const auto &v : more) {
		if (std::find(out.begin(), out.end(), v) == out.end()) {
			out.push_back(v);
		}
	}
}

// Whether a predicate constraint could be satisfied by one specific IRI. Used
// to prune the synthetic rr:class (subject, rdf:type, classIRI) candidates,
// whose predicate is always exactly rdf:type.
bool predicateCouldMatchIri(const PredicateConstraint &predicateSpec, const std::string &iri) {
	switch (predicateSpec.kind) {
	case PredicateConstraint::ConstantIri:
		return predicateSpec.iri == iri;
	case PredicateConstraint::Variable:
		return true;
	case PredicateConstraint::NotIn:
		return std::find(predicateSpec.excludedIris.begin(), predicateSpec.excludedIris.end(), iri) ==
		       predicateSpec.excludedIris.end();
	}
	return true;
}

// Negate a conjunction of SQL conditions: NOT ((c1) AND (c2) ...). Used only
// for a negated property set, whose per-IRI exclusion is the complement of the
// inversion conditions that would have matched that IRI.
std::string negateConjunction(const std::vector<std::string> &conditions) {
	std::string joined;
	joined.reserve(64 + conditions.size() * 32); // rough guess to avoid too many reallocs
	for (std::size_t i = 0; i < conditions.size(); ++i) {
		if (i > 0) {
			joined += " AND ";
		}
		joined += "(" + conditions[i] + ")";
	}
	return "NOT (" + joined + ")";
}

// Apply a negated property set's exclusions to one candidate. Returns false if
// the candidate is entirely excluded (its predicate always equals one of the
// excluded IRIs); otherwise appends whatever conditions are needed to rule the
// excluded IRIs out.
//
// Complements invertTermMapAgainstBoundTerm's three outcomes:
//   possible == false                   -> this candidate can never carry that
//                                          predicate, so the exclusion is
//                                          vacuous and needs no condition;
//   possible, no where conditions       -> it always carries it: excluded;
//   possible, with where conditions     -> it carries it exactly when those
//                                          hold, so negate them.
bool applyPredicateExclusions(const TermSource &predicateSrc, const std::vector<std::string> &excludedIris,
                              const SqlDialect &dialect, const TypeCatalog *catalog,
                              std::vector<std::string> &whereConditions) {
	for (const auto &iri : excludedIris) {
		sparql::ast::Iri excludedTerm(iri, iri);
		InversionResult inv = resolveInversion(predicateSrc, excludedTerm, dialect, catalog);
		if (!inv.possible) {
			continue;
		}
		if (inv.whereConditions.empty()) {
			return false;
		}
		addUnique(whereConditions, {negateConjunction(inv.whereConditions)});
	}
	return true;
}

// --- R2RML Section 12 graph sets ------------------------------------------
//
// One graph a candidate's triples can land in. A candidate with N applicable
// graph maps produces N of these (plus possibly a default-graph one), and
// translateAtomicPattern emits one SpjRelation per (candidate, branch): a row
// genuinely does produce the same triple in every one of its graphs, and under
// `GRAPH ?g` each is a distinct solution with a distinct ?g value, which a
// single branch with a disjunction could not express.
struct GraphBranch {
	bool isDefaultGraph = false;
	TermSource graphSrc;                 ///< valid iff !isDefaultGraph
	std::vector<std::string> extraConds; ///< guards; see graphBranchesFor
};

// A default-graph branch. Spelled as a named helper rather than a bare
// `GraphBranch()` because the flag's default is false: a value-initialised
// instance is a *named* branch with no term map, which resolves to a null
// dereference rather than to anything meaningful.
GraphBranch defaultGraphBranch() {
	GraphBranch b;
	b.isDefaultGraph = true;
	return b;
}

// The rr:defaultGraph IRI, as a SQL string literal comparison target. A graph
// map that *dynamically* produces this IRI denotes the default graph, not a
// named graph called "...#defaultGraph", so it needs a runtime test rather than
// the static classification a constant term map gets.
const char *const kDefaultGraphIri = "http://www.w3.org/ns/r2rml#defaultGraph";

// Whether a graph map is a constant term map naming exactly rr:defaultGraph.
bool isStaticDefaultGraph(const r2rml::GraphMap &gm) {
	const auto *constant = dynamic_cast<const r2rml::ConstantTermMap *>(gm.valueTermMap());
	if (constant == nullptr) {
		return false;
	}
	SerdEnv *env = serd_env_new(nullptr);
	r2rml::MapSQLRow empty;
	SerdNode node = constant->generateRDFTerm(empty, *env);
	bool isDefault = node.type == SERD_URI &&
	                 std::string(reinterpret_cast<const char *>(node.buf), node.n_bytes) == kDefaultGraphIri;
	serd_env_free(env);
	return isDefault;
}

// Whether a graph map's value is fixed at translation time (rr:constant), as
// opposed to derived per row (rr:column/rr:template).
bool isStaticGraph(const r2rml::GraphMap &gm) {
	return dynamic_cast<const r2rml::ConstantTermMap *>(gm.valueTermMap()) != nullptr;
}

// Decompose a candidate's applicable graph set - per R2RML Section 12, the
// union of its subject map's and its predicate-object map's graph maps - into
// the branches translateAtomicPattern should emit, then filter that list by the
// active graph constraint.
//
// An **empty** set yields exactly one default-graph branch with no conditions
// and no projected graph column, which is the code path a mapping that never
// mentions rr:graph takes. That is what keeps this whole feature SQL-neutral
// for such mappings.
//
// rr:defaultGraph is a *member* of the set, not a suppressor (matching
// r2rml::forEachGraphNode), so a set of {rr:defaultGraph, ex:g1} yields both a
// named branch for ex:g1 and a default branch.
std::vector<GraphBranch> graphBranchesFor(const std::vector<std::unique_ptr<r2rml::GraphMap>> &subjectGraphMaps,
                                          const std::vector<std::unique_ptr<r2rml::GraphMap>> &pomGraphMaps,
                                          const std::string &alias, const std::string &tableIdentity,
                                          TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	const GraphConstraint &active = ctx.activeGraph();

	// Gather the set, skipping null entries and any graph map whose value
	// strategy the parser never supplied (a test double, per GraphMap's docs).
	std::vector<const r2rml::GraphMap *> set;
	for (const auto *list : {&subjectGraphMaps, &pomGraphMaps}) {
		for (const auto &gm : *list) {
			if (gm && gm->valueTermMap() != nullptr) {
				set.push_back(gm.get());
			}
		}
	}

	std::vector<GraphBranch> branches;

	// --- resolve the active constraint against the dataset ---------------
	// Yields at most one of: the mapping's own default-graph branch, or named
	// branches restricted to `requiredIris` (empty meaning "any named graph").
	//
	// Two SPARQL 1.1 Section 13.2 rules do the surprising work here: FROM
	// *replaces* the default graph rather than adding to it (so with FROM <g>
	// the mapping's ungraphed triples become invisible and the default branch is
	// dropped), and naming only FROM NAMED graphs leaves the default graph empty
	// (so a pattern outside any GRAPH block matches nothing at all).
	const ActiveDataset &ds = ctx.dataset();
	bool wantDefaultBranch = false;
	bool wantNamedBranches = false;
	std::vector<std::string> requiredIris;
	if (active.isDefault()) {
		if (!ds.restricted) {
			wantDefaultBranch = true;
		} else if (!ds.defaultGraphIris.empty()) {
			// FROM merged these into the default graph, so a graph-less pattern
			// matches exactly the triples in them - as named branches, but with no
			// graph column, since there is no graph variable to bind.
			wantNamedBranches = true;
			requiredIris = ds.defaultGraphIris;
		}
		// else: FROM NAMED only -> the default graph is empty -> no branches.
	} else if (active.kind == GraphConstraint::Kind::BoundIri) {
		if (!ds.restricted || ds.namesNamedGraph(active.iri)) {
			wantNamedBranches = true;
			requiredIris.push_back(active.iri);
		}
		// else: not a nameable graph in this dataset -> no branches.
	} else { // Variable
		if (!ds.restricted) {
			wantNamedBranches = true; // any named graph; requiredIris stays empty
		} else if (!ds.namedGraphIris.empty()) {
			wantNamedBranches = true;
			requiredIris = ds.namedGraphIris;
		}
	}
	if (!wantDefaultBranch && !wantNamedBranches) {
		return branches;
	}

	// --- the default-graph branch ---------------------------------------
	// Only reachable without a GRAPH block and without a FROM clause: inside a
	// GRAPH block the default graph is not a named graph, and FROM replaces it.
	if (wantDefaultBranch) {
		if (set.empty()) {
			branches.push_back(defaultGraphBranch()); // no conditions, no graph column
			return branches;
		}
		GraphBranch def = defaultGraphBranch();
		// A candidate reaches the default graph iff some entry denotes
		// rr:defaultGraph, OR every entry resolves to NULL (an all-NULL set is
		// indistinguishable from an empty one under R2RML's set formulation, and
		// forEachGraphNode reads it the same way).
		bool alwaysApplies = false;
		bool allNullPossible = true; // a constant named graph rules it out
		std::vector<std::string> disjuncts;
		std::vector<std::string> nullConj;
		sparql::ast::Iri defaultGraphTerm(kDefaultGraphIri, kDefaultGraphIri);
		for (const auto *gm : set) {
			TermSource src;
			src.termMap = gm->valueTermMap();
			src.alias = alias;
			src.tableIdentity = tableIdentity;

			// Ask the same inversion machinery the named branches use, rather
			// than emitting a raw string comparison: it can *statically* rule out
			// a template like "http://ex.org/g/{ID}" ever denoting
			// rr:defaultGraph, which prunes this branch instead of leaving the
			// engine an always-false predicate to discover.
			InversionResult inv = resolveInversion(src, defaultGraphTerm, dialect, ctx.catalog());
			if (inv.possible && inv.whereConditions.empty()) {
				alwaysApplies = true; // statically rr:defaultGraph
				break;
			}
			if (inv.possible) {
				std::string conj;
				for (std::size_t i = 0; i < inv.whereConditions.size(); ++i) {
					conj += (i > 0 ? " AND " : "") + inv.whereConditions[i];
				}
				disjuncts.push_back("(" + conj + ")");
			}

			if (isStaticGraph(*gm)) {
				allNullPossible = false; // a constant is never NULL
				continue;
			}
			Resolved r = resolveSource(src, dialect, ctx.catalog());
			nullConj.push_back(r.expr + " IS NULL");
		}
		if (alwaysApplies) {
			branches.push_back(def);
			return branches;
		}
		if (allNullPossible && !nullConj.empty()) {
			std::string conj;
			for (std::size_t i = 0; i < nullConj.size(); ++i) {
				conj += (i > 0 ? " AND " : "") + nullConj[i];
			}
			disjuncts.push_back("(" + conj + ")");
		}
		if (disjuncts.empty()) {
			return branches; // provably never in the default graph: branch pruned
		}
		std::string ors;
		for (std::size_t i = 0; i < disjuncts.size(); ++i) {
			ors += (i > 0 ? " OR " : "") + disjuncts[i];
		}
		def.extraConds.push_back("(" + ors + ")");
		branches.push_back(def);
		return branches;
	}

	// --- named-graph branches -------------------------------------------
	for (const auto *gm : set) {
		if (isStaticDefaultGraph(*gm)) {
			continue; // denotes the default graph, never a named one
		}
		GraphBranch b;
		b.graphSrc.termMap = gm->valueTermMap();
		b.graphSrc.alias = alias;
		b.graphSrc.tableIdentity = tableIdentity;

		if (!requiredIris.empty()) {
			// This graph map must be able to produce one of the wanted IRIs.
			// Several is the normal case for a FROM/FROM NAMED list, so the
			// per-IRI inversion conditions are OR'd; an IRI a constant graph map
			// matches outright contributes no condition at all, which then makes
			// the whole branch unconditional.
			bool anyPossible = false;
			bool unconditional = false;
			std::vector<std::string> perIri;
			for (const auto &wantedIri : requiredIris) {
				sparql::ast::Iri wanted(wantedIri, wantedIri);
				InversionResult inv = resolveInversion(b.graphSrc, wanted, dialect, ctx.catalog());
				if (!inv.possible) {
					continue;
				}
				anyPossible = true;
				if (inv.whereConditions.empty()) {
					unconditional = true;
					break;
				}
				std::string conj;
				for (std::size_t i = 0; i < inv.whereConditions.size(); ++i) {
					conj += (i > 0 ? " AND " : "") + inv.whereConditions[i];
				}
				perIri.push_back("(" + conj + ")");
			}
			if (!anyPossible) {
				continue; // this graph map can never produce any wanted IRI
			}
			if (!unconditional) {
				std::string ors;
				for (std::size_t i = 0; i < perIri.size(); ++i) {
					ors += (i > 0 ? " OR " : "") + perIri[i];
				}
				b.extraConds.push_back("(" + ors + ")");
			}
			// No rr:defaultGraph guard needed: the wanted list is made of real
			// named graphs, so matching it already excludes the default graph.
		} else if (!isStaticGraph(*gm)) {
			// GRAPH ?g over a per-row graph name: a row whose graph column
			// happens to hold rr:defaultGraph is in the default graph, so it is
			// not a named-graph solution and must be excluded here.
			Resolved r = resolveSource(b.graphSrc, dialect, ctx.catalog());
			b.extraConds.push_back(r.expr + " <> " + dialect.stringLiteral(kDefaultGraphIri));
		}
		branches.push_back(b);
	}
	return branches;
}

// Copy a resolved position's structured provenance onto a projected column.
void fillColumnFromResolved(ColumnInfo &col, const Resolved &r, const SqlDialect &dialect) {
	col.renderedExpr = r.expr;
	col.prov = r.prov;
	col.sourceAlias = r.sourceAlias;
	col.columnName = r.columnName;
	col.tableIdentity = r.tableIdentity;
	col.templateString = r.templateString;
	if (r.prov == Provenance::PureColumn && !r.columnName.empty()) {
		col.nativeColumnRef = r.sourceAlias + "." + dialect.quoteIdentifier(r.columnName);
	} else if (r.prov == Provenance::TemplateExpr && !r.templateString.empty() && !r.sourceAlias.empty()) {
		fillTemplateKeyInfo(col, dialect);
	}
	col.nonNull = true;
	col.term = r.term;
	col.tagExpr = tagLiteral(r.term, dialect);
}

// One arm of the term universe: a single-source SpjRelation projecting one
// term map's generated term under every requested variable name.
void addTermArm(std::vector<RelNodePtr> &arms, const std::string &fromSql, const std::string &alias,
                const std::string &tableIdentity, const r2rml::TermMap &termMap,
                const std::vector<std::string> &varNames, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	TermSource src;
	src.termMap = &termMap;
	src.alias = alias;
	src.tableIdentity = tableIdentity;
	Resolved r = resolveSource(src, dialect, ctx.catalog());

	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	SpjSource source;
	source.sql = fromSql;
	source.alias = alias;
	source.tableIdentity = tableIdentity;
	spj.sources.push_back(source);
	for (const auto &guard : r.requiredNonNull) {
		spj.whereConds.push_back(guard + " IS NOT NULL");
	}
	spj.distinct = true;
	for (const auto &v : varNames) {
		ColumnInfo col;
		col.var = v;
		fillColumnFromResolved(col, r, dialect);
		spj.schema().push_back(col);
	}
	arms.push_back(std::move(node));
}

// One arm of the term universe for a fixed IRI (an rr:class, which is the
// object of an rdf:type triple and so a node of the graph). Needs no FROM.
void addConstantTermArm(std::vector<RelNodePtr> &arms, const std::string &value,
                        const std::vector<std::string> &varNames, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	const std::string literal = dialect.stringLiteral(value);

	RelNodePtr node(new RawRelation());
	RawRelation &raw = static_cast<RawRelation &>(*node);
	std::string sql;
	sql.reserve(64 + varNames.size() * 32); // rough guess to avoid too many reallocs
	sql = "SELECT ";
	for (std::size_t i = 0; i < varNames.size(); ++i) {
		if (i > 0) {
			sql += ", ";
		}
		sql += literal + " AS " + mangleVar(varNames[i], dialect);
	}
	raw.sql = sql;
	for (const auto &v : varNames) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = true;
		// This arm only ever carries an rr:class IRI (see the caller).
		col.term.kind = RdfTermKind::Iri;
		col.tagExpr = tagLiteral(col.term, dialect);
		raw.schema().push_back(col);
	}
	arms.push_back(std::move(node));
}

// Attempt to build one candidate branch's SpjRelation. Appends to `branches`
// on success; silently does nothing if the candidate is statically prunable
// (a bound position can never match this candidate's source).
//
// `parentAlias`/`parentJoinSig` are only ever non-empty for a
// referencing-object-map candidate (`fromSql` is "child JOIN parent ON
// ..."): `parentAlias` is the join's second alias, and `parentJoinSig` is an
// alias-independent signature of the parent table + join condition, folded
// into the emitted subjectKeySig so self-join elimination only merges two
// such sources when their entire FROM clause - not just the child table -
// is identical (see SpjSource::subjectKeySig).
void tryAddCandidate(std::vector<RelNodePtr> &branches, const std::string &fromSql, const std::string &fromAlias,
                     const std::string &fromIdentity, const TermSource &subjectSrc, const TermSource &predicateSrc,
                     const TermSource &objectSrc, const GraphBranch &graphBranch, const TermSpec &subjectSpec,
                     const PredicateConstraint &predicateSpec, const TermSpec &objectSpec, bool mergeableSubject,
                     TranslationContext &ctx, const std::string &parentAlias = std::string(),
                     const std::string &parentJoinSig = std::string()) {
	const SqlDialect &dialect = ctx.dialect();

	std::vector<std::string> whereConditions;
	std::vector<std::string> requiredNonNull;

	// The branch's own guards (graph-IRI inversion, or the default-graph
	// disjunction) apply regardless of the three term positions.
	addUnique(whereConditions, graphBranch.extraConds);

	Resolved subjectR = resolveSource(subjectSrc, dialect, ctx.catalog());
	addUnique(requiredNonNull, subjectR.requiredNonNull);
	if (!subjectSpec.isVar) {
		InversionResult inv = resolveInversion(subjectSrc, *subjectSpec.boundTerm, dialect, ctx.catalog());
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	}

	Resolved predicateR = resolveSource(predicateSrc, dialect, ctx.catalog());
	addUnique(requiredNonNull, predicateR.requiredNonNull);
	if (predicateSpec.kind == PredicateConstraint::ConstantIri) {
		sparql::ast::Iri predicateBoundTerm(predicateSpec.iri, predicateSpec.iri);
		InversionResult inv = resolveInversion(predicateSrc, predicateBoundTerm, dialect, ctx.catalog());
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	} else if (predicateSpec.kind == PredicateConstraint::NotIn) {
		if (!applyPredicateExclusions(predicateSrc, predicateSpec.excludedIris, dialect, ctx.catalog(),
		                              whereConditions)) {
			return;
		}
	}

	Resolved objectR = resolveSource(objectSrc, dialect, ctx.catalog());
	addUnique(requiredNonNull, objectR.requiredNonNull);
	if (!objectSpec.isVar) {
		InversionResult inv = resolveInversion(objectSrc, *objectSpec.boundTerm, dialect, ctx.catalog());
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	}

	// A named-graph branch resolves its graph term like any other position. Its
	// nullness guard joins requiredNonNull; the *default* branch is deliberately
	// exempt, because its graph nullness is inverted (a NULL graph column is one
	// of the ways a row lands in the default graph) and is already expressed in
	// graphBranch.extraConds.
	Resolved graphR;
	const bool graphIsVar = ctx.activeGraph().kind == GraphConstraint::Kind::Variable;
	if (!graphBranch.isDefaultGraph) {
		graphR = resolveSource(graphBranch.graphSrc, dialect, ctx.catalog());
		addUnique(requiredNonNull, graphR.requiredNonNull);
		// A graph name is always an IRI, whatever the term map declares: export
		// writes it in the quad's graph position regardless. Asserting that here
		// keeps requiredKindFor's union pruning honest on a malformed mapping.
		graphR.term.kind = RdfTermKind::Iri;
		graphR.term.datatypeIri.clear();
		graphR.term.lang.clear();
	}

	// Self-join guard: the same variable in more than one position must
	// resolve to equal source expressions, and is projected exactly once.
	struct PositionEntry {
		bool isVar;
		std::string varName;
		const Resolved *resolved;
	};
	const bool predicateIsVar = predicateSpec.kind == PredicateConstraint::Variable;
	// Graph LAST, so varOrder and projection order are untouched when there is
	// no graph variable, and so `GRAPH ?s { ?s :p ?o }` treats the subject as
	// the first occurrence (the graph then equates to it) rather than the
	// reverse.
	PositionEntry positions[4] = {
	    {subjectSpec.isVar, subjectSpec.varName, &subjectR},
	    {predicateIsVar, predicateSpec.varName, &predicateR},
	    {objectSpec.isVar, objectSpec.varName, &objectR},
	    {graphIsVar && !graphBranch.isDefaultGraph, ctx.activeGraph().varName, &graphR},
	};

	std::vector<ColumnInfo> projections; // first occurrence of each var wins
	for (int i = 0; i < 4; ++i) {
		if (!positions[i].isVar) {
			continue;
		}
		bool seen = false;
		for (int j = 0; j < i; ++j) {
			if (positions[j].isVar && positions[j].varName == positions[i].varName) {
				whereConditions.push_back(positions[j].resolved->expr + " = " + positions[i].resolved->expr);
				// The WHERE forces the two expressions equal, but the two term
				// maps may still *declare* different kinds or datatypes, so the
				// projected column can only claim what both agree on. (Position j
				// is the earliest occurrence, so it is already in `projections`.)
				for (auto &existing : projections) {
					if (existing.var == positions[i].varName) {
						existing.term = meet(existing.term, positions[i].resolved->term);
						break;
					}
				}
				seen = true;
				break;
			}
		}
		if (!seen) {
			ColumnInfo col;
			col.var = positions[i].varName;
			fillColumnFromResolved(col, *positions[i].resolved, dialect);
			projections.push_back(col);
		}
	}

	for (const auto &col : requiredNonNull) {
		whereConditions.push_back(col + " IS NOT NULL");
	}

	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	SpjSource source;
	source.sql = fromSql;
	source.alias = fromAlias;
	source.tableIdentity = fromIdentity;
	if (mergeableSubject && subjectSpec.isVar) {
		std::string sig;
		if (subjectR.prov == Provenance::TemplateExpr && !subjectR.templateString.empty()) {
			sig = "tmpl:" + subjectR.templateString;
		} else if (subjectR.prov == Provenance::PureColumn && !subjectR.columnName.empty()) {
			sig = "col:" + subjectR.columnName;
		}
		if (!sig.empty()) {
			if (!parentJoinSig.empty()) {
				sig += "\x1f" + parentJoinSig;
			}
			source.subjectVar = subjectSpec.varName;
			source.subjectKeySig = sig;
			source.parentAlias = parentAlias;
		}
	}
	spj.sources.push_back(source);
	spj.whereConds = whereConditions;
	spj.distinct = true;
	spj.schema() = projections;
	branches.push_back(std::move(node));
}

} // namespace

TermSpec termSpecFor(const sparql::ast::Term &term, TranslationContext &ctx) {
	using sparql::ast::BlankNode;
	using sparql::ast::TermKind;
	using sparql::ast::Var;
	TermSpec spec;
	if (term.kind() == TermKind::Var) {
		spec.isVar = true;
		spec.varName = static_cast<const Var &>(term).name;
	} else if (term.kind() == TermKind::BlankNode) {
		spec.isVar = true;
		spec.varName = "_bnode_" + static_cast<const BlankNode &>(term).label;
		// A blank node is scoped like a variable during translation but is not
		// a query variable, so it must never be projected by `SELECT *`.
		ctx.markInternal(spec.varName);
	} else {
		spec.isVar = false;
		spec.boundTerm = &term;
	}
	return spec;
}

TermSpec varTermSpec(const std::string &varName) {
	TermSpec spec;
	spec.isVar = true;
	spec.varName = varName;
	return spec;
}

PredicateConstraint constantPredicate(const std::string &iri) {
	PredicateConstraint spec;
	spec.kind = PredicateConstraint::ConstantIri;
	spec.iri = iri;
	return spec;
}

PredicateConstraint variablePredicate(const std::string &varName) {
	PredicateConstraint spec;
	spec.kind = PredicateConstraint::Variable;
	spec.varName = varName;
	return spec;
}

PredicateConstraint negatedPredicate(std::vector<std::string> excludedIris) {
	PredicateConstraint spec;
	spec.kind = PredicateConstraint::NotIn;
	spec.excludedIris = std::move(excludedIris);
	return spec;
}

RelNodePtr translateAtomicPattern(const TermSpec &subjectSpec, const PredicateConstraint &predicateSpec,
                                  const TermSpec &objectSpec, TranslationContext &ctx) {
	// The pattern's variables, in subject/predicate/object first-occurrence
	// order (matches the projection order tryAddCandidate produces).
	std::vector<std::string> varOrder;
	auto addVar = [&](const std::string &v) {
		if (std::find(varOrder.begin(), varOrder.end(), v) == varOrder.end()) {
			varOrder.push_back(v);
		}
	};
	if (subjectSpec.isVar) {
		addVar(subjectSpec.varName);
	}
	if (predicateSpec.kind == PredicateConstraint::Variable) {
		addVar(predicateSpec.varName);
	}
	if (objectSpec.isVar) {
		addVar(objectSpec.varName);
	}
	// Appended last, matching tryAddCandidate's positions[] order.
	if (ctx.activeGraph().kind == GraphConstraint::Kind::Variable) {
		addVar(ctx.activeGraph().varName);
	}

	std::vector<RelNodePtr> branches;
	branches.reserve(ctx.mapping().triplesMaps.size() * 2); // rough guess to avoid too many reallocs
	for (const auto &tmPtr : ctx.mapping().triplesMaps) {
		const r2rml::TriplesMap &tm = *tmPtr;
		if (!tm.logicalTable || !tm.subjectMap) {
			continue;
		}
		const r2rml::TermMap *subjectValueMap = tm.subjectMap->valueTermMap();
		if (!subjectValueMap) {
			continue;
		}

		const std::string childIdentity = logicalTableIdentity(*tm.logicalTable);
		const bool predicateCouldBeRdfType = predicateCouldMatchIri(predicateSpec, kRdfTypeIri);

		// --- rr:class candidates: synthetic (subject, rdf:type, classIRI) ---
		if (predicateCouldBeRdfType && !tm.subjectMap->classIRIs.empty()) {
			std::string alias = ctx.nextAlias();
			std::string fromSql = logicalTableFromSql(*tm.logicalTable, alias, ctx);
			TermSource subjectSrc;
			subjectSrc.termMap = subjectValueMap;
			subjectSrc.alias = alias;
			subjectSrc.tableIdentity = childIdentity;
			// Only the SUBJECT map's graph maps apply to an rr:class triple - it
			// has no predicate-object map to contribute any (matching
			// TriplesMap::generateTriples, which passes an empty pom list). So a
			// mapping with rr:graph on a POM but not on its subject map puts its
			// rdf:type triples in the DEFAULT graph while the POM's triples are
			// named. Spec-correct, and surprising.
			static const std::vector<std::unique_ptr<r2rml::GraphMap>> kNoGraphMaps;
			std::vector<GraphBranch> graphBranches =
			    graphBranchesFor(tm.subjectMap->graphMaps, kNoGraphMaps, alias, childIdentity, ctx);
			for (const std::string &classIri : tm.subjectMap->classIRIs) {
				TermSource predicateSrc;
				predicateSrc.isConstant = true;
				predicateSrc.constantValue = kRdfTypeIri;
				TermSource objectSrc;
				objectSrc.isConstant = true;
				objectSrc.constantValue = classIri;
				for (const auto &gb : graphBranches) {
					tryAddCandidate(branches, fromSql, alias, childIdentity, subjectSrc, predicateSrc, objectSrc, gb,
					                subjectSpec, predicateSpec, objectSpec, /*mergeableSubject=*/true, ctx);
				}
			}
		}

		// --- PredicateObjectMap candidates ---
		for (const auto &pomPtr : tm.predicateObjectMaps) {
			const r2rml::PredicateObjectMap &pom = *pomPtr;
			for (const auto &predMapPtr : pom.predicateMaps) {
				if (!predMapPtr) {
					continue;
				}
				for (const auto &objMapPtr : pom.objectMaps) {
					if (!objMapPtr) {
						continue;
					}

					const auto *refObjMap = dynamic_cast<const r2rml::ReferencingObjectMap *>(objMapPtr.get());
					if (refObjMap) {
						if (!refObjMap->parentTriplesMap || refObjMap->joinConditions.empty()) {
							continue;
						}
						const r2rml::TriplesMap &parentTm = *refObjMap->parentTriplesMap;
						if (!parentTm.logicalTable || !parentTm.subjectMap) {
							continue;
						}
						const r2rml::TermMap *parentSubjectValueMap = parentTm.subjectMap->valueTermMap();
						if (!parentSubjectValueMap) {
							continue;
						}

						std::string childAlias = ctx.nextAlias();
						std::string parentAlias = ctx.nextAlias();
						std::string fromSql = logicalTableFromSql(*tm.logicalTable, childAlias, ctx);
						fromSql += " JOIN " + logicalTableFromSql(*parentTm.logicalTable, parentAlias, ctx);
						fromSql += " ON ";
						for (std::size_t i = 0; i < refObjMap->joinConditions.size(); ++i) {
							const r2rml::JoinCondition &jc = refObjMap->joinConditions[i];
							if (i > 0) {
								fromSql += " AND ";
							}
							fromSql += childAlias + "." + ctx.dialect().quoteIdentifier(jc.childColumn) + " = " +
							           parentAlias + "." + ctx.dialect().quoteIdentifier(jc.parentColumn);
						}

						TermSource subjectSrc;
						subjectSrc.termMap = subjectValueMap;
						subjectSrc.alias = childAlias;
						subjectSrc.tableIdentity = childIdentity;
						TermSource predicateSrc;
						predicateSrc.termMap = predMapPtr.get();
						predicateSrc.alias = childAlias;
						predicateSrc.tableIdentity = childIdentity;
						TermSource objectSrc;
						objectSrc.termMap = parentSubjectValueMap;
						objectSrc.alias = parentAlias;
						objectSrc.tableIdentity = logicalTableIdentity(*parentTm.logicalTable);

						// The subject side is still a simple single-table key on the
						// child table - the join is a *second*, non-subject-defining
						// FROM element - so self-join elimination may still merge two
						// occurrences of this exact candidate, as long as the parent
						// table and join condition match too (folded into the sig via
						// parentJoinSig, so it never conflates this with a plain
						// candidate over the same child table, or with a
						// referencing-object-map candidate through a different parent).
						std::string parentJoinSig = "rom:" + logicalTableIdentity(*parentTm.logicalTable);
						for (const auto &jc : refObjMap->joinConditions) {
							parentJoinSig += "\x1f" + jc.childColumn + "=" + jc.parentColumn;
						}
						// Child subject map ∪ this POM. The PARENT's subject graph
						// maps deliberately do not apply: PredicateObjectMap::
						// processRow receives the child triples map's, not the
						// parent's. Graph terms come off the child alias for the
						// same reason.
						for (const auto &gb : graphBranchesFor(tm.subjectMap->graphMaps, pom.graphMaps, childAlias,
						                                       childIdentity, ctx)) {
							tryAddCandidate(branches, fromSql, childAlias, childIdentity, subjectSrc, predicateSrc,
							                objectSrc, gb, subjectSpec, predicateSpec, objectSpec,
							                /*mergeableSubject=*/true, ctx, parentAlias, parentJoinSig);
						}
						continue;
					}

					std::string alias = ctx.nextAlias();
					std::string fromSql = logicalTableFromSql(*tm.logicalTable, alias, ctx);
					TermSource subjectSrc;
					subjectSrc.termMap = subjectValueMap;
					subjectSrc.alias = alias;
					subjectSrc.tableIdentity = childIdentity;
					TermSource predicateSrc;
					predicateSrc.termMap = predMapPtr.get();
					predicateSrc.alias = alias;
					predicateSrc.tableIdentity = childIdentity;
					TermSource objectSrc;
					objectSrc.termMap = objMapPtr.get();
					objectSrc.alias = alias;
					objectSrc.tableIdentity = childIdentity;

					for (const auto &gb :
					     graphBranchesFor(tm.subjectMap->graphMaps, pom.graphMaps, alias, childIdentity, ctx)) {
						tryAddCandidate(branches, fromSql, alias, childIdentity, subjectSrc, predicateSrc, objectSrc,
						                gb, subjectSpec, predicateSpec, objectSpec, /*mergeableSubject=*/true, ctx);
					}
				}
			}
		}
	}

	if (branches.empty()) {
		RelNodePtr node(new EmptyNode());
		for (const auto &v : varOrder) {
			ColumnInfo col;
			col.var = v;
			col.nonNull = true;
			// Term annotation stays Unknown: an empty relation has no rows, so
			// no term kind is ever observable through it.
			node->schema().push_back(col);
		}
		return node;
	}
	if (branches.size() == 1) {
		return std::move(branches.front());
	}

	RelNodePtr node(new UnionByNameNode());
	UnionByNameNode &un = static_cast<UnionByNameNode &>(*node);
	un.all = false; // candidate union dedups (matches combineByName(all=false)).
	// Meet each variable across the arms *before* moving them: this is the
	// dominant source of disagreement in practice, since one predicate mapped by
	// several triples maps produces one arm per candidate.
	for (const auto &v : varOrder) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = true;
		col.term = meetAcrossArms(v, branches);
		un.schema().push_back(col);
	}
	un.arms = std::move(branches);
	return node;
}

RelNodePtr allTermsRelation(const std::vector<std::string> &varNames, TranslationContext &ctx) {
	std::vector<RelNodePtr> arms;
	arms.reserve(ctx.mapping().triplesMaps.size() * 2); // rough guess to avoid too many reallocs
	for (const auto &tmPtr : ctx.mapping().triplesMaps) {
		const r2rml::TriplesMap &tm = *tmPtr;
		if (!tm.logicalTable || !tm.subjectMap) {
			continue;
		}
		const r2rml::TermMap *subjectValueMap = tm.subjectMap->valueTermMap();
		if (!subjectValueMap) {
			continue;
		}
		// A TriplesMap with neither an rr:class nor any predicate-object map
		// emits no triples at all, so its subjects are not nodes of the graph.
		if (tm.subjectMap->classIRIs.empty() && tm.predicateObjectMaps.empty()) {
			continue;
		}

		const std::string identity = logicalTableIdentity(*tm.logicalTable);

		std::string subjectAlias = ctx.nextAlias();
		addTermArm(arms, logicalTableFromSql(*tm.logicalTable, subjectAlias, ctx), subjectAlias, identity,
		           *subjectValueMap, varNames, ctx);

		for (const std::string &classIri : tm.subjectMap->classIRIs) {
			addConstantTermArm(arms, classIri, varNames, ctx);
		}

		for (const auto &pomPtr : tm.predicateObjectMaps) {
			for (const auto &objMapPtr : pomPtr->objectMaps) {
				if (!objMapPtr) {
					continue;
				}
				// A referencing object map's object terms are by construction
				// the parent TriplesMap's subject terms, already contributed by
				// that parent's own subject arm above.
				if (dynamic_cast<const r2rml::ReferencingObjectMap *>(objMapPtr.get())) {
					continue;
				}
				std::string alias = ctx.nextAlias();
				addTermArm(arms, logicalTableFromSql(*tm.logicalTable, alias, ctx), alias, identity, *objMapPtr,
				           varNames, ctx);
			}
		}
	}

	if (arms.empty()) {
		RelNodePtr node(new EmptyNode());
		for (const auto &v : varNames) {
			ColumnInfo col;
			col.var = v;
			col.nonNull = true;
			// Unknown: no rows, so no term kind is observable (as above).
			node->schema().push_back(col);
		}
		return node;
	}
	if (arms.size() == 1) {
		return std::move(arms.front());
	}

	RelNodePtr node(new UnionByNameNode());
	UnionByNameNode &un = static_cast<UnionByNameNode &>(*node);
	un.all = false; // the term universe is a set, not a bag.
	// Meet before moving the arms - see the candidate union above. The term
	// universe spans every subject and object term map in the mapping, so this
	// realistically only stays known for a single-term-map mapping.
	for (const auto &v : varNames) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = true;
		col.term = meetAcrossArms(v, arms);
		un.schema().push_back(col);
	}
	un.arms = std::move(arms);
	return node;
}

RelNodePtr allNamedGraphsRelation(const std::string &varName, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();

	// Enumerate the graph sets the mapping can produce, reusing the same
	// classification (and dataset filtering) candidate enumeration uses, so
	// there is one definition of "a named graph of this mapping" rather than
	// two that can drift. The active graph is forced to Variable for the sweep:
	// we want every named branch, whatever block we were called from.
	TranslationContext::ActiveGraphGuard guard(ctx, variableGraph(varName));

	std::vector<RelNodePtr> arms;
	// A subject-level graph map applies to every predicate-object map of its
	// triples map, so the same (graph expression, source table) pair is reached
	// once per POM. Emitting an arm apiece would produce several identical
	// SELECTs whose only difference is the alias - correct, since the union
	// dedups, but needlessly wide SQL. Keyed on the arm's *content* rather than
	// its alias so those collapse to one.
	std::set<std::string> emitted;
	for (const auto &tmPtr : ctx.mapping().triplesMaps) {
		const r2rml::TriplesMap &tm = *tmPtr;
		if (!tm.logicalTable || !tm.subjectMap || !tm.subjectMap->valueTermMap()) {
			continue;
		}
		const std::string identity = logicalTableIdentity(*tm.logicalTable);

		// One pass per distinct graph-set source: the subject map alone (which is
		// what an rr:class triple carries), then each predicate-object map.
		std::vector<const std::vector<std::unique_ptr<r2rml::GraphMap>> *> pomSets;
		static const std::vector<std::unique_ptr<r2rml::GraphMap>> kNoGraphMaps;
		if (!tm.subjectMap->classIRIs.empty()) {
			pomSets.push_back(&kNoGraphMaps);
		}
		for (const auto &pomPtr : tm.predicateObjectMaps) {
			pomSets.push_back(&pomPtr->graphMaps);
		}

		for (const auto *pomGraphMaps : pomSets) {
			std::string alias = ctx.nextAlias();
			std::string fromSql = logicalTableFromSql(*tm.logicalTable, alias, ctx);
			for (const auto &gb : graphBranchesFor(tm.subjectMap->graphMaps, *pomGraphMaps, alias, identity, ctx)) {
				if (gb.isDefaultGraph) {
					continue; // the default graph is not a named graph
				}
				Resolved r = resolveSource(gb.graphSrc, dialect, ctx.catalog());
				// The alias is baked into `expr`, so strip it back out for the key:
				// two arms over the same table with the same graph strategy differ
				// only by alias and are the same relation.
				std::string key = identity + "\x1f" + replaceAllText(r.expr, alias + ".", "%A%.");
				for (const auto &c : gb.extraConds) {
					key += "\x1f" + replaceAllText(c, alias + ".", "%A%.");
				}
				if (!emitted.insert(key).second) {
					continue;
				}
				RelNodePtr node(new SpjRelation());
				SpjRelation &spj = static_cast<SpjRelation &>(*node);
				SpjSource source;
				source.sql = fromSql;
				source.alias = alias;
				source.tableIdentity = identity;
				spj.sources.push_back(source);
				spj.whereConds = gb.extraConds;
				for (const auto &g : r.requiredNonNull) {
					spj.whereConds.push_back(g + " IS NOT NULL");
				}
				spj.distinct = true;
				ColumnInfo col;
				col.var = varName;
				fillColumnFromResolved(col, r, dialect);
				// A graph name is always an IRI (see tryAddCandidate).
				col.term = TermInfo();
				col.term.kind = RdfTermKind::Iri;
				col.tagExpr = tagLiteral(col.term, dialect);
				spj.schema().push_back(col);
				arms.push_back(std::move(node));
			}
		}
	}

	if (arms.empty()) {
		RelNodePtr node(new EmptyNode());
		ColumnInfo col;
		col.var = varName;
		col.nonNull = true;
		node->schema().push_back(col);
		return node;
	}
	if (arms.size() == 1) {
		return std::move(arms.front());
	}
	RelNodePtr node(new UnionByNameNode());
	UnionByNameNode &un = static_cast<UnionByNameNode &>(*node);
	un.all = false; // a set of graphs, not a bag
	ColumnInfo col;
	col.var = varName;
	col.nonNull = true;
	col.term = meetAcrossArms(varName, arms);
	un.schema().push_back(col);
	un.arms = std::move(arms);
	return node;
}

RelNodePtr translateTriplePattern(const sparql::ast::TriplePattern &tp, TranslationContext &ctx) {
	// Evaluate the endpoints before the path: termSpecFor registers blank-node
	// variables as internal, and translatePath may mint further internal
	// variables of its own.
	TermSpec subjectSpec = termSpecFor(*tp.subject, ctx);
	TermSpec objectSpec = termSpecFor(*tp.object, ctx);
	return translatePath(*tp.predicate, subjectSpec, objectSpec, ctx);
}

} // namespace sparql2sql
