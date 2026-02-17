#pragma once

#include "TermMap.h"

namespace r2rml {

/**
 * A term map which always returns a fixed RDF term.
 */
class ConstantTermMap : public TermMap {
public:
    ConstantTermMap() = default;
    explicit ConstantTermMap(const SerdNode& node);
    ~ConstantTermMap() override;

    SerdNode generateRDFTerm(const SQLRow& row,
                             const SerdEnv& env) const override;

    SerdNode constantValue{0};
};

} // namespace r2rml
