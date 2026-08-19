#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "r2rml/R2RMLMapping.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/TypeCatalog.h"
#include "sparql2sql/ir/Optimizer.h"
#include "sparql2sql/ir/RelNode.h"
#include "sparql2sql/ir/SqlRenderer.h"

using r2rml::R2RMLMapping;
using sparql2sql::ColumnInfo;
using sparql2sql::DuckDbDialect;
using sparql2sql::EquiKey;
using sparql2sql::JoinKind;
using sparql2sql::JoinNode;
using sparql2sql::optimize;
using sparql2sql::OptimizerOptions;
using sparql2sql::Provenance;
using sparql2sql::RelKind;
using sparql2sql::RelNodePtr;
using sparql2sql::renderRelation;
using sparql2sql::SpjRelation;
using sparql2sql::SpjSource;
using sparql2sql::TranslatedPattern;
using sparql2sql::TranslationContext;
using sparql2sql::TypeCatalog;

namespace {

ColumnInfo pureCol(const std::string &var, const std::string &alias, const std::string &col, const std::string &table,
                   bool nonNull) {
	ColumnInfo c;
	c.var = var;
	c.renderedExpr = "CAST(" + alias + ".\"" + col + "\" AS VARCHAR)";
	c.prov = Provenance::PureColumn;
	c.sourceAlias = alias;
	c.columnName = col;
	c.nativeColumnRef = alias + ".\"" + col + "\"";
	c.tableIdentity = table;
	c.nonNull = nonNull;
	return c;
}

// A single-source SpjRelation over `table AS alias` projecting `cols`.
RelNodePtr makeSpj(const std::string &alias, const std::string &table, std::vector<ColumnInfo> cols, bool distinct) {
	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	SpjSource src;
	src.sql = "\"" + table + "\" AS " + alias;
	src.alias = alias;
	src.tableIdentity = table;
	spj.sources.push_back(src);
	spj.distinct = distinct;
	spj.schema() = std::move(cols);
	return node;
}

// An inner/outer JoinNode over two children, joined on the single shared var.
RelNodePtr makeJoin(JoinKind kind, RelNodePtr left, RelNodePtr right, const std::string &sharedVar, bool nullSafe) {
	RelNodePtr node(new JoinNode());
	JoinNode &j = static_cast<JoinNode &>(*node);
	j.joinKind = kind;
	EquiKey k;
	k.var = sharedVar;
	if (const ColumnInfo *lc = left->column(sharedVar)) {
		k.leftCol = *lc;
	}
	if (const ColumnInfo *rc = right->column(sharedVar)) {
		k.rightCol = *rc;
	}
	k.nullSafe = nullSafe;
	j.keys.push_back(k);
	// Output schema: union of both sides' vars (nonNull carried from children;
	// for LeftOuter the right-only vars would be optional, but these tests only
	// inspect structure, so a simple union suffices).
	for (const auto &c : left->schema()) {
		j.schema().push_back(c);
	}
	for (const auto &c : right->schema()) {
		if (c.var != sharedVar) {
			j.schema().push_back(c);
		}
	}
	j.left = std::move(left);
	j.right = std::move(right);
	return node;
}

} // namespace

TEST_CASE("RelNode schema helpers partition bound and optional vars", "[sparql2sql][ir]") {
	SpjRelation rel;
	SpjSource src;
	src.sql = "\"company\" AS t1";
	src.alias = "t1";
	src.tableIdentity = "company";
	rel.sources.push_back(src);
	rel.distinct = true;
	rel.schema().push_back(pureCol("id", "t1", "ID", "company", /*nonNull=*/true));
	rel.schema().push_back(pureCol("name", "t1", "legalName", "company", /*nonNull=*/false));

	CHECK(rel.boundVars() == std::set<std::string> {"id"});
	CHECK(rel.optionalVars() == std::set<std::string> {"name"});
	CHECK(rel.allVars() == std::set<std::string> {"id", "name"});

	const ColumnInfo *idCol = rel.column("id");
	REQUIRE(idCol != nullptr);
	CHECK(idCol->prov == Provenance::PureColumn);
	CHECK(idCol->columnName == "ID");
	CHECK(rel.column("missing") == nullptr);
}

