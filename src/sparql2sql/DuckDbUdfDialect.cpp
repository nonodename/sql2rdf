#include "sparql2sql/DuckDbUdfDialect.h"

#include <stdexcept>

#include "sparql2sql/TemplateUtil.h"

namespace sparql2sql {

DuckDbUdfDialect::DuckDbUdfDialect(Coverage coverage) : coverage_(coverage) {
}

std::string DuckDbUdfDialect::name() const {
	return coverage_ == Coverage::All ? "duckdb-udf-all" : "duckdb-udf";
}

const SqlDialect &DuckDbUdfDialect::nativeDialect() const {
	return native_;
}

std::string DuckDbUdfDialect::iriTemplateProjection(const std::vector<TemplateSegment> &segments,
                                                    const std::string &sourceAlias) const {
	// Strictly alternating literal/value arguments, starting and ending with a
	// literal (possibly ''), so the function never has to be told which
	// argument is which. A template without placeholders is already a constant
	// and gains nothing from a call.
	if (coverage_ != Coverage::All) {
		return native_.iriTemplateProjection(segments, sourceAlias);
	}
	bool anyPlaceholder = false;
	for (const auto &seg : segments) {
		anyPlaceholder = anyPlaceholder || seg.isPlaceholder;
	}
	if (!anyPlaceholder) {
		return native_.iriTemplateProjection(segments, sourceAlias);
	}
	std::string sql = "sql2rdf_iri(";
	bool expectLiteral = true;
	for (const auto &seg : segments) {
		if (seg.isPlaceholder) {
			if (expectLiteral) {
				sql += stringLiteral("") + ", ";
			}
			sql += "CAST(" + sourceAlias + "." + quoteIdentifier(seg.text) + " AS VARCHAR), ";
			expectLiteral = true;
		} else {
			if (!expectLiteral) {
				// Two literal segments in a row cannot come out of parseTemplate,
				// but keep the alternation invariant regardless.
				sql += "NULL, ";
			}
			sql += stringLiteral(seg.text) + ", ";
			expectLiteral = false;
		}
	}
	if (expectLiteral) {
		sql += stringLiteral("");
	} else {
		sql.resize(sql.size() - 2);
	}
	return sql + ")";
}

std::string DuckDbUdfDialect::coalesceTag(const std::vector<std::pair<std::string, std::string>> &valueTagPairs) const {
	if (coverage_ != Coverage::All) {
		return native_.coalesceTag(valueTagPairs);
	}
	std::string sql = "sql2rdf_tag_coalesce(";
	for (std::size_t i = 0; i < valueTagPairs.size(); ++i) {
		sql += (i > 0 ? ", " : "");
		sql += valueTagPairs[i].first + ", " + valueTagPairs[i].second;
	}
	return sql + ")";
}

std::string DuckDbUdfDialect::termValueSpace(const std::string &tagSql) const {
	return "sql2rdf_value_space(" + tagSql + ")";
}

std::string DuckDbUdfDialect::termKindRank(const std::string &tagSql) const {
	return "sql2rdf_kind_rank(" + tagSql + ")";
}

std::string DuckDbUdfDialect::termEquality(const std::string &leftSql, const std::string &leftTag,
                                           const std::string &rightSql, const std::string &rightTag,
                                           bool negated) const {
	return std::string(negated ? "sql2rdf_ne(" : "sql2rdf_eq(") + leftSql + ", " + leftTag + ", " + rightSql + ", " +
	       rightTag + ")";
}

std::string DuckDbUdfDialect::termOrdering(const std::string &leftSql, const std::string &leftTag,
                                           const std::string &rightSql, const std::string &rightTag,
                                           const std::string &op) const {
	return std::string("sql2rdf_") + comparisonFunctionSuffix(op) + "(" + leftSql + ", " + leftTag + ", " + rightSql +
	       ", " + rightTag + ")";
}

std::string DuckDbUdfDialect::untypedCompare(const std::string &leftSql, const std::string &rightSql,
                                             const std::string &op) const {
	return std::string("sql2rdf_untyped_") + comparisonFunctionSuffix(op) + "(" + leftSql + ", " + rightSql + ")";
}

const char *comparisonFunctionSuffix(const std::string &op) {
	if (op == "=") {
		return "eq";
	}
	if (op == "<>") {
		return "ne";
	}
	if (op == "<") {
		return "lt";
	}
	if (op == ">") {
		return "gt";
	}
	if (op == "<=") {
		return "le";
	}
	if (op == ">=") {
		return "ge";
	}
	throw std::invalid_argument("comparisonFunctionSuffix: unsupported operator '" + op + "'");
}

} // namespace sparql2sql
