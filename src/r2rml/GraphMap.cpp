#include "r2rml/GraphMap.h"

#include <string>

#include "r2rml/MappingParser.h"

namespace r2rml {

GraphMap::~GraphMap() = default;

namespace {

/// True if `node` is a URI equal to rr:defaultGraph, i.e. an explicit way of
/// saying "the default graph" rather than a real named graph.
bool isDefaultGraphNode(const SerdNode &node) {
	if (node.type != SERD_URI) {
		return false;
	}
	return std::string(reinterpret_cast<const char *>(node.buf), node.n_bytes) == vocab::RR_DEFAULT_GRAPH;
}

} // namespace

void forEachGraphNode(const std::vector<std::unique_ptr<GraphMap>> &subjectGraphMaps,
                      const std::vector<std::unique_ptr<GraphMap>> &pomGraphMaps, const SQLRow &row, const SerdEnv &env,
                      const std::function<void(const SerdNode *)> &emit) {
	bool emittedNamed = false;
	// An explicit rr:defaultGraph is a *member* of the target graph set, not a
	// no-op: it must still produce a default-graph statement even when a sibling
	// graph map resolves to a real named graph. Tracked separately from
	// `emittedNamed` so the two can both hold for one triple.
	bool wantsDefault = false;

	auto tryEmit = [&](const GraphMap *gm) {
		if (!gm) {
			return;
		}
		SerdNode node = gm->generateRDFTerm(row, env);
		if (node.type == SERD_NOTHING) {
			return;
		}
		if (isDefaultGraphNode(node)) {
			wantsDefault = true;
			return;
		}
		emit(&node);
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
		emit(nullptr);
	}
}

} // namespace r2rml
