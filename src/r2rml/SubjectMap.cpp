#include "r2rml/SubjectMap.h"
#include "r2rml/GraphMap.h"

#include <algorithm>
#include <ostream>

namespace r2rml {

SubjectMap::~SubjectMap() = default;
bool SubjectMap::isValid() const {
	return std::all_of(graphMaps.begin(), graphMaps.end(),
	                   [](const std::unique_ptr<GraphMap> &gm) { return gm && gm->isValid(); });
}

std::ostream &SubjectMap::print(std::ostream &os) const {
	os << "SubjectMap {";
	if (!classIRIs.empty()) {
		os << " classes=[";
		for (std::size_t i = 0; i < classIRIs.size(); ++i) {
			if (i)
				os << ", ";
			os << classIRIs[i];
		}
		os << "]";
	}
	if (!graphMaps.empty()) {
		os << " graphMaps=[";
		for (std::size_t i = 0; i < graphMaps.size(); ++i) {
			if (i)
				os << ", ";
			if (graphMaps[i])
				os << *graphMaps[i];
		}
		os << "]";
	}
	os << " }";
	return os;
}

} // namespace r2rml
