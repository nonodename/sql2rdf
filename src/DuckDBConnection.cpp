#include "DuckDBConnection.h"
#include "r2rml/MapSQLRow.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include "sparql2sql/DuckDbUdfDialect.h"
#include "sparql2sql/TagSql.h"
#include "sparql2sql/TermInfo.h"

#include <duckdb.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace r2rml {

namespace {

// ---------------------------------------------------------------------------
// DuckDBSQLValue
//
// A single cell's type and lexical form, converted eagerly while the owning
// duckdb_result is still live: the data chunk/vector accessors this uses
// only operate against a live duckdb_data_chunk, unlike the C++ duckdb::Value
// this replaced, which owned a copy of its data and could convert lazily on
// first access.
// ---------------------------------------------------------------------------
class DuckDBSQLValue : public SQLValue {
public:
	DuckDBSQLValue(bool isNull, Type type, std::string lexical)
	    : isNull_(isNull), type_(type), string_(std::move(lexical)) {
	}

	bool isNull() const override {
		return isNull_;
	}

	Type type() const override {
		return type_;
	}

	const std::string &asString() const override {
		return string_;
	}

	std::unique_ptr<SQLValue> clone() const override {
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(isNull_, type_, string_));
	}

private:
	bool isNull_;
	Type type_;
	std::string string_;
};

std::unique_ptr<SQLValue> makeValue(SQLValue::Type type, std::string lexical) {
	return std::unique_ptr<SQLValue>(new DuckDBSQLValue(false, type, std::move(lexical)));
}

// ---------------------------------------------------------------------------
// 128-bit integer formatting
//
// DuckDB's row-wise duckdb_value_varchar()/duckdb_value_blob() accessors read
// through a legacy "deprecated" result materialization that stores VARCHAR
// cells as NUL-terminated char* - silently truncating any value with an
// embedded NUL byte. The only way to get length-aware string data is the
// chunk/vector API (duckdb_result_get_chunk + duckdb_string_t), and DuckDB's
// C API does not allow mixing the two accessor families on one duckdb_result
// (the first call picks a materialization mode and locks it in). So every
// column in a result - not just VARCHAR - is read natively through the
// vector API here, which means BIGINT-and-wider integers and DECIMAL need
// their own text formatting instead of delegating to duckdb_value_varchar().
// ---------------------------------------------------------------------------

// Converts an unsigned 128-bit value (hi * 2^64 + lo) to decimal digits via
// schoolbook long division by 10, processed in 32-bit limbs so every
// intermediate product fits in 64 bits without relying on a 128-bit integer
// type (not standard C++17).
std::string unsignedHugeintToDigits(uint64_t hi, uint64_t lo) {
	if (hi == 0 && lo == 0) {
		return "0";
	}
	uint32_t limb[4] = {static_cast<uint32_t>(lo), static_cast<uint32_t>(lo >> 32u), static_cast<uint32_t>(hi),
	                    static_cast<uint32_t>(hi >> 32u)};
	std::string digits;
	bool nonzero = true;
	while (nonzero) {
		uint64_t remainder = 0;
		nonzero = false;
		for (int i = 3; i >= 0; --i) {
			uint64_t cur = (remainder << 32u) | limb[i];
			limb[i] = static_cast<uint32_t>(cur / 10);
			remainder = cur % 10;
			if (limb[i] != 0) {
				nonzero = true;
			}
		}
		digits.push_back(static_cast<char>('0' + remainder));
	}
	std::reverse(digits.begin(), digits.end());
	return digits;
}

// Two's-complement negation of a 128-bit (hi, lo) pair, in place.
void negateHugeint(uint64_t &hi, uint64_t &lo) {
	uint64_t negLo = ~lo + 1;
	uint64_t carry = (negLo == 0) ? 1 : 0;
	hi = ~hi + carry;
	lo = negLo;
}

std::string hugeintToString(duckdb_hugeint v) {
	bool negative = v.upper < 0;
	uint64_t hi = static_cast<uint64_t>(v.upper);
	uint64_t lo = v.lower;
	if (negative) {
		negateHugeint(hi, lo);
	}
	std::string digits = unsignedHugeintToDigits(hi, lo);
	return negative ? "-" + digits : digits;
}

std::string decimalToString(duckdb_hugeint raw, uint8_t scale) {
	bool negative = raw.upper < 0;
	uint64_t hi = static_cast<uint64_t>(raw.upper);
	uint64_t lo = raw.lower;
	if (negative) {
		negateHugeint(hi, lo);
	}
	std::string digits = unsignedHugeintToDigits(hi, lo);

	std::string result;
	if (scale == 0) {
		result = digits;
	} else if (digits.size() <= scale) {
		result = "0." + std::string(static_cast<size_t>(scale) - digits.size(), '0') + digits;
	} else {
		result = digits.substr(0, digits.size() - scale) + "." + digits.substr(digits.size() - scale);
	}
	return negative ? "-" + result : result;
}

duckdb_hugeint widenToHugeint(int64_t v) {
	duckdb_hugeint h;
	h.lower = static_cast<uint64_t>(v);
	h.upper = (v < 0) ? -1 : 0;
	return h;
}

std::string formatDate(duckdb_date_struct d) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", d.year, d.month, d.day);
	return buf;
}

std::string formatTime(duckdb_time_struct t) {
	char buf[32];
	if (t.micros == 0) {
		std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.min, t.sec);
	} else {
		std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06d", t.hour, t.min, t.sec, t.micros);
	}
	return buf;
}

std::string formatTimestamp(duckdb_timestamp_struct ts) {
	return formatDate(ts.date) + " " + formatTime(ts.time);
}

