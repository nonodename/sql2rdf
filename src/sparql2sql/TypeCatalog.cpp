#include "sparql2sql/TypeCatalog.h"

#include <algorithm>
#include <cctype>

namespace sparql2sql {

namespace {

// Uppercase and strip any "(...)" precision/width suffix (e.g. DECIMAL(18,2),
// VARCHAR(255)) so type names compare on their base kind.
std::string normalizeType(const std::string &raw) {
	std::string out;
	for (char c : raw) {
		if (c == '(') {
			break;
		}
		out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	// Trim trailing spaces.
	while (!out.empty() && out.back() == ' ') {
		out.pop_back();
	}
	return out;
}

// Coarse comparability category. Only same-category, non-Other columns are
// safe to compare on their native (non-VARCHAR-cast) values. Integer and Float
// are intentionally kept distinct (conservative): mixing them could differ
// from a lexical comparison.
enum class TypeCategory { Integer, Float, String, Other };

TypeCategory categoryOf(const std::string &normalized) {
	if (normalized == "TINYINT" || normalized == "SMALLINT" || normalized == "INTEGER" || normalized == "INT" ||
	    normalized == "BIGINT" || normalized == "HUGEINT" || normalized == "UTINYINT" || normalized == "USMALLINT" ||
	    normalized == "UINTEGER" || normalized == "UBIGINT" || normalized == "UHUGEINT" || normalized == "INT4" ||
	    normalized == "INT8" || normalized == "INT2" || normalized == "INT1") {
		return TypeCategory::Integer;
	}
	if (normalized == "DOUBLE" || normalized == "REAL" || normalized == "FLOAT" || normalized == "FLOAT4" ||
	    normalized == "FLOAT8" || normalized == "DECIMAL" || normalized == "NUMERIC") {
		return TypeCategory::Float;
	}
	if (normalized == "VARCHAR" || normalized == "CHAR" || normalized == "TEXT" || normalized == "STRING" ||
	    normalized == "BPCHAR") {
		return TypeCategory::String;
	}
	return TypeCategory::Other;
}

} // namespace

std::string TypeCatalog::typeOf(const std::string &table, const std::string &column) const {
	auto t = columnTypes.find(table);
	if (t == columnTypes.end()) {
		return std::string();
	}
	auto c = t->second.find(column);
	if (c == t->second.end()) {
		return std::string();
	}
	return c->second;
}

bool TypeCatalog::comparable(const std::string &tableA, const std::string &columnA, const std::string &tableB,
                            const std::string &columnB) const {
	std::string a = typeOf(tableA, columnA);
	std::string b = typeOf(tableB, columnB);
	if (a.empty() || b.empty()) {
		return false;
	}
	TypeCategory ca = categoryOf(normalizeType(a));
	TypeCategory cb = categoryOf(normalizeType(b));
	if (ca == TypeCategory::Other || cb == TypeCategory::Other) {
		return false;
	}
	return ca == cb;
}

} // namespace sparql2sql
