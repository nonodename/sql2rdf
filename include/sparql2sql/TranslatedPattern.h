#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sparql2sql/TermInfo.h"

namespace r2rml {
class R2RMLMapping;
} // namespace r2rml

namespace sparql2sql {

class SqlDialect;
struct TypeCatalog;

/// One entry of a top-level WITH [RECURSIVE] clause: `name AS (bodySql)`.
struct CteDef {
	std::string name;
	std::string bodySql;
};

/// The intermediate representation threaded through translation: a SQL
/// relation (as a full "SELECT ..." statement, valid to wrap as
/// "(<sql>) AS aliasN") plus which SPARQL variables it binds and whether
/// each one is guaranteed non-NULL.
struct TranslatedPattern {
	std::string sql;
	std::set<std::string> boundVars;    // guaranteed non-NULL in every row
	std::set<std::string> optionalVars; // may be NULL in some rows
	bool isIdentity = false;            // true only for the fold's starting relation

	/// Per-variable static RDF term dimension, as far as the R2RML mapping
	/// determines it. A variable ABSENT from this map is Unknown, so the map is
	/// always safe to under-populate but never to over-populate: every consumer
	/// falls back to the pre-term-tracking behaviour on Unknown. Always read it
	/// through termInfoOf(), never with operator[] or a raw find().
	///
	/// Deliberately kept alongside boundVars/optionalVars rather than folded
	/// into one map-of-everything: nullability already has two well-tested
	/// homes across dozens of assertion sites, and a second copy of that fact
	/// here would be a second source of truth. Both views are derived by the
	/// same fillScopeFromSchema(), which is what keeps them in step.
	std::map<std::string, TermInfo> termInfo;

	std::set<std::string> allVars() const {
		std::set<std::string> out = boundVars;
		out.insert(optionalVars.begin(), optionalVars.end());
		return out;
	}

	bool hasOuterJoinLineage() const {
		return !optionalVars.empty();
	}

	/// The static term annotation of `var`, or a default (Unknown) one if the
	/// variable is absent - which is always a safe answer.
	TermInfo termInfoOf(const std::string &var) const {
		std::map<std::string, TermInfo>::const_iterator it = termInfo.find(var);
		return it == termInfo.end() ? TermInfo() : it->second;
	}
};

/// Mutable state threaded by reference through the whole translation: the
/// mapping/dialect being translated against, a monotonic alias generator,
/// and (implicitly) the "throw TranslationError on anything unsupported"
/// policy that every translation function follows.
class TranslationContext {
public:
	TranslationContext(const r2rml::R2RMLMapping &mapping, const SqlDialect &dialect,
	                   const TypeCatalog *catalog = nullptr)
	    : mapping_(mapping), dialect_(dialect), catalog_(catalog), aliasCounter_(0) {
	}

	const r2rml::R2RMLMapping &mapping() const {
		return mapping_;
	}

	const SqlDialect &dialect() const {
		return dialect_;
	}

	/// Optional column-type catalog (nullptr if none supplied).
	const TypeCatalog *catalog() const {
		return catalog_;
	}

	/// Produce a fresh, unique table alias ("t1", "t2", ...).
	std::string nextAlias() {
		return "t" + std::to_string(++aliasCounter_);
	}

	/// Mint a fresh internal variable name and register it as internal. Used
	/// for the intermediate node of a sequence property path (`E1/E2` binds a
	/// variable that joins the two halves but is not part of the query).
	///
	/// The "%" prefix cannot appear in a SPARQL VARNAME, so a minted name can
	/// never collide with a user variable; registration (rather than a prefix
	/// test) is what isInternal() actually consults, so the prefix is only a
	/// readability aid in generated SQL.
	std::string nextInternalVar() {
		std::string name = "%p" + std::to_string(++internalVarCounter_);
		markInternal(name);
		return name;
	}

	/// Register a variable as internal: bound and joinable during translation,
	/// but never projected by `SELECT *` (see translateQueryPattern). Also used
	/// for blank-node positions, which are scoped variables rather than query
	/// variables and so must not appear in query output.
	void markInternal(const std::string &varName) {
		internalVars_.insert(varName);
	}

	bool isInternal(const std::string &varName) const {
		return internalVars_.count(varName) != 0;
	}

	/// Mint a fresh CTE name ("cte1", "cte2", ...) - a distinct prefix and a
	/// separate counter from nextAlias()'s "t"+N sequence, so the two can
	/// never collide even though both are monotonic per-context counters.
	std::string nextCteName() {
		return "cte" + std::to_string(++cteCounter_);
	}

	/// Register one WITH-clause entry (name already minted via
	/// nextCteName()). Shared across the whole translation of one query,
	/// including reentrant nested-subquery translation (SubSelectElement
	/// folding calls translateQueryPattern with this same ctx and splices the
	/// result as literal text into the outer tree), so a CTE registered while
	/// rendering a nested query is still valid at the single top-level WITH
	/// clause the outermost caller emits.
	void addCte(const std::string &name, const std::string &bodySql) {
		pendingCtes_.push_back(CteDef {name, bodySql});
	}

	const std::vector<CteDef> &pendingCtes() const {
		return pendingCtes_;
	}

	/// SPARQL 1.1 §17.4.1.7 requires NOW() to return the same value for every
	/// call within a single query evaluation. Stamp the current UTC time into
	/// a lexical xsd:dateTime string the first time NOW() is translated, then
	/// return that same string for every later call in this translation -
	/// never emit a SQL function like current_timestamp, which would be
	/// re-evaluated per row instead of once per query.
	const std::string &nowLiteral() {
		if (nowLiteral_.empty()) {
			std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
			std::tm utc;
#if defined(_WIN32)
			gmtime_s(&utc, &t);
#else
			gmtime_r(&t, &utc);
#endif
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", utc.tm_year + 1900, utc.tm_mon + 1,
			              utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
			nowLiteral_ = buf;
		}
		return nowLiteral_;
	}

private:
	const r2rml::R2RMLMapping &mapping_;
	const SqlDialect &dialect_;
	const TypeCatalog *catalog_;
	std::size_t aliasCounter_;
	std::size_t internalVarCounter_ = 0;
	std::set<std::string> internalVars_;
	std::string nowLiteral_;
	std::size_t cteCounter_ = 0;
	std::vector<CteDef> pendingCtes_;
};

/// Mangle a SPARQL variable name into its projected SQL column name
/// (always quoted via the dialect, so case-sensitivity and any
/// PN_CHARS/Unicode edge cases in the variable name are never an issue).
std::string mangleVar(const std::string &sparqlVarName, const SqlDialect &dialect);

} // namespace sparql2sql
