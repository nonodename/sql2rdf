#pragma once

#include "TermMap.h"

#include <string>
#include <memory>

namespace r2rml {

/**
 * A term map which always returns a fixed RDF term.
 *
 * The constructor deep-copies the string data from the supplied SerdNode so
 * that instances remain valid even after the source buffer is freed.
 */
class ConstantTermMap : public TermMap {
public:
    ConstantTermMap() = default;
    explicit ConstantTermMap(const SerdNode& node);
    ~ConstantTermMap() override;

    SerdNode generateRDFTerm(const SQLRow& row,
                             const SerdEnv& env) const override;

    bool isValid() const override {
        // constantValue must not be a null SerdNode
        return constantValue.type != 0;
    }

    SerdNode constantValue{0};

private:
    /// Owns the string data that constantValue.buf points into (when non-empty).
    std::string ownedUri_;
};

} // namespace r2rml
