#pragma once

#include <cstddef>
#include <set>
#include <string>

namespace r2rml {
class R2RMLMapping;
} // namespace r2rml

namespace sparql2sql {

class SqlDialect;

/// The intermediate representation threaded through translation: a SQL
/// relation (as a full "SELECT ..." statement, valid to wrap as
/// "(<sql>) AS aliasN") plus which SPARQL variables it binds and whether
/// each one is guaranteed non-NULL.
struct TranslatedPattern {
	std::string sql;
	std::set<std::string> boundVars;    // guaranteed non-NULL in every row
	std::set<std::string> optionalVars; // may be NULL in some rows
	bool isIdentity = false;            // true only for the fold's starting relation

	std::set<std::string> allVars() const {
		std::set<std::string> out = boundVars;
		out.insert(optionalVars.begin(), optionalVars.end());
		return out;
	}

	bool hasOuterJoinLineage() const {
		return !optionalVars.empty();
	}
};

/// Mutable state threaded by reference through the whole translation: the
/// mapping/dialect being translated against, a monotonic alias generator,
/// and (implicitly) the "throw TranslationError on anything unsupported"
/// policy that every translation function follows.
class TranslationContext {
public:
	TranslationContext(const r2rml::R2RMLMapping &mapping, const SqlDialect &dialect)
	    : mapping_(mapping), dialect_(dialect), aliasCounter_(0) {
	}

	const r2rml::R2RMLMapping &mapping() const {
		return mapping_;
	}

	const SqlDialect &dialect() const {
		return dialect_;
	}

	/// Produce a fresh, unique table alias ("t1", "t2", ...).
	std::string nextAlias() {
		return "t" + std::to_string(++aliasCounter_);
	}

private:
	const r2rml::R2RMLMapping &mapping_;
	const SqlDialect &dialect_;
	std::size_t aliasCounter_;
};

/// Mangle a SPARQL variable name into its projected SQL column name
/// (always quoted via the dialect, so case-sensitivity and any
/// PN_CHARS/Unicode edge cases in the variable name are never an issue).
std::string mangleVar(const std::string &sparqlVarName, const SqlDialect &dialect);

} // namespace sparql2sql
