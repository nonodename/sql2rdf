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

	/**
	 * The underlying `duckdb_connection` handle, type-erased so this header
	 * stays free of duckdb.h. Intended for dev tooling (the benchmark harness)
	 * that needs the C API's prepared-statement and PRAGMA surface; the pointer
	 * is owned by this object and valid only for its lifetime.
	 */
	[[nodiscard]] void *nativeConnection() const;

	/**
	 * Register the sql2rdf_* scalar functions that SQL translated for the
	 * "duckdb-udf" dialect (sparql2sql::DuckDbUdfDialect) calls. Idempotent;
	 * must run on this connection before executing any such SQL. Throws
	 * std::runtime_error if DuckDB refuses a registration.
	 */
	void registerRdfFunctions();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	bool rdfFunctionsRegistered_ = false;
};

} // namespace r2rml