// Per-column state captured once per chunk and reused across its rows: the
// raw data/validity pointers from the vector, plus (for DECIMAL) the scale
// and physical storage width, which DuckDB chooses per-column to fit the
// declared precision.
struct ColumnCursor {
	void *data {nullptr};
	uint64_t *validity {nullptr};
	uint8_t decimalScale {0};
	duckdb_type decimalInternalType {DUCKDB_TYPE_INVALID};
};

std::unique_ptr<SQLValue> convertCell(const ColumnCursor &cursor, duckdb_type type, idx_t row) {
	if (!duckdb_validity_row_is_valid(cursor.validity, row)) {
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(true, SQLValue::Type::Null, ""));
	}

	switch (type) {
	case DUCKDB_TYPE_BOOLEAN:
		return makeValue(SQLValue::Type::Boolean, static_cast<bool *>(cursor.data)[row] ? "true" : "false");

	case DUCKDB_TYPE_TINYINT:
		return makeValue(SQLValue::Type::Integer, std::to_string(static_cast<int8_t *>(cursor.data)[row]));
	case DUCKDB_TYPE_SMALLINT:
		return makeValue(SQLValue::Type::Integer, std::to_string(static_cast<int16_t *>(cursor.data)[row]));
	case DUCKDB_TYPE_INTEGER:
		return makeValue(SQLValue::Type::Integer, std::to_string(static_cast<int32_t *>(cursor.data)[row]));
	case DUCKDB_TYPE_UTINYINT:
		return makeValue(SQLValue::Type::Integer, std::to_string(static_cast<uint8_t *>(cursor.data)[row]));
	case DUCKDB_TYPE_USMALLINT:
		return makeValue(SQLValue::Type::Integer, std::to_string(static_cast<uint16_t *>(cursor.data)[row]));
	case DUCKDB_TYPE_UINTEGER:
		return makeValue(SQLValue::Type::Integer, std::to_string(static_cast<uint32_t *>(cursor.data)[row]));

	// BIGINT and larger: format natively (as text, to avoid precision loss).
	case DUCKDB_TYPE_BIGINT:
		return makeValue(SQLValue::Type::String, std::to_string(static_cast<int64_t *>(cursor.data)[row]));
	case DUCKDB_TYPE_UBIGINT:
		return makeValue(SQLValue::Type::String, std::to_string(static_cast<uint64_t *>(cursor.data)[row]));
	case DUCKDB_TYPE_HUGEINT:
		return makeValue(SQLValue::Type::String, hugeintToString(static_cast<duckdb_hugeint *>(cursor.data)[row]));
	case DUCKDB_TYPE_UHUGEINT: {
		duckdb_uhugeint v = static_cast<duckdb_uhugeint *>(cursor.data)[row];
		return makeValue(SQLValue::Type::String, unsignedHugeintToDigits(v.upper, v.lower));
	}

	case DUCKDB_TYPE_FLOAT:
		return makeValue(SQLValue::Type::Double,
		                 std::to_string(static_cast<double>(static_cast<float *>(cursor.data)[row])));
	case DUCKDB_TYPE_DOUBLE:
		return makeValue(SQLValue::Type::Double, std::to_string(static_cast<double *>(cursor.data)[row]));

	// VARCHAR and BLOB share the same string_t physical layout; both are
	// read length-aware here so embedded NUL bytes survive intact.
	case DUCKDB_TYPE_VARCHAR:
	case DUCKDB_TYPE_BLOB: {
		duckdb_string_t cell = static_cast<duckdb_string_t *>(cursor.data)[row];
		return makeValue(SQLValue::Type::String,
		                 std::string(duckdb_string_t_data(&cell), duckdb_string_t_length(cell)));
	}

	case DUCKDB_TYPE_DATE:
		return makeValue(SQLValue::Type::String,
		                 formatDate(duckdb_from_date(static_cast<duckdb_date *>(cursor.data)[row])));
	case DUCKDB_TYPE_TIME:
		return makeValue(SQLValue::Type::String,
		                 formatTime(duckdb_from_time(static_cast<duckdb_time *>(cursor.data)[row])));
	case DUCKDB_TYPE_TIMESTAMP:
		return makeValue(SQLValue::Type::String,
		                 formatTimestamp(duckdb_from_timestamp(static_cast<duckdb_timestamp *>(cursor.data)[row])));

	case DUCKDB_TYPE_DECIMAL: {
		duckdb_hugeint raw;
		switch (cursor.decimalInternalType) {
		case DUCKDB_TYPE_SMALLINT:
			raw = widenToHugeint(static_cast<int16_t *>(cursor.data)[row]);
			break;
		case DUCKDB_TYPE_INTEGER:
			raw = widenToHugeint(static_cast<int32_t *>(cursor.data)[row]);
			break;
		case DUCKDB_TYPE_BIGINT:
			raw = widenToHugeint(static_cast<int64_t *>(cursor.data)[row]);
			break;
		case DUCKDB_TYPE_HUGEINT:
		default:
			raw = static_cast<duckdb_hugeint *>(cursor.data)[row];
			break;
		}
		return makeValue(SQLValue::Type::String, decimalToString(raw, cursor.decimalScale));
	}

	default:
		throw std::runtime_error("DuckDB column type " + std::to_string(static_cast<int>(type)) +
		                         " is not supported by the row adapter");
	}
}

// RAII guard ensuring duckdb_destroy_result() runs even if conversion throws.
struct DuckDBResultGuard {
	duckdb_result result {};
	~DuckDBResultGuard() {
		duckdb_destroy_result(&result);
	}
};

