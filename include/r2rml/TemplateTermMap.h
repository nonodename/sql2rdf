#pragma once

#include "TermMap.h"

#include <string>

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

    std::string templateString;
};

} // namespace r2rml
