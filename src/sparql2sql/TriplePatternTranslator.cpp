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
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TemplateUtil.h"
#include "sparql2sql/TermMapSql.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

namespace {

const char *const kRdfTypeIri = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";

// A triple-pattern position (subject/predicate/object): either a variable
// (or blank node, treated as an internally-scoped variable) or a bound
// constant term.
struct PositionSpec {
	bool isVar = false;
	std::string varName;                          // valid iff isVar
	const sparql::ast::Term *boundTerm = nullptr; // valid iff !isVar
};

PositionSpec specFor(const sparql::ast::Term &term) {
	using sparql::ast::BlankNode;
	using sparql::ast::TermKind;
	using sparql::ast::Var;
	PositionSpec spec;
	if (term.kind() == TermKind::Var) {
		spec.isVar = true;
		spec.varName = static_cast<const Var &>(term).name;
	} else if (term.kind() == TermKind::BlankNode) {
		spec.isVar = true;
		spec.varName = "_bnode_" + static_cast<const BlankNode &>(term).label;
	} else {
		spec.isVar = false;
		spec.boundTerm = &term;
	}
	return spec;
}

struct PredicateSpec {
	bool isVar = false;
	std::string varName;     // valid iff isVar
	std::string constantIri; // valid iff !isVar
};

PredicateSpec predicateSpecFor(const sparql::ast::PropertyPathExpr &path) {
	using sparql::ast::PathKind;
	using sparql::ast::PredicatePath;
	using sparql::ast::VariablePath;
	switch (path.kind()) {
	case PathKind::Predicate: {
		PredicateSpec spec;
		spec.isVar = false;
		spec.constantIri = static_cast<const PredicatePath &>(path).iri->value;
		return spec;
	}
	case PathKind::Variable: {
		PredicateSpec spec;
		spec.isVar = true;
		spec.varName = static_cast<const VariablePath &>(path).var->name;
		return spec;
	}
	default:
		throw TranslationError(
		    "unsupported: property paths are not supported in this phase - only a single IRI/`a` or a bare "
		    "variable may appear in predicate position");
	}
}

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

Resolved resolveSource(const TermSource &src, const SqlDialect &dialect) {
	Resolved r;
	if (src.isConstant) {
		r.expr = dialect.stringLiteral(src.constantValue);
		r.prov = Provenance::ConstantExpr;
		return r;
	}
	SqlExpr e = termMapToSqlExpr(*src.termMap, src.alias, dialect);
	r.expr = e.expr;
	r.requiredNonNull = e.requiredNonNullColumns;
	r.prov = provenanceOf(*src.termMap, r.columnName, r.templateString);
	r.sourceAlias = src.alias;
	r.tableIdentity = src.tableIdentity;
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

// Attempt to build one candidate branch's SpjRelation. Appends to `branches`
// on success; silently does nothing if the candidate is statically prunable
// (a bound position can never match this candidate's source).
void tryAddCandidate(std::vector<RelNodePtr> &branches, const std::string &fromSql, const std::string &fromAlias,
                     const std::string &fromIdentity, const TermSource &subjectSrc, const TermSource &predicateSrc,
                     const TermSource &objectSrc, const PositionSpec &subjectSpec, const PredicateSpec &predicateSpec,
                     const PositionSpec &objectSpec, bool mergeableSubject, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();

	std::vector<std::string> whereConditions;
	std::vector<std::string> requiredNonNull;

	Resolved subjectR = resolveSource(subjectSrc, dialect);
	addUnique(requiredNonNull, subjectR.requiredNonNull);
	if (!subjectSpec.isVar) {
		InversionResult inv = resolveInversion(subjectSrc, *subjectSpec.boundTerm, dialect);
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	}

	Resolved predicateR = resolveSource(predicateSrc, dialect);
	addUnique(requiredNonNull, predicateR.requiredNonNull);
	if (!predicateSpec.isVar) {
		sparql::ast::Iri predicateBoundTerm(predicateSpec.constantIri, predicateSpec.constantIri);
		InversionResult inv = resolveInversion(predicateSrc, predicateBoundTerm, dialect);
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	}

	Resolved objectR = resolveSource(objectSrc, dialect);
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
	PositionEntry positions[3] = {
	    {subjectSpec.isVar, subjectSpec.varName, &subjectR},
	    {predicateSpec.isVar, predicateSpec.varName, &predicateR},
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
				seen = true;
				break;
			}
		}
		if (!seen) {
			const Resolved &r = *positions[i].resolved;
			ColumnInfo col;
			col.var = positions[i].varName;
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

RelNodePtr translateTriplePattern(const sparql::ast::TriplePattern &tp, TranslationContext &ctx) {
	PositionSpec subjectSpec = specFor(*tp.subject);
	PredicateSpec predicateSpec = predicateSpecFor(*tp.predicate);
	PositionSpec objectSpec = specFor(*tp.object);

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
	if (predicateSpec.isVar) {
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
		const bool predicateCouldBeRdfType = predicateSpec.isVar || predicateSpec.constantIri == kRdfTypeIri;

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
	un.arms = std::move(branches);
	for (const auto &v : varOrder) {
		ColumnInfo col;
		col.var = v;
		col.nonNull = true;
		un.schema().push_back(col);
	}
	return node;
}

} // namespace sparql2sql
