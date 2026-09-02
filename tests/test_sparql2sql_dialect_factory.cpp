/**
 * Unit tests for sparql2sql::createDialect (DialectFactory.cpp) - the single
 * seam future dialects plug into. Only reachable from the CLI otherwise, so
 * it had no direct unit-test coverage.
 */

#include <catch2/catch.hpp>

#include <memory>
#include <stdexcept>

#include "sparql2sql/DialectFactory.h"
#include "sparql2sql/DuckDbDialect.h"

using sparql2sql::createDialect;
using sparql2sql::DuckDbDialect;

TEST_CASE("createDialect(\"duckdb\") returns a DuckDbDialect") {
	auto dialect = createDialect("duckdb");
	REQUIRE(dialect != nullptr);
	REQUIRE(dynamic_cast<DuckDbDialect *>(dialect.get()) != nullptr);
}

TEST_CASE("createDialect rejects an unknown dialect name") {
	REQUIRE_THROWS_AS(createDialect("postgres"), std::runtime_error);
}