// RAII guard for a single duckdb_data_chunk fetched from a result.
struct DuckDBChunkGuard {
	duckdb_data_chunk chunk;
	explicit DuckDBChunkGuard(duckdb_data_chunk c) : chunk(c) {
	}
	~DuckDBChunkGuard() {
		duckdb_destroy_data_chunk(&chunk);
	}
	DuckDBChunkGuard(const DuckDBChunkGuard &) = delete;
	DuckDBChunkGuard &operator=(const DuckDBChunkGuard &) = delete;
};

// ---------------------------------------------------------------------------
// sql2rdf_* scalar functions (the "duckdb-udf" dialect's runtime half)
//
// Each function below must return, for every input, exactly what the inline
// SQL it replaces (sparql2sql::SqlDialect's default hooks, i.e. TagSql.h)
// returns when DuckDB evaluates it - including SQL NULL and three-valued
// logic. That inline SQL leans on DuckDB's own TRY_CAST(VARCHAR AS
// DOUBLE/TIMESTAMP/BOOLEAN), which the C API does not expose, so the three
// parsers here re-implement those casts as observed on DuckDB 1.5 (offsets
// are parsed but ignored by a TIMESTAMP cast, '_' digit separators, the
// 'infinity'/'epoch' keywords, ...). tests/duckdb's differential test runs
// both spellings over a corpus of edge cases and must stay at zero mismatches;
// re-run it after upgrading DuckDB.
// ---------------------------------------------------------------------------
namespace rdffn {

using sparql2sql::kValueSpaceBlankNode;
using sparql2sql::kValueSpaceBoolean;
using sparql2sql::kValueSpaceIri;
using sparql2sql::kValueSpaceLangString;
using sparql2sql::kValueSpaceNumeric;
using sparql2sql::kValueSpaceString;
using sparql2sql::kValueSpaceTemporal;
using sparql2sql::kValueSpaceUnknown;

/// A flattened VARCHAR input column.
struct StrCol {
	duckdb_string_t *data;
	uint64_t *validity;

	explicit StrCol(duckdb_vector v)
	    : data(static_cast<duckdb_string_t *>(duckdb_vector_get_data(v))), validity(duckdb_vector_get_validity(v)) {
	}
	bool isNull(idx_t row) const {
		return !duckdb_validity_row_is_valid(validity, row);
	}
	std::string_view get(idx_t row) const {
		return {duckdb_string_t_data(&data[row]), duckdb_string_t_length(data[row])};
	}
};

void setNull(duckdb_vector out, idx_t row) {
	duckdb_vector_ensure_validity_writable(out);
	duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), row);
}

