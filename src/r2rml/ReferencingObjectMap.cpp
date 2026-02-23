#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/TriplesMap.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include <algorithm>
#include <ostream>
#include <vector>

namespace r2rml {

// ---------------------------------------------------------------------------
// In-memory result set used to return filtered join rows.
// ---------------------------------------------------------------------------
namespace {
class VectorResultSet : public SQLResultSet {
public:
    explicit VectorResultSet(std::vector<SQLRow> rows)
        : rows_(std::move(rows)) {}

    bool next() override {
        ++cursor_;
        return cursor_ < static_cast<int>(rows_.size());
    }

    SQLRow getCurrentRow() const override {
        return rows_[static_cast<std::size_t>(cursor_)];
    }

private:
    std::vector<SQLRow> rows_;
    int cursor_{-1};
};
} // anonymous namespace

ReferencingObjectMap::ReferencingObjectMap() = default;
ReferencingObjectMap::~ReferencingObjectMap() = default;

bool ReferencingObjectMap::isValid() const {
    if (!parentTriplesMap) return false;
    return std::all_of(joinConditions.begin(), joinConditions.end(),
                       [](const JoinCondition& jc) { return jc.isValid(); });
}

std::unique_ptr<SQLResultSet>
ReferencingObjectMap::getJoinedRows(SQLConnection& dbConnection,
                                    const SQLRow& childRow) const
{
    if (!parentTriplesMap || !parentTriplesMap->logicalTable)
        return nullptr;

    // Execute the parent's logical table query.
    auto parentResult = parentTriplesMap->logicalTable->getRows(dbConnection);
    if (!parentResult) return nullptr;

    // Collect parent rows that satisfy all join conditions.
    std::vector<SQLRow> matched;
    while (parentResult->next()) {
        SQLRow parentRow = parentResult->getCurrentRow();
        bool ok = true;
        for (const JoinCondition& jc : joinConditions) {
            SQLValue childVal  = childRow.getValue(jc.childColumn);
            SQLValue parentVal = parentRow.getValue(jc.parentColumn);
            if (childVal.isNull() || parentVal.isNull() ||
                childVal.asString() != parentVal.asString()) {
                ok = false;
                break;
            }
        }
        if (ok) matched.push_back(std::move(parentRow));
    }

    return std::unique_ptr<SQLResultSet>(new VectorResultSet(std::move(matched)));
}

SerdNode ReferencingObjectMap::generateRDFTerm(const SQLRow& /*childRow*/,
                                               const SQLRow& parentRow,
                                               const SerdEnv& env) const
{
    if (!parentTriplesMap || !parentTriplesMap->subjectMap)
        return SERD_NODE_NULL;

    return parentTriplesMap->subjectMap->generateRDFTerm(parentRow, env);
}

std::ostream& ReferencingObjectMap::print(std::ostream& os) const {
    os << "ReferencingObjectMap { parent=";
    if (parentTriplesMap) os << "<" << parentTriplesMap->id << ">";
    else                  os << "(unresolved)";
    if (!joinConditions.empty()) {
        os << " joins=[";
        for (std::size_t i = 0; i < joinConditions.size(); ++i) {
            if (i) os << ", ";
            os << joinConditions[i];
        }
        os << "]";
    }
    os << " }";
    return os;
}

} // namespace r2rml
