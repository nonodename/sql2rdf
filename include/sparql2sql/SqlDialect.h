#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sparql2sql {

struct TemplateSegment;

/// Abstracts the handful of SQL syntax points that genuinely vary across
/// relational engines and are actually exercised by the translator.
/// Constructs close enough to universal across engines (UPPER/LOWER/LENGTH/
/// ABS/CEIL/FLOOR/ROUND/COALESCE/CASE WHEN/CONTAINS/MD5/SHA256, ...) are
/// emitted directly by ExpressionTranslator rather than routed through this
/// interface - only add a seam here when a second dialect actually needs one.
class SqlDialect {
public:
	virtual ~SqlDialect();

	/// Short, lowercase dialect name (e.g. "duckdb"); matches the string
	/// accepted by createDialect().
	virtual std::string name() const = 0;

	/// Quote a table/column/alias identifier for safe embedding in generated
	/// SQL (escaping any embedded quote characters).
	virtual std::string quoteIdentifier(const std::string &identifier) const = 0;

	/// Quote and escape a string as a SQL string literal.
	virtual std::string stringLiteral(const std::string &value) const = 0;

	/// Concatenate a list of already-valid SQL scalar expressions into one
	/// string-valued expression.
	virtual std::string concat(const std::vector<std::string> &parts) const = 0;

	/// Wrap a SQL scalar expression so it evaluates, at query time, to the
	/// RFC3986 percent-encoding of its value (unreserved: A-Z a-z 0-9 - _ . ~,
	/// everything else as an uppercase %XX escape). Must match
	/// r2rml::AbstractMap::percentEncode's behavior exactly, since this is
	/// used to reconstruct rr:template-generated terms so they agree with
	/// forward R2RML generation.
	virtual std::string percentEncode(const std::string &expr) const = 0;

	/// Render a "LIMIT n OFFSET m" style clause (leading space included,
	/// empty string if neither is set).
	virtual std::string limitOffsetClause(bool hasLimit, int64_t limit, bool hasOffset, int64_t offset) const = 0;

	virtual std::string booleanLiteral(bool value) const = 0;

	/// Render "[NOT] EXISTS (<subquerySql>)".
	virtual std::string existsClause(bool negated, const std::string &subquerySql) const = 0;

	/// Attempt a numeric cast that yields SQL NULL (rather than an error) for
	/// non-numeric input.
	virtual std::string tryCastToDouble(const std::string &expr) const = 0;

	/// Attempt a cast to a 64-bit integer type that yields SQL NULL (rather
	/// than an error) for non-integral input. Used to keep arithmetic over
	/// statically integral operands integral, so `?a + 1` renders "10" rather
	/// than the "10.0" a DOUBLE round-trip would give.
	virtual std::string tryCastToBigInt(const std::string &expr) const = 0;

	/// Attempt a cast to a timestamp type that yields SQL NULL (rather than
	/// an error) for non-timestamp-parseable input.
	virtual std::string tryCastToTimestamp(const std::string &expr) const = 0;

	/// Attempt a cast to a boolean type that yields SQL NULL (rather than an
	/// error) for non-boolean-parseable input.
	virtual std::string tryCastToBoolean(const std::string &expr) const = 0;

	/// Attempt a cast to a date type that yields SQL NULL (rather than an
	/// error) for non-date-parseable input.
	virtual std::string tryCastToDate(const std::string &expr) const = 0;

	/// Attempt a cast to a fixed-point decimal type that yields SQL NULL
	/// (rather than an error) for non-numeric input.
	virtual std::string tryCastToDecimal(const std::string &expr) const = 0;

	/// Render a (possibly negated) regular-expression match test.
	virtual std::string regexMatch(const std::string &text, const std::string &pattern, const std::string &flags,
	                               bool negated) const = 0;

	/// Render a GROUP_CONCAT-style string aggregate.
	virtual std::string stringAgg(const std::string &expr, const std::string &separatorLiteral,
	                              bool distinct) const = 0;

	/// Render a SAMPLE-style "pick any one value" aggregate.
	virtual std::string anyValueAgg(const std::string &expr) const = 0;

