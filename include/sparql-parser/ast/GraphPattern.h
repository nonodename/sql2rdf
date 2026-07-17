#pragma once

#include <memory>
#include <vector>

#include "sparql-parser/ast/Path.h"
#include "sparql-parser/ast/Term.h"

namespace sparql {
namespace ast {

// Forward declarations to break the GraphPattern <-> Expression <-> Query
// include cycle (ExistsExpr needs GroupGraphPattern; Filter/Bind need
// Expression; SubSelectElement needs Query). Classes below that hold these
// as members declare (rather than default) their destructors, defined
// out-of-line in Ast.cpp once the pointee types are complete.
class Expression;
class Query;

class TriplePattern {
public:
	std::unique_ptr<Term> subject;
	std::unique_ptr<PropertyPathExpr> predicate;
	std::unique_ptr<Term> object;
};

enum class ElementKind {
	BasicGraphPattern,
	GroupGraphPattern,
	UnionGraphPattern,
	OptionalGraphPattern,
	MinusGraphPattern,
	GraphGraphPattern,
	ServiceGraphPattern,
	Filter,
	Bind,
	InlineData,
	SubSelect
};

/// One element of a GroupGraphPatternSub's ordered element list (grammar
/// rule 54): a basic graph pattern, a nested combinator, or FILTER/BIND/
/// VALUES/a subquery. Kept as one ordered list (rather than separate
/// buckets) to preserve source order, which matters for FILTER/BIND scope.
class GroupElement {
public:
	explicit GroupElement(ElementKind kind) : kind_(kind) {}
	virtual ~GroupElement() = default;

	ElementKind kind() const { return kind_; }

private:
	ElementKind kind_;
};

class BasicGraphPattern : public GroupElement {
public:
	BasicGraphPattern() : GroupElement(ElementKind::BasicGraphPattern) {}

	std::vector<TriplePattern> triples;
};

/// The outermost graph pattern in a query, and any `{ ... }` nested inside
/// one (grammar rule 53: GroupGraphPattern ::= '{' (SubSelect |
/// GroupGraphPatternSub) '}').
class GroupGraphPattern : public GroupElement {
public:
	GroupGraphPattern() : GroupElement(ElementKind::GroupGraphPattern) {}

	std::vector<std::unique_ptr<GroupElement>> elements;
};

/// GroupOrUnionGraphPattern (rule 67) with >1 branch; a single branch
/// collapses directly to a GroupGraphPattern per the spec's own
/// simplification step (§18.2.2.8), so this node only appears for an
/// actual UNION.
class UnionGraphPattern : public GroupElement {
public:
	UnionGraphPattern() : GroupElement(ElementKind::UnionGraphPattern) {}

	std::vector<std::unique_ptr<GroupGraphPattern>> branches;
};

class OptionalGraphPattern : public GroupElement {
public:
	OptionalGraphPattern() : GroupElement(ElementKind::OptionalGraphPattern) {}

	std::unique_ptr<GroupGraphPattern> pattern;
};

class MinusGraphPattern : public GroupElement {
public:
	MinusGraphPattern() : GroupElement(ElementKind::MinusGraphPattern) {}

	std::unique_ptr<GroupGraphPattern> pattern;
};

/// GRAPH (iri|var) { ... }. `graphNameOrVar` is an Iri or a Var.
class GraphGraphPattern : public GroupElement {
public:
	GraphGraphPattern() : GroupElement(ElementKind::GraphGraphPattern) {}

	std::unique_ptr<Term> graphNameOrVar;
	std::unique_ptr<GroupGraphPattern> pattern;
};

/// SERVICE [SILENT] (iri|var) { ... } - parsed structurally only; no
/// federated-query execution semantics are implemented.
class ServiceGraphPattern : public GroupElement {
public:
	ServiceGraphPattern() : GroupElement(ElementKind::ServiceGraphPattern) {}

	bool silent = false;
	std::unique_ptr<Term> endpoint;
	std::unique_ptr<GroupGraphPattern> pattern;
};

class Filter : public GroupElement {
public:
	Filter() : GroupElement(ElementKind::Filter) {}
	~Filter() override; // out-of-line: Expression is only forward-declared here

	std::unique_ptr<Expression> constraint;
};

class Bind : public GroupElement {
public:
	Bind() : GroupElement(ElementKind::Bind) {}
	~Bind() override; // out-of-line: Expression is only forward-declared here

	std::unique_ptr<Var> var;
	std::unique_ptr<Expression> expr;
};

/// VALUES block, either as a GroupGraphPatternSub element (rule 61) or as
/// a query's trailing ValuesClause (rule 28) - same shape either way.
/// A null entry in a row represents UNDEF.
class InlineData : public GroupElement {
public:
	InlineData() : GroupElement(ElementKind::InlineData) {}

	std::vector<std::unique_ptr<Var>> vars;
	std::vector<std::vector<std::unique_ptr<Term>>> rows;
};

/// A `{ SELECT ... }` subquery used as a GroupGraphPatternSub element
/// (rule 8's SubSelect, reused via ast::Query tagged as a Select form with
/// an empty Prologue/no DatasetClauses - see Query.h).
class SubSelectElement : public GroupElement {
public:
	explicit SubSelectElement(std::unique_ptr<Query> query)
	    : GroupElement(ElementKind::SubSelect), query(std::move(query)) {}
	~SubSelectElement() override; // out-of-line: Query is only forward-declared here

	std::unique_ptr<Query> query;
};

} // namespace ast
} // namespace sparql
