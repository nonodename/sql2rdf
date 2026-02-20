#pragma once

#include "TermMap.h"
#include <memory>

namespace r2rml {

/**
 * A term map that derives its value from a column in the logical table.
 */
class ColumnTermMap : public TermMap {
public:
    ColumnTermMap() = default;
    explicit ColumnTermMap(const std::string& column);
    ~ColumnTermMap() override;

    SerdNode generateRDFTerm(const SQLRow& row,
                             const SerdEnv& env) const override;

    bool isValid() const override {
        // columnName must not be empty
        return !columnName.empty();
    }

    std::string columnName;
};

} // namespace r2rml
