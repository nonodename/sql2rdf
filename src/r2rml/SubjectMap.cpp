#include "r2rml/SubjectMap.h"
#include "r2rml/GraphMap.h"

#include <algorithm>

namespace r2rml {

SubjectMap::~SubjectMap() = default;
bool SubjectMap::isValid() const {
    return std::all_of(graphMaps.begin(), graphMaps.end(),
                       [](const std::unique_ptr<GraphMap>& gm) {
                           return gm && gm->isValid();
                       });
}

} // namespace r2rml
