#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sparql-parser/ast/Expression.h"
#include "sparql-parser/ast/GraphPattern.h"
#include "sparql-parser/ast/Term.h"

namespace sparql {
namespace ast {

enum class QueryForm { Select, Construct, Describe, Ask };
enum class OrderDirection { Asc, Desc };
enum class DatasetClauseKind { Default, Named };

struct PrefixDecl {
	std::string prefix; // without the trailing ':'
	std::string iri;
};

struct Prologue {
	std::string baseIri;
	std::vector<PrefixDecl> prefixes;
};

struct DatasetClause {
	DatasetClauseKind kind = DatasetClauseKind::Default;
	std::unique_ptr<Iri> iri;
};

/// A projected SELECT variable: either a bare `?var`, or `(expr AS ?var)`
/// (expr is null for the bare-variable case).
struct SelectItem {
	std::unique_ptr<Var> var;
	std::unique_ptr<Expression> expr;
};

struct OrderCondition {
	OrderDirection direction = OrderDirection::Asc;
	std::unique_ptr<Expression> expr;
};

/// One GROUP BY term: a bare variable, or `(expr AS ?var)` (asVar null for
/// a bare expression with no alias).
struct GroupCondition {
	std::unique_ptr<Expression> expr;
	std::unique_ptr<Var> asVar;
};

struct SolutionModifier {
	std::vector<GroupCondition> groupBy;
	std::vector<std::unique_ptr<Expression>> having;
	std::vector<OrderCondition> orderBy;
	bool hasLimit = false;
	int64_t limit = 0;
	bool hasOffset = false;
	int64_t offset = 0;
};

/// Top-level parse result for a whole SPARQL query, and also reused
/// (Select form, empty Prologue/DatasetClauses) as the shape of a `{
/// SELECT ... }` subquery embedded in a GroupGraphPattern - see
/// SubSelectElement in GraphPattern.h.
class Query {
public:
	QueryForm form = QueryForm::Select;
	Prologue prologue;
	std::vector<DatasetClause> datasetClauses;

	// SELECT
	bool distinct = false;
	bool reduced = false;
	bool selectStar = false;
	std::vector<SelectItem> selectItems;

	// CONSTRUCT
	std::vector<TriplePattern> constructTemplate;
	bool hasConstructTemplate = false;

	// DESCRIBE
	bool describeStar = false;
	std::vector<std::unique_ptr<Term>> describeTargets; // Iri or Var

	// Shared by all forms
	std::unique_ptr<GroupGraphPattern> where; // may be null (DESCRIBE without WHERE)
	SolutionModifier solutionModifier;
	std::unique_ptr<InlineData> valuesClause; // trailing VALUES; may be null
};

} // namespace ast
} // namespace sparql
