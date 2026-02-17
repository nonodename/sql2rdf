#pragma once

#include "TermMap.h"

namespace r2rml {

/**
 * A simple alias for a term map used for predicates.  Present for clarity of
 * the object model; behaviour is inherited from TermMap.
 */
class PredicateMap : public TermMap {
public:
    PredicateMap() = default;
    ~PredicateMap() override;
};

} // namespace r2rml
