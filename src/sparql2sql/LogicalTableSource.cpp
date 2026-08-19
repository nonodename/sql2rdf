#include "sparql2sql/LogicalTableSource.h"

#include <cctype>
#include <cstring>
#include <set>

#include "r2rml/BaseTableOrView.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLView.h"
#include "r2rml/TriplesMap.h"

namespace sparql2sql {

namespace {

std::string trim(const std::string &s) {
	std::size_t begin = s.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) {
		return std::string();
	}
	std::size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(begin, end - begin + 1);
}

bool isIdentChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Case-insensitive `keyword` match at `pos`, requiring a non-identifier
// character (or the string boundary) on both sides, so e.g. "from" never
// matches inside "fromage".
bool matchKeywordAt(const std::string &s, std::size_t pos, const char *keyword) {
	std::size_t len = std::strlen(keyword);
	if (pos + len > s.size()) {
		return false;
	}
	for (std::size_t i = 0; i < len; ++i) {
		if (std::tolower(static_cast<unsigned char>(s[pos + i])) !=
		    std::tolower(static_cast<unsigned char>(keyword[i]))) {
			return false;
		}
	}
	if (pos > 0 && isIdentChar(s[pos - 1])) {
		return false;
	}
	if (pos + len < s.size() && isIdentChar(s[pos + len])) {
		return false;
	}
	return true;
}

bool iequalsWord(const std::string &s, const char *word) {
	std::size_t len = std::strlen(word);
	if (s.size() != len) {
		return false;
	}
	for (std::size_t i = 0; i < len; ++i) {
		if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(word[i]))) {
			return false;
		}
	}
	return true;
}

bool isIntegerLiteral(const std::string &s) {
	std::size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
	if (i >= s.size()) {
		return false;
	}
	for (std::size_t j = i; j < s.size(); ++j) {
		if (!std::isdigit(static_cast<unsigned char>(s[j]))) {
			return false;
		}
	}
	return true;
}

// Find the first top-level (paren depth 0, outside a '...' string literal)
// occurrence of `keyword` in `s[from, s.size())`; npos if none.
std::size_t findTopLevelKeyword(const std::string &s, std::size_t from, const char *keyword) {
	int depth = 0;
	bool inString = false;
	for (std::size_t i = from; i < s.size(); ++i) {
		char c = s[i];
		if (inString) {
			if (c == '\'') {
				if (i + 1 < s.size() && s[i + 1] == '\'') {
					++i;
					continue;
				}
				inString = false;
			}
			continue;
		}
		if (c == '\'') {
			inString = true;
			continue;
		}
		if (c == '(') {
			++depth;
			continue;
		}
		if (c == ')') {
			--depth;
			continue;
		}
		if (depth == 0 && matchKeywordAt(s, i, keyword)) {
			return i;
		}
	}
	return std::string::npos;
}

// Split `s` on top-level (paren depth 0, outside a '...' string literal)
// commas.
std::vector<std::string> splitTopLevel(const std::string &s) {
	std::vector<std::string> parts;
	int depth = 0;
	bool inString = false;
	std::size_t start = 0;
	for (std::size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (inString) {
			if (c == '\'') {
				if (i + 1 < s.size() && s[i + 1] == '\'') {
					++i;
					continue;
				}
				inString = false;
			}
			continue;
		}
		if (c == '\'') {
			inString = true;
			continue;
		}
		if (c == '(') {
			++depth;
			continue;
		}
		if (c == ')') {
			--depth;
			continue;
		}
		if (c == ',' && depth == 0) {
			parts.push_back(s.substr(start, i - start));
			start = i + 1;
		}
	}
	parts.push_back(s.substr(start));
	return parts;
}

// Split one SELECT-list item on its last top-level "AS", e.g.
// "true AS FLAG" -> ("true", "FLAG"). False if there is no top-level AS (an
// unaliased item can't name a column, so it's out of scope for this scan).
bool splitAliasedItem(const std::string &item, std::string &exprOut, std::string &aliasOut) {
	std::size_t asPos = std::string::npos;
	int depth = 0;
	bool inString = false;
	for (std::size_t i = 0; i < item.size(); ++i) {
		char c = item[i];
		if (inString) {
			if (c == '\'') {
				if (i + 1 < item.size() && item[i + 1] == '\'') {
					++i;
					continue;
				}
				inString = false;
			}
			continue;
		}
		if (c == '\'') {
			inString = true;
			continue;
		}
		if (c == '(') {
			++depth;
			continue;
		}
		if (c == ')') {
			--depth;
			continue;
		}
		if (depth == 0 && matchKeywordAt(item, i, "as")) {
			asPos = i;
		}
	}
	if (asPos == std::string::npos) {
		return false;
	}
	exprOut = trim(item.substr(0, asPos));
	aliasOut = trim(item.substr(asPos + 2));
	if (aliasOut.size() >= 2 && aliasOut.front() == '"' && aliasOut.back() == '"') {
		aliasOut = aliasOut.substr(1, aliasOut.size() - 2);
	}
	return true;
}

