#pragma once

#include "TermMap.h"
#include "JoinCondition.h"

#include <vector>

namespace r2rml {

class TriplesMap;
class SQLConnection;
class SQLRow;
class SQLResultSet;

/**
 * A special object map that uses the subject produced by another triples map
 * (the parent) as the object value.  Supports join conditions.
 */
class ReferencingObjectMap : public TermMap {
public:
    ReferencingObjectMap();
    ~ReferencingObjectMap() override;

    std::unique_ptr<SQLResultSet> getJoinedRows(SQLConnection& dbConnection,
                                                const SQLRow& childRow) const;

    SerdNode generateRDFTerm(const SQLRow& childRow,
                             const SQLRow& parentRow,
                             const SerdEnv& env) const;

    TriplesMap* parentTriplesMap{nullptr};
    std::vector<JoinCondition> joinConditions;
};

} // namespace r2rml
