#pragma once

#include "TermMap.h"
#include <memory>

namespace r2rml {

/**
 * A specialization of TermMap used for objects.  No additional members are
 * required beyond the base class at the moment.
 */
class ObjectMap : public TermMap {
public:
    ObjectMap() = default;
    ~ObjectMap() override;
};

} // namespace r2rml
