#pragma once

#include "r2rml/SQLConnection.h"
#include <memory>
#include <string>

namespace r2rml {

/**
 * Concrete SQLConnection implementation backed by DuckDB.
 *
 * Uses the PIMPL idiom so consumers of this header do not need duckdb.hpp
 * on their include path.
 *
 * Usage:
 *   DuckDBConnection conn("path/to/database.db");
 *   // or in-memory:
 *   DuckDBConnection conn(":memory:");
 */
class DuckDBConnection : public SQLConnection {
public:
    /**
     * Open (or create) the DuckDB database at the given file path.
     * Pass ":memory:" for a transient in-memory database.
     */
    explicit DuckDBConnection(const std::string& path);
    ~DuckDBConnection() override;

    std::unique_ptr<SQLResultSet> execute(const std::string& sqlQuery) override;

    /** Returns "main", DuckDB's default schema name. */
    std::string getDefaultSchema() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace r2rml
