#include "sparql2sql/SqlDialect.h"

#include "sparql2sql/TagSql.h"
#include "sparql2sql/TemplateUtil.h"

namespace sparql2sql {

SqlDialect::~SqlDialect() = default;

const SqlDialect &SqlDialect::nativeDialect() const {
	return *this;
}

std::string SqlDialect::iriTemplateProjection(const std::vector<TemplateSegment> &segments,
                                              const std::string &sourceAlias) const {
	return buildProjectionSql(segments, sourceAlias, *this, true);
}

std::string SqlDialect::coalesceTag(const std::vector<std::pair<std::string, std::string>> &valueTagPairs) const {
	return inlineCoalesceTag(valueTagPairs);
}

std::string SqlDialect::termValueSpace(const std::string &tagSql) const {
	return tagValueSpace(tagSql, *this);
}

std::string SqlDialect::termKindRank(const std::string &tagSql) const {
	return tagKindRank(tagSql, *this);
}

std::string SqlDialect::termEquality(const std::string &leftSql, const std::string &leftTag,
                                     const std::string &rightSql, const std::string &rightTag, bool negated) const {
	const std::string fallback = untypedComparison(leftSql, rightSql, negated ? "<>" : "=", *this);
	return dynamicEquality(leftSql, leftTag, rightSql, rightTag, negated, fallback, *this);
}

std::string SqlDialect::termOrdering(const std::string &leftSql, const std::string &leftTag,
                                     const std::string &rightSql, const std::string &rightTag,
                                     const std::string &op) const {
	const std::string fallback = untypedComparison(leftSql, rightSql, op, *this);
	return dynamicOrdering(leftSql, leftTag, rightSql, rightTag, op, fallback, *this);
}

std::string SqlDialect::untypedCompare(const std::string &leftSql, const std::string &rightSql,
                                       const std::string &op) const {
	return untypedComparison(leftSql, rightSql, op, *this);
}

} // namespace sparql2sql
