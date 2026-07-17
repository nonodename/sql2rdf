#pragma once

#include <memory>
#include <vector>

#include "sparql-parser/ast/Term.h"

namespace sparql {
namespace ast {

enum class PathKind {
	Predicate,
	Variable,
	Inverse,
	Sequence,
	Alternative,
	ZeroOrMore,
	OneOrMore,
	ZeroOrOne,
	NegatedPropertySet
};

/// Property path expression (§9, grammar rules 88-96), extended with a
/// `Variable` leaf for the plain-variable-as-predicate case (Verb ::=
/// VarOrIri | 'a', and VerbSimple ::= Var in the path grammar) - not part
/// of §9's path grammar itself, but every predicate position in the AST
/// is unified under this one hierarchy for simplicity: a bare `iri`/`a`
/// predicate becomes a trivial one-step PredicatePath, and a bare `?p`
/// predicate becomes a VariablePath (see Parser plan notes on this
/// deliberate AST simplification).
class PropertyPathExpr {
public:
	explicit PropertyPathExpr(PathKind kind) : kind_(kind) {}
	virtual ~PropertyPathExpr() = default;

	PathKind kind() const { return kind_; }

private:
	PathKind kind_;
};

class PredicatePath : public PropertyPathExpr {
public:
	explicit PredicatePath(std::unique_ptr<Iri> iri) : PropertyPathExpr(PathKind::Predicate), iri(std::move(iri)) {}

	std::unique_ptr<Iri> iri;
};

class VariablePath : public PropertyPathExpr {
public:
	explicit VariablePath(std::unique_ptr<Var> var) : PropertyPathExpr(PathKind::Variable), var(std::move(var)) {}

	std::unique_ptr<Var> var;
};

class InversePath : public PropertyPathExpr {
public:
	explicit InversePath(std::unique_ptr<PropertyPathExpr> child)
	    : PropertyPathExpr(PathKind::Inverse), child(std::move(child)) {}

	std::unique_ptr<PropertyPathExpr> child;
};

class SequencePath : public PropertyPathExpr {
public:
	SequencePath(std::unique_ptr<PropertyPathExpr> left, std::unique_ptr<PropertyPathExpr> right)
	    : PropertyPathExpr(PathKind::Sequence), left(std::move(left)), right(std::move(right)) {}

	std::unique_ptr<PropertyPathExpr> left;
	std::unique_ptr<PropertyPathExpr> right;
};

class AlternativePath : public PropertyPathExpr {
public:
	AlternativePath(std::unique_ptr<PropertyPathExpr> left, std::unique_ptr<PropertyPathExpr> right)
	    : PropertyPathExpr(PathKind::Alternative), left(std::move(left)), right(std::move(right)) {}

	std::unique_ptr<PropertyPathExpr> left;
	std::unique_ptr<PropertyPathExpr> right;
};

class ZeroOrMorePath : public PropertyPathExpr {
public:
	explicit ZeroOrMorePath(std::unique_ptr<PropertyPathExpr> child)
	    : PropertyPathExpr(PathKind::ZeroOrMore), child(std::move(child)) {}

	std::unique_ptr<PropertyPathExpr> child;
};

class OneOrMorePath : public PropertyPathExpr {
public:
	explicit OneOrMorePath(std::unique_ptr<PropertyPathExpr> child)
	    : PropertyPathExpr(PathKind::OneOrMore), child(std::move(child)) {}

	std::unique_ptr<PropertyPathExpr> child;
};

class ZeroOrOnePath : public PropertyPathExpr {
public:
	explicit ZeroOrOnePath(std::unique_ptr<PropertyPathExpr> child)
	    : PropertyPathExpr(PathKind::ZeroOrOne), child(std::move(child)) {}

	std::unique_ptr<PropertyPathExpr> child;
};

/// !iri, !(iri1|...|irin), with an optional inverse (^) marker per element,
/// per grammar rules 95-96.
class NegatedPropertySet : public PropertyPathExpr {
public:
	NegatedPropertySet() : PropertyPathExpr(PathKind::NegatedPropertySet) {}

	std::vector<std::unique_ptr<Iri>> forward; // iri_1 .. iri_j (not inverted)
	std::vector<std::unique_ptr<Iri>> inverse; // ^iri_j+1 .. ^iri_n
};

} // namespace ast
} // namespace sparql
