/**
 * Unit tests for translateTriplePattern: the "alpha/beta inversion" that
 * turns a single SPARQL triple pattern into a SQL relation by enumerating
 * every candidate R2RML TriplesMap/PredicateObjectMap/rr:class source that
 * could produce a matching triple.
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

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
#include "sparql2sql/TermInfo.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/TriplePatternTranslator.h"
#include "sparql2sql/ir/RelNode.h"
#include "sparql2sql/ir/SqlRenderer.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql::ast::BasicGraphPattern;
using sparql2sql::DuckDbDialect;
using sparql2sql::renderRelation;
using sparql2sql::TranslatedPattern;
using sparql2sql::translateTriplePattern;
using sparql2sql::TranslationContext;
using sparql2sql::TranslationError;

namespace {

const sparql::ast::TriplePattern &nthTriple(const sparql::ast::Query &q, std::size_t elementIndex,
                                            std::size_t tripleIndex = 0) {
	const auto &el = *q.where->elements.at(elementIndex);
	const auto &bgp = static_cast<const BasicGraphPattern &>(el);
	return bgp.triples.at(tripleIndex);
}

// Translate a triple pattern to IR and render it back to a TranslatedPattern
// for structural (substring) inspection.
TranslatedPattern translated(const sparql::ast::TriplePattern &tp, TranslationContext &ctx) {
	return renderRelation(*translateTriplePattern(tp, ctx), ctx);
}

} // namespace

TEST_CASE("translateTriplePattern: multiple candidates combine via UNION BY NAME") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_simple_select.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translated(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.optionalVars.empty());
	// Both TriplesMap1 (EMP.ENAME) and TriplesMap2 (DEPT.DNAME) have an
	// ex:name predicate-object map, so this must be a UNION of >=2 candidates.
	CHECK(result.sql.find("UNION BY NAME") != std::string::npos);
	CHECK(result.sql.find("\"EMP\"") != std::string::npos);
	CHECK(result.sql.find("\"ENAME\"") != std::string::npos);
	CHECK(result.sql.find("\"DNAME\"") != std::string::npos);
}

TEST_CASE("translateTriplePattern: a ReferencingObjectMap POM generates a real SQL JOIN") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_join.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translated(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("d") == 1);
	CHECK(result.optionalVars.empty());
	CHECK(result.sql.find("JOIN") != std::string::npos);
	CHECK(result.sql.find("\"DEPTNO\"") != std::string::npos);
	CHECK(result.sql.find("\"EMP\"") != std::string::npos);
	// The parent (DEPT) side's logical table is an R2RMLView; its embedded
	// query text must appear (trailing ';' stripped) as a wrapped subquery.
	CHECK(result.sql.find("FROM DEPT") != std::string::npos);
}

TEST_CASE("translateTriplePattern: rr:class candidates for a constant rdf:type predicate") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_multi_placeholder.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_multi_placeholder.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translated(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("o") == 1);
	CHECK(result.sql.find("\"ORDERS\"") != std::string::npos);
	CHECK(result.sql.find("\"CUSTID\"") != std::string::npos);
	CHECK(result.sql.find("\"ORDERID\"") != std::string::npos);
}

TEST_CASE("translateTriplePattern: variable predicate matches both rr:class and predicate-object candidates") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_class.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translated(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("c") == 1);
	// Two TriplesMaps each with exactly one rr:class -> two branches. Since
	// the predicate ("a") is a bound constant equal to rdf:type, the match
	// is resolved statically (no runtime predicate check needed, so
	// rdf:type's IRI text need not appear in the SQL); ?c's value - the
	// class IRI itself - is what gets projected as a literal per branch.
	CHECK(result.sql.find("UNION BY NAME") != std::string::npos);
	CHECK(result.sql.find("http://example.com/ns#Employee") != std::string::npos);
	CHECK(result.sql.find("http://example.com/ns#Department") != std::string::npos);
}

TEST_CASE("translateTriplePattern: self-join guard for a variable repeated across positions") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_self_ref.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_self_ref.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translated(nthTriple(*q, 0), ctx);

	// Only ONE output column for ?e, despite it appearing as both subject
	// and object of the triple pattern.
	REQUIRE(result.boundVars.size() == 1);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.sql.find(" = ") != std::string::npos);
}

TEST_CASE("translateTriplePattern: bound predicate and bound object (rr:predicate/rr:object shortcuts)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_constant_pom.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_constant_pom.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	TranslatedPattern result = translated(nthTriple(*q, 0, 0), ctx);
	REQUIRE(result.boundVars.size() == 1);
	CHECK(result.boundVars.count("p") == 1);
	CHECK(result.sql.find("\"PEOPLE\"") != std::string::npos);
}

TEST_CASE("translateTriplePattern: bound predicate via the general rr:predicateMap[rr:constant] form") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_constant_pom.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_constant_pom.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	// Both triples sit in the same BasicGraphPattern (no combinator between
	// them), so the second triple is triples[1] of element 0.
	TranslatedPattern result = translated(nthTriple(*q, 0, 1), ctx);
	CHECK(result.boundVars.count("p") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.sql.find("\"NAME\"") != std::string::npos);
}

TEST_CASE("translateTriplePattern: a pattern matching zero candidates is a valid empty relation") {
	Parser parser;
	auto q = parser.parseString("PREFIX ex: <http://example.com/ns#>\n"
	                            "SELECT ?x ?y WHERE { ?x ex:doesNotExist ?y . }");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translated(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("x") == 1);
	CHECK(result.boundVars.count("y") == 1);
	CHECK(result.sql.find("WHERE FALSE") != std::string::npos);
}

TEST_CASE("translateTriplePattern: a pure rr:column subject map takes the native-key merge-signature path") {
	// parser_full_forms.ttl's TriplesMapFull uses rr:subjectMap [ rr:column
	// "EMPNO"; rr:class ex:Employee ] -- i.e. a *plain column* subject map
	// (Provenance::PureColumn), not rr:template. Matching its synthetic
	// rr:class candidate (mergeableSubject=true, subject is a variable) drives
	// tryAddCandidate's subjectKeySig computation through the "col:<column>"
	// branch, the sibling of the already-covered "tmpl:<template>" branch.
	Parser parser;
	auto q = parser.parseString("PREFIX ex: <http://example.com/ns#>\n"
	                            "SELECT * WHERE { ?s a ex:Employee }");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "parser_full_forms.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translated(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("s") == 1);
	CHECK(result.sql.find("\"EMP\"") != std::string::npos);
	CHECK(result.sql.find("\"EMPNO\"") != std::string::npos);
}

TEST_CASE("translateTriplePattern: a composite (multi-column) join condition renders AND-joined ON clauses") {
	// sparql2sql_composite_join.ttl's ReferencingObjectMap has TWO
	// rr:joinCondition entries (ORDERID and REGION), exercising the loop in
	// translateAtomicPattern that joins each ON-clause equality with " AND "
	// (only the first of two conditions was previously covered).
	Parser parser;
	auto q = parser.parseString("PREFIX ex: <http://example.com/ns#>\n"
	                            "SELECT * WHERE { ?l ex:order ?o }");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_composite_join.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translated(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("l") == 1);
	CHECK(result.boundVars.count("o") == 1);
	CHECK(result.sql.find("JOIN") != std::string::npos);
	CHECK(result.sql.find("\"ORDERID\"") != std::string::npos);
	CHECK(result.sql.find("\"REGION\"") != std::string::npos);
	CHECK(result.sql.find(" AND ") != std::string::npos);
}

// Every path operator, including the two recursive ones, desugars into an IR
// relation rather than reaching a translation error; see
// test_sparql2sql_paths.cpp for the property-path-specific structural
// coverage of `+`/`*`. This is a smoke test that translateTriplePattern's own
// entry point reaches the same result for `+` rather than throwing.
TEST_CASE("translateTriplePattern: the one-or-more property path operator translates without throwing") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "unsupported_property_path.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translated(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("s") == 1);
	CHECK(result.boundVars.count("o") == 1);
}

// ---------------------------------------------------------------------------
// The static RDF term dimension a triple pattern's translation carries. These
// are the observable half of the ColumnInfo::term plumbing: the annotation is
// derived at the producers (termMapToSqlExpr), met across candidate arms, and
// surfaced on the rendered TranslatedPattern by fillScopeFromSchema.
//
// Unknown reproduces the pre-annotation behaviour exactly, so a regression that
// wipes an annotation out is invisible to every assertion that looks at SQL
// text. These look at the annotation directly for that reason.
// ---------------------------------------------------------------------------
namespace {

TranslatedPattern translateTermsFixture(const char *rq, std::size_t elementIndex = 0, std::size_t tripleIndex = 0) {
	static Parser parser;
	static R2RMLParser mappingParser;
	static R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_terms.ttl");
	static DuckDbDialect dialect;
	// Kept alive for the returned pattern's lifetime by the caller's use of it.
	static std::vector<std::unique_ptr<sparql::ast::Query>> queries;
	queries.push_back(parser.parseString(rq));
	TranslationContext ctx(mapping, dialect);
	return translated(nthTriple(*queries.back(), elementIndex, tripleIndex), ctx);
}

const char *const kTermsPrefix = "PREFIX ex: <http://example.com/ns#>\n";

} // namespace

TEST_CASE("term annotation: a template subject is a known IRI") {
	TranslatedPattern r =
	    translateTermsFixture(std::string(std::string(kTermsPrefix) + "SELECT * WHERE { ?m ex:amount ?a }").c_str());
	CHECK(r.termInfoOf("m").kind == sparql2sql::RdfTermKind::Iri);
}

TEST_CASE("term annotation: a declared rr:datatype reaches the projected column") {
	TranslatedPattern r =
	    translateTermsFixture(std::string(std::string(kTermsPrefix) + "SELECT * WHERE { ?m ex:amount ?a }").c_str());
	const sparql2sql::TermInfo a = r.termInfoOf("a");
	CHECK(a.kind == sparql2sql::RdfTermKind::Literal);
	CHECK(a.datatypeIri == sparql2sql::xsd::kInteger);
	CHECK(a.isIntegral());
}

TEST_CASE("term annotation: rr:language yields rdf:langString plus the tag") {
	TranslatedPattern r =
	    translateTermsFixture(std::string(std::string(kTermsPrefix) + "SELECT * WHERE { ?m ex:title ?t }").c_str());
	const sparql2sql::TermInfo t = r.termInfoOf("t");
	CHECK(t.kind == sparql2sql::RdfTermKind::Literal);
	CHECK(t.datatypeIri == sparql2sql::kRdfLangString);
	CHECK(t.lang == "en");
}

TEST_CASE("term annotation: a bare rr:column is a known Literal with an unknown datatype") {
	// The distinction that keeps DATATYPE() honest: the *kind* is known (so
	// isLITERAL() may fold) while the datatype genuinely is not, because R2RML
	// would have derived it from the column's SQL type and no catalog is given.
	TranslatedPattern r =
	    translateTermsFixture(std::string(std::string(kTermsPrefix) + "SELECT * WHERE { ?m ex:plain ?p }").c_str());
	const sparql2sql::TermInfo p = r.termInfoOf("p");
	CHECK(p.kind == sparql2sql::RdfTermKind::Literal);
	CHECK(p.datatypeIri.empty());
	CHECK_FALSE(p.isNumeric());
}

TEST_CASE("term annotation: rr:termType rr:IRI on a column overrides the object-map default") {
	TranslatedPattern r =
	    translateTermsFixture(std::string(std::string(kTermsPrefix) + "SELECT * WHERE { ?m ex:homepage ?h }").c_str());
	CHECK(r.termInfoOf("h").kind == sparql2sql::RdfTermKind::Iri);
}

TEST_CASE("term annotation: an rr:BlankNode subject map is a known blank node") {
	TranslatedPattern r =
	    translateTermsFixture(std::string(std::string(kTermsPrefix) + "SELECT * WHERE { ?n ex:body ?b }").c_str());
	CHECK(r.termInfoOf("n").kind == sparql2sql::RdfTermKind::BlankNode);
}

TEST_CASE("term annotation: candidate arms that disagree on term kind meet to Unknown") {
	// ex:mixed is mapped by two triples maps, one producing an IRI object and
	// one a literal. Neither the kind nor the subject's kind survives.
	TranslatedPattern r =
	    translateTermsFixture(std::string(std::string(kTermsPrefix) + "SELECT * WHERE { ?s ex:mixed ?x }").c_str());
	CHECK(r.termInfoOf("x").kind == sparql2sql::RdfTermKind::Unknown);
	CHECK(r.termInfoOf("s").kind == sparql2sql::RdfTermKind::Unknown);
}
