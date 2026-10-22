#include "DuckDBConnection.h"
#include "r2rml/MapSQLRow.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include <duckdb.h>

#include <algorithm>
#include <cctype>
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
// duckdb_result is still live: the C API's typed getters (duckdb_value_*)
// only operate against a live duckdb_result/row index, unlike the C++
// duckdb::Value this replaced, which owned a copy of its data and could
// convert lazily on first access.
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

// Owns a char* returned by duckdb_value_varchar(), freeing it via duckdb_free().
class DuckDBOwnedString {
public:
	explicit DuckDBOwnedString(char *ptr) : ptr_(ptr) {
	}
	~DuckDBOwnedString() {
		duckdb_free(ptr_);
	}
	DuckDBOwnedString(const DuckDBOwnedString &) = delete;
	DuckDBOwnedString &operator=(const DuckDBOwnedString &) = delete;

	std::string str() const {
		return ptr_ ? std::string(ptr_) : std::string();
	}

private:
	char *ptr_;
};

std::unique_ptr<SQLValue> convertCell(duckdb_result *result, idx_t col, idx_t row) {
	if (duckdb_value_is_null(result, col, row)) {
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(true, SQLValue::Type::Null, ""));
	}

	switch (duckdb_column_type(result, col)) {
	case DUCKDB_TYPE_BOOLEAN:
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(false, SQLValue::Type::Boolean,
		                                                    duckdb_value_boolean(result, col, row) ? "true" : "false"));

	case DUCKDB_TYPE_TINYINT:
	case DUCKDB_TYPE_SMALLINT:
	case DUCKDB_TYPE_INTEGER:
	case DUCKDB_TYPE_UTINYINT:
	case DUCKDB_TYPE_USMALLINT:
	case DUCKDB_TYPE_UINTEGER:
		return std::unique_ptr<SQLValue>(
		    new DuckDBSQLValue(false, SQLValue::Type::Integer, std::to_string(duckdb_value_int32(result, col, row))));

	// BIGINT and larger: use the VARCHAR conversion to avoid precision loss
	case DUCKDB_TYPE_BIGINT:
	case DUCKDB_TYPE_UBIGINT:
	case DUCKDB_TYPE_HUGEINT: {
		DuckDBOwnedString text(duckdb_value_varchar(result, col, row));
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(false, SQLValue::Type::String, text.str()));
	}

	case DUCKDB_TYPE_FLOAT:
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(
		    false, SQLValue::Type::Double, std::to_string(static_cast<double>(duckdb_value_float(result, col, row)))));

	case DUCKDB_TYPE_DOUBLE:
		return std::unique_ptr<SQLValue>(
		    new DuckDBSQLValue(false, SQLValue::Type::Double, std::to_string(duckdb_value_double(result, col, row))));

	case DUCKDB_TYPE_VARCHAR: {
		DuckDBOwnedString text(duckdb_value_varchar(result, col, row));
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(false, SQLValue::Type::String, text.str()));
	}

	case DUCKDB_TYPE_BLOB: {
		duckdb_blob blob = duckdb_value_blob(result, col, row);
		std::string text(static_cast<const char *>(blob.data), blob.size);
		duckdb_free(blob.data);
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(false, SQLValue::Type::String, std::move(text)));
	}

	default: {
		// Dates, timestamps, decimals, etc.: use the VARCHAR conversion
		DuckDBOwnedString text(duckdb_value_varchar(result, col, row));
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(false, SQLValue::Type::String, text.str()));
	}
	}
}

// RAII guard ensuring duckdb_destroy_result() runs even if conversion throws.
struct DuckDBResultGuard {
	duckdb_result result {};
	~DuckDBResultGuard() {
		duckdb_destroy_result(&result);
	}
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
	idx_t rowCount = duckdb_row_count(&guard.result);

	std::vector<std::string> columnNames(columnCount);
	for (idx_t col = 0; col < columnCount; ++col) {
		std::string name = duckdb_column_name(&guard.result, col);
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
		columnNames[col] = std::move(name);
	}

	std::vector<MapSQLRow> rows;
	rows.reserve(rowCount);
	for (idx_t row = 0; row < rowCount; ++row) {
		std::map<std::string, std::unique_ptr<SQLValue>> columns;
		for (idx_t col = 0; col < columnCount; ++col) {
			columns[columnNames[col]] = convertCell(&guard.result, col, row);
		}
		rows.emplace_back(std::move(columns));
	}

	return std::unique_ptr<SQLResultSet>(new DuckDBResultSet(std::move(rows)));
}

} // namespace r2rml
