#pragma once

#include "TermMap.h"

#include <string>
#include <vector>
#include <memory>

namespace r2rml {

class GraphMap;

/**
 * A specialization of TermMap used for subjects.  In addition to the base
 * term generation behaviour it can carry rdf:class assertions and graph maps.
 */
class SubjectMap : public TermMap {
public:
    SubjectMap() = default;
    ~SubjectMap() override;

    bool isValid() const override;

    std::vector<std::string> classIRIs;
    std::vector<std::unique_ptr<GraphMap>> graphMaps;
};

} // namespace r2rml
