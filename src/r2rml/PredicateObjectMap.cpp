#include "r2rml/PredicateObjectMap.h"
#include "r2rml/TermMap.h"
#include "r2rml/GraphMap.h"

#include <algorithm>

namespace r2rml {

PredicateObjectMap::PredicateObjectMap() = default;
PredicateObjectMap::~PredicateObjectMap() = default;
void PredicateObjectMap::processRow(const SQLRow& row,
                                   const SerdNode& subject,
                                   SerdWriter& rdfWriter,
                                   const R2RMLMapping& mapping) const {
    // stub
}
bool PredicateObjectMap::isValid() const {
    if (predicateMaps.empty() || objectMaps.empty()) return false;
    return std::all_of(predicateMaps.begin(), predicateMaps.end(),
                       [](const std::unique_ptr<TermMap>& pm) {
                           return pm && pm->isValid();
                       }) &&
           std::all_of(objectMaps.begin(), objectMaps.end(),
                       [](const std::unique_ptr<TermMap>& om) {
                           return om && om->isValid();
                       });
}

} // namespace r2rml
