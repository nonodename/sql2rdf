/**
 * Integration-level tests for Optimizer.cpp's union-branch pruning pass
 * (pruneUnionBranches): a triple pattern with an ambiguous predicate compiles
 * to a UNION over every candidate mapping, but once joined with another
 * pattern on a shared variable, a candidate whose subject/object rr:template
 * can never equal the other side's is provably dead and dropped. Uses the
 * full translateQuery pipeline (fold -> optimize -> render) against
 * example_emp_dept.ttl, where ex:name is ambiguous (EMP.ENAME vs DEPT.DNAME)
 * but ex:department only ever comes from EMP.
 */

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("translateQuery: an ambiguous predicate alone still unions every candidate") {
	// Baseline: ex:name by itself is genuinely ambiguous (EMP.ENAME or
	// DEPT.DNAME), so nothing should be pruned without the join constraint.
	std::string sql = translateFixture("emp_dept_simple_select.rq");
	CHECK(sql.find("UNION BY NAME") != std::string::npos);
	CHECK(sql.find("\"ENAME\"") != std::string::npos);
	CHECK(sql.find("\"DNAME\"") != std::string::npos);
}

TEST_CASE("translateQuery: joining on the subject prunes the union arm with an incompatible subject template") {
	// ex:department only ever comes from TriplesMap1 (EMP, subject template
	// ".../employee/{EMPNO}"). Once ?e is also constrained by ex:department,
	// ex:name's DEPT.DNAME candidate (subject template ".../department/{DEPTNO}")
	// can never share that subject and is dead - the whole UNION collapses away
	// entirely, letting flatten() fuse both patterns into one joined SELECT.
	std::string sql = translateFixture("emp_dept_name_department_join.rq");
	CHECK(sql.find("UNION") == std::string::npos);
	CHECK(sql.find("\"DNAME\"") == std::string::npos);
	CHECK(sql.find("\"DEPT\"") == std::string::npos);
	CHECK(sql.find("\"ENAME\"") != std::string::npos);
	CHECK(sql.find("\"EMP\"") != std::string::npos);
}
