#pragma once

#include <memory>
#include <string>
#include <vector>
#include <memory>

#include <serd/serd.h>

namespace r2rml {

class LogicalTable;
class SubjectMap;
class PredicateObjectMap;
class SQLRow;
class R2RMLMapping;

/**
 * A TriplesMap describes how each row of a logical table is converted into a
 * set of RDF triples sharing a common subject.
 */
class TriplesMap {
public:
    TriplesMap();
    ~TriplesMap();

    /**
     * Process the supplied row, emitting zero or more triples via the
     * SerdWriter object.  The mapping context may be needed for referencing
     * other maps (e.g. for referencing object maps).
     */
    void generateTriples(const SQLRow& row,
                         SerdWriter& rdfWriter,
                         const R2RMLMapping& mapping) const;

    bool isValid() const;

    std::string id;
    std::unique_ptr<LogicalTable> logicalTable;
    std::unique_ptr<SubjectMap> subjectMap;
    std::vector<std::unique_ptr<PredicateObjectMap>> predicateObjectMaps;
};

} // namespace r2rml