bool isSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
bool isDigit(char c) {
	return c >= '0' && c <= '9';
}
char lower(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
/// Case-insensitive test that `s` has `word` at `pos`.
bool hasWordAt(std::string_view s, std::size_t pos, std::string_view word) {
	if (s.size() - pos < word.size()) {
		return false;
	}
	for (std::size_t i = 0; i < word.size(); ++i) {
		if (lower(s[pos + i]) != word[i]) {
			return false;
		}
	}
	return true;
}

// --- tag classification (TagSql.h tagValueSpace / tagKindRank) -------------

int valueSpace(std::string_view tag) {
	using sparql2sql::kTagBlankNode;
	using sparql2sql::kTagDatatypePrefix;
	using sparql2sql::kTagIri;
	using sparql2sql::kTagLangPrefix;
	namespace xsd = sparql2sql::xsd;
	if (tag == kTagIri) {
		return kValueSpaceIri;
	}
	if (tag == kTagBlankNode) {
		return kValueSpaceBlankNode;
	}
	if (!tag.empty() && tag[0] == kTagLangPrefix) {
		return kValueSpaceLangString;
	}
	if (tag.empty() || tag[0] != kTagDatatypePrefix) {
		return kValueSpaceUnknown;
	}
	const std::string_view iri = tag.substr(1);
	for (const char *n :
	     {xsd::kInteger, xsd::kLong, xsd::kInt, xsd::kShort, xsd::kByte, xsd::kDecimal, xsd::kDouble, xsd::kFloat}) {
		if (iri == n) {
			return kValueSpaceNumeric;
		}
	}
	if (iri == xsd::kString) {
		return kValueSpaceString;
	}
	if (iri == xsd::kBoolean) {
		return kValueSpaceBoolean;
	}
	if (iri == xsd::kDate || iri == xsd::kDateTime) {
		return kValueSpaceTemporal;
	}
	return kValueSpaceUnknown;
}

int kindRank(const StrCol &tag, idx_t row) {
	if (tag.isNull(row)) {
		return 0;
	}
	const std::string_view t = tag.get(row);
	if (t == sparql2sql::kTagBlankNode) {
		return 1;
	}
	return t == sparql2sql::kTagIri ? 2 : 3;
}

// --- TRY_CAST(VARCHAR AS DOUBLE) ---------------------------------------------

/// Consume a run of digits in which a single '_' may separate two digits, as
/// DuckDB's numeric casts accept; appends the digits (sans '_') to `out`.
std::size_t digitRun(std::string_view s, std::size_t i, std::string &out) {
	while (i < s.size() && isDigit(s[i])) {
		out += s[i++];
		if (i + 1 < s.size() && s[i] == '_' && isDigit(s[i + 1])) {
			++i;
		}
	}
	return i;
}

bool tryCastDouble(std::string_view s, double &result) {
	std::size_t i = 0;
	const std::size_t n = s.size();
	while (i < n && isSpace(s[i])) {
		++i;
	}
	if (i < n && s[i] == '+') {
		++i;
	}
	const bool negative = i < n && s[i] == '-';
	if (negative) {
		++i;
	}
	if (i >= n) {
		return false;
	}
	if (!isDigit(s[i]) && s[i] != '.') {
		if (hasWordAt(s, i, "infinity")) {
			i += 8;
			result = std::numeric_limits<double>::infinity();
		} else if (hasWordAt(s, i, "inf")) {
			i += 3;
			result = std::numeric_limits<double>::infinity();
		} else if (hasWordAt(s, i, "nan")) {
			i += 3;
			if (i < n && s[i] == '(') {
				std::size_t j = i + 1;
				while (j < n && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) {
					++j;
				}
				if (j < n && s[j] == ')') {
					i = j + 1;
				}
			}
			result = std::numeric_limits<double>::quiet_NaN();
		} else {
			return false;
		}
		if (negative) {
			result = -result;
		}
	} else {
		// Mantissa digits are collected without the decimal point (the scale
		// goes into the exponent instead), so strtod never sees a
		// locale-dependent radix character.
		std::string digits;
		i = digitRun(s, i, digits);
		int64_t scale = 0;
		if (i < n && s[i] == '.') {
			const std::size_t before = digits.size();
			i = digitRun(s, i + 1, digits);
			scale = -static_cast<int64_t>(digits.size() - before);
		}
		if (digits.empty()) {
			return false;
		}
		int64_t exponent = 0;
		if (i < n && (s[i] == 'e' || s[i] == 'E')) {
			std::size_t j = i + 1;
			bool expNegative = false;
			if (j < n && (s[j] == '+' || s[j] == '-')) {
				expNegative = s[j] == '-';
				++j;
			}
			std::string expDigits;
			const std::size_t end = digitRun(s, j, expDigits);
			if (!expDigits.empty()) {
				i = end;
				const std::size_t significant = expDigits.find_first_not_of('0');
				if (significant != std::string::npos) {
					// Saturate: anything this large is already inf/0 for a double.
					exponent = expDigits.size() - significant > 9 ? 999999999LL : std::stoll(expDigits);
				}
				if (expNegative) {
					exponent = -exponent;
				}
			}
		}
		const std::string text = (negative ? "-" : "") + digits + "e" + std::to_string(exponent + scale);
		result = std::strtod(text.c_str(), nullptr);
	}
	while (i < n && isSpace(s[i])) {
		++i;
	}
	return i == n;
}

// --- TRY_CAST(VARCHAR AS BOOLEAN) --------------------------------------------

bool tryCastBoolean(std::string_view s, bool &result) {
	std::string l;
	l.reserve(s.size());
	for (char c : s) {
		l += lower(c);
	}
	if (l == "t" || l == "true" || l == "y" || l == "yes" || l == "1") {
		result = true;
		return true;
	}
	if (l == "f" || l == "false" || l == "n" || l == "no" || l == "0") {
		result = false;
		return true;
	}
	return false;
}

// --- TRY_CAST(VARCHAR AS TIMESTAMP), as microseconds since the epoch ---------

constexpr int64_t kMicrosPerDay = 86400000000LL;
constexpr int64_t kTimestampInfinity = std::numeric_limits<int64_t>::max();

bool parseDoubleDigit(std::string_view s, std::size_t &pos, int &out) {
	if (pos < s.size() && isDigit(s[pos])) {
		out = s[pos++] - '0';
		if (pos < s.size() && isDigit(s[pos])) {
			out = out * 10 + (s[pos++] - '0');
		}
		return true;
	}
	return false;
}

bool isLeapYear(int64_t y) {
	return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

/// Days since 1970-01-01 in the proleptic Gregorian calendar (year 0 = 1 BC).
int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
	y -= m <= 2 ? 1 : 0;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const int64_t yoe = y - era * 400;
	const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

/// A trailing UTC offset: +HH, +HHMM, +HH:MM, optionally :SS. Parsed (so the
/// cast accepts it) but ignored, as DuckDB's TIMESTAMP cast does.
bool skipUtcOffset(std::string_view s, std::size_t &pos) {
	std::size_t cur = pos;
	if (cur + 3 > s.size() || (s[cur] != '+' && s[cur] != '-') || !isDigit(s[cur + 1]) || !isDigit(s[cur + 2])) {
		return false;
	}
	cur += 3;
	if (cur < s.size() && s[cur] == ':') {
		++cur;
	}
	if (cur + 2 <= s.size() && isDigit(s[cur]) && isDigit(s[cur + 1])) {
		cur += 2;
		if (cur < s.size() && s[cur] == ':') {
			++cur;
			if (cur + 2 <= s.size() && isDigit(s[cur]) && isDigit(s[cur + 1])) {
				cur += 2;
			}
		}
	}
	pos = cur;
	return true;
}

bool isTimeZoneChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '/' || c == '+' || c == '-';
}

bool tryCastTimestamp(std::string_view s, int64_t &result) {
	const std::size_t n = s.size();
	std::size_t pos = 0;
	while (pos < n && isSpace(s[pos])) {
		++pos;
	}
	if (pos >= n) {
		return false;
	}
	const bool yearNegative = s[pos] == '-';
	if (yearNegative && ++pos >= n) {
		return false;
	}
	if (!isDigit(s[pos])) {
		if (hasWordAt(s, pos, "infinity")) {
			pos += 8;
			result = yearNegative ? -kTimestampInfinity : kTimestampInfinity;
		} else if (hasWordAt(s, pos, "epoch")) {
			pos += 5;
			result = 0;
		} else if (n - pos == 3 && hasWordAt(s, pos, "inf")) {
			pos += 3;
			result = yearNegative ? -kTimestampInfinity : kTimestampInfinity;
		} else {
			return false;
		}
		while (pos < n && isSpace(s[pos])) {
			++pos;
		}
		return pos == n;
	}
	int64_t year = 0;
	for (; pos < n && isDigit(s[pos]); ++pos) {
		if (year >= 100000000) {
			return false;
		}
		year = year * 10 + (s[pos] - '0');
	}
	if (yearNegative) {
		year = -year;
	}
	if (pos >= n) {
		return false;
	}
	const char sep = s[pos++];
	if (sep != ' ' && sep != '-' && sep != '/' && sep != '\\') {
		return false;
	}
	int month = 0;
	int day = 0;
	if (!parseDoubleDigit(s, pos, month) || pos >= n || s[pos] != sep) {
		return false;
	}
	++pos;
	if (pos >= n || !parseDoubleDigit(s, pos, day)) {
		return false;
	}
	if (n - pos >= 5 && isSpace(s[pos]) && s[pos + 1] == '(' && lower(s[pos + 2]) == 'b' && lower(s[pos + 3]) == 'c' &&
	    s[pos + 4] == ')') {
		if (yearNegative || year == 0) {
			return false;
		}
		year = -year + 1;
		pos += 5;
	}
	if (pos < n && isDigit(s[pos])) {
		return false;
	}
	static const int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month < 1 || month > 12 || day < 1 ||
	    day > kDaysInMonth[month - 1] + ((month == 2 && isLeapYear(year)) ? 1 : 0)) {
		return false;
	}
	int64_t micros = 0;
	if (__builtin_mul_overflow(daysFromCivil(year, month, day), kMicrosPerDay, &micros)) {
		return false;
	}
	if (pos < n) {
		if (s[pos] == ' ' || s[pos] == 'T') {
			++pos;
		}
		while (pos < n && isSpace(s[pos])) {
			++pos;
		}
		if (pos >= n || !isDigit(s[pos])) {
			return false;
		}
		int64_t hour = 0;
		for (int digits = 0; pos < n && isDigit(s[pos]); ++pos, ++digits) {
			if (digits == 9) {
				return false;
			}
			hour = hour * 10 + (s[pos] - '0');
		}
		int minute = 0;
		int second = 0;
		if (pos >= n || s[pos] != ':') {
			return false;
		}
		++pos;
		if (!parseDoubleDigit(s, pos, minute) || minute >= 60) {
			return false;
		}
		bool hasSeconds = false;
		if (pos < n && s[pos] == ':') {
			++pos;
			hasSeconds = parseDoubleDigit(s, pos, second);
			if (hasSeconds && second >= 60) {
				return false;
			}
		}
		int64_t fraction = 0;
		if (hasSeconds && pos < n && s[pos] == '.') { // no fraction without seconds
			++pos;
			for (int64_t mult = 100000; pos < n && isDigit(s[pos]); ++pos, mult /= 10) {
				fraction += (s[pos] - '0') * mult;
			}
		}
		const int64_t timeMicros = ((hour * 60 + minute) * 60 + second) * 1000000 + fraction;
		if (timeMicros > kMicrosPerDay || __builtin_add_overflow(micros, timeMicros, &micros)) {
			return false;
		}
		if (pos < n) {
			if (s[pos] == 'Z') {
				++pos;
			} else if (!skipUtcOffset(s, pos)) {
				if (s[pos++] != ' ') {
					return false;
				}
				const std::size_t tzStart = pos;
				while (pos < n && isTimeZoneChar(s[pos])) {
					++pos;
				}
				const std::size_t tzLen = pos - tzStart;
				if (tzLen != 0 && !(tzLen == 3 && hasWordAt(s, tzStart, "utc"))) {
					return false; // a named, non-UTC zone: an error for TIMESTAMP
				}
			}
			while (pos < n && isSpace(s[pos])) {
				++pos;
			}
			if (pos < n) {
				return false;
			}
		}
	}
	if (micros >= kTimestampInfinity || micros <= -kTimestampInfinity) {
		return false;
	}
	result = micros;
	return true;
}

