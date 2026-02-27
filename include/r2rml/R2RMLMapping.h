#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include <serd/serd.h>

namespace r2rml {

class TriplesMap;
class SQLConnection;

/**
 * Represents a complete R2RML mapping document. Handles loading the mapping
 * and driving RDF generation over a database connection.
 */
class R2RMLMapping {
public:
    R2RMLMapping();
    ~R2RMLMapping();

    // prohibit copying since the object owns unique_ptrs which are move-only
    R2RMLMapping(const R2RMLMapping&) = delete;
    R2RMLMapping& operator=(const R2RMLMapping&) = delete;

    // allow moves so the mapping can be returned or stored in containers
    R2RMLMapping(R2RMLMapping&&) noexcept;
    R2RMLMapping& operator=(R2RMLMapping&&) noexcept;

    /**
     * Load an R2RML mapping from a Turtle (or other RDF) file.  The method
     * will parse the file, populate the internal triplesMaps vector and
     * configure the Serd environment with any declared prefixes/base URI.
     */
    void loadMapping(const std::string& mappingFilePath);

    /**
     * Process the provided database connection using the loaded mapping rules
     * and serialize generated triples via the supplied SerdWriter.
     */
    void processDatabase(SQLConnection& dbConnection, SerdWriter& rdfWriter);

    /**
     * Return true if all contained triples maps are valid.
     */
    bool isValid() const;

    /**
     * Return true if the mapping is valid for "inside-out" execution – i.e.
     * when used as an export within a SQL query where the row source is
     * provided by the surrounding SQL context rather than by the mapping.
     *
     * In this mode the following R2RML constructs are not supported and must
     * be absent:
     *   - rr:LogicalTable  (any LogicalTable, including rr:tableName tables)
     *   - rr:sqlQuery      (R2RMLView logical tables)
     *   - rr:refObjectMap  (ReferencingObjectMap)
     *   - rr:JoinCondition (implicit when refObjectMaps are absent)
     */
    bool isValidInsideOut() const;

    friend std::ostream& operator<<(std::ostream& os, const R2RMLMapping& m);

    std::vector<std::unique_ptr<TriplesMap>> triplesMaps;
    SerdEnv* serdEnvironment{nullptr};
};

} // namespace r2rml