	/// Render "the value of `expr` in the row where `orderBy` is smallest
	/// (`wantMax` false) or largest (true)".
	///
	/// Needed because a MIN()/MAX() over terms whose dimension varies per row has
	/// to return the winning row's *tag* as well as its lexical form, and the two
	/// must come from the same row - which a second independent MIN() would not
	/// guarantee.
	virtual std::string argMinMaxBy(const std::string &expr, const std::string &orderBy, bool wantMax) const = 0;

	/// Combine several already-valid "SELECT ..." statements via a
	/// name-matching (rather than positional) union, auto-padding any
	/// column present in one arm but not another with NULL. DuckDB's
	/// "UNION [ALL] BY NAME" extension implements exactly the paper's
	/// schema-extending union (Rule 10/simplification 5) without any
	/// manual padding logic on the translator's part; a future
	/// non-DuckDB dialect without this extension would need to implement
	/// the padding itself. `all=false` also deduplicates the combined
	/// result (SQL UNION's usual behavior).
	virtual std::string combineByName(bool all, const std::vector<std::string> &armSqls) const = 0;

	// ---------------------------------------------------------------------
	// RDF term-logic hooks.
	//
	// Each one renders a piece of RDF term logic the translator otherwise
	// spells as inline (often deeply nested CASE) SQL. The defaults return that
	// inline SQL byte for byte, so a dialect that overrides none of them emits
	// exactly what the translator always emitted. A dialect may override them to
	// call engine-registered scalar functions instead (see DuckDbUdfDialect).
	//
	// An override must be *result-identical* to the default, and the translator
	// only ever calls an override where an opaque function call cannot cost the
	// engine a pushdown: never for a join key, and never while the optimizer is
	// building an SPJ block's WHERE list. In those places it calls
	// nativeDialect()'s version instead (see TranslationContext::dialect()).
	// ---------------------------------------------------------------------

	/// The dialect whose hooks are guaranteed to render native SQL. The
	/// translator switches to it wherever a UDF would be unsafe (join keys and
	/// optimizer-folded WHERE conjuncts). Default: this dialect, which is
	/// already native unless it overrides a hook below.
	virtual const SqlDialect &nativeDialect() const;

	/// The lexical form of an rr:IRI rr:template term, built from the
	/// template's segments over `sourceAlias`'s columns, with every substituted
	/// value percent-encoded. Used for an SPJ block's SELECT list only - the
	/// same term's join-key/WHERE spelling always comes from buildProjectionSql.
	/// Default: buildProjectionSql(segments, sourceAlias, *this, true).
	virtual std::string iriTemplateProjection(const std::vector<TemplateSegment> &segments,
	                                          const std::string &sourceAlias) const;

	/// The type tag of whichever term a `COALESCE(v1, v2, ...)` returns:
	/// `t_i` for the first `v_i` that is non-NULL, else NULL. Each pair is
	/// (value SQL, tag SQL). Default: TagSql.h's CASE form (see coalescedTag).
	virtual std::string coalesceTag(const std::vector<std::pair<std::string, std::string>> &valueTagPairs) const;

	/// A tag's value-space id (TagSql.h's kValueSpace*), NULL for a NULL tag.
	/// Default: tagValueSpace().
	virtual std::string termValueSpace(const std::string &tagSql) const;

	/// SPARQL 1.1 Section 15.1's kind rank of a tag. Default: tagKindRank().
	virtual std::string termKindRank(const std::string &tagSql) const;

	/// RDF term / SPARQL value (in)equality dispatched on both tags at run time.
	/// Default: dynamicEquality() with untypedComparison() as its fallback.
	virtual std::string termEquality(const std::string &leftSql, const std::string &leftTag,
	                                 const std::string &rightSql, const std::string &rightTag, bool negated) const;

	/// SPARQL ordering comparison (`op` one of < > <= >=) dispatched on both tags
	/// at run time. Default: dynamicOrdering() with untypedComparison() as its
	/// fallback.
	virtual std::string termOrdering(const std::string &leftSql, const std::string &leftTag,
	                                 const std::string &rightSql, const std::string &rightTag,
	                                 const std::string &op) const;

	/// The numeric-aware untyped comparison used when neither tag is available
	/// (`op` one of = <> < > <= >=). Default: untypedComparison().
	virtual std::string untypedCompare(const std::string &leftSql, const std::string &rightSql,
	                                   const std::string &op) const;
};

} // namespace sparql2sql
