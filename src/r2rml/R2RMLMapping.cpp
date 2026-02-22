#include "r2rml/R2RMLMapping.h"
#include "r2rml/TriplesMap.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"

#include <algorithm>

namespace r2rml {

R2RMLMapping::R2RMLMapping() = default;
R2RMLMapping::~R2RMLMapping() {
    if (serdEnvironment) {
        serd_env_free(serdEnvironment);
    }
}

void R2RMLMapping::loadMapping(const std::string& /*mappingFilePath*/) {
    // stub – callers should use R2RMLParser::parse() instead.
}

void R2RMLMapping::processDatabase(SQLConnection& dbConnection,
                                   SerdWriter& rdfWriter)
{
    for (const auto& tm : triplesMaps) {
        if (!tm || !tm->isValid()) continue;

        auto rows = tm->logicalTable->getRows(dbConnection);
        if (!rows) continue;

        while (rows->next()) {
            SQLRow row = rows->getCurrentRow();
            tm->generateTriples(row, rdfWriter, *this, dbConnection);
        }
    }
}

bool R2RMLMapping::isValid() const {
    return std::all_of(triplesMaps.begin(), triplesMaps.end(),
                       [](const std::unique_ptr<TriplesMap>& tm) {
                           return tm && tm->isValid();
                       });
}

} // namespace r2rml