TEST_CASE("RelNode kinds are preserved through the base pointer", "[sparql2sql][ir]") {
	RelNodePtr node(new SpjRelation());
	CHECK(node->kind() == RelKind::Spj);

	RelNodePtr join(new JoinNode());
	CHECK(join->kind() == RelKind::Join);
	static_cast<JoinNode &>(*join).joinKind = JoinKind::LeftOuter;
	CHECK(static_cast<const JoinNode &>(*join).joinKind == JoinKind::LeftOuter);
}

TEST_CASE("TypeCatalog::comparable gates native join keys by type category", "[sparql2sql][ir]") {
	TypeCatalog cat;
	cat.columnTypes["company"]["capIQCompanyID"] = "BIGINT";
	cat.columnTypes["company"]["ID"] = "VARCHAR";
	cat.columnTypes["relations"]["parent"] = "BIGINT";
	cat.columnTypes["relations"]["pct"] = "DOUBLE";

	// Same integer category on both sides: safe.
	CHECK(cat.comparable("company", "capIQCompanyID", "relations", "parent"));
	// Integer vs varchar: not safe.
	CHECK_FALSE(cat.comparable("company", "capIQCompanyID", "company", "ID"));
	// Integer vs float: conservatively not safe.
	CHECK_FALSE(cat.comparable("relations", "parent", "relations", "pct"));
	// Unknown column: not safe.
	CHECK_FALSE(cat.comparable("company", "capIQCompanyID", "relations", "missing"));
}

TEST_CASE("optimize: an inner join of two SpjRelations flattens into one multi-source block", "[sparql2sql][ir]") {
	std::vector<ColumnInfo> leftCols = {pureCol("a", "t1", "A", "company", true),
	                                    pureCol("k", "t1", "K", "company", true)};
	std::vector<ColumnInfo> rightCols = {pureCol("k", "t2", "K2", "relations", true),
	                                     pureCol("b", "t2", "B", "relations", true)};
	RelNodePtr join = makeJoin(JoinKind::Inner, makeSpj("t1", "company", leftCols, true),
	                           makeSpj("t2", "relations", rightCols, true), "k", /*nullSafe=*/false);

	OptimizerOptions opts; // no catalog, not top-level distinct
	RelNodePtr result = optimize(std::move(join), opts);

	REQUIRE(result->kind() == RelKind::Spj);
	const SpjRelation &spj = static_cast<const SpjRelation &>(*result);
	CHECK(spj.sources.size() == 2);
	CHECK(spj.distinct); // lifted from the per-pattern DISTINCTs
	// The join equality was folded into WHERE, VARCHAR-cast (no catalog).
	bool foundJoinEq = false;
	for (const auto &c : spj.whereConds) {
		if (c == "CAST(t1.\"K\" AS VARCHAR) = CAST(t2.\"K2\" AS VARCHAR)") {
			foundJoinEq = true;
		}
	}
	CHECK(foundJoinEq);
	CHECK(spj.allVars() == std::set<std::string> {"a", "k", "b"});
}

TEST_CASE("optimize: a left outer join is a flattening boundary (not merged)", "[sparql2sql][ir]") {
	std::vector<ColumnInfo> leftCols = {pureCol("k", "t1", "K", "company", true)};
	std::vector<ColumnInfo> rightCols = {pureCol("k", "t2", "K2", "relations", true),
	                                     pureCol("b", "t2", "B", "relations", true)};
	RelNodePtr join = makeJoin(JoinKind::LeftOuter, makeSpj("t1", "company", leftCols, true),
	                           makeSpj("t2", "relations", rightCols, true), "k", /*nullSafe=*/false);

	OptimizerOptions opts;
	RelNodePtr result = optimize(std::move(join), opts);
	CHECK(result->kind() == RelKind::Join); // preserved
}

TEST_CASE("optimize: top-level DISTINCT strips the per-pattern DISTINCT after flattening", "[sparql2sql][ir]") {
	std::vector<ColumnInfo> leftCols = {pureCol("k", "t1", "K", "company", true)};
	std::vector<ColumnInfo> rightCols = {pureCol("k", "t2", "K2", "relations", true)};
	RelNodePtr join = makeJoin(JoinKind::Inner, makeSpj("t1", "company", leftCols, true),
	                           makeSpj("t2", "relations", rightCols, true), "k", /*nullSafe=*/false);

	OptimizerOptions opts;
	opts.topLevelDistinct = true;
	RelNodePtr result = optimize(std::move(join), opts);

	REQUIRE(result->kind() == RelKind::Spj);
	CHECK_FALSE(static_cast<const SpjRelation &>(*result).distinct);
}