// --- comparisons, in SQL three-valued logic ----------------------------------

enum class Tri : uint8_t { False, True, Null };
enum class Op : uint8_t { Eq, Ne, Lt, Gt, Le, Ge };

Tri tri(bool b) {
	return b ? Tri::True : Tri::False;
}
Tri notTri(Tri t) {
	return t == Tri::Null ? Tri::Null : tri(t == Tri::False);
}

/// Apply `op` given only "less" and "equal" - the shape every DuckDB
/// comparison reduces to.
template <class Less, class Equal>
bool applyOp(Op op, Less less, Equal equal) {
	switch (op) {
	case Op::Eq:
		return equal();
	case Op::Ne:
		return !equal();
	case Op::Lt:
		return less(false);
	case Op::Gt:
		return less(true);
	case Op::Le:
		return !less(true);
	case Op::Ge:
		return !less(false);
	}
	return false;
}

/// DuckDB's DOUBLE ordering: NaN equals NaN and sorts above everything else.
bool compareDouble(Op op, double l, double r) {
	auto lessThan = [](double a, double b) {
		if (std::isnan(a)) {
			return false;
		}
		return std::isnan(b) || a < b;
	};
	return applyOp(
	    op, [&](bool swapped) { return swapped ? lessThan(r, l) : lessThan(l, r); },
	    [&] { return (std::isnan(l) && std::isnan(r)) || l == r; });
}

template <class T>
bool compareOrdered(Op op, const T &l, const T &r) {
	return applyOp(
	    op, [&](bool swapped) { return swapped ? r < l : l < r; }, [&] { return l == r; });
}

