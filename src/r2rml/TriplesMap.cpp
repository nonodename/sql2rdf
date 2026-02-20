#include "r2rml/TriplesMap.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/PredicateObjectMap.h"

#include <algorithm>

namespace r2rml {

TriplesMap::TriplesMap() = default;
TriplesMap::~TriplesMap() = default;
void TriplesMap::generateTriples(const SQLRow& row,
                                 SerdWriter& rdfWriter,
                                 const R2RMLMapping& mapping) const {
    // stub
}
bool TriplesMap::isValid() const {
    if (!logicalTable || !logicalTable->isValid()) return false;
    if (!subjectMap   || !subjectMap->isValid())   return false;
    return std::all_of(predicateObjectMaps.begin(), predicateObjectMaps.end(),
                       [](const std::unique_ptr<PredicateObjectMap>& pom) {
                           return pom && pom->isValid();
                       });
}

} // namespace r2rml
