#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sparql2sql/GraphConstraint.h"
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

	/// Variables whose runtime type-tag column `sql` projects. Only ever
	/// populated by translateQueryPattern's nested-subquery mode, for the
	/// variables whose annotation is not fully determined - see
	/// RawRelation::providedTagVars for why those specifically.
	std::set<std::string> providedTagVars;

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
	                   const TypeCatalog *catalog = nullptr, bool prettyPrint = false)
	    : mapping_(mapping), dialect_(dialect), catalog_(catalog), aliasCounter_(0), prettyPrint_(prettyPrint) {
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

	/// Whether generated SQL should be laid out for human readability (newlines,
	/// indentation, one-column-per-line) rather than the default single-line
	/// form. A debug/readability aid only - every renderer reads this (and the
	/// depth below) through nl()/indent()/clauseSep()/onSep() instead of
	/// branching on it directly, so the "if pretty" test lives in one place
	/// per formatting decision rather than at each of the many SQL-emitting
	/// call sites.
	bool pretty() const {
		return prettyPrint_;
	}

	/// "\n" in pretty mode, "" in compact mode.
	std::string nl() const {
		return prettyPrint_ ? std::string("\n") : std::string();
	}

	/// `extraLevels` beyond the current subquery nesting depth (see
	/// SubqueryDepthGuard), rendered as two spaces per level; always "" in
	/// compact mode.
	std::string indent(std::size_t extraLevels = 0) const {
		return prettyPrint_ ? std::string((subqueryDepth_ + extraLevels) * 2, ' ') : std::string();
	}

	/// Separator introducing a major clause keyword (FROM/WHERE/GROUP BY/
	/// HAVING/ORDER BY/JOIN/...): a newline at the current margin when pretty,
	/// a single space (today's behaviour) when compact.
	std::string clauseSep() const {
		return prettyPrint_ ? (nl() + indent()) : std::string(" ");
	}

	/// Separator introducing a JOIN's ON keyword, indented one level deeper
	/// than the JOIN itself.
	std::string onSep() const {
		return prettyPrint_ ? (nl() + indent(1)) : std::string(" ");
	}

	/// The RDF graph triple patterns are currently being matched against -
	/// SPARQL's active graph. `Default` (no enclosing GRAPH block) unless an
	/// ActiveGraphGuard is in scope.
	const GraphConstraint &activeGraph() const {
		return activeGraph_;
	}

	/// The dataset the query is evaluated against (its FROM / FROM NAMED
	/// clauses). Unrestricted unless setDataset() was called.
	const ActiveDataset &dataset() const {
		return dataset_;
	}

	/// Fix the dataset for this translation. Unlike the active graph this needs
	/// no guard and no scoping: the grammar puts DatasetClause only on the
	/// top-level Query, so there is exactly one dataset for the whole
	/// translation and nested sub-selects / EXISTS bodies correctly inherit it.
	///
	/// Must be called before anything folds - translateQuery does so
	/// immediately, before its first fold().
	void setDataset(ActiveDataset dataset) {
		dataset_ = std::move(dataset);
	}

	/// Make `graph` the active graph for as long as the guard is alive.
	///
	/// Save/restore rather than the increment/decrement SubqueryDepthGuard uses,
	/// because a nested GRAPH block *replaces* the active graph outright rather
	/// than composing with the enclosing one (SPARQL 1.1 Section 13.3) - and the
	/// enclosing one must come back when the inner block ends.
	///
	/// Covers every reentrant path that runs *inside* fold(), including a
	/// sub-select folded partway through a GRAPH block (which is correctly
	/// evaluated against that same active graph). It deliberately does NOT
	/// cover FILTER/BIND expressions, whose AST is borrowed and rendered after
	/// fold() has returned and every guard has been destroyed - those carry
	/// their own copy on FilterNode/BindNode and reinstate it at render time.
	class ActiveGraphGuard {
	public:
		ActiveGraphGuard(TranslationContext &ctx, GraphConstraint graph) : ctx_(ctx), saved_(ctx.activeGraph_) {
			ctx_.activeGraph_ = std::move(graph);
		}
		~ActiveGraphGuard() {
			ctx_.activeGraph_ = saved_;
		}
		ActiveGraphGuard(const ActiveGraphGuard &) = delete;
		ActiveGraphGuard &operator=(const ActiveGraphGuard &) = delete;

	private:
		TranslationContext &ctx_;
		GraphConstraint saved_;
	};

	/// One more level of subquery nesting for as long as the guard is alive -
	/// a no-op in compact mode, since indent() always returns "" there.
	class SubqueryDepthGuard {
	public:
		explicit SubqueryDepthGuard(TranslationContext &ctx) : ctx_(ctx) {
			++ctx_.subqueryDepth_;
		}
		~SubqueryDepthGuard() {
			--ctx_.subqueryDepth_;
		}
		SubqueryDepthGuard(const SubqueryDepthGuard &) = delete;
		SubqueryDepthGuard &operator=(const SubqueryDepthGuard &) = delete;

	private:
		TranslationContext &ctx_;
	};

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

	/// Mark `varName` as needing a runtime type-tag column materialised
	/// alongside its lexical-form column, because some consumer's static
	/// TermInfo is not enough to answer what it asks about the term.
	///
	/// Deliberately keyed by variable *name* globally rather than per relation
	/// node: a tag column has to exist on every node between its producer and
	/// its consumer, and combineByName() pads a column missing from one arm with
	/// NULL - which the "tag is NULL iff the value is NULL" invariant would read
	/// as "unbound". One global flag makes every arm project it.
	///
	/// Set by markJoinKeyTagNeeds()/markExpressionTagNeeds() (TagDemand.h), which
	/// run either side of optimize() so that filter pushdown's per-arm static
	/// resolution gets first refusal - a query whose types the mapping pins down
	/// materialises no tag columns at all.
	void markNeedsTag(const std::string &varName) {
		tagVars_.insert(varName);
	}

	bool needsTag(const std::string &varName) const {
		return tagVars_.count(varName) != 0;
	}

	/// Mint a fresh CTE name ("cte1", "cte2", ...) - a distinct prefix and a
	/// separate counter from nextAlias()'s "t"+N sequence, so the two can
	/// never collide even though both are monotonic per-context counters.
	std::string nextCteName() {
		return "cte" + std::to_string(++cteCounter_);
	}

	/// Register one *recursive* WITH-clause entry (name already minted via
	/// nextCteName()). Shared across the whole translation of one query,
	/// including reentrant nested-subquery translation (SubSelectElement
	/// folding calls translateQueryPattern with this same ctx and splices the
	/// result as literal text into the outer tree), so a CTE registered while
	/// rendering a nested query is still valid at the single top-level WITH
	/// clause the outermost caller emits.
	///
	/// Property-path closure rendering is the only caller; hoisted rr:sqlQuery
	/// views go through viewCteName() into a separate list instead (see there).
	void addCte(const std::string &name, const std::string &bodySql) {
		pendingCtes_.push_back(CteDef {name, bodySql});
	}

	const std::vector<CteDef> &pendingCtes() const {
		return pendingCtes_;
	}

	/// Name of the CTE hoisting one rr:sqlQuery logical table, minting and
	/// registering the entry on first use. Keyed by logicalTableIdentity() -
	/// i.e. the exact rr:sqlQuery text - so every use site of one view, and
	/// equally two TriplesMaps that declare the identical query, all share a
	/// single CTE rather than re-inlining the text as a derived table apiece.
	/// That is what gives the engine a syntactic signal these are one relation,
	/// which it needs before it can consider materialising the view once.
	///
	/// Names come from the same nextCteName() counter as addCte()'s, so a view
	/// CTE and a closure CTE can never collide.
	const std::string &viewCteName(const std::string &identity, const std::string &bodySql) {
		std::map<std::string, std::string>::iterator it = viewCteNames_.find(identity);
		if (it != viewCteNames_.end()) {
			return it->second;
		}
		std::string name = nextCteName();
		viewCtes_.push_back(CteDef {name, bodySql});
		return viewCteNames_.insert(std::make_pair(identity, name)).first->second;
	}

	/// The hoisted view entries, in mint order. Held separately from
	/// pendingCtes_ rather than interleaved because they must be *emitted*
	/// first: a nested subquery is rendered (registering its closure CTEs)
	/// before the enclosing tree finishes constructing its own view sources, so
	/// one insertion-ordered list would not reliably put views ahead of the
	/// closures that may reference them. A view body is raw user SQL and never
	/// references another CTE, so front-placing them is always safe.
	const std::vector<CteDef> &viewCtes() const {
		return viewCtes_;
	}

	/// Whether the emitted WITH clause needs the RECURSIVE keyword: true iff
	/// some closure CTE was registered. Hoisted views alone do not need it, and
	/// are better off without - under RECURSIVE a real table named "cteN"
	/// inside a view body would be shadowed by our own CTE of that name.
	bool needsRecursiveWith() const {
		return !pendingCtes_.empty();
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
	std::set<std::string> tagVars_;
	std::string nowLiteral_;
	std::size_t cteCounter_ = 0;
	std::vector<CteDef> pendingCtes_;
	std::map<std::string, std::string> viewCteNames_;
	std::vector<CteDef> viewCtes_;
	bool prettyPrint_;
	std::size_t subqueryDepth_ = 0;
	GraphConstraint activeGraph_;
	ActiveDataset dataset_;
};

/// Mangle a SPARQL variable name into its projected SQL column name
/// (always quoted via the dialect, so case-sensitivity and any
/// PN_CHARS/Unicode edge cases in the variable name are never an issue).
std::string mangleVar(const std::string &sparqlVarName, const SqlDialect &dialect);

/// Mangle a SPARQL variable name into the SQL column name of its runtime
/// **type tag** - the companion VARCHAR carrying the RDF term's
/// kind/datatype/language in encodeTag()'s encoding, so the term dimension can
/// be evaluated per row rather than only at translation time.
///
/// A distinct prefix from mangleVar's "v_" and from the renderer's hidden
/// native-join-key "k_" columns, so the three can never collide.
///
/// INVARIANT, relied on by the OPTIONAL join's paired COALESCE and by
/// combineByName's NULL padding: this column is SQL NULL exactly when the
/// matching "v_" column is.
std::string mangleVarTag(const std::string &sparqlVarName, const SqlDialect &dialect);

/// Join `items` as a SELECT/GROUP BY/ORDER BY-style list: in compact mode,
/// ", "-separated on one line (the introducing keyword supplies its own
/// trailing space); in pretty mode, one item per line indented one level
/// under the keyword, with a leading comma before every item but the first.
/// Returns "" for an empty list.
std::string joinColumnList(const std::vector<std::string> &items, const TranslationContext &ctx);

/// Join `items` as a WHERE/HAVING/ON-style boolean-AND list: " AND "-joined
/// on one line in compact mode; one condition per line indented one level
/// under the introducing keyword, each preceded by "AND ", in pretty mode.
/// Returns "" for an empty list.
std::string joinConditions(const std::vector<std::string> &items, const TranslationContext &ctx);

} // namespace sparql2sql
