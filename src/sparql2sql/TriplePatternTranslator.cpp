#include "sparql2sql/TriplePatternTranslator.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

#include "r2rml/BaseTableOrView.h"
#include "r2rml/ColumnTermMap.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLView.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/TemplateTermMap.h"
#include "r2rml/TermMap.h"
#include "r2rml/TriplesMap.h"
#include "sparql-parser/ast/Term.h"
#include "sparql2sql/PropertyPathTranslator.h"
#include "sparql2sql/SqlDialect.h"
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

InversionResult resolveInversion(const TermSource &src, const sparql::ast::Term &boundTerm, const SqlDialect &dialect) {
	if (src.isConstant) {
		InversionResult r;
		r.possible = (src.constantValue == termLexicalForm(boundTerm));
		return r;
	}
	return invertTermMapAgainstBoundTerm(*src.termMap, boundTerm, src.alias, dialect);
}

std::string stripTrailingSemicolon(std::string sql) {
	std::size_t end = sql.find_last_not_of(" \t\r\n");
	if (end == std::string::npos) {
		return std::string();
	}
	sql.erase(end + 1);
	if (!sql.empty() && sql.back() == ';') {
		sql.pop_back();
	}
	return sql;
}

std::string logicalTableFromSql(const r2rml::LogicalTable &lt, const std::string &alias, const SqlDialect &dialect) {
	if (const auto *base = dynamic_cast<const r2rml::BaseTableOrView *>(&lt)) {
		return dialect.quoteIdentifier(base->tableName) + " AS " + alias;
	}
	if (const auto *view = dynamic_cast<const r2rml::R2RMLView *>(&lt)) {
		return "(" + stripTrailingSemicolon(view->sqlQuery) + ") AS " + alias;
	}
	throw std::logic_error("logicalTableFromSql: unrecognized LogicalTable subtype");
}

