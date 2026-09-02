/**
 * Integration-level tests for self-join elimination on star-shaped BGPs
 * (Optimizer.cpp's eliminateSelfJoinsInSpj). A star pattern where every
 * triple resolves to a plain PredicateObjectMap on the same logical table
 * and the same subject template - after pruneUnionBranches disambiguates
 * ex:name against the rr:class/subject constraint - should compile to a
 * single scan with no JOIN and no UNION, rather than N self-joined scans.
 * Uses the full translateQuery pipeline against example_emp_dept.ttl.
 */

#include <catch2/catch.hpp>

#include <string>

#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif
#ifndef SOURCE_SPARQL2SQL_DIR
#define SOURCE_SPARQL2SQL_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/Translator.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::DuckDbDialect;
using sparql2sql::translateQuery;

namespace {

std::string translateFixture(const char *rqFile) {
	Parser parser;
	auto q = parser.parseFile(std::string(SOURCE_SPARQL2SQL_DIR) + rqFile);
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect);
}

} // namespace

TEST_CASE("translateQuery: a star BGP over one table collapses to a single scan") {
	std::string sql = translateFixture("emp_dept_star.rq");

	CHECK(sql.find("JOIN") == std::string::npos);
	CHECK(sql.find("UNION") == std::string::npos);

	// All four predicates resolve against DEPT; ex:name's ambiguous EMP.ENAME
	// candidate must be pruned, and none of EMP's columns should survive.
	CHECK(sql.find("\"DNAME\"") != std::string::npos);
	CHECK(sql.find("\"LOC\"") != std::string::npos);
	CHECK(sql.find("\"STAFF\"") != std::string::npos);
	CHECK(sql.find("\"ENAME\"") == std::string::npos);
	CHECK(sql.find("\"EMP\"") == std::string::npos);
}
