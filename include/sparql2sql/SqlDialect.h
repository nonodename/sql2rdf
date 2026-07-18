#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sparql2sql {

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

	/// Render a "LIMIT n OFFSET m" style clause (leading space included,
	/// empty string if neither is set).
	virtual std::string limitOffsetClause(bool hasLimit, int64_t limit, bool hasOffset, int64_t offset) const = 0;

	virtual std::string booleanLiteral(bool value) const = 0;

	/// Render "[NOT] EXISTS (<subquerySql>)".
	virtual std::string existsClause(bool negated, const std::string &subquerySql) const = 0;

	/// Attempt a numeric cast that yields SQL NULL (rather than an error) for
	/// non-numeric input.
	virtual std::string tryCastToDouble(const std::string &expr) const = 0;

	/// Render a (possibly negated) regular-expression match test.
	virtual std::string regexMatch(const std::string &text, const std::string &pattern, const std::string &flags,
	                               bool negated) const = 0;

	/// Render a GROUP_CONCAT-style string aggregate.
	virtual std::string stringAgg(const std::string &expr, const std::string &separatorLiteral,
	                              bool distinct) const = 0;

	/// Render a SAMPLE-style "pick any one value" aggregate.
	virtual std::string anyValueAgg(const std::string &expr) const = 0;

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
};

} // namespace sparql2sql
