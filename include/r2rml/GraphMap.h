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
};

/**
 * Invoke `emit` once per RDF graph a triple should be written into, given the
 * rr:graph/rr:graphMap annotations of its enclosing subject map and (if any)
 * predicate-object map. Per R2RML §12, the graphs that apply to a triple are
 * the union of both sets.
 *
 * If that union is empty, or every entry resolves to a null term or to the
 * special rr:defaultGraph IRI, `emit` is invoked exactly once with a null
 * graph pointer, i.e. the default graph (this also preserves prior
 * quad-less-output behaviour for mappings that don't use rr:graph at all).
 */
void forEachGraphNode(const std::vector<std::unique_ptr<GraphMap>> &subjectGraphMaps,
                      const std::vector<std::unique_ptr<GraphMap>> &pomGraphMaps, const SQLRow &row, const SerdEnv &env,
                      const std::function<void(const SerdNode *)> &emit);

} // namespace r2rml
