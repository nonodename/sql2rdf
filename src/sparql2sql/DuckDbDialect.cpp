#include "sparql2sql/DuckDbDialect.h"

namespace sparql2sql {

namespace {

std::string escapeAndWrap(const std::string &value, char quoteChar) {
	std::string out;
	out.reserve(value.size() + 2);
	out += quoteChar;
	for (char c : value) {
		if (c == quoteChar) {
			out += quoteChar;
		}
		out += c;
	}
	out += quoteChar;
	return out;
}

} // namespace

std::string DuckDbDialect::name() const {
	return "duckdb";
}

std::string DuckDbDialect::quoteIdentifier(const std::string &identifier) const {
	return escapeAndWrap(identifier, '"');
}

std::string DuckDbDialect::stringLiteral(const std::string &value) const {
	return escapeAndWrap(value, '\'');
}

std::string DuckDbDialect::concat(const std::vector<std::string> &parts) const {
	if (parts.empty()) {
		return stringLiteral("");
	}
	std::string out = "(";
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (i > 0) {
			out += " || ";
		}
		out += parts[i];
	}
	out += ")";
	return out;
}

std::string DuckDbDialect::limitOffsetClause(bool hasLimit, int64_t limit, bool hasOffset, int64_t offset) const {
	std::string out;
	if (hasLimit) {
		out += " LIMIT " + std::to_string(limit);
	} else if (hasOffset) {
		// DuckDB (like Postgres) requires a LIMIT before a bare OFFSET.
		out += " LIMIT ALL";
	}
	if (hasOffset) {
		out += " OFFSET " + std::to_string(offset);
	}
	return out;
}

std::string DuckDbDialect::booleanLiteral(bool value) const {
	return value ? "TRUE" : "FALSE";
}

std::string DuckDbDialect::existsClause(bool negated, const std::string &subquerySql) const {
	return std::string(negated ? "NOT EXISTS (" : "EXISTS (") + subquerySql + ")";
}

std::string DuckDbDialect::tryCastToDouble(const std::string &expr) const {
	return "TRY_CAST(" + expr + " AS DOUBLE)";
}

std::string DuckDbDialect::regexMatch(const std::string &text, const std::string &pattern, const std::string &flags,
                                      bool negated) const {
	std::string call = "regexp_matches(" + text + ", " + pattern;
	if (!flags.empty()) {
		call += ", " + stringLiteral(flags);
	}
	call += ")";
	return negated ? "(NOT " + call + ")" : call;
}

std::string DuckDbDialect::stringAgg(const std::string &expr, const std::string &separatorLiteral,
                                     bool distinct) const {
	return std::string("string_agg(") + (distinct ? "DISTINCT " : "") + expr + ", " + separatorLiteral + ")";
}

std::string DuckDbDialect::anyValueAgg(const std::string &expr) const {
	return "any_value(" + expr + ")";
}

std::string DuckDbDialect::combineByName(bool all, const std::vector<std::string> &armSqls) const {
	std::string keyword = all ? " UNION ALL BY NAME " : " UNION BY NAME ";
	std::string out;
	for (std::size_t i = 0; i < armSqls.size(); ++i) {
		if (i > 0) {
			out += keyword;
		}
		out += "(" + armSqls[i] + ")";
	}
	return out;
}

} // namespace sparql2sql
