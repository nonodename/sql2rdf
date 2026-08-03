#pragma once

#include <cstddef>
#include <set>
#include <string>

namespace r2rml {
class R2RMLMapping;
} // namespace r2rml

namespace sparql2sql {

class SqlDialect;
struct TypeCatalog;

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

private:
	const r2rml::R2RMLMapping &mapping_;
	const SqlDialect &dialect_;
	const TypeCatalog *catalog_;
	std::size_t aliasCounter_;
	std::size_t internalVarCounter_ = 0;
	std::set<std::string> internalVars_;
};

/// Mangle a SPARQL variable name into its projected SQL column name
/// (always quoted via the dialect, so case-sensitivity and any
/// PN_CHARS/Unicode edge cases in the variable name are never an issue).
std::string mangleVar(const std::string &sparqlVarName, const SqlDialect &dialect);

} // namespace sparql2sql