// Classify a SELECT-list expression as one of the three literal forms this
// scan recognizes, filling `renderingOut` with its VARCHAR-cast rendering.
// False for anything else (a real column reference, NULL, a function call,
// a decimal literal whose VARCHAR rendering isn't guaranteed identical to
// its source text) - always the safe answer, since the caller only acts on a
// positive classification.
bool literalVarcharRendering(const std::string &expr, std::string &renderingOut) {
	if (expr.size() >= 2 && expr.front() == '\'' && expr.back() == '\'') {
		renderingOut = unquoteSqlStringLiteral(expr);
		return true;
	}
	if (iequalsWord(expr, "true")) {
		renderingOut = "true";
		return true;
	}
	if (iequalsWord(expr, "false")) {
		renderingOut = "false";
		return true;
	}
	if (isIntegerLiteral(expr)) {
		renderingOut = expr;
		return true;
	}
	return false;
}

} // namespace

std::string logicalTableIdentity(const r2rml::LogicalTable &logicalTable) {
	if (const auto *base = dynamic_cast<const r2rml::BaseTableOrView *>(&logicalTable)) {
		return base->tableName;
	}
	if (const auto *view = dynamic_cast<const r2rml::R2RMLView *>(&logicalTable)) {
		return "view:" + view->sqlQuery;
	}
	return std::string();
}

std::string stripTrailingSemicolon(std::string sql) {
	std::size_t end = sql.find_last_not_of(" \t\r\n");
	if (end == std::string::npos) {
		return std::string();
	}
	sql.erase(end + 1);
	if (!sql.empty() && sql.back() == ';') {
		sql.pop_back();
		// Trim again: "SELECT 1 ;" would otherwise keep the space the
		// terminator was hiding.
		end = sql.find_last_not_of(" \t\r\n");
		sql.erase(end == std::string::npos ? 0 : end + 1);
	}
	return sql;
}

std::vector<ViewSource> mappingViewSources(const r2rml::R2RMLMapping &mapping) {
	std::vector<ViewSource> out;
	out.reserve(mapping.triplesMaps.size());
	std::set<std::string> seen;
	for (const auto &tmPtr : mapping.triplesMaps) {
		if (!tmPtr || !tmPtr->logicalTable) {
			continue;
		}
		const auto *view = dynamic_cast<const r2rml::R2RMLView *>(tmPtr->logicalTable.get());
		if (view == nullptr) {
			continue;
		}
		ViewSource source;
		source.identity = logicalTableIdentity(*tmPtr->logicalTable);
		source.sql = stripTrailingSemicolon(view->sqlQuery);
		if (source.sql.empty() || !seen.insert(source.identity).second) {
			continue;
		}
		out.push_back(source);
	}
	return out;
}

std::string unquoteSqlStringLiteral(const std::string &quoted) {
	if (quoted.size() < 2 || quoted.front() != '\'' || quoted.back() != '\'') {
		return quoted;
	}
	std::string inner = quoted.substr(1, quoted.size() - 2);
	std::string out;
	out.reserve(inner.size());
	for (std::size_t i = 0; i < inner.size(); ++i) {
		if (inner[i] == '\'' && i + 1 < inner.size() && inner[i + 1] == '\'') {
			out += '\'';
			++i;
		} else {
			out += inner[i];
		}
	}
	return out;
}

std::map<std::string, std::string> detectConstantSelectColumns(const std::string &sql) {
	std::map<std::string, std::string> out;
	std::size_t selectPos = sql.find_first_not_of(" \t\r\n");
	if (selectPos == std::string::npos || !matchKeywordAt(sql, selectPos, "select")) {
		return out; // not a plain "SELECT ... FROM ..." shape this scan understands.
	}
	selectPos += 6;
	std::size_t fromPos = findTopLevelKeyword(sql, selectPos, "from");
	if (fromPos == std::string::npos) {
		return out;
	}
	std::string selectList = sql.substr(selectPos, fromPos - selectPos);
	for (const auto &rawItem : splitTopLevel(selectList)) {
		std::string item = trim(rawItem);
		if (item.empty()) {
			continue;
		}
		std::string expr;
		std::string alias;
		std::string rendering;
		if (splitAliasedItem(item, expr, alias) && !alias.empty() && literalVarcharRendering(expr, rendering)) {
			out[alias] = rendering;
		}
	}
	return out;
}

} // namespace sparql2sql
