#include "r2rml/GraphMap.h"

#include <string>

#include "r2rml/MappingParser.h"

namespace r2rml {

GraphMap::~GraphMap() = default;

namespace {

/// True if `term` is an IRI equal to rr:defaultGraph, i.e. an explicit way of
/// saying "the default graph" rather than a real named graph.
bool isDefaultGraphTerm(const rdf::Term &term) {
	return term.isIri() && term.lexical() == vocab::RR_DEFAULT_GRAPH;
}

} // namespace

void forEachGraphNode(const std::vector<std::unique_ptr<GraphMap>> &subjectGraphMaps,
                      const std::vector<std::unique_ptr<GraphMap>> &pomGraphMaps, const SQLRow &row,
                      const std::function<void(const rdf::Term &)> &emit) {
	bool emittedNamed = false;
	// An explicit rr:defaultGraph is a *member* of the target graph set, not a
	// no-op: it must still produce a default-graph statement even when a sibling
	// graph map resolves to a real named graph. Tracked separately from
	// `emittedNamed` so the two can both hold for one triple.
	bool wantsDefault = false;

	// One reused term for the whole graph set: generateRDFTerm clears and
	// refills it, preserving capacity across entries.
	rdf::Term graph;
	auto tryEmit = [&](const GraphMap *gm) {
		if (!gm) {
			return;
		}
		gm->generateRDFTerm(row, graph);
		// An absent term here means "this graph map produced nothing for this
		// row" - NOT "the default graph". Returning without setting either flag
		// is what keeps those two apart.
		if (graph.isNull()) {
			return;
		}
		if (isDefaultGraphTerm(graph)) {
			wantsDefault = true;
			return;
		}
		emit(graph);
		emittedNamed = true;
	};

	for (const auto &gm : subjectGraphMaps) {
		tryEmit(gm.get());
	}
	for (const auto &gm : pomGraphMaps) {
		tryEmit(gm.get());
	}

	// No named graph at all means the default graph applies (this also covers
	// the "no graph maps declared" case, preserving triple-only output for
	// mappings that never mention rr:graph).
	if (wantsDefault || !emittedNamed) {
		emit(rdf::Term());
	}
}

} // namespace r2rml
