/**
 * Unit tests for sparql2sql::createDialect (DialectFactory.cpp) - the single
 * seam future dialects plug into. Only reachable from the CLI otherwise, so
 * it had no direct unit-test coverage.
 */

#include <catch2/catch_all.hpp>

#include <memory>
#include <stdexcept>

#include "sparql2sql/DialectFactory.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/DuckDbUdfDialect.h"
#include "sparql2sql/TemplateUtil.h"

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

TEST_CASE("createDialect(\"duckdb-udf\") / (\"duckdb-udf-all\") return the UDF dialect") {
	auto measured = createDialect("duckdb-udf");
	auto all = createDialect("duckdb-udf-all");
	REQUIRE(dynamic_cast<sparql2sql::DuckDbUdfDialect *>(measured.get()) != nullptr);
	REQUIRE(dynamic_cast<sparql2sql::DuckDbUdfDialect *>(all.get()) != nullptr);
	CHECK(measured->name() == "duckdb-udf");
	CHECK(all->name() == "duckdb-udf-all");
}

TEST_CASE("SqlDialect hooks: the inline defaults, and what each UDF coverage level emits") {
	const DuckDbDialect inl;
	const sparql2sql::DuckDbUdfDialect measured(sparql2sql::DuckDbUdfDialect::Coverage::Measured);
	const sparql2sql::DuckDbUdfDialect all(sparql2sql::DuckDbUdfDialect::Coverage::All);
	const auto segments = sparql2sql::parseTemplate("http://ex/{A}{B}");

	// The plain dialect is its own native dialect; the UDF ones defer to a plain one.
	CHECK(&inl.nativeDialect() == &inl);
	CHECK(measured.nativeDialect().name() == "duckdb");

	// Defaults are the inline SQL.
	CHECK(inl.iriTemplateProjection(segments, "t") ==
	      "('http://ex/' || url_encode(CAST(t.\"A\" AS VARCHAR)) || url_encode(CAST(t.\"B\" AS VARCHAR)))");
	CHECK(inl.termKindRank("d") == "(CASE WHEN d IS NULL THEN 0 WHEN d = 'B' THEN 1 WHEN d = 'I' THEN 2 ELSE 3 END)");
	CHECK(inl.coalesceTag({{"a", "x"}, {"b", "y"}}) ==
	      "(CASE WHEN a IS NOT NULL THEN x WHEN b IS NOT NULL THEN y ELSE NULL END)");

	// Measured coverage: comparisons and ORDER BY keys only.
	CHECK(measured.termEquality("l", "lt", "r", "rt", false) == "sql2rdf_eq(l, lt, r, rt)");
	CHECK(measured.termEquality("l", "lt", "r", "rt", true) == "sql2rdf_ne(l, lt, r, rt)");
	CHECK(measured.termOrdering("l", "lt", "r", "rt", "<=") == "sql2rdf_le(l, lt, r, rt)");
	CHECK(measured.untypedCompare("l", "r", ">") == "sql2rdf_untyped_gt(l, r)");
	CHECK(measured.termKindRank("d") == "sql2rdf_kind_rank(d)");
	CHECK(measured.termValueSpace("d") == "sql2rdf_value_space(d)");
	CHECK(measured.iriTemplateProjection(segments, "t") == inl.iriTemplateProjection(segments, "t"));
	CHECK(measured.coalesceTag({{"a", "x"}}) == inl.coalesceTag({{"a", "x"}}));

	// All: adjacent placeholders get an empty literal between them.
	CHECK(all.iriTemplateProjection(segments, "t") ==
	      "sql2rdf_iri('http://ex/', CAST(t.\"A\" AS VARCHAR), '', CAST(t.\"B\" AS VARCHAR), '')");
	CHECK(all.coalesceTag({{"a", "x"}, {"b", "y"}}) == "sql2rdf_tag_coalesce(a, x, b, y)");
	CHECK_THROWS_AS(measured.termOrdering("l", "lt", "r", "rt", "~"), std::invalid_argument);
}
