#pragma once

#include <memory>
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
    R2RMLMapping(R2RMLMapping&&) noexcept = default;
    R2RMLMapping& operator=(R2RMLMapping&&) noexcept = default;

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

    std::vector<std::unique_ptr<TriplesMap>> triplesMaps;
    SerdEnv* serdEnvironment{nullptr};
};

} // namespace r2rml