/// One operand of a comparison: a lexical form that may be NULL.
struct Operand {
	bool null;
	std::string_view text;
};

Tri compareText(Op op, const Operand &l, const Operand &r) {
	if (l.null || r.null) {
		return Tri::Null;
	}
	return tri(compareOrdered(op, l.text, r.text));
}

Tri compareAsDouble(Op op, const Operand &l, const Operand &r) {
	double a = 0;
	double b = 0;
	if (l.null || r.null || !tryCastDouble(l.text, a) || !tryCastDouble(r.text, b)) {
		return Tri::Null;
	}
	return tri(compareDouble(op, a, b));
}

Tri compareAsTimestamp(Op op, const Operand &l, const Operand &r) {
	int64_t a = 0;
	int64_t b = 0;
	if (l.null || r.null || !tryCastTimestamp(l.text, a) || !tryCastTimestamp(r.text, b)) {
		return Tri::Null;
	}
	return tri(compareOrdered(op, a, b));
}

Tri compareAsBoolean(Op op, const Operand &l, const Operand &r) {
	bool a = false;
	bool b = false;
	if (l.null || r.null || !tryCastBoolean(l.text, a) || !tryCastBoolean(r.text, b)) {
		return Tri::Null;
	}
	return tri(compareOrdered(op, a, b));
}

/// TagSql.h untypedComparison: numeric when both sides cast, else as text.
Tri untyped(Op op, const Operand &l, const Operand &r) {
	double a = 0;
	double b = 0;
	if (!l.null && !r.null && tryCastDouble(l.text, a) && tryCastDouble(r.text, b)) {
		return tri(compareDouble(op, a, b));
	}
	return compareText(op, l, r);
}

/// TagSql.h dynamicEquality / dynamicOrdering.
Tri dynamic(Op op, const Operand &l, const Operand &lt, const Operand &r, const Operand &rt) {
	if (lt.null || rt.null) {
		return Tri::Null;
	}
	const int ls = valueSpace(lt.text);
	const int rs = valueSpace(rt.text);
	if (ls == kValueSpaceUnknown || rs == kValueSpaceUnknown) {
		return untyped(op, l, r);
	}
	const bool equality = op == Op::Eq || op == Op::Ne;
	if (ls != rs) {
		return equality ? tri(op == Op::Ne) : Tri::Null;
	}
	switch (ls) {
	case kValueSpaceNumeric:
		return compareAsDouble(op, l, r);
	case kValueSpaceTemporal:
		return compareAsTimestamp(op, l, r);
	case kValueSpaceBoolean:
		return compareAsBoolean(op, l, r);
	default:
		break;
	}
	if (!equality) {
		return ls == kValueSpaceString ? compareText(op, l, r) : Tri::Null;
	}
	if (ls == kValueSpaceLangString) {
		// (lt = rt AND l = r), negated as a whole for <>.
		const Tri sameTerm = lt.text != rt.text ? Tri::False : compareText(Op::Eq, l, r);
		return op == Op::Ne ? notTri(sameTerm) : sameTerm;
	}
	return compareText(op, l, r);
}

void writeTri(duckdb_vector out, bool *data, idx_t row, Tri t) {
	if (t == Tri::Null) {
		setNull(out, row);
	} else {
		data[row] = t == Tri::True;
	}
}

Op opOf(duckdb_function_info info) {
	return *static_cast<const Op *>(duckdb_scalar_function_get_extra_info(info));
}

// --- the scalar function bodies ---------------------------------------------

void dynamicCompareFn(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector out) {
	const Op op = opOf(info);
	const idx_t count = duckdb_data_chunk_get_size(input);
	const StrCol l(duckdb_data_chunk_get_vector(input, 0));
	const StrCol lt(duckdb_data_chunk_get_vector(input, 1));
	const StrCol r(duckdb_data_chunk_get_vector(input, 2));
	const StrCol rt(duckdb_data_chunk_get_vector(input, 3));
	auto *data = static_cast<bool *>(duckdb_vector_get_data(out));
	auto operand = [](const StrCol &c, idx_t row) {
		return c.isNull(row) ? Operand {true, {}} : Operand {false, c.get(row)};
	};
	for (idx_t row = 0; row < count; ++row) {
		writeTri(out, data, row, dynamic(op, operand(l, row), operand(lt, row), operand(r, row), operand(rt, row)));
	}
}

void untypedCompareFn(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector out) {
	const Op op = opOf(info);
	const idx_t count = duckdb_data_chunk_get_size(input);
	const StrCol l(duckdb_data_chunk_get_vector(input, 0));
	const StrCol r(duckdb_data_chunk_get_vector(input, 1));
	auto *data = static_cast<bool *>(duckdb_vector_get_data(out));
	for (idx_t row = 0; row < count; ++row) {
		const Operand a = l.isNull(row) ? Operand {true, {}} : Operand {false, l.get(row)};
		const Operand b = r.isNull(row) ? Operand {true, {}} : Operand {false, r.get(row)};
		writeTri(out, data, row, untyped(op, a, b));
	}
}

void valueSpaceFn(duckdb_function_info, duckdb_data_chunk input, duckdb_vector out) {
	const idx_t count = duckdb_data_chunk_get_size(input);
	const StrCol tag(duckdb_data_chunk_get_vector(input, 0));
	auto *data = static_cast<int32_t *>(duckdb_vector_get_data(out));
	for (idx_t row = 0; row < count; ++row) {
		if (tag.isNull(row)) {
			setNull(out, row);
		} else {
			data[row] = valueSpace(tag.get(row));
		}
	}
}

