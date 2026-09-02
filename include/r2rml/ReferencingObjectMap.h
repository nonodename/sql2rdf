#pragma once

#include "TermMap.h"
#include "JoinCondition.h"

#include <vector>
#include <memory>

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

	bool isValid() const override;

	std::unique_ptr<SQLResultSet> getJoinedRows(SQLConnection &dbConnection, const SQLRow &childRow) const;

	/// The two-row variant: the object term is the PARENT triples map's subject
	/// for the joined parent row. Leaves `out` absent if the parent map or its
	/// subject map is missing.
	void generateRDFTerm(const SQLRow &childRow, const SQLRow &parentRow, rdf::Term &out) const;

	std::ostream &print(std::ostream &os) const override;

	TriplesMap *parentTriplesMap {nullptr};
	std::vector<JoinCondition> joinConditions;
};

} // namespace r2rml
