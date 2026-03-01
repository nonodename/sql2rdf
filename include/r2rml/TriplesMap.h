#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include <serd/serd.h>

namespace r2rml {

class LogicalTable;
class SubjectMap;
class PredicateObjectMap;
class SQLRow;
class SQLConnection;
class R2RMLMapping;

/**
 * A TriplesMap describes how each row of a logical table is converted into a
 * set of RDF triples sharing a common subject.
 */
class TriplesMap {
public:
	TriplesMap();
	~TriplesMap(); // NOLINT(performance-trivially-destructible)

	/**
	 * Process the supplied row, emitting zero or more triples via the
	 * SerdWriter object.  The mapping context may be needed for referencing
	 * other maps (e.g. for referencing object maps).
	 */
	void generateTriples(const SQLRow &row, SerdWriter &rdfWriter, const R2RMLMapping &mapping,
	                     SQLConnection &dbConnection) const;

	bool isValid() const;

	/**
	 * Return true if this TriplesMap is valid for inside-out (SQL-export)
	 * execution.  Requires no logicalTable, a valid subjectMap, and all
	 * predicate-object maps to also be valid inside-out.
	 */
	bool isValidInsideOut() const;

	friend std::ostream &operator<<(std::ostream &os, const TriplesMap &tm);

	std::string id;
	std::unique_ptr<LogicalTable> logicalTable;
	std::unique_ptr<SubjectMap> subjectMap;
	std::vector<std::unique_ptr<PredicateObjectMap>> predicateObjectMaps;
};

} // namespace r2rml