void kindRankFn(duckdb_function_info, duckdb_data_chunk input, duckdb_vector out) {
	const idx_t count = duckdb_data_chunk_get_size(input);
	const StrCol tag(duckdb_data_chunk_get_vector(input, 0));
	auto *data = static_cast<int32_t *>(duckdb_vector_get_data(out));
	for (idx_t row = 0; row < count; ++row) {
		data[row] = kindRank(tag, row);
	}
}

void tagCoalesceFn(duckdb_function_info, duckdb_data_chunk input, duckdb_vector out) {
	const idx_t count = duckdb_data_chunk_get_size(input);
	const idx_t columns = duckdb_data_chunk_get_column_count(input);
	std::vector<StrCol> cols;
	cols.reserve(columns);
	for (idx_t c = 0; c < columns; ++c) {
		cols.emplace_back(duckdb_data_chunk_get_vector(input, c));
	}
	for (idx_t row = 0; row < count; ++row) {
		const StrCol *chosen = nullptr;
		for (idx_t c = 0; c + 1 < columns; c += 2) {
			if (!cols[c].isNull(row)) {
				chosen = &cols[c + 1];
				break;
			}
		}
		if (chosen == nullptr || chosen->isNull(row)) {
			setNull(out, row);
		} else {
			const std::string_view t = chosen->get(row);
			duckdb_vector_assign_string_element_len(out, row, t.data(), t.size());
		}
	}
}

/// r2rml::AbstractMap::percentEncode (and DuckDB's url_encode): RFC 3986
/// unreserved bytes verbatim, every other byte as an uppercase %XX escape.
void appendPercentEncoded(std::string &out, std::string_view v) {
	static const char kHex[] = "0123456789ABCDEF";
	for (char c : v) {
		const auto b = static_cast<unsigned char>(c);
		if (std::isalnum(b) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += c;
		} else {
			out += '%';
			out += kHex[b >> 4];
			out += kHex[b & 0xF];
		}
	}
}

void iriFn(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector out) {
	const idx_t count = duckdb_data_chunk_get_size(input);
	const idx_t columns = duckdb_data_chunk_get_column_count(input);
	if (columns % 2 == 0) {
		duckdb_scalar_function_set_error(info, "sql2rdf_iri: expects literal, value, literal, ..., literal");
		return;
	}
	std::vector<StrCol> cols;
	cols.reserve(columns);
	for (idx_t c = 0; c < columns; ++c) {
		cols.emplace_back(duckdb_data_chunk_get_vector(input, c));
	}
	std::string buf;
	for (idx_t row = 0; row < count; ++row) {
		buf.clear();
		bool null = false;
		for (idx_t c = 0; c < columns && !null; ++c) {
			if (cols[c].isNull(row)) {
				null = true;
			} else if (c % 2 == 0) {
				buf += cols[c].get(row);
			} else {
				appendPercentEncoded(buf, cols[c].get(row));
			}
		}
		if (null) {
			setNull(out, row);
		} else {
			duckdb_vector_assign_string_element_len(out, row, buf.data(), buf.size());
		}
	}
}

// --- registration ------------------------------------------------------------

struct LogicalType {
	duckdb_logical_type t;
	explicit LogicalType(duckdb_type type) : t(duckdb_create_logical_type(type)) {
	}
	~LogicalType() {
		duckdb_destroy_logical_type(&t);
	}
	LogicalType(const LogicalType &) = delete;
	LogicalType &operator=(const LogicalType &) = delete;
};

void registerOne(duckdb_connection con, const std::string &name, std::size_t fixedVarcharArgs, bool varargs,
                 duckdb_type returnType, duckdb_scalar_function_t fn, Op *op, bool specialNullHandling) {
	const LogicalType varchar(DUCKDB_TYPE_VARCHAR);
	const LogicalType ret(returnType);
	duckdb_scalar_function f = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(f, name.c_str());
	for (std::size_t i = 0; i < fixedVarcharArgs; ++i) {
		duckdb_scalar_function_add_parameter(f, varchar.t);
	}
	if (varargs) {
		duckdb_scalar_function_set_varargs(f, varchar.t);
	}
	duckdb_scalar_function_set_return_type(f, ret.t);
	if (specialNullHandling) {
		// NULL is a meaningful input (an unbound term), not "result is NULL".
		duckdb_scalar_function_set_special_handling(f);
	}
	if (op != nullptr) {
		duckdb_scalar_function_set_extra_info(f, op, nullptr);
	}
	duckdb_scalar_function_set_function(f, fn);
	const duckdb_state state = duckdb_register_scalar_function(con, f);
	duckdb_destroy_scalar_function(&f);
	if (state == DuckDBError) {
		throw std::runtime_error("DuckDB refused to register scalar function " + name);
	}
}

void registerAll(duckdb_connection con) {
	// Non-const only because duckdb_scalar_function_set_extra_info takes a
	// void*; the functions never write through it.
	static Op kOps[] = {Op::Eq, Op::Ne, Op::Lt, Op::Gt, Op::Le, Op::Ge};
	static const char *const kOpSql[] = {"=", "<>", "<", ">", "<=", ">="};
	for (std::size_t i = 0; i < 6; ++i) {
		const std::string suffix = sparql2sql::comparisonFunctionSuffix(kOpSql[i]);
		registerOne(con, "sql2rdf_" + suffix, 4, false, DUCKDB_TYPE_BOOLEAN, dynamicCompareFn, &kOps[i], true);
		registerOne(con, "sql2rdf_untyped_" + suffix, 2, false, DUCKDB_TYPE_BOOLEAN, untypedCompareFn, &kOps[i], false);
	}
	registerOne(con, "sql2rdf_value_space", 1, false, DUCKDB_TYPE_INTEGER, valueSpaceFn, nullptr, false);
	registerOne(con, "sql2rdf_kind_rank", 1, false, DUCKDB_TYPE_INTEGER, kindRankFn, nullptr, true);
	registerOne(con, "sql2rdf_tag_coalesce", 0, true, DUCKDB_TYPE_VARCHAR, tagCoalesceFn, nullptr, true);
	registerOne(con, "sql2rdf_iri", 0, true, DUCKDB_TYPE_VARCHAR, iriFn, nullptr, false);
}

} // namespace rdffn
} // namespace

