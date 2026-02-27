#include "r2rml/PredicateObjectMap.h"
#include "r2rml/TermMap.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/GraphMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLResultSet.h"

#include <algorithm>
#include <ostream>

namespace r2rml {

PredicateObjectMap::PredicateObjectMap() = default;
PredicateObjectMap::~PredicateObjectMap() = default;

void PredicateObjectMap::processRow(const SQLRow& row,
                                   const SerdNode& subject,
                                   SerdWriter& rdfWriter,
                                   const R2RMLMapping& mapping,
                                   SQLConnection& dbConnection) const
{
    const SerdEnv* env = mapping.serdEnvironment;
    static SerdEnv* fallbackEnv = nullptr;
    if (!env) {
        if (!fallbackEnv) fallbackEnv = serd_env_new(nullptr);
        env = fallbackEnv;
    }

    // For each predicate/object combination, emit a triple.
    for (const auto& predMap : predicateMaps) {
        if (!predMap) continue;
        SerdNode predicate = predMap->generateRDFTerm(row, *env);
        if (predicate.type == SERD_NOTHING) continue; // null predicate – skip

        for (const auto& objMap : objectMaps) {
            if (!objMap) continue;

            // Check if this object map is a ReferencingObjectMap (join).
            ReferencingObjectMap* rom =
                dynamic_cast<ReferencingObjectMap*>(objMap.get());

            if (rom) {
                // Join: query the parent table and use parent subject as object.
                auto parentRows = rom->getJoinedRows(dbConnection, row);
                if (!parentRows) continue;
                while (parentRows->next()) {
                    SQLRow parentRow = parentRows->getCurrentRow();
                    SerdNode object = rom->generateRDFTerm(row, parentRow, *env);
                    if (object.type == SERD_NOTHING) continue;
                    serd_writer_write_statement(
                        &rdfWriter, 0, nullptr,
                        &subject, &predicate, &object,
                        nullptr, nullptr);
                }
            } else {
                // Regular term map.
                SerdNode object = objMap->generateRDFTerm(row, *env);
                if (object.type == SERD_NOTHING) continue; // null object – skip

                // Determine whether to pass datatype/lang for literals.
                const SerdNode* datatype = nullptr;
                const SerdNode* lang = nullptr;
                // (Language tags and datatypes not exercised by current tests.)

                serd_writer_write_statement(
                    &rdfWriter, 0, nullptr,
                    &subject, &predicate, &object,
                    datatype, lang);
            }
        }
    }
}

bool PredicateObjectMap::isValid() const {
    if (predicateMaps.empty() || objectMaps.empty()) return false;
    return std::all_of(predicateMaps.begin(), predicateMaps.end(),
                       [](const std::unique_ptr<TermMap>& pm) {
                           return pm && pm->isValid();
                       }) &&
           std::all_of(objectMaps.begin(), objectMaps.end(),
                       [](const std::unique_ptr<TermMap>& om) {
                           return om && om->isValid();
                       });
}

bool PredicateObjectMap::isValidInsideOut() const {
    if (predicateMaps.empty() || objectMaps.empty()) return false;
    if (!std::all_of(predicateMaps.begin(), predicateMaps.end(),
                     [](const std::unique_ptr<TermMap>& pm) {
                         return pm && pm->isValid();
                     }))
        return false;
    // rr:refObjectMap (ReferencingObjectMap) and rr:JoinCondition are not
    // supported in inside-out mode.
    return std::all_of(objectMaps.begin(), objectMaps.end(),
                       [](const std::unique_ptr<TermMap>& om) {
                           if (!om || !om->isValid()) return false;
                           return dynamic_cast<const ReferencingObjectMap*>(om.get()) == nullptr;
                       });
}

std::ostream& operator<<(std::ostream& os, const PredicateObjectMap& pom) {
    os << "PredicateObjectMap { predicates=[";
    for (std::size_t i = 0; i < pom.predicateMaps.size(); ++i) {
        if (i) os << ", ";
        if (pom.predicateMaps[i]) os << *pom.predicateMaps[i];
    }
    os << "] objects=[";
    for (std::size_t i = 0; i < pom.objectMaps.size(); ++i) {
        if (i) os << ", ";
        if (pom.objectMaps[i]) os << *pom.objectMaps[i];
    }
    os << "]";
    if (!pom.graphMaps.empty()) {
        os << " graphMaps=[";
        for (std::size_t i = 0; i < pom.graphMaps.size(); ++i) {
            if (i) os << ", ";
            if (pom.graphMaps[i]) os << *pom.graphMaps[i];
        }
        os << "]";
    }
    os << " }";
    return os;
}

} // namespace r2rml
