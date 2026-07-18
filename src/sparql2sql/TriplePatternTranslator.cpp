#include "sparql2sql/TriplePatternTranslator.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "r2rml/BaseTableOrView.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLView.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/TermMap.h"
#include "r2rml/TriplesMap.h"
#include "sparql-parser/ast/Term.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TermMapSql.h"
#include "sparql2sql/TranslationError.h"

namespace sparql2sql {

namespace {

const char *const kRdfTypeIri = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";

// A triple-pattern position (subject/predicate/object): either a variable
// (or blank node, which is treated as an internally-scoped variable - it
// can never appear in a SELECT/GROUP BY/ORDER BY since those only accept
// ast::Var, but it still needs to participate in join unification the same
// way an ordinary variable does) or a bound constant term.
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
// a real R2RML TermMap evaluated against a given source-table alias.
struct TermSource {
	bool isConstant = false;
	std::string constantValue;               // valid iff isConstant
	const r2rml::TermMap *termMap = nullptr; // valid iff !isConstant
	std::string alias;                       // valid iff !isConstant
};

SqlExpr resolveSource(const TermSource &src, const SqlDialect &dialect) {
	if (src.isConstant) {
		SqlExpr e;
		e.expr = dialect.stringLiteral(src.constantValue);
		return e;
	}
	return termMapToSqlExpr(*src.termMap, src.alias, dialect);
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

void addUnique(std::vector<std::string> &out, const std::vector<std::string> &more) {
	for (const auto &v : more) {
		if (std::find(out.begin(), out.end(), v) == out.end()) {
			out.push_back(v);
		}
	}
}

// Attempt to build one candidate branch's SQL. Appends to `branches` on
// success; silently does nothing if the candidate is statically prunable
// (a bound position can never match this candidate's source).
void tryAddCandidate(std::vector<std::string> &branches, const std::string &fromSql, const TermSource &subjectSrc,
                     const TermSource &predicateSrc, const TermSource &objectSrc, const PositionSpec &subjectSpec,
                     const PredicateSpec &predicateSpec, const PositionSpec &objectSpec, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();

	std::vector<std::string> whereConditions;
	std::vector<std::string> requiredNonNull;

	SqlExpr subjectExpr = resolveSource(subjectSrc, dialect);
	addUnique(requiredNonNull, subjectExpr.requiredNonNullColumns);
	if (!subjectSpec.isVar) {
		InversionResult inv = resolveInversion(subjectSrc, *subjectSpec.boundTerm, dialect);
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	}

	SqlExpr predicateExpr = resolveSource(predicateSrc, dialect);
	addUnique(requiredNonNull, predicateExpr.requiredNonNullColumns);
	if (!predicateSpec.isVar) {
		sparql::ast::Iri predicateBoundTerm(predicateSpec.constantIri, predicateSpec.constantIri);
		InversionResult inv = resolveInversion(predicateSrc, predicateBoundTerm, dialect);
		if (!inv.possible) {
			return;
		}
		addUnique(whereConditions, inv.whereConditions);
	}

	SqlExpr objectExpr = resolveSource(objectSrc, dialect);
	addUnique(requiredNonNull, objectExpr.requiredNonNullColumns);
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
		std::string expr;
	};
	PositionEntry positions[3] = {
	    {subjectSpec.isVar, subjectSpec.varName, subjectExpr.expr},
	    {predicateSpec.isVar, predicateSpec.varName, predicateExpr.expr},
	    {objectSpec.isVar, objectSpec.varName, objectExpr.expr},
	};

	std::vector<std::pair<std::string, std::string>> projections; // (var, expr), first occurrence wins
	for (int i = 0; i < 3; ++i) {
		if (!positions[i].isVar) {
			continue;
		}
		bool seen = false;
		for (int j = 0; j < i; ++j) {
			if (positions[j].isVar && positions[j].varName == positions[i].varName) {
				whereConditions.push_back(positions[j].expr + " = " + positions[i].expr);
				seen = true;
				break;
			}
		}
		if (!seen) {
			projections.emplace_back(positions[i].varName, positions[i].expr);
		}
	}

	for (const auto &col : requiredNonNull) {
		whereConditions.push_back(col + " IS NOT NULL");
	}

	std::string sql = "SELECT DISTINCT ";
	if (projections.empty()) {
		sql += "1 AS " + dialect.quoteIdentifier("_dummy");
	} else {
		for (std::size_t i = 0; i < projections.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			sql += projections[i].second + " AS " + mangleVar(projections[i].first, dialect);
		}
	}
	sql += " FROM " + fromSql;
	if (!whereConditions.empty()) {
		sql += " WHERE ";
		for (std::size_t i = 0; i < whereConditions.size(); ++i) {
			if (i > 0) {
				sql += " AND ";
			}
			sql += "(" + whereConditions[i] + ")";
		}
	}
	branches.push_back(sql);
}

} // namespace

TranslatedPattern translateTriplePattern(const sparql::ast::TriplePattern &tp, TranslationContext &ctx) {
	PositionSpec subjectSpec = specFor(*tp.subject);
	PredicateSpec predicateSpec = predicateSpecFor(*tp.predicate);
	PositionSpec objectSpec = specFor(*tp.object);

	std::set<std::string> vars;
	if (subjectSpec.isVar) {
		vars.insert(subjectSpec.varName);
	}
	if (predicateSpec.isVar) {
		vars.insert(predicateSpec.varName);
	}
	if (objectSpec.isVar) {
		vars.insert(objectSpec.varName);
	}

	std::vector<std::string> branches;

	for (const auto &tmPtr : ctx.mapping().triplesMaps) {
		const r2rml::TriplesMap &tm = *tmPtr;
		if (!tm.logicalTable || !tm.subjectMap) {
			continue;
		}
		const r2rml::TermMap *subjectValueMap = tm.subjectMap->valueTermMap();
		if (!subjectValueMap) {
			continue;
		}

		const bool predicateCouldBeRdfType = predicateSpec.isVar || predicateSpec.constantIri == kRdfTypeIri;

		// --- rr:class candidates: synthetic (subject, rdf:type, classIRI) ---
		if (predicateCouldBeRdfType && !tm.subjectMap->classIRIs.empty()) {
			std::string alias = ctx.nextAlias();
			std::string fromSql = logicalTableFromSql(*tm.logicalTable, alias, ctx.dialect());
			TermSource subjectSrc;
			subjectSrc.termMap = subjectValueMap;
			subjectSrc.alias = alias;
			for (const std::string &classIri : tm.subjectMap->classIRIs) {
				TermSource predicateSrc;
				predicateSrc.isConstant = true;
				predicateSrc.constantValue = kRdfTypeIri;
				TermSource objectSrc;
				objectSrc.isConstant = true;
				objectSrc.constantValue = classIri;
				tryAddCandidate(branches, fromSql, subjectSrc, predicateSrc, objectSrc, subjectSpec, predicateSpec,
				                objectSpec, ctx);
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
						TermSource predicateSrc;
						predicateSrc.termMap = predMapPtr.get();
						predicateSrc.alias = childAlias;
						TermSource objectSrc;
						objectSrc.termMap = parentSubjectValueMap;
						objectSrc.alias = parentAlias;

						tryAddCandidate(branches, fromSql, subjectSrc, predicateSrc, objectSrc, subjectSpec,
						                predicateSpec, objectSpec, ctx);
						continue;
					}

					std::string alias = ctx.nextAlias();
					std::string fromSql = logicalTableFromSql(*tm.logicalTable, alias, ctx.dialect());
					TermSource subjectSrc;
					subjectSrc.termMap = subjectValueMap;
					subjectSrc.alias = alias;
					TermSource predicateSrc;
					predicateSrc.termMap = predMapPtr.get();
					predicateSrc.alias = alias;
					TermSource objectSrc;
					objectSrc.termMap = objMapPtr.get();
					objectSrc.alias = alias;

					tryAddCandidate(branches, fromSql, subjectSrc, predicateSrc, objectSrc, subjectSpec, predicateSpec,
					                objectSpec, ctx);
				}
			}
		}
	}

	TranslatedPattern result;
	if (branches.empty()) {
		if (vars.empty()) {
			result.sql = "SELECT 1 AS " + ctx.dialect().quoteIdentifier("_dummy") + " WHERE FALSE";
		} else {
			std::string sql = "SELECT ";
			bool first = true;
			for (const auto &v : vars) {
				if (!first) {
					sql += ", ";
				}
				first = false;
				sql += "CAST(NULL AS VARCHAR) AS " + mangleVar(v, ctx.dialect());
			}
			sql += " WHERE FALSE";
			result.sql = sql;
		}
	} else if (branches.size() == 1) {
		result.sql = branches.front();
	} else {
		result.sql = ctx.dialect().combineByName(/*all=*/false, branches);
	}
	result.boundVars = vars;
	return result;
}

} // namespace sparql2sql