// ---------------------------------------------------------------------------
// DuckDBResultSet
//
// Materialises all rows from a query result into memory, then iterates
// over them.  This avoids keeping the DuckDB result object alive across
// calls to next()/getCurrentRow() and keeps the interface simple.
// ---------------------------------------------------------------------------
class DuckDBResultSet : public SQLResultSet {
public:
	explicit DuckDBResultSet(std::vector<MapSQLRow> rows) : rows_(std::move(rows)) {
	}

	bool next() override {
		return ++cursor_ < static_cast<int>(rows_.size());
	}

	const SQLRow &getCurrentRow() const override {
		return rows_[static_cast<size_t>(cursor_)];
	}

private:
	std::vector<MapSQLRow> rows_;
	int cursor_ {-1};
};

// ---------------------------------------------------------------------------
// DuckDBConnection::Impl
// ---------------------------------------------------------------------------
struct DuckDBConnection::Impl {
	duckdb_database db {nullptr};
	duckdb_connection con {nullptr};

	explicit Impl(const std::string &path) {
		char *error = nullptr;
		if (duckdb_open_ext(path.c_str(), &db, nullptr, &error) == DuckDBError) {
			std::string message = error ? error : "unknown error";
			duckdb_free(error);
			throw std::runtime_error("Failed to open DuckDB database '" + path + "': " + message);
		}
		if (duckdb_connect(db, &con) == DuckDBError) {
			duckdb_close(&db);
			throw std::runtime_error("Failed to connect to DuckDB database '" + path + "'");
		}
	}

	~Impl() {
		duckdb_disconnect(&con);
		duckdb_close(&db);
	}
};

// ---------------------------------------------------------------------------
// DuckDBConnection
// ---------------------------------------------------------------------------
void *DuckDBConnection::nativeConnection() const {
	return impl_->con;
}

void DuckDBConnection::registerRdfFunctions() {
	if (!rdfFunctionsRegistered_) {
		rdffn::registerAll(impl_->con);
		rdfFunctionsRegistered_ = true;
	}
}

DuckDBConnection::DuckDBConnection(const std::string &path) : impl_(new Impl(path)) {
}

DuckDBConnection::~DuckDBConnection() = default;

std::string DuckDBConnection::getDefaultSchema() {
	return "main";
}

std::unique_ptr<SQLResultSet> DuckDBConnection::execute(const std::string &sqlQuery) {
	DuckDBResultGuard guard;
	if (duckdb_query(impl_->con, sqlQuery.c_str(), &guard.result) == DuckDBError) {
		throw std::runtime_error("DuckDB query error: " + std::string(duckdb_result_error(&guard.result)));
	}

	idx_t columnCount = duckdb_column_count(&guard.result);

	std::vector<std::string> columnNames(columnCount);
	std::vector<duckdb_type> columnTypes(columnCount);
	for (idx_t col = 0; col < columnCount; ++col) {
		std::string name = duckdb_column_name(&guard.result, col);
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
		columnNames[col] = std::move(name);
		columnTypes[col] = duckdb_column_type(&guard.result, col);
	}

	std::vector<MapSQLRow> rows;
	rows.reserve(duckdb_row_count(&guard.result));

	// Read through the chunk/vector API exclusively (see the comment above
	// convertCell()): DuckDB locks a duckdb_result into either the legacy
	// row-wise accessors or this chunk-based one on first use, so every
	// column must be read this way even though only VARCHAR/BLOB strictly
	// need the length-aware access it provides.
	idx_t chunkCount = duckdb_result_chunk_count(guard.result);
	for (idx_t chunkIdx = 0; chunkIdx < chunkCount; ++chunkIdx) {
		DuckDBChunkGuard chunk(duckdb_result_get_chunk(guard.result, chunkIdx));
		idx_t chunkSize = duckdb_data_chunk_get_size(chunk.chunk);

		std::vector<ColumnCursor> cursors(columnCount);
		for (idx_t col = 0; col < columnCount; ++col) {
			duckdb_vector vector = duckdb_data_chunk_get_vector(chunk.chunk, col);
			cursors[col].data = duckdb_vector_get_data(vector);
			cursors[col].validity = duckdb_vector_get_validity(vector);
			if (columnTypes[col] == DUCKDB_TYPE_DECIMAL) {
				duckdb_logical_type logicalType = duckdb_vector_get_column_type(vector);
				cursors[col].decimalScale = duckdb_decimal_scale(logicalType);
				cursors[col].decimalInternalType = duckdb_decimal_internal_type(logicalType);
				duckdb_destroy_logical_type(&logicalType);
			}
		}

		for (idx_t row = 0; row < chunkSize; ++row) {
			std::map<std::string, std::unique_ptr<SQLValue>> columns;
			for (idx_t col = 0; col < columnCount; ++col) {
				columns[columnNames[col]] = convertCell(cursors[col], columnTypes[col], row);
			}
			rows.emplace_back(std::move(columns));
		}
	}

	return std::unique_ptr<SQLResultSet>(new DuckDBResultSet(std::move(rows)));
}

} // namespace r2rml
