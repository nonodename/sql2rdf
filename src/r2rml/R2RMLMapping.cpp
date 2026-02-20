#include "r2rml/R2RMLMapping.h"
#include "r2rml/TriplesMap.h"

#include <algorithm>

namespace r2rml {

R2RMLMapping::R2RMLMapping() = default;
R2RMLMapping::~R2RMLMapping() {
    if (serdEnvironment) {
        serd_env_free(serdEnvironment);
    }
}
void R2RMLMapping::loadMapping(const std::string& mappingFilePath) {
    // stub
}
void R2RMLMapping::processDatabase(SQLConnection& dbConnection,
                                   SerdWriter& rdfWriter) {
    // stub
}
bool R2RMLMapping::isValid() const {
    return std::all_of(triplesMaps.begin(), triplesMaps.end(),
                       [](const std::unique_ptr<TriplesMap>& tm) {
                           return tm && tm->isValid();
                       });
}

} // namespace r2rml
