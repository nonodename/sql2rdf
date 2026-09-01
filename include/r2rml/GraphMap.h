#pragma once

#include "TermMap.h"
#include <functional>
#include <memory>
#include <vector>

namespace r2rml {

/**
 * Represents a mapping that yields the named graph IRI for a triple.  Uses
 * the same machinery as other term maps.
 */
class GraphMap : public TermMap {
public:
	GraphMap() = default;
	~GraphMap() override;

	/// The term-generation strategy (rr:template/rr:column/rr:constant) that
	/// determines this graph map's value, mirroring
	/// SubjectMap::valueTermMap(): the parser composes the value strategy by
	/// delegation rather than inheritance (see R2RMLParser.cpp's
	/// ConcreteGraphMap) so it can reuse the same term-map-building code used
	/// for predicate/object maps.
	///
	/// Needed by consumers that must inspect the *shape* of the strategy
	/// rather than just evaluate it — notably sparql2sql, whose
	/// termMapToSqlExpr/invertTermMapAgainstBoundTerm dispatch on
	/// ColumnTermMap/TemplateTermMap/ConstantTermMap and so cannot see through
	/// a GraphMap otherwise.
	///
	/// Unlike SubjectMap's, this is deliberately NOT pure: GraphMap is
	/// directly derivable (tests/test_coverage_gaps.cpp's TestGraphMap does
	/// so), and a pure virtual here would break those test doubles. Returns
	/// null only for such a double; the parser always supplies a strategy.
	virtual const TermMap *valueTermMap() const {
		return nullptr;
	}
};

/**
 * Invoke `emit` once per RDF graph a triple should be written into, given the
 * rr:graph/rr:graphMap annotations of its enclosing subject map and (if any)
 * predicate-object map. Per R2RML §12, the graphs that apply to a triple are
 * the union of both sets.
 *
 * The special rr:defaultGraph IRI denotes the default graph as a *member* of
 * that union, so an entry resolving to it contributes a default-graph
 * statement (`emit` called with an ABSENT term) alongside — not instead
 * of — any sibling entry that resolves to a real named graph.
 *
 * If the union is empty, or every entry resolves to a null term, `emit` is
 * likewise invoked once with an absent term: an all-null set is
 * indistinguishable from an empty one under R2RML's set formulation. This is
 * also what preserves quad-less output for mappings that don't use rr:graph
 * at all.
 *
 * NOTE the deliberate collapsing of two distinct conditions onto one signal.
 * "This graph map produced no term" and "this triple belongs in the default
 * graph" are different facts, but they reach `emit` the same way — as an
 * absent term — because R2RML's set formulation makes them indistinguishable
 * at the point of emission. The distinction is kept internally, in the
 * separate wantsDefault/emittedNamed flags below; conflating those two is what
 * would silently turn quads into triples or emit them twice.
 */
void forEachGraphNode(const std::vector<std::unique_ptr<GraphMap>> &subjectGraphMaps,
                      const std::vector<std::unique_ptr<GraphMap>> &pomGraphMaps, const SQLRow &row,
                      const std::function<void(const rdf::Term &)> &emit);

} // namespace r2rml
