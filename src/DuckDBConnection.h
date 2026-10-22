#pragma once

#include "r2rml/SQLConnection.h"
#include <memory>
#include <string>

namespace r2rml {

/**
 * Concrete SQLConnection implementation backed by DuckDB.
 *
 * Uses the PIMPL idiom so consumers of this header do not need duckdb.h
 * on their include path.
 *
 * Implemented against DuckDB's stable C API (duckdb.h), not the internal,
 * unstable C++ API (duckdb.hpp) - DuckDB 2.0 stopped bundling duckdb.hpp
 * in its prebuilt release packages, so the C API is the only portable way
 * to link a prebuilt DuckDB from CI.
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
	explicit DuckDBConnection(const std::string &path);
	~DuckDBConnection() override;

	std::unique_ptr<SQLResultSet> execute(const std::string &sqlQuery) override;

	/** Returns "main", DuckDB's default schema name. */
	[[nodiscard]] std::string getDefaultSchema() override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace r2rml
