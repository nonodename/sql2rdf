#include "r2rml/R2RMLMapping.h"
#include "r2rml/TriplesMap.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"

#include <algorithm>
#include <ostream>
#include <utility>

namespace r2rml {

R2RMLMapping::R2RMLMapping() = default;

R2RMLMapping::R2RMLMapping(R2RMLMapping&& other) noexcept
    : triplesMaps(std::move(other.triplesMaps))
    , serdEnvironment(std::exchange(other.serdEnvironment, nullptr))
{}

R2RMLMapping& R2RMLMapping::operator=(R2RMLMapping&& other) noexcept {
    if (this != &other) {
        if (serdEnvironment) serd_env_free(serdEnvironment);
        triplesMaps     = std::move(other.triplesMaps);
        serdEnvironment = std::exchange(other.serdEnvironment, nullptr);
    }
    return *this;
}

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

std::ostream& operator<<(std::ostream& os, const R2RMLMapping& m) {
    os << "R2RMLMapping (" << m.triplesMaps.size() << " TriplesMap(s)):\n";
    for (std::size_t i = 0; i < m.triplesMaps.size(); ++i) {
        os << "\nTriplesMap[" << i << "]: ";
        if (m.triplesMaps[i]) os << *m.triplesMaps[i];
        else                  os << "(none)";
        os << "\n";
    }
    return os;
}

} // namespace r2rml
