#include "r2rml/DuckDBConnection.h"
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
// DuckDBResultSet
//
// Materialises all rows from a query result into memory, then iterates
// over them.  This avoids keeping the DuckDB result object alive across
// calls to next()/getCurrentRow() and keeps the interface simple.
// ---------------------------------------------------------------------------
class DuckDBResultSet : public SQLResultSet {
public:
    explicit DuckDBResultSet(std::vector<SQLRow> rows)
        : rows_(std::move(rows)) {}

    bool next() override {
        return ++cursor_ < static_cast<int>(rows_.size());
    }

    SQLRow getCurrentRow() const override {
        return rows_[static_cast<size_t>(cursor_)];
    }

private:
    std::vector<SQLRow> rows_;
    int cursor_{-1};
};

// ---------------------------------------------------------------------------
// DuckDBConnection::Impl
// ---------------------------------------------------------------------------
struct DuckDBConnection::Impl {
    duckdb::DuckDB    db;
    duckdb::Connection con;

    explicit Impl(const std::string& path) : db(path), con(db) {}
};

// ---------------------------------------------------------------------------
// Type conversion helper
// ---------------------------------------------------------------------------
static SQLValue duckValueToSQLValue(const duckdb::Value& val) {
    if (val.IsNull()) {
        return SQLValue();
    }

    switch (val.type().id()) {
        case duckdb::LogicalTypeId::BOOLEAN:
            return SQLValue(val.GetValue<bool>());

        case duckdb::LogicalTypeId::TINYINT:
        case duckdb::LogicalTypeId::SMALLINT:
        case duckdb::LogicalTypeId::INTEGER:
        case duckdb::LogicalTypeId::UTINYINT:
        case duckdb::LogicalTypeId::USMALLINT:
        case duckdb::LogicalTypeId::UINTEGER:
            return SQLValue(val.GetValue<int32_t>());

        // BIGINT and larger: store as string to avoid precision loss
        case duckdb::LogicalTypeId::BIGINT:
        case duckdb::LogicalTypeId::UBIGINT:
        case duckdb::LogicalTypeId::HUGEINT:
            return SQLValue(val.ToString());

        case duckdb::LogicalTypeId::FLOAT:
            return SQLValue(static_cast<double>(val.GetValue<float>()));

        case duckdb::LogicalTypeId::DOUBLE:
            return SQLValue(val.GetValue<double>());

        case duckdb::LogicalTypeId::VARCHAR:
        case duckdb::LogicalTypeId::BLOB:
            return SQLValue(val.GetValue<std::string>());

        default:
            // Dates, timestamps, decimals, etc.: use string representation
            return SQLValue(val.ToString());
    }
}

// ---------------------------------------------------------------------------
// DuckDBConnection
// ---------------------------------------------------------------------------
DuckDBConnection::DuckDBConnection(const std::string& path)
    : impl_(new Impl(path)) {}

DuckDBConnection::~DuckDBConnection() = default;

std::string DuckDBConnection::getDefaultSchema() {
    return "main";
}

std::unique_ptr<SQLResultSet>
DuckDBConnection::execute(const std::string& sqlQuery) {
    auto result = impl_->con.Query(sqlQuery);

    if (result->HasError()) {
        throw std::runtime_error("DuckDB query error: " + result->GetError());
    }

    std::vector<SQLRow> rows;

    while (true) {
        auto chunk = result->Fetch();
        if (!chunk || chunk->size() == 0) {
            break;
        }

        for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
            std::map<std::string, SQLValue> columns;
            for (duckdb::idx_t col = 0; col < chunk->ColumnCount(); ++col) {
                std::string colName = result->ColumnName(col);
                std::transform(colName.begin(), colName.end(),
                               colName.begin(),
                               [](unsigned char c){ return std::toupper(c); });
                columns[colName] =
                    duckValueToSQLValue(chunk->GetValue(col, row));
            }
            rows.emplace_back(std::move(columns));
        }
    }

    return std::unique_ptr<SQLResultSet>(new DuckDBResultSet(std::move(rows)));
}

} // namespace r2rml
