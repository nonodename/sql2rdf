#include "sparql2sql/TypeCatalog.h"

#include <algorithm>
#include <cctype>

#include "sparql2sql/TermInfo.h"

namespace sparql2sql {

namespace {

// Uppercase and strip any "(...)" precision/width suffix (e.g. DECIMAL(18,2),
// VARCHAR(255)) so type names compare on their base kind.
std::string normalizeType(const std::string &raw) {
	std::string out;
	out.reserve(raw.size());
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

bool iequals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return true;
}

} // namespace

std::string TypeCatalog::typeOf(const std::string &table, const std::string &column) const {
	// Case-insensitive lookup: SQL identifiers are case-insensitive, and the
	// mapping's declared table/column names (e.g. "COMPANY") often differ in
	// case from what a backend's information_schema reports (DuckDB folds
	// unquoted identifiers to lowercase).
	for (const auto &t : columnTypes) {
		if (!iequals(t.first, table)) {
			continue;
		}
		for (const auto &c : t.second) {
			if (iequals(c.first, column)) {
				return c.second;
			}
		}
	}
	return std::string();
}

bool TypeCatalog::isNotNull(const std::string &table, const std::string &column) const {
	// Case-insensitive on both levels, for the same reason typeOf is.
	for (const auto &t : notNullColumns) {
		if (!iequals(t.first, table)) {
			continue;
		}
		for (const auto &c : t.second) {
			if (iequals(c, column)) {
				return true;
			}
		}
	}
	return false;
}

bool TypeCatalog::coversUniqueKey(const std::string &table, const std::set<std::string> &columns) const {
	for (const auto &t : uniqueKeys) {
		if (!iequals(t.first, table)) {
			continue;
		}
		for (const auto &key : t.second) {
			// An empty constraint column list proves nothing - it would otherwise
			// vacuously "cover" every projection.
			if (key.empty()) {
				continue;
			}
			bool contained = true;
			for (const auto &keyCol : key) {
				bool found = false;
				for (const auto &have : columns) {
					if (iequals(have, keyCol)) {
						found = true;
						break;
					}
				}
				if (!found) {
					contained = false;
					break;
				}
			}
			if (contained) {
				return true;
			}
		}
	}
	return false;
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

bool TypeCatalog::isStringType(const std::string &table, const std::string &column) const {
	std::string type = typeOf(table, column);
	if (type.empty()) {
		return false;
	}
	return categoryOf(normalizeType(type)) == TypeCategory::String;
}

std::string naturalXsdDatatype(const std::string &sqlTypeName) {
	// R2RML Section 10.2's table, plus the spellings DuckDB and common
	// information_schema implementations actually report. Two entries look like
	// bugs and are not:
	//   - FLOAT/REAL map to xsd:double, not xsd:float. That is what Section 10.2
	//     says; don't "fix" it.
	//   - TIMESTAMP WITH TIME ZONE is deliberately absent. Section 10.2 gives
	//     xsd:dateTime, but only with a correctly rendered UTC offset, which
	//     this translator's lexical form does not carry - the same reason
	//     TIMEZONE()/TZ() are unsupported.
	// Multi-word names must appear verbatim: normalizeType truncates at '(' and
	// trims trailing spaces, so it leaves interior spaces intact.
	struct Entry {
		const char *sqlType;
		const char *xsdIri;
	};
	static const Entry kTable[] = {
	    {"CHARACTER", xsd::kString},
	    {"CHAR", xsd::kString},
	    {"BPCHAR", xsd::kString},
	    {"VARCHAR", xsd::kString},
	    {"CHARACTER VARYING", xsd::kString},
	    {"NCHAR", xsd::kString},
	    {"NVARCHAR", xsd::kString},
	    {"TEXT", xsd::kString},
	    {"STRING", xsd::kString},
	    {"CLOB", xsd::kString},

	    {"BINARY", xsd::kHexBinary},
	    {"VARBINARY", xsd::kHexBinary},
	    {"BLOB", xsd::kHexBinary},
	    {"BYTEA", xsd::kHexBinary},

	    {"TINYINT", xsd::kInteger},
	    {"SMALLINT", xsd::kInteger},
	    {"INT1", xsd::kInteger},
	    {"INT2", xsd::kInteger},
	    {"UTINYINT", xsd::kInteger},
	    {"USMALLINT", xsd::kInteger},
	    {"INTEGER", xsd::kInteger},
	    {"INT", xsd::kInteger},
	    {"INT4", xsd::kInteger},
	    {"UINTEGER", xsd::kInteger},
	    {"BIGINT", xsd::kInteger},
	    {"INT8", xsd::kInteger},
	    {"HUGEINT", xsd::kInteger},
	    {"UBIGINT", xsd::kInteger},
	    {"UHUGEINT", xsd::kInteger},

	    {"DECIMAL", xsd::kDecimal},
	    {"NUMERIC", xsd::kDecimal},

	    {"FLOAT", xsd::kDouble},
	    {"REAL", xsd::kDouble},
	    {"FLOAT4", xsd::kDouble},
	    {"DOUBLE", xsd::kDouble},
	    {"DOUBLE PRECISION", xsd::kDouble},
	    {"FLOAT8", xsd::kDouble},

	    {"BOOLEAN", xsd::kBoolean},
	    {"BOOL", xsd::kBoolean},

	    {"DATE", xsd::kDate},

	    {"TIME", xsd::kTime},
	    {"TIME WITHOUT TIME ZONE", xsd::kTime},

	    {"TIMESTAMP", xsd::kDateTime},
	    {"DATETIME", xsd::kDateTime},
	    {"TIMESTAMP WITHOUT TIME ZONE", xsd::kDateTime},
	};

	const std::string normalized = normalizeType(sqlTypeName);
	if (normalized.empty()) {
		return std::string();
	}
	for (const auto &entry : kTable) {
		if (normalized == entry.sqlType) {
			return entry.xsdIri;
		}
	}
	return std::string();
}

} // namespace sparql2sql