TEST_CASE("optimize: a type catalog enables a native (uncast) join key", "[sparql2sql][ir]") {
	std::vector<ColumnInfo> leftCols = {pureCol("k", "t1", "capIQCompanyID", "company", true)};
	std::vector<ColumnInfo> rightCols = {pureCol("k", "t2", "parent", "relations", true)};
	RelNodePtr join = makeJoin(JoinKind::Inner, makeSpj("t1", "company", leftCols, true),
	                           makeSpj("t2", "relations", rightCols, true), "k", /*nullSafe=*/false);

	TypeCatalog cat;
	cat.columnTypes["company"]["capIQCompanyID"] = "BIGINT";
	cat.columnTypes["relations"]["parent"] = "BIGINT";
	OptimizerOptions opts;
	opts.catalog = &cat;
	RelNodePtr result = optimize(std::move(join), opts);

	REQUIRE(result->kind() == RelKind::Spj);
	const SpjRelation &spj = static_cast<const SpjRelation &>(*result);
	bool nativeJoin = false;
	for (const auto &c : spj.whereConds) {
		if (c == "t1.\"capIQCompanyID\" = t2.\"parent\"") {
			nativeJoin = true;
		}
	}
	CHECK(nativeJoin);
}

namespace {

// A projected column produced by an rr:template over a single placeholder
// column - the shape almost every R2RML subject map has.
ColumnInfo tmplCol(const std::string &var, const std::string &alias, const std::string &prefix,
                   const std::string &placeholder, const std::string &table) {
	ColumnInfo c;
	c.var = var;
	c.renderedExpr = "('" + prefix + "' || CAST(" + alias + ".\"" + placeholder + "\" AS VARCHAR))";
	c.prov = Provenance::TemplateExpr;
	c.sourceAlias = alias;
	c.tableIdentity = table;
	// Note the template string embeds the placeholder's column NAME, so two
	// sides can only share a template if they also share that column name -
	// they differ in table and alias, which is exactly the shape a SPARQL
	// variable bound to one map's subject IRI and another's object IRI takes.
	c.templateString = prefix + "{" + placeholder + "}";
	c.templateColumnNames.push_back(placeholder);
	c.templateColumnRefs.push_back(alias + ".\"" + placeholder + "\"");
	c.templateInvertible = true;
	c.nonNull = true;
	return c;
}

bool hasCond(const sparql2sql::RelNode &node, const std::string &cond) {
	const SpjRelation &spj = static_cast<const SpjRelation &>(node);
	for (const auto &c : spj.whereConds) {
		if (c == cond) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("optimize: two same-template subject keys join on their placeholder columns", "[sparql2sql][ir]") {
	// The dominant real-world join shape: a SPARQL variable bound to an IRI
	// built by the same rr:template on both sides. Without this rewrite the join
	// hashes constructed IRI strings instead of the underlying key columns.
	std::vector<ColumnInfo> leftCols = {tmplCol("k", "t1", "http://x/d/", "DEPTNO", "company")};
	std::vector<ColumnInfo> rightCols = {tmplCol("k", "t2", "http://x/d/", "DEPTNO", "relations")};
	RelNodePtr join = makeJoin(JoinKind::Inner, makeSpj("t1", "company", leftCols, true),
	                           makeSpj("t2", "relations", rightCols, true), "k", /*nullSafe=*/false);

	TypeCatalog cat;
	cat.columnTypes["company"]["DEPTNO"] = "BIGINT";
	cat.columnTypes["relations"]["DEPTNO"] = "BIGINT";
	OptimizerOptions opts;
	opts.catalog = &cat;
	RelNodePtr result = optimize(std::move(join), opts);

	REQUIRE(result->kind() == RelKind::Spj);
	CHECK(hasCond(*result, "t1.\"DEPTNO\" = t2.\"DEPTNO\""));
}

namespace {

// The VARCHAR-cast fallback the rewrite must leave in place when it is unsound.
const char *const kStringCompare = "('http://x/d/' || CAST(t1.\"DEPTNO\" AS VARCHAR)) = "
                                   "('http://x/d/' || CAST(t2.\"DEPTNO\" AS VARCHAR))";

TypeCatalog comparableCatalog() {
	TypeCatalog cat;
	cat.columnTypes["company"]["DEPTNO"] = "BIGINT";
	cat.columnTypes["relations"]["DEPTNO"] = "BIGINT";
	return cat;
}

RelNodePtr templateJoin(std::vector<ColumnInfo> leftCols, std::vector<ColumnInfo> rightCols) {
	return makeJoin(JoinKind::Inner, makeSpj("t1", "company", std::move(leftCols), true),
	                makeSpj("t2", "relations", std::move(rightCols), true), "k", /*nullSafe=*/false);
}

} // namespace

TEST_CASE("optimize: different template strings keep the term-text join key", "[sparql2sql][ir]") {
	// Two templates that only coincide by construction in this fixture: equal
	// generated text does not imply equal placeholder columns.
	std::vector<ColumnInfo> leftCols = {tmplCol("k", "t1", "http://x/d/", "DEPTNO", "company")};
	std::vector<ColumnInfo> rightCols = {tmplCol("k", "t2", "http://y/d/", "DEPTNO", "relations")};
	rightCols[0].renderedExpr = "('http://x/d/' || CAST(t2.\"DEPTNO\" AS VARCHAR))";
	TypeCatalog cat = comparableCatalog();
	OptimizerOptions opts;
	opts.catalog = &cat;
	CHECK(hasCond(*optimize(templateJoin(leftCols, rightCols), opts), kStringCompare));
}

TEST_CASE("optimize: a non-invertible template keeps the term-text join key", "[sparql2sql][ir]") {
	// Adjacent placeholders make the generated text ambiguous to split, so it
	// does not determine the placeholder values.
	std::vector<ColumnInfo> leftCols = {tmplCol("k", "t1", "http://x/d/", "DEPTNO", "company")};
	std::vector<ColumnInfo> rightCols = {tmplCol("k", "t2", "http://x/d/", "DEPTNO", "relations")};
	leftCols[0].templateInvertible = false;
	TypeCatalog cat = comparableCatalog();
	OptimizerOptions opts;
	opts.catalog = &cat;
	CHECK(hasCond(*optimize(templateJoin(leftCols, rightCols), opts), kStringCompare));
}

TEST_CASE("optimize: without a catalog a same-template join key stays VARCHAR-cast", "[sparql2sql][ir]") {
	std::vector<ColumnInfo> leftCols = {tmplCol("k", "t1", "http://x/d/", "DEPTNO", "company")};
	std::vector<ColumnInfo> rightCols = {tmplCol("k", "t2", "http://x/d/", "DEPTNO", "relations")};
	OptimizerOptions opts; // no catalog: placeholder types unknown
	CHECK(hasCond(*optimize(templateJoin(leftCols, rightCols), opts), kStringCompare));
}

namespace {

// A single-source SpjRelation carrying self-join-elimination subject metadata.
RelNodePtr makeSubjectSpj(const std::string &alias, const std::string &table, const std::string &subjectVar,
                          const std::string &subjectKeySig, std::vector<ColumnInfo> cols, bool distinct) {
	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	SpjSource src;
	src.sql = "\"" + table + "\" AS " + alias;
	src.alias = alias;
	src.tableIdentity = table;
	src.subjectVar = subjectVar;
	src.subjectKeySig = subjectKeySig;
	spj.sources.push_back(src);
	spj.distinct = distinct;
	spj.schema() = std::move(cols);
	return node;
}

ColumnInfo templateSubjectCol(const std::string &var, const std::string &alias) {
	ColumnInfo c;
	c.var = var;
	c.renderedExpr = "('person/' || CAST(" + alias + ".\"ID\" AS VARCHAR))";
	c.prov = Provenance::TemplateExpr;
	c.sourceAlias = alias;
	c.templateString = "person/{ID}";
	c.nonNull = true;
	return c;
}

} // namespace

TEST_CASE("optimize: self-join elimination collapses same-table same-subject scans", "[sparql2sql][ir]") {
	// Two patterns over PEOPLE both with subject ?p (template person/{ID}),
	// projecting different object columns - a self-join on the subject key.
	std::vector<ColumnInfo> leftCols = {templateSubjectCol("p", "t1"), pureCol("a", "t1", "A", "people", true)};
	std::vector<ColumnInfo> rightCols = {templateSubjectCol("p", "t2"), pureCol("b", "t2", "B", "people", true)};
	RelNodePtr join =
	    makeJoin(JoinKind::Inner, makeSubjectSpj("t1", "people", "p", "tmpl:person/{ID}", leftCols, true),
	             makeSubjectSpj("t2", "people", "p", "tmpl:person/{ID}", rightCols, true), "p", /*nullSafe=*/false);

	OptimizerOptions opts;
	RelNodePtr result = optimize(std::move(join), opts);

	REQUIRE(result->kind() == RelKind::Spj);
	const SpjRelation &spj = static_cast<const SpjRelation &>(*result);
	// Both PEOPLE scans merged into one source.
	CHECK(spj.sources.size() == 1);
	CHECK(spj.allVars() == std::set<std::string> {"p", "a", "b"});
	// ?b's column reference was rewritten from the dropped t2 onto the kept t1.
	const ColumnInfo *b = spj.column("b");
	REQUIRE(b != nullptr);
	CHECK(b->sourceAlias == "t1");
	CHECK(b->renderedExpr == "CAST(t1.\"B\" AS VARCHAR)");
	// The subject-key equality collapsed to trivially-true and was dropped.
	for (const auto &c : spj.whereConds) {
		CHECK(c.find(" = ") == std::string::npos);
	}
}

// --- SqlRenderer: renderSpj / renderChild native-key hidden projection ---
//
// planNativeKeys only ever populates a non-empty `extra` (the hidden
// hidden-key-column projections renderChild needs to append across a
// derived-table boundary) when both join sides are directly an SpjRelation
// and the key is non-null-safe with catalog-comparable native columns; it is
// only ever called by renderJoin/renderAntiJoin on an unflattened JoinNode
// (LeftOuter is a flattening boundary, and these tests never call optimize()
// at all), so renderRelation on a hand-built JoinNode is what actually drives
// the native-key path end to end.

TEST_CASE("renderSpj: a native-key hidden projection is appended alongside existing schema columns",
          "[sparql2sql][ir]") {
	// Both join sides keep their own projected columns (schema non-empty) while
	// also picking up the hidden native-key column - the common real shape,
	// and previously untested: every existing renderSpj call rendered with
	// `extra == nullptr` (see NativeKey-free tests above), never a non-null,
	// non-empty one alongside a populated schema.
	std::vector<ColumnInfo> leftCols = {pureCol("k", "t1", "capIQCompanyID", "company", true),
	                                    pureCol("a", "t1", "A", "company", true)};
	std::vector<ColumnInfo> rightCols = {pureCol("k", "t2", "parent", "relations", true),
	                                     pureCol("b", "t2", "B", "relations", true)};
	RelNodePtr join = makeJoin(JoinKind::LeftOuter, makeSpj("t1", "company", leftCols, false),
	                           makeSpj("t2", "relations", rightCols, false), "k", /*nullSafe=*/false);

	TypeCatalog cat;
	cat.columnTypes["company"]["capIQCompanyID"] = "BIGINT";
	cat.columnTypes["relations"]["parent"] = "BIGINT";
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect, &cat);

	// Rendered directly (no optimize()): a LeftOuter JoinNode is never
	// flattened, so renderJoin -> planNativeKeys -> renderChild -> renderSpj
	// run exactly as they would for a real OPTIONAL query.
	TranslatedPattern result = renderRelation(*join, ctx);

	// The hidden "k_0" native-key column is projected by both sides (each via
	// renderChild's extra-carrying renderSpj call) and used for the join
	// condition instead of the term-text comparison.
	CHECK(result.sql.find("\"k_0\"") != std::string::npos);
	CHECK(result.sql.find("t1.\"capIQCompanyID\"") != std::string::npos);
	CHECK(result.sql.find("t2.\"parent\"") != std::string::npos);
}

TEST_CASE("renderSpj: a schema-less join side still surfaces its native-key hidden projection", "[sparql2sql][ir]") {
	// A degenerate but legal IR shape: the left join side contributes no
	// columns of its own to the output (its schema is empty), but the
	// EquiKey's leftCol/rightCol - independent of what is or isn't in that
	// side's own schema - still make it native-key-eligible. This targets
	// renderSpj's `rel.schema().empty() && (extra == nullptr || extra->empty())`
	// condition: schema *is* empty, but extra is non-null and non-empty, so
	// the overall condition is false and the "1 AS _dummy" placeholder must
	// NOT be emitted - the hidden key column becomes the entire SELECT list.
	RelNodePtr left(new SpjRelation());
	{
		SpjRelation &spj = static_cast<SpjRelation &>(*left);
		SpjSource src;
		src.sql = "\"company\" AS t1";
		src.alias = "t1";
		src.tableIdentity = "company";
		spj.sources.push_back(src);
		// No schema columns pushed: this side projects nothing of its own.
	}
	RelNodePtr right(new SpjRelation());
	{
		SpjRelation &spj = static_cast<SpjRelation &>(*right);
		SpjSource src;
		src.sql = "\"relations\" AS t2";
		src.alias = "t2";
		src.tableIdentity = "relations";
		spj.sources.push_back(src);
		spj.schema().push_back(pureCol("b", "t2", "B", "relations", true));
	}

	RelNodePtr node(new JoinNode());
	JoinNode &j = static_cast<JoinNode &>(*node);
	j.joinKind = JoinKind::Inner;
	EquiKey k;
	k.var = "k";
	k.leftCol = pureCol("k", "t1", "capIQCompanyID", "company", true);
	k.rightCol = pureCol("k", "t2", "parent", "relations", true);
	k.nullSafe = false;
	j.keys.push_back(k);
	j.schema().push_back(pureCol("b", "t2", "B", "relations", true));
	j.left = std::move(left);
	j.right = std::move(right);

	TypeCatalog cat;
	cat.columnTypes["company"]["capIQCompanyID"] = "BIGINT";
	cat.columnTypes["relations"]["parent"] = "BIGINT";
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect, &cat);

	TranslatedPattern result = renderRelation(*node, ctx);

	// The hidden key column is present (the empty-schema side is not reduced
	// to the dummy placeholder), sourced from the native, uncast column.
	CHECK(result.sql.find("\"k_0\"") != std::string::npos);
	CHECK(result.sql.find("t1.\"capIQCompanyID\"") != std::string::npos);
}

// --- optimize: redundant-predicate removal --------------------------------

TEST_CASE("optimize: a redundant IS NOT NULL is dropped when the column is already proven = 'literal'",
          "[sparql2sql][ir]") {
	// Mirrors what TriplePatternTranslator actually builds: a requiredNonNull
	// guard on the uncast column ref, plus a CAST-wrapped equality from
	// inverting the same term map against a bound object term.
	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	SpjSource src;
	src.sql = "\"EMP\" AS t1";
	src.alias = "t1";
	src.tableIdentity = "EMP";
	spj.sources.push_back(src);
	spj.whereConds.emplace_back("t1.\"ENAME\" IS NOT NULL");
	spj.whereConds.emplace_back("CAST(t1.\"ENAME\" AS VARCHAR) = 'SMITH'");
	spj.distinct = true;
	spj.schema().push_back(pureCol("s", "t1", "EMPNO", "EMP", true));

	OptimizerOptions opts;
	RelNodePtr result = optimize(std::move(node), opts);

	REQUIRE(result->kind() == RelKind::Spj);
	const SpjRelation &out = static_cast<const SpjRelation &>(*result);
	REQUIRE(out.whereConds.size() == 1);
	CHECK(out.whereConds[0] == "CAST(t1.\"ENAME\" AS VARCHAR) = 'SMITH'");
}

TEST_CASE("optimize: an IS NOT NULL guard survives when no literal equality proves it", "[sparql2sql][ir]") {
	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	SpjSource src;
	src.sql = "\"EMP\" AS t1";
	src.alias = "t1";
	src.tableIdentity = "EMP";
	spj.sources.push_back(src);
	spj.whereConds.emplace_back("t1.\"ENAME\" IS NOT NULL");
	spj.distinct = true;
	spj.schema().push_back(pureCol("s", "t1", "EMPNO", "EMP", true));

	OptimizerOptions opts;
	RelNodePtr result = optimize(std::move(node), opts);

	const SpjRelation &out = static_cast<const SpjRelation &>(*result);
	REQUIRE(out.whereConds.size() == 1);
	CHECK(out.whereConds[0] == "t1.\"ENAME\" IS NOT NULL");
}

TEST_CASE("optimize: an IS NOT NULL guard on a different column is unaffected", "[sparql2sql][ir]") {
	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	SpjSource src;
	src.sql = "\"EMP\" AS t1";
	src.alias = "t1";
	src.tableIdentity = "EMP";
	spj.sources.push_back(src);
	spj.whereConds.emplace_back("t1.\"JOB\" IS NOT NULL");
	spj.whereConds.emplace_back("CAST(t1.\"ENAME\" AS VARCHAR) = 'SMITH'");
	spj.distinct = true;
	spj.schema().push_back(pureCol("s", "t1", "EMPNO", "EMP", true));

	OptimizerOptions opts;
	RelNodePtr result = optimize(std::move(node), opts);

	const SpjRelation &out = static_cast<const SpjRelation &>(*result);
	REQUIRE(out.whereConds.size() == 2);
	CHECK(out.whereConds[0] == "t1.\"JOB\" IS NOT NULL");
}

TEST_CASE("optimize: a comparison against a view's constant SELECT column is dropped, given a context",
          "[sparql2sql][ir]") {
	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	const std::string viewSql = "SELECT EMPNO, true AS FLAG FROM EMP";
	SpjSource src;
	src.sql = "(" + viewSql + ") AS t1";
	src.alias = "t1";
	src.tableIdentity = "view:" + viewSql;
	spj.sources.push_back(src);
	// requiredNonNull guard plus the inverted equality, exactly as the
	// translator would build for `?s ex:flag "true" .` against this view.
	spj.whereConds.emplace_back("t1.\"FLAG\" IS NOT NULL");
	spj.whereConds.emplace_back("CAST(t1.\"FLAG\" AS VARCHAR) = 'true'");
	spj.distinct = true;
	spj.schema().push_back(pureCol("s", "t1", "EMPNO", "view:" + viewSql, true));

	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	OptimizerOptions opts;
	opts.ctx = &ctx;
	RelNodePtr result = optimize(std::move(node), opts);

	const SpjRelation &out = static_cast<const SpjRelation &>(*result);
	// Both the guard (case 1) and the now-vacuous equality (case 2) are gone.
	CHECK(out.whereConds.empty());
}

TEST_CASE("optimize: a comparison against a view's constant column survives without a context", "[sparql2sql][ir]") {
	// Without a TranslationContext there is no SqlDialect to render the
	// expected column reference against, so the view-constant pass is
	// skipped entirely - the always-correct (if suboptimal) fallback.
	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	const std::string viewSql = "SELECT EMPNO, true AS FLAG FROM EMP";
	SpjSource src;
	src.sql = "(" + viewSql + ") AS t1";
	src.alias = "t1";
	src.tableIdentity = "view:" + viewSql;
	spj.sources.push_back(src);
	spj.whereConds.emplace_back("CAST(t1.\"FLAG\" AS VARCHAR) = 'true'");
	spj.distinct = true;
	spj.schema().push_back(pureCol("s", "t1", "EMPNO", "view:" + viewSql, true));

	OptimizerOptions opts;
	RelNodePtr result = optimize(std::move(node), opts);

	const SpjRelation &out = static_cast<const SpjRelation &>(*result);
	REQUIRE(out.whereConds.size() == 1);
	CHECK(out.whereConds[0] == "CAST(t1.\"FLAG\" AS VARCHAR) = 'true'");
}

TEST_CASE("optimize: a comparison against a non-constant value doesn't match the view's fixed literal",
          "[sparql2sql][ir]") {
	RelNodePtr node(new SpjRelation());
	SpjRelation &spj = static_cast<SpjRelation &>(*node);
	const std::string viewSql = "SELECT EMPNO, true AS FLAG FROM EMP";
	SpjSource src;
	src.sql = "(" + viewSql + ") AS t1";
	src.alias = "t1";
	src.tableIdentity = "view:" + viewSql;
	spj.sources.push_back(src);
	// "false" never appears for FLAG in this view, so the comparison is not
	// vacuously true and must be left alone.
	spj.whereConds.emplace_back("CAST(t1.\"FLAG\" AS VARCHAR) = 'false'");
	spj.distinct = true;
	spj.schema().push_back(pureCol("s", "t1", "EMPNO", "view:" + viewSql, true));

	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	OptimizerOptions opts;
	opts.ctx = &ctx;
	RelNodePtr result = optimize(std::move(node), opts);

	const SpjRelation &out = static_cast<const SpjRelation &>(*result);
	REQUIRE(out.whereConds.size() == 1);
	CHECK(out.whereConds[0] == "CAST(t1.\"FLAG\" AS VARCHAR) = 'false'");
}
