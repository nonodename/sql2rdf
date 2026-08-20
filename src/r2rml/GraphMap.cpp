#include "r2rml/GraphMap.h"

#include <string>

namespace r2rml {

GraphMap::~GraphMap() = default;

namespace {

/// True if `node` is a URI equal to rr:defaultGraph, i.e. an explicit way of
/// saying "the default graph" rather than a real named graph.
bool isDefaultGraphNode(const SerdNode &node) {
	if (node.type != SERD_URI) {
		return false;
	}
	static const char *const kDefaultGraphIri = "http://www.w3.org/ns/r2rml#defaultGraph";
	return std::string(reinterpret_cast<const char *>(node.buf), node.n_bytes) == kDefaultGraphIri;
}

} // namespace

void forEachGraphNode(const std::vector<std::unique_ptr<GraphMap>> &subjectGraphMaps,
                      const std::vector<std::unique_ptr<GraphMap>> &pomGraphMaps, const SQLRow &row, const SerdEnv &env,
                      const std::function<void(const SerdNode *)> &emit) {
	bool emitted = false;

	auto tryEmit = [&](const GraphMap *gm) {
		if (!gm) {
			return;
		}
		SerdNode node = gm->generateRDFTerm(row, env);
		if (node.type == SERD_NOTHING || isDefaultGraphNode(node)) {
			return;
		}
		emit(&node);
		emitted = true;
	};

	for (const auto &gm : subjectGraphMaps) {
		tryEmit(gm.get());
	}
	for (const auto &gm : pomGraphMaps) {
		tryEmit(gm.get());
	}

	if (!emitted) {
		emit(nullptr);
	}
}

} // namespace r2rml
