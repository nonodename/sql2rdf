#pragma once

#include "TermMap.h"

#include <string>
#include <memory>

namespace r2rml {

/**
 * A term map defined by an RFC 6570–style template string.  Placeholders are
 * filled with column values from the current row.
 */
class TemplateTermMap : public TermMap {
public:
    TemplateTermMap() = default;
    explicit TemplateTermMap(const std::string& templ);
    ~TemplateTermMap() override;

    SerdNode generateRDFTerm(const SQLRow& row,
                             const SerdEnv& env) const override;

    bool isValid() const override {
        // templateString must not be empty
        return !templateString.empty();
    }

    std::string templateString;

private:
    /// Buffer for the last expanded URI; keeps buf pointer in returned SerdNode valid.
    mutable std::string expanded_;
};

} // namespace r2rml
