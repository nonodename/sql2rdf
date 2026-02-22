#include "r2rml/TriplesMap.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLRow.h"

#include <algorithm>

namespace r2rml {

static const uint8_t RDF_TYPE_URI[] =
    "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";

TriplesMap::TriplesMap() = default;
TriplesMap::~TriplesMap() = default;

void TriplesMap::generateTriples(const SQLRow& row,
                                 SerdWriter& rdfWriter,
                                 const R2RMLMapping& mapping,
                                 SQLConnection& dbConnection) const
{
    if (!subjectMap) return;

    // Determine the Serd environment to use for term generation.
    const SerdEnv* env = mapping.serdEnvironment;
    // Fall back to a null-like check; all our nodes are absolute IRIs so env
    // content rarely matters here, but we need a valid reference.
    static SerdEnv* fallbackEnv = nullptr;
    if (!env) {
        if (!fallbackEnv) fallbackEnv = serd_env_new(nullptr);
        env = fallbackEnv;
    }

    // Generate the subject node for this row.
    SerdNode subject = subjectMap->generateRDFTerm(row, *env);
    if (subject.type == SERD_NOTHING) return; // null subject – skip row

    // Emit rdf:type triples for each rr:class.
    if (!subjectMap->classIRIs.empty()) {
        SerdNode rdfType = serd_node_from_string(SERD_URI, RDF_TYPE_URI);
        for (const std::string& classIRI : subjectMap->classIRIs) {
            SerdNode classNode = serd_node_from_string(
                SERD_URI,
                reinterpret_cast<const uint8_t*>(classIRI.c_str()));
            serd_writer_write_statement(
                &rdfWriter, 0, nullptr,
                &subject, &rdfType, &classNode,
                nullptr, nullptr);
        }
    }

    // Process each predicate-object map.
    for (const auto& pom : predicateObjectMaps) {
        if (pom)
            pom->processRow(row, subject, rdfWriter, mapping, dbConnection);
    }
}

bool TriplesMap::isValid() const {
    if (!logicalTable || !logicalTable->isValid()) return false;
    if (!subjectMap   || !subjectMap->isValid())   return false;
    return std::all_of(predicateObjectMaps.begin(), predicateObjectMaps.end(),
                       [](const std::unique_ptr<PredicateObjectMap>& pom) {
                           return pom && pom->isValid();
                       });
}

} // namespace r2rml