// A stable identity for a logical table, used by self-join elimination to
// recognize two scans of the same source. Base tables key on their name;
// views key on their SQL text (never merged in practice, but distinct).
std::string logicalTableIdentity(const r2rml::LogicalTable &lt) {
	if (const auto *base = dynamic_cast<const r2rml::BaseTableOrView *>(&lt)) {
		return base->tableName;
	}
	if (const auto *view = dynamic_cast<const r2rml::R2RMLView *>(&lt)) {
		return "view:" + view->sqlQuery;
	}
	return std::string();
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

void addUnique(std::vector<std::string> &out, const std::vector<std::string> &more) {
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
                              const SqlDialect &dialect, std::vector<std::string> &whereConditions) {
	for (const auto &iri : excludedIris) {
		sparql::ast::Iri excludedTerm(iri, iri);
		InversionResult inv = resolveInversion(predicateSrc, excludedTerm, dialect);
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
	std::string sql = "SELECT ";
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
		raw.schema().push_back(col);
	}
	arms.push_back(std::move(node));
}

// Attempt to build one candidate branch's SpjRelation. Appends to `branches`
// on success; silently does nothing if the candidate is statically prunable
// (a bound position can never match this candidate's source).
void tryAddCandidate(std::vector<RelNodePtr> &branches, const std::string &fromSql, const std::string &fromAlias,
                     const std::string &fromIdentity, const TermSource &subjectSrc, const TermSource &predicateSrc,
                     const TermSource &objectSrc, const TermSpec &subjectSpec, const PredicateConstraint &predicateSpec,
                     const TermSpec &objectSpec, bool mergeableSubject, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();

	std::vector<std::string> whereConditions;
	std::vector<std::string> requiredNonNull;

	Resolved subjectR = resolveSource(subjectSrc, dialect, ctx.catalog());
	addUnique(requiredNonNull, subjectR.requiredNonNull);
	if (!subjectSpec.isVar) {
		InversionResult inv = resolveInversion(subjectSrc, *subjectSpec.boundTerm, dialect);
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	}

	Resolved predicateR = resolveSource(predicateSrc, dialect, ctx.catalog());
	addUnique(requiredNonNull, predicateR.requiredNonNull);
	if (predicateSpec.kind == PredicateConstraint::ConstantIri) {
		sparql::ast::Iri predicateBoundTerm(predicateSpec.iri, predicateSpec.iri);
		InversionResult inv = resolveInversion(predicateSrc, predicateBoundTerm, dialect);
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	} else if (predicateSpec.kind == PredicateConstraint::NotIn) {
		if (!applyPredicateExclusions(predicateSrc, predicateSpec.excludedIris, dialect, whereConditions)) {
			return;
		}
	}

	Resolved objectR = resolveSource(objectSrc, dialect, ctx.catalog());
	addUnique(requiredNonNull, objectR.requiredNonNull);
	if (!objectSpec.isVar) {
		InversionResult inv = resolveInversion(objectSrc, *objectSpec.boundTerm, dialect);
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	}

	// Self-join guard: the same variable in more than one position must
	// resolve to equal source expressions, and is projected exactly once.
	struct PositionEntry {
		bool isVar;
		std::string varName;
		const Resolved *resolved;
	};
	const bool predicateIsVar = predicateSpec.kind == PredicateConstraint::Variable;
	PositionEntry positions[3] = {
	    {subjectSpec.isVar, subjectSpec.varName, &subjectR},
	    {predicateIsVar, predicateSpec.varName, &predicateR},
	    {objectSpec.isVar, objectSpec.varName, &objectR},
	};

	std::vector<ColumnInfo> projections; // first occurrence of each var wins
	for (int i = 0; i < 3; ++i) {
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
			source.subjectVar = subjectSpec.varName;
			source.subjectKeySig = sig;
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

	std::vector<RelNodePtr> branches;

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
			std::string fromSql = logicalTableFromSql(*tm.logicalTable, alias, ctx.dialect());
			TermSource subjectSrc;
			subjectSrc.termMap = subjectValueMap;
			subjectSrc.alias = alias;
			subjectSrc.tableIdentity = childIdentity;
			for (const std::string &classIri : tm.subjectMap->classIRIs) {
				TermSource predicateSrc;
				predicateSrc.isConstant = true;
				predicateSrc.constantValue = kRdfTypeIri;
				TermSource objectSrc;
				objectSrc.isConstant = true;
				objectSrc.constantValue = classIri;
				tryAddCandidate(branches, fromSql, alias, childIdentity, subjectSrc, predicateSrc, objectSrc,
				                subjectSpec, predicateSpec, objectSpec, /*mergeableSubject=*/true, ctx);
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
						std::string fromSql = logicalTableFromSql(*tm.logicalTable, childAlias, ctx.dialect());
						fromSql += " JOIN " + logicalTableFromSql(*parentTm.logicalTable, parentAlias, ctx.dialect());
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

						tryAddCandidate(branches, fromSql, childAlias, childIdentity, subjectSrc, predicateSrc,
						                objectSrc, subjectSpec, predicateSpec, objectSpec,
						                /*mergeableSubject=*/false, ctx);
						continue;
					}

					std::string alias = ctx.nextAlias();
					std::string fromSql = logicalTableFromSql(*tm.logicalTable, alias, ctx.dialect());
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

					tryAddCandidate(branches, fromSql, alias, childIdentity, subjectSrc, predicateSrc, objectSrc,
					                subjectSpec, predicateSpec, objectSpec, /*mergeableSubject=*/true, ctx);
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
		addTermArm(arms, logicalTableFromSql(*tm.logicalTable, subjectAlias, ctx.dialect()), subjectAlias, identity,
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
				addTermArm(arms, logicalTableFromSql(*tm.logicalTable, alias, ctx.dialect()), alias, identity,
				           *objMapPtr, varNames, ctx);
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

RelNodePtr translateTriplePattern(const sparql::ast::TriplePattern &tp, TranslationContext &ctx) {
	// Evaluate the endpoints before the path: termSpecFor registers blank-node
	// variables as internal, and translatePath may mint further internal
	// variables of its own.
	TermSpec subjectSpec = termSpecFor(*tp.subject, ctx);
	TermSpec objectSpec = termSpecFor(*tp.object, ctx);
	return translatePath(*tp.predicate, subjectSpec, objectSpec, ctx);
}

} // namespace sparql2sql
