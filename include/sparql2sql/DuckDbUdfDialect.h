#pragma once

#include "sparql2sql/DuckDbDialect.h"

namespace sparql2sql {

/// DuckDB dialect that renders RDF term logic as calls to scalar functions
/// registered on the connection, instead of DuckDbDialect's inline CASE SQL.
/// Selected by name as "duckdb-udf".
///
/// This library only *emits* the calls - it has no DuckDB dependency. The
/// functions themselves are implemented and registered by the DuckDB-backed
/// connection (src/DuckDBConnection.cpp, `registerRdfFunctions()`), which must
/// be called on every connection that runs SQL translated for this dialect.
///
/// Every function is result-identical to the inline SQL it replaces. The
/// translator calls these hooks only where an opaque call cannot cost DuckDB a
/// pushdown (never in a join key, never in an optimizer-folded WHERE conjunct;
/// see TranslationContext::dialect()), and falls back to nativeDialect() - a
/// plain DuckDbDialect - everywhere else.
///
/// Emitted functions (all VARCHAR in, see doc/api.md for exact semantics):
///   sql2rdf_iri(lit0, v1, lit1, ..., vn, litn)   -> VARCHAR  rr:IRI template
///   sql2rdf_tag_coalesce(v1, t1, ..., vn, tn)    -> VARCHAR  tag of COALESCE
///   sql2rdf_value_space(tag)                     -> INTEGER  kValueSpace* id
///   sql2rdf_kind_rank(tag)                       -> INTEGER  Section 15.1 rank
///   sql2rdf_{eq,ne,lt,gt,le,ge}(l, ltag, r, rtag) -> BOOLEAN  tag-dispatched compare
///   sql2rdf_untyped_{eq,ne,lt,gt,le,ge}(l, r)     -> BOOLEAN  untyped fallback compare
class DuckDbUdfDialect : public DuckDbDialect {
public:
	std::string name() const override;

	const SqlDialect &nativeDialect() const override;
	std::string iriTemplateProjection(const std::vector<TemplateSegment> &segments,
	                                  const std::string &sourceAlias) const override;
	std::string coalesceTag(const std::vector<std::pair<std::string, std::string>> &valueTagPairs) const override;
	std::string termValueSpace(const std::string &tagSql) const override;
	std::string termKindRank(const std::string &tagSql) const override;
	std::string termEquality(const std::string &leftSql, const std::string &leftTag, const std::string &rightSql,
	                         const std::string &rightTag, bool negated) const override;
	std::string termOrdering(const std::string &leftSql, const std::string &leftTag, const std::string &rightSql,
	                         const std::string &rightTag, const std::string &op) const override;
	std::string untypedCompare(const std::string &leftSql, const std::string &rightSql,
	                           const std::string &op) const override;

private:
	DuckDbDialect native_;
};

/// The suffix sql2rdf_<suffix> / sql2rdf_untyped_<suffix> uses for a SQL
/// comparison operator (= <> < > <= >= map to eq ne lt gt le ge). Throws
/// std::invalid_argument for anything else. Shared with the function
/// registration so the two cannot drift.
const char *comparisonFunctionSuffix(const std::string &op);

} // namespace sparql2sql
