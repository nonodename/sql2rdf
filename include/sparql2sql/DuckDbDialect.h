#pragma once

#include "sparql2sql/SqlDialect.h"

namespace sparql2sql {

/// The only supported SQL dialect for now. DuckDB is broadly Postgres-
/// compatible, which keeps this implementation small.
class DuckDbDialect : public SqlDialect {
public:
	std::string name() const override;
	std::string quoteIdentifier(const std::string &identifier) const override;
	std::string stringLiteral(const std::string &value) const override;
	std::string concat(const std::vector<std::string> &parts) const override;
	std::string limitOffsetClause(bool hasLimit, int64_t limit, bool hasOffset, int64_t offset) const override;
	std::string booleanLiteral(bool value) const override;
	std::string existsClause(bool negated, const std::string &subquerySql) const override;
	std::string tryCastToDouble(const std::string &expr) const override;
	std::string regexMatch(const std::string &text, const std::string &pattern, const std::string &flags,
	                       bool negated) const override;
	std::string stringAgg(const std::string &expr, const std::string &separatorLiteral, bool distinct) const override;
	std::string anyValueAgg(const std::string &expr) const override;
	std::string combineByName(bool all, const std::vector<std::string> &armSqls) const override;
};

} // namespace sparql2sql
