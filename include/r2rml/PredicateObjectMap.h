#pragma once

#include <memory>
#include <vector>
#include <memory>

#include <serd/serd.h>

namespace r2rml {

class TermMap;
class GraphMap;
class SQLRow;
class SQLConnection;
class R2RMLMapping;

/**
 * Encapsulates mapping rules that generate predicate-object pairs (and
 * optionally graph names) for each input row.
 */
class PredicateObjectMap {
public:
    PredicateObjectMap();
    ~PredicateObjectMap();

    /**
     * Process a single row given a subject node and emit one or more triples
     * by invoking the provided SerdWriter.
     */
    void processRow(const SQLRow& row,
                    const SerdNode& subject,
                    SerdWriter& rdfWriter,
                    const R2RMLMapping& mapping,
                    SQLConnection& dbConnection) const;

    bool isValid() const;

    std::vector<std::unique_ptr<TermMap>> predicateMaps;
    std::vector<std::unique_ptr<TermMap>> objectMaps;
    std::vector<std::unique_ptr<GraphMap>> graphMaps;
};

} // namespace r2rml
