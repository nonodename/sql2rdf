#include <catch2/catch_test_macros.hpp>

#include "sparql2sql/TypeCatalog.h"
#include "sparql2sql/ir/RelNode.h"

using namespace sparql2sql;

namespace {

ColumnInfo pureCol(const std::string &var, const std::string &alias, const std::string &col,
                   const std::string &table, bool nonNull) {
	ColumnInfo c;
	c.var = var;
	c.renderedExpr = "CAST(" + alias + ".\"" + col + "\" AS VARCHAR)";
	c.prov = Provenance::PureColumn;
	c.sourceAlias = alias;
	c.columnName = col;
	c.tableIdentity = table;
	c.nonNull = nonNull;
	return c;
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

	CHECK(rel.boundVars() == std::set<std::string>{"id"});
	CHECK(rel.optionalVars() == std::set<std::string>{"name"});
	CHECK(rel.allVars() == std::set<std::string>{"id", "name"});

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
