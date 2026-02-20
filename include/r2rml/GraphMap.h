#pragma once

#include "TermMap.h"
#include <memory>

namespace r2rml {

/**
 * Represents a mapping that yields the named graph IRI for a triple.  Uses
 * the same machinery as other term maps.
 */
class GraphMap : public TermMap {
public:
    GraphMap() = default;
    ~GraphMap() override;
};

} // namespace r2rml
