#include "DuckDBConnection.h"
#include "r2rml/MapSQLRow.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include "duckdb.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace r2rml {

// ---------------------------------------------------------------------------
// DuckDBSQLValue
//
// Lazily wraps a duckdb::Value, computing type and string representation on
// first access.  This is the DuckDB-specific concrete SQLValue implementation.
// ---------------------------------------------------------------------------
class DuckDBSQLValue : public SQLValue {
public:
	explicit DuckDBSQLValue(duckdb::Value val) : val_(std::move(val)) {
	}

	bool isNull() const override {
		return val_.IsNull();
	}

	Type type() const override {
		ensureConverted();
		return type_;
	}

	const std::string &asString() const override {
		ensureConverted();
		return string_;
	}

	std::unique_ptr<SQLValue> clone() const override {
		return std::unique_ptr<SQLValue>(new DuckDBSQLValue(val_));
	}

private:
	duckdb::Value val_;
	mutable Type type_ {Type::Null};
	mutable std::string string_;
	mutable bool converted_ {false};

	void ensureConverted() const {
		if (converted_) {
			return;
		}
		converted_ = true;

		if (val_.IsNull()) {
			type_ = Type::Null;
			return;
		}

		switch (val_.type().id()) {
		case duckdb::LogicalTypeId::BOOLEAN:
			type_ = Type::Boolean;
			string_ = val_.GetValue<bool>() ? "true" : "false";
			break;

		case duckdb::LogicalTypeId::TINYINT:
		case duckdb::LogicalTypeId::SMALLINT:
		case duckdb::LogicalTypeId::INTEGER:
		case duckdb::LogicalTypeId::UTINYINT:
		case duckdb::LogicalTypeId::USMALLINT:
		case duckdb::LogicalTypeId::UINTEGER:
			type_ = Type::Integer;
			string_ = std::to_string(val_.GetValue<int32_t>());
			break;

		// BIGINT and larger: store as string to avoid precision loss
		case duckdb::LogicalTypeId::BIGINT:
		case duckdb::LogicalTypeId::UBIGINT:
		case duckdb::LogicalTypeId::HUGEINT:
			type_ = Type::String;
			string_ = val_.ToString();
			break;

		case duckdb::LogicalTypeId::FLOAT:
			type_ = Type::Double;
			string_ = std::to_string(static_cast<double>(val_.GetValue<float>()));
			break;

		case duckdb::LogicalTypeId::DOUBLE:
			type_ = Type::Double;
			string_ = std::to_string(val_.GetValue<double>());
			break;

		case duckdb::LogicalTypeId::VARCHAR:
		case duckdb::LogicalTypeId::BLOB:
			type_ = Type::String;
			string_ = val_.GetValue<std::string>();
			break;

		default:
			// Dates, timestamps, decimals, etc.: use string representation
			type_ = Type::String;
			string_ = val_.ToString();
			break;
		}
	}
};

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
	duckdb::DuckDB db;
	duckdb::Connection con;

	explicit Impl(const std::string &path) : db(path), con(db) {
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
	auto result = impl_->con.Query(sqlQuery);

	if (result->HasError()) {
		throw std::runtime_error("DuckDB query error: " + result->GetError());
	}

	std::vector<MapSQLRow> rows;

	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}

		for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
			std::map<std::string, std::unique_ptr<SQLValue>> columns;
			for (duckdb::idx_t col = 0; col < chunk->ColumnCount(); ++col) {
				std::string colName = result->ColumnName(col);
				std::transform(colName.begin(), colName.end(), colName.begin(),
				               [](unsigned char c) { return std::toupper(c); });
				columns[colName] = std::unique_ptr<SQLValue>(new DuckDBSQLValue(chunk->GetValue(col, row)));
			}
			rows.emplace_back(std::move(columns));
		}
	}

	return std::unique_ptr<SQLResultSet>(new DuckDBResultSet(std::move(rows)));
}

} // namespace r2rml
