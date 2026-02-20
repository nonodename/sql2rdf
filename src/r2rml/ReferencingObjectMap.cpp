#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/SQLResultSet.h"

#include <algorithm>

namespace r2rml {

ReferencingObjectMap::ReferencingObjectMap() = default;
ReferencingObjectMap::~ReferencingObjectMap() = default;
bool ReferencingObjectMap::isValid() const {
    if (!parentTriplesMap) return false;
    return std::all_of(joinConditions.begin(), joinConditions.end(),
                       [](const JoinCondition& jc) { return jc.isValid(); });
}
std::unique_ptr<SQLResultSet>
ReferencingObjectMap::getJoinedRows(SQLConnection& dbConnection,
                                    const SQLRow& childRow) const {
    return nullptr;
}
SerdNode ReferencingObjectMap::generateRDFTerm(const SQLRow&,
                                               const SQLRow&,
                                               const SerdEnv&) const {
    return SerdNode{0};
}

} // namespace r2rml
