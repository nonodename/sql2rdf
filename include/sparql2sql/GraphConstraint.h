#pragma once

#include <string>

namespace sparql2sql {

/// Which RDF graph a triple pattern is being matched against - SPARQL's
/// "active graph" (SPARQL 1.1 Section 13.3), as determined by the enclosing
/// GRAPH block (if any).
///
/// A value-initialised instance is `Default`, i.e. "no GRAPH block", which is
/// what every construction site outside a GRAPH block gets for free. That is
/// deliberate: it is what lets named-graph support be added without changing
/// the SQL generated for any query that does not mention GRAPH.
struct GraphConstraint {
	enum class Kind {
		Default,  ///< No enclosing GRAPH block: match the dataset's default graph.
		BoundIri, ///< `GRAPH <iri> { ... }`: match only that named graph.
		Variable  ///< `GRAPH ?g { ... }`: match any named graph, binding ?g to it.
	};

	Kind kind = Kind::Default;
	std::string iri;     ///< valid iff kind == BoundIri
	std::string varName; ///< valid iff kind == Variable

	bool isDefault() const {
		return kind == Kind::Default;
	}
};

inline GraphConstraint boundGraph(const std::string &iri) {
	GraphConstraint g;
	g.kind = GraphConstraint::Kind::BoundIri;
	g.iri = iri;
	return g;
}

inline GraphConstraint variableGraph(const std::string &varName) {
	GraphConstraint g;
	g.kind = GraphConstraint::Kind::Variable;
	g.varName = varName;
	return g;
}

} // namespace sparql2sql
