/**
 * Tests for constant-folding a VALUES binding into the triple patterns that
 * read it (see ValuesFolder.h): a VALUES column pinned to one constant term
 * must translate exactly like that constant written in the pattern, so the
 * R2RML term map gets inverted into a raw column equality instead of being
 * built forwards and joined on. Plus the cases that must NOT fold.
 */

#include <catch2/catch.hpp>

#include <string>

#ifndef SOURCE_SPARQL2SQL_DIR
#define SOURCE_SPARQL2SQL_DIR ""
#endif
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
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

std::string translateFixture(const std::string &queryFile, const char *mappingFile = "example_emp_dept.ttl") {
	Parser parser;
	auto query = parser.parseFile(SOURCE_SPARQL2SQL_DIR + queryFile);
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(std::string(SOURCE_R2RML_DIR) + mappingFile);
	REQUIRE(mapping.isValid());
	DuckDbDialect dialect;
	return translateQuery(*query, mapping, dialect);
}

// The inverted base-column equality, written without a table alias (which is
// a counter the fixtures must not depend on). No TypeCatalog is supplied here,
// so every column comparison goes through the always-correct VARCHAR cast.
const char *const kEmpnoEq = "\"EMPNO\" AS VARCHAR) = '7369'";
const char *const kEnameEqSmith = "\"ENAME\" AS VARCHAR) = 'SMITH'";

// The forward direction: the subject IRI assembled from its template columns.
// Its absence is what says the inversion actually happened.
bool buildsSubjectIri(const std::string &sql) {
	return sql.find("'http://data.example.com/employee/' || ") != std::string::npos;
}

} // namespace

TEST_CASE("values folding: a single-constant VALUES subject inverts the subject template") {
	const std::string sql = translateFixture("values_constant_subject.rq");
	CHECK(sql.find(kEmpnoEq) != std::string::npos);
	CHECK_FALSE(buildsSubjectIri(sql));
	// The inline relation is still emitted and joined, so ?e stays bound.
	CHECK(sql.find("'http://data.example.com/employee/7369'") != std::string::npos);
}

TEST_CASE("values folding: the inverted equality reaches every hop of a property path") {
	const std::string sql = translateFixture("values_constant_subject_path.rq");
	CHECK(sql.find(kEmpnoEq) != std::string::npos);
	CHECK_FALSE(buildsSubjectIri(sql));
}

TEST_CASE("values folding: a single-constant VALUES object inverts the object term map") {
	const std::string sql = translateFixture("values_constant_object.rq");
	CHECK(sql.find(kEnameEqSmith) != std::string::npos);
}

TEST_CASE("values folding: a query's trailing VALUES clause folds like an inline one") {
	const std::string sql = translateFixture("values_constant_trailing.rq");
	CHECK(sql.find(kEmpnoEq) != std::string::npos);
}

TEST_CASE("values folding: an OPTIONAL body inherits the enclosing constant") {
	const std::string sql = translateFixture("values_constant_optional.rq");
	CHECK(sql.find(kEmpnoEq) != std::string::npos);
	CHECK(sql.find("LEFT OUTER JOIN") != std::string::npos);
}

TEST_CASE("values folding: a multi-row VALUES is left alone") {
	const std::string sql = translateFixture("emp_dept_values.rq");
	CHECK(sql.find("'SMITH'") != std::string::npos);
	CHECK(sql.find("'JONES'") != std::string::npos);
	// Two alternatives, so no single constant to invert against ENAME.
	CHECK(sql.find(kEnameEqSmith) == std::string::npos);
}

TEST_CASE("values folding: an UNDEF-only column binds no term and does not fold") {
	const std::string sql = translateFixture("values_constant_undef.rq");
	CHECK(sql.find("= '7369'") == std::string::npos);
	CHECK(buildsSubjectIri(sql));
}

TEST_CASE("values folding: a zero-row VALUES still produces its WHERE FALSE table") {
	const std::string sql = translateFixture("emp_dept_values_empty.rq");
	CHECK(sql.find("WHERE FALSE") != std::string::npos);
}

TEST_CASE("values folding: a variable a FILTER reads is not folded out of its scope") {
	// Folding ?e away would leave STR(?e) with nothing to read in the relation
	// the FILTER is applied to.
	const std::string sql = translateFixture("values_constant_filtered.rq");
	CHECK(sql.find(kEmpnoEq) == std::string::npos);
	CHECK(buildsSubjectIri(sql));
}

TEST_CASE("values folding: a MINUS body keeps the shared variable that makes it an anti-join") {
	const std::string sql = translateFixture("values_constant_minus.rq");
	// The left side folds; the right side must not, or the two would share no
	// variable and MINUS would become a spec-mandated no-op.
	CHECK(sql.find("NOT EXISTS") != std::string::npos);
	CHECK(buildsSubjectIri(sql));
}
