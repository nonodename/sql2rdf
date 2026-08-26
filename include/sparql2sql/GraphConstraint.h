#pragma once

#include <algorithm>
#include <string>
#include <vector>

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

/// The RDF dataset a query is evaluated against, as given by its FROM /
/// FROM NAMED clauses (SPARQL 1.1 Section 13.2).
///
/// `restricted` is false when the query has no dataset clauses at all, which is
/// the "whole mapping" default and the only shape that existed before dataset
/// support. When it is true, two rules routinely surprise people:
///
///   - FROM *replaces* the default graph rather than adding to it, so with
///     `FROM <g>` a triple that has no graph map at all becomes invisible.
///   - Naming only FROM NAMED graphs leaves the default graph EMPTY, so a
///     pattern outside any GRAPH block matches nothing.
///
/// Both are spelled out because each makes a query legitimately return zero
/// rows, which reads like a bug.
struct ActiveDataset {
	bool restricted = false;
	std::vector<std::string> defaultGraphIris; ///< FROM <g>, deduplicated
	std::vector<std::string> namedGraphIris;   ///< FROM NAMED <g>, deduplicated

	bool namesDefaultGraph(const std::string &iri) const {
		return std::find(defaultGraphIris.begin(), defaultGraphIris.end(), iri) != defaultGraphIris.end();
	}

	bool namesNamedGraph(const std::string &iri) const {
		return std::find(namedGraphIris.begin(), namedGraphIris.end(), iri) != namedGraphIris.end();
	}
};

} // namespace sparql2sql
