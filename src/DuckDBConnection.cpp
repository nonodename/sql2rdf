#include "DuckDBConnection.h"
#include "r2rml/MapSQLRow.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include <duckdb.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
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
	uint32_t limb[4] = {static_cast<uint32_t>(lo), static_cast<uint32_t>(lo >> 32), static_cast<uint32_t>(hi),
	                    static_cast<uint32_t>(hi >> 32)};
	std::string digits;
	bool nonzero = true;
	while (nonzero) {
		uint64_t remainder = 0;
		nonzero = false;
		for (int i = 3; i >= 0; --i) {
			uint64_t cur = (remainder << 32) | limb[i];
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
